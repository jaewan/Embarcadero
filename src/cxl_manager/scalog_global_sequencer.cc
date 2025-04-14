#include "scalog_global_sequencer.h"

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
		num_replicas_per_broker_ = request->replication_factor() + 1;
	}

    {
        absl::WriterMutexLock lock(&registered_brokers_mu_);
        registered_brokers_.insert(broker_id);

		if (registered_brokers_.size() == NUM_MAX_BROKERS) {
			global_cut_thread_ = std::thread(&ScalogGlobalSequencer::SendGlobalCut, this);
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
				if (global_cut_.size() != (NUM_MAX_BROKERS * num_replicas_per_broker_)) {
					continue;
				}

				for (const auto& entry : global_cut_) {
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
						last_sent_global_cut_[entry.first][replica_entry.first] = logical_offsets_[entry.first][min_entry->first];
					}
				}
			}

			for (auto&stream : local_sequencers_) {
				{
					if (!stream->Write(global_cut)) {}
				}
			}
		}

		// Sleep until interval passes to send next local cut
		std::this_thread::sleep_for(std::chrono::milliseconds(SCALOG_SEQ_LOCAL_CUT_INTERVAL));
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
					if (epoch == 0) {
						global_cut_[broker_id][replica_id] = local_cut + 1;
						logical_offsets_[broker_id][replica_id] = local_cut;
						last_sent_global_cut_[broker_id][replica_id] = -1;
					} else {
						global_cut_[broker_id][replica_id] = local_cut - last_sent_global_cut_[broker_id][replica_id];
						logical_offsets_[broker_id][replica_id] = local_cut;
					}
				}
			}
		}
	}

	shutdown_requested_ = true;
}

int main(int argc, char* argv[]){
    // Initialize scalog global sequencer
    std::string scalog_seq_address = std::string(SCLAOG_SEQUENCER_IP) + ":" + std::to_string(SCALOG_SEQ_PORT);
    ScalogGlobalSequencer scalog_global_sequencer(scalog_seq_address);

	scalog_global_sequencer.Run();

    return 0;
}
