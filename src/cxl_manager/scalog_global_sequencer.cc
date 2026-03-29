#include "scalog_global_sequencer.h"
#include <glog/logging.h>
#include <chrono>
#include <sstream>

namespace {

int ExpectedScalogBrokerCount() {
	const auto& config = Embarcadero::GetConfig().config();
	if (!config.cluster.data_broker_ids.empty()) {
		return static_cast<int>(config.cluster.data_broker_ids.size());
	}
	return NUM_MAX_BROKERS_CONFIG;
}

#ifdef SCALOG_SEQUENCER_IP
const char* ScalogSequencerIp() {
	return SCALOG_SEQUENCER_IP;
}
#else
const char* ScalogSequencerIp() {
	return SCLAOG_SEQUENCER_IP;
}
#endif

}  // namespace

// NOTE: The global sequencer will only begin sending global cuts after NUM_MAX_BROKERS have sent HandleRegisterBroker requests.
ScalogGlobalSequencer::ScalogGlobalSequencer(std::string scalog_seq_address) {
	LOG(INFO) << "Starting Scalog global sequencer with interval: " << SCALOG_SEQ_LOCAL_CUT_INTERVAL;

	global_epoch_ = 0;

    grpc::ServerBuilder builder;
    builder.AddListeningPort(scalog_seq_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);
    scalog_server_ = builder.BuildAndStart();
}

void ScalogGlobalSequencer::Run() {
	if (!scalog_server_) {
		LOG(ERROR) << "gRPC server failed to start (check bind address/port)";
		return;
	}
	scalog_server_->Wait();
}

grpc::Status ScalogGlobalSequencer::HandleTerminateGlobalSequencer(grpc::ServerContext* context,
		const TerminateGlobalSequencerRequest* request, TerminateGlobalSequencerResponse* response) {
	LOG(INFO) << "Terminating Scalog global sequencer";

    // Signal shutdown to waiting threads
	stop_reading_from_stream_ = true;

	if (global_cut_thread_.joinable()) {
		global_cut_thread_.join();
	}

	std::thread([this]() {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		scalog_server_->Shutdown();
	}).detach();

    return grpc::Status::OK;
}

grpc::Status ScalogGlobalSequencer::HandleRegisterBroker(grpc::ServerContext* context,
        const RegisterBrokerRequest* request, RegisterBrokerResponse* response) {

	std::unique_lock<std::mutex> lock(mutex_);

    int broker_id = request->broker_id();

	if (broker_id == 0) {
		num_replicas_per_broker_ = request->replication_factor();
	}

	{
        absl::WriterMutexLock lock(&registered_brokers_mu_);
		registered_brokers_.insert(broker_id);
		const int expected_brokers = ExpectedScalogBrokerCount();
		LOG(INFO) << "Scalog global sequencer registered broker=" << broker_id
		          << " registered=" << registered_brokers_.size()
		          << "/" << expected_brokers
		          << " replicas_per_broker=" << num_replicas_per_broker_;

		if ((int)registered_brokers_.size() == expected_brokers && !global_cut_thread_.joinable()) {
			global_cut_thread_ = std::thread(&ScalogGlobalSequencer::SendGlobalCut, this);
			LOG(INFO) << "Scalog global sequencer starting cut distribution thread";
		}
    }

    return grpc::Status::OK;
}

