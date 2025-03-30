#include "scalog_local_sequencer.h"

namespace Scalog {
    
//TODO (tony) priority 2 (failure test)  make the scalog code failure prone.
//Current logic proceeds epoch with all brokers at the same pace. 
//If a broker fails, the entire cluster is stuck. If a failure is detected from the heartbeat, GetRegisteredBroker will return the alive brokers
//after heartbeat_interval (failure is detected), if there is a change in the cluster, only proceed with the brokers
ScalogLocalSequencer::ScalogLocalSequencer(Embarcadero::CXLManager* cxl_manager, int broker_id, void* cxl_addr) :
	cxl_manager_(cxl_manager),
	broker_id_(broker_id),
	cxl_addr_(cxl_addr) {

	// int unique_port = SCALOG_SEQ_PORT + scalog_local_sequencer_port_offset_.fetch_add(1);
	int unique_port = SCALOG_SEQ_PORT;
	std::string scalog_seq_address = scalog_global_sequencer_ip_ + ":" + std::to_string(unique_port);
	std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(scalog_seq_address, grpc::InsecureChannelCredentials());
	stub_ = ScalogSequencer::NewStub(channel);

	// Send register request to the global sequencer
	Register();
	SetEpochToOrder(local_epoch_);
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

void ScalogLocalSequencer::Register() {
	RegisterBrokerRequest request;
	request.set_broker_id(broker_id_);

	RegisterBrokerResponse response;
	grpc::ClientContext context;

	grpc::Status status = stub_->HandleRegisterBroker(&context, request, &response);
	if (!status.ok()) {
		LOG(ERROR) << "Error registering local sequencer: " << status.error_message();
	}

	// Set local epoch to the global epoch
	local_epoch_ = response.global_epoch();
}

void ScalogLocalSequencer::LocalSequencer(std::string topic_str){
	static char topic[TOPIC_NAME_SIZE];
	memcpy(topic, topic_str.data(), topic_str.size());
	struct TInode *tinode = cxl_manager_->GetTInode(topic);

	auto start_time = std::chrono::high_resolution_clock::now();
	/// Send epoch and tinode->offsets[broker_id_].written to global sequencer
	int local_cut = tinode->offsets[broker_id_].written;
	SendLocalCut(local_cut, topic);
	auto end_time = std::chrono::high_resolution_clock::now();

	/// We measure the time it takes to send the local cut
	auto elapsed_time_us = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);
	
	/// In the case where we receive the global cut before the interval has passed, we wait for the remaining time left in the interval
	while(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now() - start_time) 
			< local_cut_interval_){
		std::this_thread::yield();
	}
}

void ScalogLocalSequencer::SendLocalCut(int local_cut, const char* topic) {
	SendLocalCutRequest request;
	request.set_epoch(local_epoch_);
	request.set_local_cut(local_cut);
	request.set_topic(topic);
	request.set_broker_id(broker_id_);

	SendLocalCutResponse response;
	grpc::ClientContext context;

	// Set a timeout of 5 seconds for the gRPC call
	auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
	context.set_deadline(deadline);

	// Synchronous call to HandleSendLocalCut
	grpc::Status status = stub_->HandleSendLocalCut(&context, request, &response);

	if (!status.ok()) {
		if (status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED) {
			LOG(ERROR) << "Timeout error sending local cut: " << status.error_message();
		} else {
			LOG(ERROR) << "Error sending local cut: " << status.error_message();
		}
	} else {
		// Convert google::protobuf::Map<int64_t, int64_t> to absl::flat_hash_map<int, int>
		for (const auto& entry : response.global_cut()) {
			global_cut_[local_epoch_][static_cast<int>(entry.first)] = static_cast<int>(entry.second);
		}

		ScalogSequencer(topic, global_cut_);
	}

	local_epoch_++;
}

void ScalogLocalSequencer::ScalogSequencer(const char* topic,
		absl::flat_hash_map<int, absl::btree_map<int, int>> &global_cut) {
	static char topic_char[TOPIC_NAME_SIZE];
	static size_t seq = 0;
	static TInode *tinode = nullptr; 
	static MessageHeader* msg_to_order = nullptr;
	memcpy(topic_char, topic, TOPIC_NAME_SIZE);
	if(tinode == nullptr){
		tinode = cxl_manager_->GetTInode(topic);
		msg_to_order = ((MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id_].log_offset));
	}

	if(global_cut.contains(epoch_to_order_)){
		for(auto &cut : global_cut[epoch_to_order_]){
			if(cut.first == broker_id_){
				for(int i = 0; i<cut.second; i++){
					msg_to_order->total_order = seq;
					std::atomic_thread_fence(std::memory_order_release);
					/*
					tinode->offsets[broker_id_].ordered = msg_to_order->logical_offset;
					tinode->offsets[broker_id_].ordered_offset = (uint8_t*)msg_to_order - (uint8_t*)cxl_addr_;
					*/
					cxl_manager_->UpdateTinodeOrder(topic_char, tinode, broker_id_, msg_to_order->logical_offset, (uint8_t*)msg_to_order - (uint8_t*)cxl_addr_);
					msg_to_order = (MessageHeader*)((uint8_t*)msg_to_order + msg_to_order->next_msg_diff);
					seq++;
				}
			}else{
				seq += cut.second;
			}
		}
	}else{
		LOG(ERROR) << "Expected Epoch:" << epoch_to_order_ << " is not received";
	}
	epoch_to_order_++;
}

} // End of namespace Scalog