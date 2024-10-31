#include <chrono>

#include "heartbeat.h"

Status HeartBeatServiceImpl::GetClusterStatus(ServerContext* context, const ClientInfo* request,
				ClusterStatus* reply) {
	absl::flat_hash_set<int32_t> s(request->nodes_info().begin(), request->nodes_info().end());
	{
		absl::MutexLock lock(&mutex_);
		for (const auto &it : nodes_){
			if(s.contains(it.second.broker_id)){
				s.erase(it.second.broker_id);
			}else{
				reply->add_new_nodes(it.second.network_mgr_addr);
			}
		}
	}
	for (const auto &it : s){
		reply->add_removed_nodes(it);
	}
	return Status::OK;
}

Status HeartBeatServiceImpl::TerminateCluster(ServerContext* context, const google::protobuf::Empty* request,
				google::protobuf::Empty* response) {
	LOG(INFO) << "[HeartBeatServiceImpl] TerminateCluster called, shutting down server.";
	shutdown_ = true;

	if (heartbeat_thread_.joinable()) {
		heartbeat_thread_.join();  // Stop the heartbeat thread before shutting down the server
	}
	// Wait until other nodes in the cluster to shutdown
	while(nodes_.size() != 1){
		std::this_thread::yield();
	}
	 // Schedule the server shutdown
	std::thread([this]() {
		std::this_thread::sleep_for(std::chrono::seconds(1));
		server_->Shutdown();
	}).detach();
	return Status::OK;
}

Status HeartBeatServiceImpl::KillBrokers(ServerContext* context, const KillBrokersRequest* request,
		KillBrokersResponse* reply){
	size_t num_brokers_to_kill = request->num_brokers();
	{
		absl::MutexLock lock(&mutex_);
		if(num_brokers_to_kill >= nodes_.size()){
			LOG(ERROR) << "KillBrokersRequest:" << num_brokers_to_kill << " is larger than the cluster size:" << nodes_.size();
			reply->set_success(false);
			return Status::OK;
		}
		for (auto node_itr = nodes_.begin(); node_itr != nodes_.end();){
			// Does not kill head node
			int pid = std::stoi(node_itr->first);
			if(pid != 0){
				if(kill(pid, SIGKILL) != 0){
					if(errno == EAGAIN || errno == ESRCH){
						// Process might be gone, veryfying
						if(kill(pid, 0) == -1 && errno == ESRCH){
							// It is dead
							num_brokers_to_kill--;
							nodes_.erase(node_itr);
						}else{
							LOG(INFO) << "Killing process:" << pid << " failed:" << strerror(errno);
						}
					}else{
						LOG(INFO) << "Killing process:" << pid << " failed:" << strerror(errno);
					}
				}else{
					num_brokers_to_kill--;
					nodes_.erase(node_itr);
				}
				if(num_brokers_to_kill == 0){
					break;
				}
			}
			++node_itr;
		}
	}
	if(num_brokers_to_kill > 0){
		reply->set_success(false);
	}else{
		reply->set_success(true);
	}
	return Status::OK;
}

Status HeartBeatServiceImpl::CreateNewTopic(ServerContext* context, const CreateTopicRequest* request,
				CreateTopicResponse* reply){
	char topic[TOPIC_NAME_SIZE] = {0};
	memcpy(topic, request->topic().data(), request->topic().size());
	bool success = create_topic_entry_callback_(topic, (int)request->order(), (int)request->replication_factor(),
									(bool)request->replicate_tinode(), request->sequencer_type());
	reply->set_success(success);
	return Status::OK;
}

int HeartBeatServiceImpl::GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
				struct Embarcadero::MessageHeader** msg_to_order, struct Embarcadero::TInode *tinode){
	absl::flat_hash_set<int> copy_set(registered_brokers.begin(), registered_brokers.end());
	int ret = 0;
	{
		absl::MutexLock lock(&mutex_);
		for(const auto &node:nodes_){
			int broker_id = node.second.broker_id;
			if(registered_brokers.find(broker_id) == registered_brokers.end()){
				/*
					 while(tinode->offsets[broker_id].log_offset == 0){
					 _mm_pause(); //yield can cause deadlock in grpc. Instead of yield, make it spin
					 }
					 msg_to_order[broker_id] = ((struct Embarcadero::MessageHeader*)((uint8_t*)msg_to_order[broker_id] + tinode->offsets[broker_id].log_offset));
					 */
				ret++;
				registered_brokers.insert(broker_id);
			}else{
				copy_set.erase(broker_id);
			}
		}
	}
	for(auto broker_id : copy_set){
		registered_brokers.erase(broker_id);
		msg_to_order[broker_id] = nullptr;
	}
	return ret;
}


