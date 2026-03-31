#include "scalog_local_sequencer.h"
#include "cxl_manager.h"
#include <limits>

namespace Scalog {

//TODO (tony) priority 2 (failure test)  make the scalog code failure prone.
//Current logic proceeds epoch with all brokers at the same pace.
//If a broker fails, the entire cluster is stuck. If a failure is detected from the heartbeat, GetRegisteredBroker will return the alive brokers
//after heartbeat_interval (failure is detected), if there is a change in the cluster, only proceed with the brokers
ScalogLocalSequencer::ScalogLocalSequencer(TInode* tinode, int broker_id, void* cxl_addr, std::string topic_str, BatchHeader *batch_header) :
	tinode_(tinode),
	broker_id_(broker_id),
	cxl_addr_(cxl_addr),
	batch_header_(batch_header){

	int unique_port = SCALOG_SEQ_PORT;
	std::string scalog_seq_address = scalog_global_sequencer_ip_ + ":" + std::to_string(unique_port);
	std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(scalog_seq_address, grpc::InsecureChannelCredentials());
	stub_ = ScalogSequencer::NewStub(channel);

	// Send register request to the global sequencer
	Register(tinode_->replication_factor);
}

void ScalogLocalSequencer::TerminateGlobalSequencer() {
	TerminateGlobalSequencerRequest request;
	TerminateGlobalSequencerResponse response;
	grpc::ClientContext context;

	grpc::Status status = stub_->HandleTerminateGlobalSequencer(&context, request, &response);
	if (!status.ok()) {
		LOG(ERROR) << "Error terminating global sequencer: " << status.error_message();
	}
}

void ScalogLocalSequencer::Register(int replication_factor) {
	RegisterBrokerRequest request;
	request.set_broker_id(broker_id_);
	request.set_replication_factor(replication_factor);

	RegisterBrokerResponse response;
	grpc::ClientContext context;

	grpc::Status status = stub_->HandleRegisterBroker(&context, request, &response);
	if (!status.ok()) {
		LOG(ERROR) << "Error registering local sequencer: " << status.error_message();
	} else {
		LOG(INFO) << "Scalog local sequencer registered broker=" << broker_id_
		          << " replication_factor=" << replication_factor;
	}
}

void ScalogLocalSequencer::SendLocalCut(std::string topic_str, volatile bool& stop_thread){
	static char topic[TOPIC_NAME_SIZE];
	memcpy(topic, topic_str.data(), topic_str.size());
	const bool cxl_scalog_mode = []() {
		const char* env = std::getenv("SCALOG_CXL_MODE");
		return env && std::string(env) == "1";
	}();

	grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>> stream(
        stub_->HandleSendLocalCut(&context));

	// Spawn a thread to receive global cuts, passing the stream by reference
	std::thread receive_global_cut(&ScalogLocalSequencer::ReceiveGlobalCut, this, std::ref(stream), topic_str);

	while (!stop_thread) {
		int64_t local_cut = 0;
		if (cxl_scalog_mode) {
			volatile uint64_t* rep_done_ptr = &tinode_->offsets[broker_id_].replication_done[broker_id_];
			Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(rep_done_ptr)));
			Embarcadero::CXL::full_fence();
			const uint64_t rep_done = *rep_done_ptr;
			Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
				reinterpret_cast<const volatile void*>(&tinode_->offsets[broker_id_].validated_written_byte_offset)));
			Embarcadero::CXL::full_fence();
			const size_t validated = tinode_->offsets[broker_id_].validated_written_byte_offset;
			const size_t log_start = tinode_->offsets[broker_id_].log_offset;
			local_cut = (validated <= log_start || rep_done == std::numeric_limits<uint64_t>::max())
				? 0
				: static_cast<int64_t>(rep_done + 1);
		} else {
			local_cut = static_cast<int64_t>(tinode_->offsets[broker_id_].written);
		}

		LocalCut request;
		request.set_local_cut(local_cut);
		request.set_topic(topic);
		request.set_broker_id(broker_id_);
		request.set_epoch(local_epoch_);
		request.set_replica_id(replica_id_);

		// Send the LocalCut message to the server
		if (!stream->Write(request)) {
			std::cerr << "Stream to write local cut is closed, cleaning up..." << std::endl;
			break;
		}

		static thread_local auto last_log_time = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count() >= 2000) {
			LOG(INFO) << "Scalog local cut broker=" << broker_id_
			          << " replica=" << replica_id_
			          << " epoch=" << local_epoch_
			          << " local_cut=" << local_cut
			          << " ordered=" << tinode_->offsets[broker_id_].ordered
			          << " written=" << tinode_->offsets[broker_id_].written;
			last_log_time = now;
		}

		// Increment the epoch
		local_epoch_++;

		// Sleep until interval passes to send next local cut
		std::this_thread::sleep_for(std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL));
	}

	stream->WritesDone();
	stop_reading_from_stream_.store(true, std::memory_order_release);
	receive_global_cut.join();

	// If this is the head node, terminate the global sequencer
	if (broker_id_ == 0) {
		LOG(INFO) << "Scalog Terminating global sequencer";
		TerminateGlobalSequencer();
	}
}

