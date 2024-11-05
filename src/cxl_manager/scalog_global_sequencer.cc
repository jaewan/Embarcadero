#include "scalog_global_sequencer.h"

ScalogGlobalSequencer::ScalogGlobalSequencer(std::string scalog_seq_address) {
	LOG(INFO) << "Starting Scalog global sequencer";

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
	scalog_server_->Shutdown();
	return grpc::Status::OK;
}

grpc::Status ScalogGlobalSequencer::HandleRegisterBroker(grpc::ServerContext* context,
        const RegisterBrokerRequest* request, RegisterBrokerResponse* response) {

	std::unique_lock<std::mutex> lock(mutex_);

    int broker_id = request->broker_id();
    {
        absl::WriterMutexLock lock(&registered_brokers_mu_);
        registered_brokers_.insert(broker_id);
    }

	// send back global epoch
	response->set_global_epoch(global_epoch_);

    return grpc::Status::OK;
}

grpc::Status ScalogGlobalSequencer::HandleSendLocalCut(grpc::ServerContext* context,
		const SendLocalCutRequest* request, SendLocalCutResponse* response) {
	static char topic[TOPIC_NAME_SIZE];
	memcpy(topic, request->topic().c_str(), request->topic().size());
	int epoch = request->epoch();
	int local_cut = request->local_cut();
	int broker_id = request->broker_id();

	{
		absl::WriterMutexLock lock(&global_cut_mu_);
		if (epoch == 0 || global_cut_[epoch - 1].find(broker_id) == global_cut_[epoch - 1].end()) {
			global_cut_[epoch][broker_id] = local_cut + 1;
			logical_offsets_[epoch][broker_id] = local_cut;
		} else {
			global_cut_[epoch][broker_id] = local_cut - logical_offsets_[epoch - 1][broker_id];
			logical_offsets_[epoch][broker_id] = local_cut;			
		}
	}

	ReceiveLocalCut(epoch, topic, broker_id);

	/// Convert global_cut_ to google::protobuf::Map<int64_t, int64_t>
	auto* mutable_global_cut = response->mutable_global_cut();
	{
		absl::ReaderMutexLock lock(&global_cut_mu_);
		for (const auto& entry : global_cut_[epoch]) {
			(*mutable_global_cut)[static_cast<int64_t>(entry.first)] = static_cast<int64_t>(entry.second);
		}
	}

	return grpc::Status::OK;
}

void ScalogGlobalSequencer::ReceiveLocalCut(int epoch, const char* topic, int broker_id) {
	if (epoch - 1 > global_epoch_) {
		// If the epoch is not the same as the current global epoch, there is an error
		LOG(ERROR) << "Local epoch: " << epoch << " while global epoch: " << global_epoch_;
	}

	std::unique_lock<std::mutex> lock(mutex_);

    absl::btree_set<int> registered_brokers;
    {
    absl::ReaderMutexLock lock(&registered_brokers_mu_);
	registered_brokers = registered_brokers_;
    }

	int local_cut_num = 0;
	{
		absl::ReaderMutexLock lock(&global_cut_mu_);
		local_cut_num = global_cut_[epoch].size();
	}
	if ((size_t)local_cut_num == registered_brokers.size()) {
		global_epoch_++;

		/// Notify all waiting grpc threads that all local cuts have been received
		cv_.notify_all();

		/// Safely delete older global cuts after all threads have been notified and processed
		auto it = global_cut_.find(epoch - 2);
		if (it != global_cut_.end()) {
			// The element exists, so delete it
			{
				absl::WriterMutexLock lock(&global_cut_mu_);
				global_cut_.erase(epoch - 2);
				logical_offsets_.erase(epoch - 2);
			}
		}
	} else {
		/// If we haven't received all local cuts, the grpc thread must wait until we do to send the correct global cut back to the caller
        cv_.wait(lock, [this, broker_id, epoch, registered_brokers]() {
			int local_cut_num = 0;
			{
				absl::ReaderMutexLock lock(&global_cut_mu_);
				local_cut_num = global_cut_[epoch].size();
			}
			if ((size_t)local_cut_num == registered_brokers.size()){
				return true;
			} else {
				return false;
			}
        });

	}
}

int main(int argc, char* argv[]){
    // Initialize scalog global sequencer
    std::string scalog_seq_address = "192.168.60.172:50051";
    ScalogGlobalSequencer scalog_global_sequencer(scalog_seq_address);

    return 0;
}