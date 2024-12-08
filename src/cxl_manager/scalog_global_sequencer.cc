#include "scalog_global_sequencer.h"

ScalogGlobalSequencer::ScalogGlobalSequencer(std::string scalog_seq_address) {
	LOG(INFO) << "Starting Scalog global sequencer";

	global_epoch_ = 1;

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
    {
        absl::WriterMutexLock lock(&registered_brokers_mu_);
        registered_brokers_.insert(broker_id);

		if (registered_brokers_.size() == NUM_MAX_BROKERS) {
			std::thread global_cut_thread(&ScalogGlobalSequencer::SendGlobalCut, this);
		}
    }

    return grpc::Status::OK;
}

void ScalogGlobalSequencer::SendGlobalCut() {
	while (!shutdown_requested_) {
		GlobalCut global_cut;

		{
			absl::MutexLock lock(&stream_mu_);
			/// Convert global_cut_ to google::protobuf::Map<int64_t, int64_t>
			{
				absl::WriterMutexLock lock(&global_cut_mu_);
				for (const auto& entry : global_cut_[global_epoch_]) {
					global_cut.mutable_global_cut()->insert({entry.first, entry.second});
				}
				auto it = global_cut_.find(global_epoch_ - 2);
				if (it != global_cut_.end()) {
					// The element exists, so delete it
					global_cut_.erase(global_epoch_ - 2);
					logical_offsets_.erase(global_epoch_ - 2);
				}

				global_epoch_++;
			}

			for (auto local_sequencer : local_sequencers_) {
				if (!local_sequencer->Write(global_cut)) {
					std::cerr << "Error writing GlobalCut to the client" << std::endl;
				}
			}
		}

		// Sleep until interval passes to send next local cut
		std::this_thread::sleep_for(global_cut_interval_);
	}
}

grpc::Status ScalogGlobalSequencer::HandleSendLocalCut(grpc::ServerContext* context,
		grpc::ServerReaderWriter<GlobalCut, LocalCut>* stream) {

	{
		absl::MutexLock lock(&stream_mu_);
		local_sequencers_.push_back(std::shared_ptr<grpc::ServerReaderWriter<GlobalCut, LocalCut>>(stream));
	}

    std::thread receive_global_cut(&ScalogGlobalSequencer::ReceiveLocalCut, this, std::ref(stream));

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

void ScalogGlobalSequencer::ReceiveLocalCut(grpc::ServerReaderWriter<GlobalCut, LocalCut>* stream) {
	while (!stop_reading_from_stream_) {
		LocalCut request;
		if (stream->Read(&request)) {
			static char topic[TOPIC_NAME_SIZE];
			memcpy(topic, request.topic().c_str(), request.topic().size());
			int epoch = request.epoch();
			int local_cut = request.local_cut();
			int broker_id = request.broker_id();

			{
				absl::WriterMutexLock lock(&global_cut_mu_);
				if (epoch == 0) {
					global_cut_[global_epoch_][broker_id] = local_cut + 1;
					logical_offsets_[global_epoch_][broker_id] = local_cut;
				} else {
					global_cut_[global_epoch_][broker_id] = local_cut - logical_offsets_[global_epoch_ - 1][broker_id];
					logical_offsets_[global_epoch_][broker_id] = local_cut;			
				}
			}
		}
	}
}

int main(int argc, char* argv[]){
    // Initialize scalog global sequencer
    std::string scalog_seq_address = "192.168.60.172:50051";
    ScalogGlobalSequencer scalog_global_sequencer(scalog_seq_address);

    return 0;
}