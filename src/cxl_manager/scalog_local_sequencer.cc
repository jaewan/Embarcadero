#include "scalog_local_sequencer.h"
#include "cxl_manager.h"

namespace Scalog {

//TODO (tony) priority 2 (failure test)  make the scalog code failure prone.
//Current logic proceeds epoch with all brokers at the same pace.
//If a broker fails, the entire cluster is stuck. If a failure is detected from the heartbeat, GetRegisteredBroker will return the alive brokers
//after heartbeat_interval (failure is detected), if there is a change in the cluster, only proceed with the brokers
ScalogLocalSequencer::ScalogLocalSequencer(Embarcadero::CXLManager* cxl_manager, int broker_id, void* cxl_addr, std::string topic_str) :
	cxl_manager_(cxl_manager),
	broker_id_(broker_id),
	cxl_addr_(cxl_addr) {

	// int unique_port = SCALOG_SEQ_PORT + scalog_local_sequencer_port_offset_.fetch_add(1);
	int unique_port = SCALOG_SEQ_PORT;
	std::string scalog_seq_address = scalog_global_sequencer_ip_ + ":" + std::to_string(unique_port);
	std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(scalog_seq_address, grpc::InsecureChannelCredentials());
	stub_ = ScalogSequencer::NewStub(channel);

	static char topic[TOPIC_NAME_SIZE];
	memcpy(topic, topic_str.data(), topic_str.size());
	struct TInode *tinode = cxl_manager_->GetTInode(topic);

	// Send register request to the global sequencer
	Register(tinode->replication_factor);
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
	}
}

void ScalogLocalSequencer::SendLocalCut(std::string topic_str){
	static char topic[TOPIC_NAME_SIZE];
	memcpy(topic, topic_str.data(), topic_str.size());
	struct TInode *tinode = cxl_manager_->GetTInode(topic);

	grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReaderWriter<LocalCut, GlobalCut>> stream(
        stub_->HandleSendLocalCut(&context));

	// Spawn a thread to receive global cuts, passing the stream by reference
    std::thread receive_global_cut(&ScalogLocalSequencer::ReceiveGlobalCut, this, std::ref(stream), topic_str);

	while (!cxl_manager_->GetStopThreads()) {
		/// Send epoch and tinode->offsets[broker_id_].written to global sequencer
		int local_cut = tinode->offsets[broker_id_].written;

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

		// Increment the epoch
		local_epoch_++;

		// Sleep until interval passes to send next local cut
		//std::this_thread::sleep_for(std::chrono::milliseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL));
		std::this_thread::sleep_for(std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL));
	}

	stream->WritesDone();
	stop_reading_from_stream_ = true;
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
	while (!stop_reading_from_stream_) {
		GlobalCut global_cut;
		if (stream->Read(&global_cut)) {
			// Convert google::protobuf::Map<int64_t, int64_t> to absl::flat_hash_map<int, int>
			for (const auto& entry : global_cut.global_cut()) {
				global_cut_[static_cast<int>(entry.first)] = static_cast<int>(entry.second);
			}

			ScalogSequencer(topic, global_cut_);

			num_global_cuts++;
		}
	}

    // grpc::Status status = stream->Finish();
}

void ScalogLocalSequencer::ScalogSequencer(const char* topic, absl::btree_map<int, int> &global_cut) {
	static char topic_char[TOPIC_NAME_SIZE];
	static size_t seq = 0;
	static TInode *tinode = nullptr;
	static MessageHeader* msg_to_order = nullptr;
	memcpy(topic_char, topic, TOPIC_NAME_SIZE);
	if(tinode == nullptr){
		tinode = cxl_manager_->GetTInode(topic);
		msg_to_order = ((MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id_].log_offset));
	}

	static auto last_log_time = std::chrono::steady_clock::now();
	static size_t written=0;
			auto now = std::chrono::steady_clock::now();
				if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count() >= 3000) {
					LOG(INFO) << "[DEBUG] [SCALOG] written:" << written;
					last_log_time = std::chrono::steady_clock::now();
				}
	for(auto &cut : global_cut){
		if(cut.first == broker_id_){
			for(int i = 0; i<cut.second; i++){
				msg_to_order->total_order = seq;
				std::atomic_thread_fence(std::memory_order_release);
				/*
				tinode->offsets[broker_id_].ordered = msg_to_order->logical_offset;
				tinode->offsets[broker_id_].ordered_offset = (uint8_t*)msg_to_order - (uint8_t*)cxl_addr_;
				*/
				cxl_manager_->UpdateTinodeOrder(topic_char, tinode, broker_id_, msg_to_order->logical_offset, (uint8_t*)msg_to_order - (uint8_t*)cxl_addr_);
				written = msg_to_order->logical_offset;
				msg_to_order = (MessageHeader*)((uint8_t*)msg_to_order + msg_to_order->next_msg_diff);
				seq++;
			}
		}else{
			seq += cut.second;
		}
	}
}

} // End of namespace Scalog
