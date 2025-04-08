#include "scalog_global_sequencer.h"

// NOTE: The global sequencer will only begin sending global cuts after NUM_MAX_BROKERS have sent HandleRegisterBroker requests.
ScalogGlobalSequencer::ScalogGlobalSequencer(std::string scalog_seq_address) {
	LOG(INFO) << "Starting Scalog global sequencer with interval: " << SCALOG_SEQ_LOCAL_CUT_INTERVAL;

	global_epoch_ = 0;

    grpc::ServerBuilder builder;
    builder.AddListeningPort(scalog_seq_address, grpc::InsecureServerCredentials());
    builder.RegisterService(this);
    scalog_server_ = builder.BuildAndStart();
    scalog_server_->Wait();
}

grpc::Status ScalogGlobalSequencer::HandleTerminateGlobalSequencer(grpc::ServerContext* context,
		const TerminateGlobalSequencerRequest* request, TerminateGlobalSequencerResponse* response) {
	LOG(INFO) << "Terminating Scalog global sequencer";

    // Signal shutdown to waiting threads
    shutdown_requested_ = true;

    // auto deadline = std::chrono::system_clock::now();
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
			std::thread global_cut_thread(&ScalogGlobalSequencer::SendGlobalCut, this);
			global_cut_thread.detach();
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
				for (const auto& entry : global_cut_) {
					if (entry.second.empty()) {
						global_cut.mutable_global_cut()->insert({entry.first, 0});
						continue;
					}

					size_t num_replicas = entry.second.size();
					if (num_replicas < num_replicas_per_broker_) {
						LOG(INFO) << "Not enough replicas for broker " << entry.first << " to send global cut where num replicas is " << num_replicas_per_broker_;
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

			for (auto local_sequencer : local_sequencers_) {
				auto& stream = std::get<0>(local_sequencer);
				auto& stream_lock = std::get<1>(local_sequencer);

				{
					if (!stream->Write(global_cut)) {
						std::cerr << "Error writing GlobalCut to the client" << std::endl;
					}
				}
			}
		}

		// Sleep until interval passes to send next local cut
		std::this_thread::sleep_for(global_cut_interval_);
	}
}

grpc::Status ScalogGlobalSequencer::HandleSendLocalCut(grpc::ServerContext* context,
		grpc::ServerReaderWriter<GlobalCut, LocalCut>* stream) {

    // Create a lock associated with the stream
    auto stream_lock = std::make_shared<absl::Mutex>();

    auto shared_stream = std::shared_ptr<grpc::ServerReaderWriter<GlobalCut, LocalCut>>(stream);

	{
		absl::MutexLock lock(&stream_mu_);
        // Push a tuple containing the stream and its lock into local_sequencers_
        local_sequencers_.emplace_back(shared_stream, stream_lock);
	}

    std::thread receive_local_cut(&ScalogGlobalSequencer::ReceiveLocalCut, this, std::ref(stream), stream_lock);

	// Keep the stream open and active for the client (blocking call)
	while (true) {
		// Simulate keeping the stream alive, handling disconnection, etc.
		if (context->IsCancelled()) {
			break;
		}
		std::this_thread::yield();
	}

	stop_reading_from_stream_ = true;
}

void ScalogGlobalSequencer::ReceiveLocalCut(grpc::ServerReaderWriter<GlobalCut, LocalCut>* stream, std::shared_ptr<absl::Mutex> stream_lock) {
	while (!stop_reading_from_stream_) {
		LocalCut request;
		{
			if (stream->Read(&request)) {
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
}

int main(int argc, char* argv[]){
    // Initialize scalog global sequencer
    std::string scalog_seq_address = "128.110.219.89:50051";
    ScalogGlobalSequencer scalog_global_sequencer(scalog_seq_address);

    return 0;
}