void ScalogGlobalSequencer::SendGlobalCut() {
	while (!shutdown_requested_) {
		GlobalCut global_cut;

		// TODO(Tony) Might not need this lock or might be able to move it to right before we begin iterating through local_sequencers_ vector.
		{
			absl::MutexLock lock(&stream_mu_);
			/// Convert global_cut_ to google::protobuf::Map<int64_t, int64_t>
			{
				absl::WriterMutexLock lock(&global_cut_mu_);
				// TODO(Tony) For now, ensure all local sequencers make connections to the global seq before we start distributing the global cut.
				size_t total_num_replicas = 0;
				for (const auto& [broker_id, replica_map] : global_cut_) {
					total_num_replicas += replica_map.size();
				}
				const size_t expected_replicas =
					static_cast<size_t>(ExpectedScalogBrokerCount()) * num_replicas_per_broker_;

				if (total_num_replicas != expected_replicas) {
					continue;
				}
				
				auto global_cut_copy = global_cut_;
				for (const auto& entry : global_cut_copy) {
					if (entry.second.empty()) {
						global_cut.mutable_global_cut()->insert({entry.first, 0});
						continue;
					}

					size_t num_replicas = entry.second.size();
					if (num_replicas < num_replicas_per_broker_) {
						global_cut.mutable_global_cut()->insert({entry.first, 0});
						continue;
					}

					auto min_entry = std::min_element(entry.second.begin(), entry.second.end(),
													[](const auto& a, const auto& b) {
														return a.second < b.second;
													});

					global_cut.mutable_global_cut()->insert({entry.first, min_entry->second});

					// Update all entries in last_sent_global_cut_[entry.first]
					for (const auto& replica_entry : entry.second) {
						global_cut_[entry.first][replica_entry.first] = global_cut_[entry.first][replica_entry.first] - min_entry->second;
					}
				}
			}

			static auto last_log_time = std::chrono::steady_clock::now();
			const auto now = std::chrono::steady_clock::now();
			if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_log_time).count() >= 2000) {
				std::ostringstream oss;
				bool first = true;
				for (const auto& entry : global_cut.global_cut()) {
					if (!first) {
						oss << ",";
					}
					first = false;
					oss << "b" << entry.first << "=" << entry.second;
				}
				LOG(INFO) << "Scalog global cut send replicas_seen=" << global_cut_.size()
				          << " streams=" << local_sequencers_.size()
				          << " cuts={" << oss.str() << "}";
				last_log_time = now;
			}

			for (auto&stream : local_sequencers_) {
				{
					if (!stream->Write(global_cut)) {}
				}
			}
		}

		// Sleep until interval passes to send next local cut
		//std::this_thread::sleep_for(std::chrono::milliseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL));
		std::this_thread::sleep_for(std::chrono::microseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL));
	}
}

grpc::Status ScalogGlobalSequencer::HandleSendLocalCut(grpc::ServerContext* context,
		grpc::ServerReaderWriter<GlobalCut, LocalCut>* stream) {

	{
		absl::MutexLock lock(&stream_mu_);
        local_sequencers_.emplace_back(stream);
	}

    std::thread receive_local_cut(&ScalogGlobalSequencer::ReceiveLocalCut, this, std::ref(stream));
	receive_local_cut.join();

	return grpc::Status::OK;
}

void ScalogGlobalSequencer::ReceiveLocalCut(grpc::ServerReaderWriter<GlobalCut, LocalCut>* stream) {
	while (!stop_reading_from_stream_) {
		LocalCut request;
		{
			if (stream && stream->Read(&request)) {
				static char topic[TOPIC_NAME_SIZE];
				memcpy(topic, request.topic().c_str(), request.topic().size());
				int epoch = request.epoch();
				int64_t local_cut = request.local_cut();
				int broker_id = request.broker_id();
				int replica_id = request.replica_id();

				{
					absl::WriterMutexLock lock(&global_cut_mu_);
					int64_t previous_cut = 0;
					auto broker_it = logical_offsets_.find(broker_id);
					if (broker_it != logical_offsets_.end()) {
						auto replica_it = broker_it->second.find(replica_id);
						if (replica_it != broker_it->second.end()) {
							previous_cut = replica_it->second;
						}
					}
					if (local_cut < previous_cut) {
						LOG(WARNING) << "Scalog global sequencer ignoring regressing local cut broker="
						             << broker_id << " replica=" << replica_id
						             << " previous=" << previous_cut << " current=" << local_cut;
					} else {
						global_cut_[broker_id][replica_id] += (local_cut - previous_cut);
						logical_offsets_[broker_id][replica_id] = local_cut;
						if ((epoch % 1000) == 0) {
							LOG(INFO) << "Scalog global sequencer received local cut broker=" << broker_id
							          << " replica=" << replica_id
							          << " epoch=" << epoch
							          << " local_cut=" << local_cut
							          << " delta=" << (local_cut - previous_cut);
						}
					}
				}
			}
		}
	}

	shutdown_requested_ = true;
}

int main(int argc, char* argv[]){
    // Initialize scalog global sequencer
    std::string scalog_seq_address = std::string(ScalogSequencerIp()) + ":" + std::to_string(SCALOG_SEQ_PORT);
    ScalogGlobalSequencer scalog_global_sequencer(scalog_seq_address);

	scalog_global_sequencer.Run();

    return 0;
}