void ScalogLocalSequencer::ReceiveGlobalCut(std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>>& stream, std::string topic_str) {
	static char topic[TOPIC_NAME_SIZE];
	memcpy(topic, topic_str.data(), topic_str.size());

	int num_global_cuts = 0;
	while (!stop_reading_from_stream_.load(std::memory_order_relaxed)) {
		GlobalCut global_cut;
		if (stream->Read(&global_cut)) {
			// Wire format is cumulative per broker. Convert to monotonic per-broker deltas
			// so each cut segment is applied exactly once locally.
			absl::btree_map<int, int64_t> global_cut_delta;
			for (const auto& entry : global_cut.global_cut()) {
				const int broker = static_cast<int>(entry.first);
				const int64_t cumulative_cut = static_cast<int64_t>(entry.second);
				global_cut_[broker] = cumulative_cut;

				const int64_t prev_applied = last_applied_global_cut_[broker];
				if (cumulative_cut < prev_applied) {
					LOG(WARNING) << "Scalog local sequencer ignoring regressing cumulative global cut broker="
					             << broker
					             << " previous_applied=" << prev_applied
					             << " current=" << cumulative_cut;
					continue;
				}

				const int64_t delta = cumulative_cut - prev_applied;
				if (delta > 0) {
					global_cut_delta[broker] = delta;
					last_applied_global_cut_[broker] = cumulative_cut;
				}
			}

			if (!global_cut_delta.empty()) {
				ScalogSequencer(topic, global_cut_delta);
			}

			num_global_cuts++;
			if ((num_global_cuts % 1000) == 1) {
				auto it = global_cut_.find(broker_id_);
				const int64_t local_cumulative = (it == global_cut_.end()) ? -1 : it->second;
				const auto applied_it = last_applied_global_cut_.find(broker_id_);
				const int64_t local_applied = (applied_it == last_applied_global_cut_.end()) ? 0 : applied_it->second;
				LOG(INFO) << "Scalog global cut received broker=" << broker_id_
				          << " num_global_cuts=" << num_global_cuts
				          << " local_cumulative=" << local_cumulative
				          << " local_applied=" << local_applied
				          << " map_size=" << global_cut_.size();
			}
		}
	}
}

void ScalogLocalSequencer::ScalogSequencer(const char* topic, absl::btree_map<int, int64_t> &global_cut_delta) {
	static char topic_char[TOPIC_NAME_SIZE];
	static size_t seq = 0;
	static TInode *tinode = nullptr;
	static MessageHeader* msg_to_order = nullptr;
	static size_t batch_header_idx = 0;
	const size_t kNumBatchSlots = BATCHHEADERS_SIZE / sizeof(BatchHeader);

	memcpy(topic_char, topic, TOPIC_NAME_SIZE);
	if(tinode == nullptr){
		tinode = tinode_;
		msg_to_order = ((MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id_].log_offset));
	}

	size_t total_size = 0;
	void* start_addr = (void*)msg_to_order;
	bool local_progress = false;

	// [[CORRECTNESS_FIX]] Track the last ordered values locally. We only publish
	// tinode->offsets[].ordered AFTER the batch header export slot is written, so
	// ACK1 (which reads ordered) never exceeds export visibility.
	uint64_t last_ordered_count = 0;
	size_t last_ordered_offset = 0;

	auto publish_batch = [&](void* batch_start_addr, size_t publish_size) {
		if (publish_size == 0 || batch_start_addr == nullptr) {
			return;
		}
		const size_t slot = batch_header_idx % kNumBatchSlots;
		batch_header_[slot].batch_off_to_export = 0;
		batch_header_[slot].total_size = publish_size;
		batch_header_[slot].log_idx = static_cast<size_t>(
				static_cast<uint8_t*>(batch_start_addr) - static_cast<uint8_t*>(cxl_addr_));
		batch_header_[slot].ordered = 1;
		Embarcadero::CXL::flush_cacheline(&batch_header_[slot]);
		Embarcadero::CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(&batch_header_[slot]) + 64);
		Embarcadero::CXL::store_fence();
		batch_header_idx++;

		// [[CORRECTNESS_FIX]] Publish tinode ordered frontier AFTER the export batch
		// header is visible. This ensures ACK1 <= export visibility.
		tinode->offsets[broker_id_].ordered = last_ordered_count;
		tinode->offsets[broker_id_].ordered_offset = last_ordered_offset;
		Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(&tinode->offsets[broker_id_].ordered)));
		Embarcadero::CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(&tinode->offsets[broker_id_].ordered_offset)));
		Embarcadero::CXL::store_fence();
	};
	for(auto &cut : global_cut_delta){
		if(cut.first == broker_id_){
			for(int64_t i = 0; i < cut.second; i++){
				local_progress = true;
				total_size += msg_to_order->paddedSize;
				msg_to_order->total_order = seq;
				std::atomic_thread_fence(std::memory_order_release);

				// Track locally; tinode update deferred to publish_batch
				last_ordered_count = msg_to_order->logical_offset + 1;
				last_ordered_offset = (uint8_t*)msg_to_order - (uint8_t*)cxl_addr_;

				msg_to_order = (MessageHeader*)((uint8_t*)msg_to_order + msg_to_order->next_msg_diff);
				seq++;
				if(total_size >= BATCH_SIZE){
					publish_batch(start_addr, total_size);
					start_addr = (void*)msg_to_order;
					total_size = 0;
				}
			}
		}else{
			seq += static_cast<size_t>(cut.second);
		}
	}
	if (local_progress && total_size > 0) {
		publish_batch(start_addr, total_size);
	}

	if (!global_cut_delta.empty()) {
		static thread_local auto last_order_log = std::chrono::steady_clock::now();
		const auto now = std::chrono::steady_clock::now();
		if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_order_log).count() >= 2000) {
			auto local_it = global_cut_delta.find(broker_id_);
			const int64_t local_delta = (local_it == global_cut_delta.end()) ? 0 : local_it->second;
			LOG(INFO) << "Scalog sequencer advanced broker=" << broker_id_
			          << " local_delta=" << local_delta
			          << " ordered=" << tinode_->offsets[broker_id_].ordered
			          << " seq=" << seq;
			last_order_log = now;
		}
	}
}

} // End of namespace Scalog