void HeartBeatServiceImpl::CheckHeartbeats(){
	static const int timeout = HEARTBEAT_INTERVAL * 3;
	while (!shutdown_) {
		std::this_thread::sleep_for(std::chrono::seconds(timeout));
		absl::MutexLock lock(&mutex_);
		auto now = std::chrono::steady_clock::now();
		for (auto it = nodes_.begin(); it != nodes_.end();) {
			// Do not check head node
			if(it->second.broker_id == 0){
				++it;
				continue;
			}
			if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_heartbeat).count() > timeout) {
				LOG(INFO) << "Node " << it->first << " is dead";
				auto key_to_erase = it->first;
				++it;
				nodes_.erase(key_to_erase);
			} else {
				++it;
			}
		}
	}
}
//************************* Client Part *************************

FollowerNodeClient::FollowerNodeClient(const std::string& node_id, const std::string& address,
		const std::shared_ptr<grpc::Channel>& channel)
	: node_id_(node_id), address_(address), head_alive_(true), wait_called_(false) {
		stub_ = HeartBeat::NewStub(channel);
		Register();
		heartbeat_thread_ = std::thread([this]() {
				this->HeartBeatLoop();
		});
	}

void FollowerNodeClient::Register(){
	NodeInfo node_info;
	node_info.set_node_id(node_id_);
	node_info.set_address(address_);

	RegistrationStatus reply;
	grpc::ClientContext context;

	Status status = stub_->RegisterNode(&context, node_info, &reply);
	if (status.ok() && reply.success()) {
		LOG(INFO) << "Node registered: " << reply.message();
		broker_id_ = reply.broker_id();
	} else {
		LOG(ERROR) << "Failed to register node: " << reply.message();
	}
}

void FollowerNodeClient::SendHeartbeat() {
	HeartbeatRequest request;
	request.set_node_id(node_id_);

	auto call = std::make_unique<AsyncClientCall>();
	call->response_reader = stub_->AsyncHeartbeat(&call->context, request, &cq_);
	call->response_reader->Finish(&call->reply, &call->status, call.get());

	// Set a deadline for the heartbeat
	gpr_timespec deadline = gpr_time_add(
			gpr_now(GPR_CLOCK_REALTIME),
			gpr_time_from_seconds(HEARTBEAT_INTERVAL, GPR_TIMESPAN));
	call->context.set_deadline(deadline);

	// Transfer ownership to the completion queue
	call.release();
}

bool FollowerNodeClient::CheckHeartBeatReply() {
	void* got_tag;
	bool ok;

	// Use a timeout when checking for replies
	gpr_timespec deadline = gpr_time_add(
			gpr_now(GPR_CLOCK_REALTIME),
			gpr_time_from_seconds(1, GPR_TIMESPAN)); // 1 second timeout

	while (!shutdown_) {
		grpc::CompletionQueue::NextStatus status = cq_.AsyncNext(&got_tag, &ok, deadline);

		if (status == grpc::CompletionQueue::SHUTDOWN) {
			LOG(ERROR) << "Completion Queue is down. Either the channel is down or the queue is down";
			break;
		}

		if (status == grpc::CompletionQueue::TIMEOUT) {
			break;;
		}

		auto* call = static_cast<AsyncClientCall*>(got_tag);
		if (shutdown_) {
			delete call;
			continue;
		}
		if (ok && call->status.ok() && call->reply.alive()) {
			if(call->reply.shutdown()){
				LOG(INFO) << "[CheckHeartBeatReply] terminate received. Terminating ";
				shutdown_ = true;
				return false;
			}
			head_alive_ = true;
		} else {
			head_alive_ = false;
			LOG(INFO) << "[CheckHeartBeatReply] dead ";
		}
		delete call;
	}
	return true;
}

void FollowerNodeClient::HeartBeatLoop() {
	while (!shutdown_) {
		SendHeartbeat();
		std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL / 2));
		if(!CheckHeartBeatReply()){
			wait_called_ = false;
			return;
		}
		if (!IsHeadAlive()) {
			LOG(ERROR) << "Head is down. Should initiating head election...";
		}
		std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL / 2));
	}

	// Drain the completion queue after shutdown
	void* ignored_tag;
	bool ignored_ok;
	while (cq_.Next(&ignored_tag, &ignored_ok)) {
		delete static_cast<AsyncClientCall*>(ignored_tag);
	}
}
