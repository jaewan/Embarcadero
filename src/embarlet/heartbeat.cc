#include <chrono>
#include <algorithm>
#include <cstring>
#include "heartbeat.h"

namespace heartbeat_system {

//
// HeartBeatServiceImpl implementation
//

HeartBeatServiceImpl::HeartBeatServiceImpl(std::string head_addr) {
	// Insert head node to the nodes_ map
	int head_broker_id = 0;
	std::string head_network_addr = head_addr + ":" + std::to_string(PORT + head_broker_id);

	nodes_["0"] = {
		head_broker_id,
		head_addr,
		head_network_addr,
		std::chrono::steady_clock::now()
	};

	// Start a thread to check follower node heartbeats
	heartbeat_thread_ = std::thread([this]() {
			this->CheckHeartbeats();
			});
}

HeartBeatServiceImpl::~HeartBeatServiceImpl() {
	shutdown_ = true;
	if (heartbeat_thread_.joinable()) {
		heartbeat_thread_.join();
	}
	LOG(INFO) << "[HeartBeatServiceImpl] Destructed";
}

Status HeartBeatServiceImpl::RegisterNode(
		ServerContext* context,
		const NodeInfo* request,
		RegistrationStatus* reply) {

	bool need_version_increment = false;

	{
		absl::MutexLock lock(&mutex_);
		auto nodes_it = nodes_.find(request->node_id());
		int broker_id = static_cast<int>(nodes_.size());

		if (nodes_it != nodes_.end() || broker_id >= NUM_MAX_BROKERS) {
			reply->set_success(false);
			reply->set_broker_id(broker_id);

			if (broker_id < NUM_MAX_BROKERS) {
				reply->set_message("Node already registered");
			} else {
				reply->set_message("Trying to Register too many brokers. Increase NUM_MAX_BROKERS");
			}
		} else {
			VLOG(3) << "Registering node:" << request->address() << " broker:" << broker_id;
			std::string network_mgr_addr = request->address() + ":" + std::to_string(broker_id + PORT);

			nodes_[request->node_id()] = {
				broker_id,
				request->address(),
				network_mgr_addr,
				std::chrono::steady_clock::now()
			};

			reply->set_success(true);
			reply->set_broker_id(broker_id);
			reply->set_message("Node registered successfully");

			// Mark for version increment
			need_version_increment = true;
		}
	}

	// Update cluster version if needed
	if (need_version_increment) {
		absl::MutexLock lock(&cluster_mutex_);
		cluster_version_++;

		// Notify subscribers
		ClusterStatus cluster_status;
		absl::MutexLock lock_mutex(&mutex_);
		for (const auto& node : nodes_) {
			if (node.second.broker_id == nodes_[request->node_id()].broker_id) {
				cluster_status.add_new_nodes(node.second.network_mgr_addr);
				break;
			}
		}

		absl::MutexLock subscribers_lock(&subscriber_mutex_);
		for (auto& writer : subscribers_) {
			if (writer) {
				writer->Write(cluster_status);
			}
		}
	}

	return grpc::Status::OK;
}

grpc::Status HeartBeatServiceImpl::Heartbeat(
		grpc::ServerContext* context,
		const HeartbeatRequest* request,
		HeartbeatResponse* reply) {

	bool node_exists = false;
	bool force_full_update = false;

	{
		absl::MutexLock lock(&mutex_);
		auto it = nodes_.find(request->node_id());
		reply->set_shutdown(false);

		if (it != nodes_.end()) {
			it->second.last_heartbeat = std::chrono::steady_clock::now();
			node_exists = true;

			if (shutdown_) {
				reply->set_shutdown(true);
				nodes_.erase(it);
			}
		}

		// Check if client needs a full update
		force_full_update = (request->cluster_version() < cluster_version_);
	}

	reply->set_alive(node_exists);

	// Fill cluster info if needed
	FillClusterInfo(reply, force_full_update);

	return grpc::Status::OK;
}

grpc::Status HeartBeatServiceImpl::SubscribeToCluster(
		grpc::ServerContext* context,
		const ClientInfo* request,
		grpc::ServerWriter<ClusterStatus>* writer) {

	// Create initial status to send to the new subscriber
	ClusterStatus initial_status;
	absl::flat_hash_set<int32_t> known_nodes(
			request->nodes_info().begin(),
			request->nodes_info().end()
			);

	{
		absl::MutexLock lock(&mutex_);
		for (const auto& node_entry : nodes_) {
			int broker_id = node_entry.second.broker_id;

			if (known_nodes.contains(broker_id)) {
				known_nodes.erase(broker_id);
			} else {
				initial_status.add_new_nodes(node_entry.second.network_mgr_addr);
			}
		}
	}

	// Send initial status
	writer->Write(initial_status);

	// Register this writer for future updates
	std::shared_ptr<grpc::ServerWriter<ClusterStatus>> shared_writer =
		std::make_shared<grpc::ServerWriter<ClusterStatus>>(*writer);

	{
		absl::MutexLock lock(&subscriber_mutex_);
		subscribers_.push_back(shared_writer);
	}

	// Keep the stream open and active for the client (blocking call)
	while (!context->IsCancelled()) {
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	// Cleanup the subscriber on disconnection
	{
		absl::MutexLock lock(&subscriber_mutex_);
		auto it = std::find_if(
				subscribers_.begin(),
				subscribers_.end(),
				[writer](const std::shared_ptr<grpc::ServerWriter<ClusterStatus>>& w) {
				return w.get() == writer;
				}
				);

		if (it != subscribers_.end()) {
			subscribers_.erase(it);
		}
	}

	return grpc::Status::OK;
}

grpc::Status HeartBeatServiceImpl::GetClusterStatus(
		grpc::ServerContext* context,
		const ClientInfo* request,
		ClusterStatus* reply) {

	absl::flat_hash_set<int32_t> known_nodes(
			request->nodes_info().begin(),
			request->nodes_info().end()
			);

	{
		absl::MutexLock lock(&mutex_);
		for (const auto& node_entry : nodes_) {
			int broker_id = node_entry.second.broker_id;

			if (known_nodes.contains(broker_id)) {
				known_nodes.erase(broker_id);
			} else {
				reply->add_new_nodes(node_entry.second.network_mgr_addr);
			}
		}
	}

	// Add removed nodes (nodes the client knows about but are no longer in the cluster)
	for (const auto& broker_id : known_nodes) {
		reply->add_removed_nodes(broker_id);
	}

	return grpc::Status::OK;
}

grpc::Status HeartBeatServiceImpl::TerminateCluster(
		grpc::ServerContext* context,
		const google::protobuf::Empty* request,
		google::protobuf::Empty* response) {

	LOG(INFO) << "[HeartBeatServiceImpl] TerminateCluster called, shutting down server.";
	shutdown_ = true;

	if (heartbeat_thread_.joinable()) {
		heartbeat_thread_.join();  // Stop the heartbeat thread before shutting down the server
	}

	// Wait until other nodes in the cluster have shut down
	while (true) {
		{
			absl::MutexLock lock(&mutex_);
			if (nodes_.size() <= 1) {
				break;
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(100));
	}

	// Schedule the server shutdown
	std::thread([this]() {
			std::this_thread::sleep_for(std::chrono::seconds(1));
			server_->Shutdown();
			}).detach();

	return grpc::Status::OK;
}

grpc::Status HeartBeatServiceImpl::KillBrokers(
		grpc::ServerContext* context,
		const KillBrokersRequest* request,
		KillBrokersResponse* reply) {

	size_t num_brokers_to_kill = request->num_brokers();
	bool success = true;

	{
		absl::MutexLock lock(&mutex_);
		if (num_brokers_to_kill >= nodes_.size() || num_brokers_to_kill == 0) {
			LOG(ERROR) << "KillBrokersRequest:" << num_brokers_to_kill
				<< " is larger (or 0) than the cluster size:" << nodes_.size();
			success = false;
		} else {
			for (auto node_itr = nodes_.begin(); node_itr != nodes_.end() && num_brokers_to_kill > 0;) {
				// Don't kill the head node
				int pid = std::stoi(node_itr->first);
				bool killed_broker = false;

				if (pid != 0) {
					if (kill(pid, SIGKILL) != 0) {
						if (errno == EAGAIN || errno == ESRCH) {
							// Process might be gone, verify
							if (kill(pid, 0) == -1 && errno == ESRCH) {
								// It is dead
								auto current = node_itr++;
								nodes_.erase(current);
								num_brokers_to_kill--;
								killed_broker = true;
							} else {
								LOG(INFO) << "Killing process:" << pid << " failed:" << strerror(errno);
							}
						} else {
							LOG(INFO) << "Killing process:" << pid << " failed:" << strerror(errno);
						}
					} else {
						auto current = node_itr++;
						nodes_.erase(current);
						num_brokers_to_kill--;
						killed_broker = true;
					}

					if (num_brokers_to_kill == 0) {
						break;
					}
				}

				if (!killed_broker) {
					++node_itr;
				}
			}

			success = (num_brokers_to_kill == 0);
		}
	}

	reply->set_success(success);
	return grpc::Status::OK;
}

grpc::Status HeartBeatServiceImpl::CreateNewTopic(
		grpc::ServerContext* context,
		const CreateTopicRequest* request,
		CreateTopicResponse* reply) {

	char topic[TOPIC_NAME_SIZE] = {0};
	size_t copy_size = std::min(request->topic().size(), static_cast<size_t>(TOPIC_NAME_SIZE - 1));
	memcpy(topic, request->topic().data(), copy_size);

	bool success = create_topic_entry_callback_(
			topic,
			static_cast<int>(request->order()),
			static_cast<int>(request->replication_factor()),
			static_cast<bool>(request->replicate_tinode()),
			request->sequencer_type()
			);

	reply->set_success(success);
	return grpc::Status::OK;
}

int HeartBeatServiceImpl::GetRegisteredBrokers(
		absl::btree_set<int>& registered_brokers,
		struct Embarcadero::MessageHeader** msg_to_order,
		struct Embarcadero::TInode* tinode) {

	absl::flat_hash_set<int> copy_set(registered_brokers.begin(), registered_brokers.end());
	int num_new_brokers = 0;

	{
		absl::MutexLock lock(&mutex_);
		for (const auto& node_entry : nodes_) {
			int broker_id = node_entry.second.broker_id;

			if (registered_brokers.find(broker_id) == registered_brokers.end()) {
				// This is a new broker
				num_new_brokers++;
				registered_brokers.insert(broker_id);
			} else {
				copy_set.erase(broker_id);
			}
		}
	}

	// Remove brokers that are no longer registered
	for (auto broker_id : copy_set) {
		registered_brokers.erase(broker_id);
		msg_to_order[broker_id] = nullptr;
	}

	return num_new_brokers;
}

std::string HeartBeatManager::GetNextBrokerAddr(int broker_id) {
	if (is_head_node_) {
		return service_->GetNextBrokerAddr(broker_id);
	} else {
		return follower_->GetNextBrokerAddr(broker_id);
	}
}

//TODO(Jae) Placeholder
std::string HeartBeatServiceImpl::GetNextBrokerAddr(int broker_id) {
	return nullptr;
}

int HeartBeatServiceImpl::GetNumBrokers () {
	static size_t initial_num_node = nodes_.size();
	if (initial_num_node != nodes_.size()) {
		LOG(WARNING) << "Number of nodes in the Cluster changed";
	}
	return (int)nodes_.size();
}


void HeartBeatServiceImpl::CheckHeartbeats() {
	static const int timeout_seconds = HEARTBEAT_INTERVAL * 3;
	bool cluster_changed = false;

	while (!shutdown_) {
		std::this_thread::sleep_for(std::chrono::seconds(timeout_seconds));
		cluster_changed = false;

		{
			absl::MutexLock lock(&mutex_);
			auto now = std::chrono::steady_clock::now();

			for (auto it = nodes_.begin(); it != nodes_.end();) {
				// Don't check head node
				if (it->second.broker_id == 0) {
					++it;
					continue;
				}

				auto time_since_last_heartbeat = std::chrono::duration_cast<std::chrono::seconds>(
						now - it->second.last_heartbeat
						).count();

				if (time_since_last_heartbeat > timeout_seconds) {
					LOG(INFO) << "Node " << it->first << " is dead";
					auto key_to_erase = it->first;
					++it;
					nodes_.erase(key_to_erase);
					cluster_changed = true;
				} else {
					++it;
				}
			}
		}

		// Update cluster version if nodes were removed
		if (cluster_changed) {
			absl::MutexLock lock(&cluster_mutex_);
			cluster_version_++;
		}
	}
}

void HeartBeatServiceImpl::FillClusterInfo(HeartbeatResponse* reply, bool force_full_update) {
	absl::MutexLock lock(&mutex_);

	// Set the current cluster version
	reply->set_cluster_version(cluster_version_);

	// If force_full_update is true or if there's been a change since the client's last update
	if (force_full_update) {
		// Add all brokers to the response
		for (const auto& node_entry : nodes_) {
			auto* broker_info = reply->add_cluster_info();
			broker_info->set_broker_id(node_entry.second.broker_id);
			broker_info->set_address(node_entry.second.address);
			broker_info->set_network_mgr_addr(node_entry.second.network_mgr_addr);
		}
	}
}

void HeartBeatServiceImpl::SetServer(std::shared_ptr<grpc::Server> server) {
	server_ = server;
}

void HeartBeatServiceImpl::RegisterCreateTopicEntryCallback(
		Embarcadero::CreateTopicEntryCallback callback) {
	create_topic_entry_callback_ = callback;
}

//
// FollowerNodeClient implementation
//

FollowerNodeClient::FollowerNodeClient(
		const std::string& node_id,
		const std::string& address,
		const std::shared_ptr<grpc::Channel>& channel)
	: node_id_(node_id), address_(address) {

		stub_ = HeartBeat::NewStub(channel);
		Register();

		heartbeat_thread_ = std::thread([this]() {
				this->HeartBeatLoop();
				});
	}

FollowerNodeClient::~FollowerNodeClient() {
	shutdown_ = true;
	cq_.Shutdown();

	if (!wait_called_ && heartbeat_thread_.joinable()) {
		heartbeat_thread_.join();
	}
}

void FollowerNodeClient::Wait() {
	wait_called_ = true;

	if (heartbeat_thread_.joinable()) {
		heartbeat_thread_.join();
	}
}

int FollowerNodeClient::GetNumBrokers() {
	static size_t initial_num_node = cluster_nodes_.size();
	if (initial_num_node != cluster_nodes_.size()) {
		LOG(WARNING) << "Number of nodes in the Cluster changed";
	}
	return (int)cluster_nodes_.size();
}

void FollowerNodeClient::Register() {
	NodeInfo node_info;
	node_info.set_node_id(node_id_);
	node_info.set_address(address_);

	RegistrationStatus reply;
	grpc::ClientContext context;

	grpc::Status status = stub_->RegisterNode(&context, node_info, &reply);

	if (status.ok() && reply.success()) {
		LOG(INFO) << "Node registered: " << reply.message();
		broker_id_ = reply.broker_id();
	} else {
		LOG(ERROR) << "Failed to register node: " << reply.message();
		// Consider setting head_alive_ to false or retrying
	}
}

void FollowerNodeClient::SendHeartbeat() {
	HeartbeatRequest request;
	request.set_node_id(node_id_);

	{
		absl::MutexLock lock(&cluster_mutex_);
		request.set_cluster_version(cluster_version_);
	}

	auto call = std::make_unique<AsyncClientCall>();

	// Set a deadline for the heartbeat
	gpr_timespec deadline = gpr_time_add(
			gpr_now(GPR_CLOCK_REALTIME),
			gpr_time_from_seconds(HEARTBEAT_INTERVAL, GPR_TIMESPAN)
			);
	call->context.set_deadline(deadline);

	call->response_reader = stub_->AsyncHeartbeat(&call->context, request, &cq_);
	call->response_reader->Finish(&call->reply, &call->status, call.get());

	// Transfer ownership to the completion queue
	call.release();
}

bool FollowerNodeClient::CheckHeartBeatReply() {
	void* got_tag;
	bool ok;

	// Use a timeout when checking for replies
	gpr_timespec deadline = gpr_time_add(
			gpr_now(GPR_CLOCK_REALTIME),
			gpr_time_from_seconds(1, GPR_TIMESPAN) // 1 second timeout
			);

	while (!shutdown_) {
		grpc::CompletionQueue::NextStatus status = cq_.AsyncNext(&got_tag, &ok, deadline);

		if (status == grpc::CompletionQueue::SHUTDOWN) {
			LOG(ERROR) << "Completion Queue is down. Either the channel is down or the queue is down";
			break;
		}

		if (status == grpc::CompletionQueue::TIMEOUT) {
			break;
		}

		auto* call = static_cast<AsyncClientCall*>(got_tag);

		if (shutdown_) {
			delete call;
			continue;
		}

		if (ok && call->status.ok() && call->reply.alive()) {
			if (call->reply.shutdown()) {
				LOG(INFO) << "[CheckHeartBeatReply] terminate received. Terminating ";
				shutdown_ = true;
				delete call;
				return false;
			}
			head_alive_ = true;

			// Process cluster info in the response
			ProcessClusterInfo(call->reply);
		} else {
			head_alive_ = false;
			LOG(INFO) << "[CheckHeartBeatReply] head node is dead";
		}

		delete call;
	}

	return true;
}

void FollowerNodeClient::HeartBeatLoop() {
	const auto half_interval = std::chrono::seconds(HEARTBEAT_INTERVAL / 2);

	while (!shutdown_) {
		SendHeartbeat();
		std::this_thread::sleep_for(half_interval);

		if (!CheckHeartBeatReply()) {
			wait_called_ = false;
			return;
		}

		if (!IsHeadAlive()) {
			LOG(ERROR) << "Head is down. Should initiate head election...";
			// Head election logic could be implemented here
		}

		std::this_thread::sleep_for(half_interval);
	}

	// Drain the completion queue after shutdown
	void* ignored_tag;
	bool ignored_ok;

	while (cq_.Next(&ignored_tag, &ignored_ok)) {
		delete static_cast<AsyncClientCall*>(ignored_tag);
	}
}

void FollowerNodeClient::ProcessClusterInfo(const HeartbeatResponse& reply) {
	if (!reply.cluster_info_size()) {
		return;  // No cluster info to process
	}

	// Only process if this is a newer version
	uint64_t new_version = reply.cluster_version();

	{
		absl::MutexLock lock(&cluster_mutex_);
		if (new_version <= cluster_version_ && reply.cluster_info_size() == 0) {
			return;  // We already have this version or newer
		}

		// Clear existing data for full update
		if (reply.cluster_info_size() > 0) {
			cluster_nodes_.clear();

			// Process all nodes
			for (const auto& broker_info : reply.cluster_info()) {
				NodeEntry entry;
				entry.broker_id = broker_info.broker_id();
				entry.address = broker_info.address();
				entry.network_mgr_addr = broker_info.network_mgr_addr();

				cluster_nodes_[entry.broker_id] = entry;
			}

			// Update our version
			cluster_version_ = new_version;
			LOG(INFO) << "Updated cluster info to version " << new_version
				<< " with " << cluster_nodes_.size() << " nodes";
		}
	}
}
//
// HeartBeatManager implementation
//

HeartBeatManager::HeartBeatManager(bool is_head_node, std::string head_address)
	: is_head_node_(is_head_node) {

		if (is_head_node) {
			service_ = std::make_unique<HeartBeatServiceImpl>(GetAddress());

			grpc::ServerBuilder builder;
			builder.AddListeningPort(head_address, grpc::InsecureServerCredentials());
			builder.RegisterService(service_.get());
			server_ = builder.BuildAndStart();

			service_->SetServer(server_);
		} else {
			follower_ = std::make_unique<FollowerNodeClient>(
					GetPID(),
					GetAddress(),
					grpc::CreateChannel(head_address, grpc::InsecureChannelCredentials())
					);
		}
	}

void HeartBeatManager::Wait() {
	if (is_head_node_) {
		server_->Wait();
	} else {
		follower_->Wait();
	}
}

int HeartBeatManager::GetBrokerId() {
	if (is_head_node_) {
		return 0;
	}
	return follower_->GetBrokerId();
}

int HeartBeatManager::GetRegisteredBrokers(
		absl::btree_set<int>& registered_brokers,
		struct Embarcadero::MessageHeader** msg_to_order,
		struct Embarcadero::TInode* tinode) {

	if (is_head_node_) {
		return service_->GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
	} else {
		LOG(ERROR) << "GetRegisteredBrokers should not be called from non-head brokers, "
			<< "this is for sequencer";
		return 0;
	}
}

//TODO(Jae) this is not correct. Find next broker not current
std::string FollowerNodeClient::GetNextBrokerAddr(int broker_id) {
	absl::MutexLock lock(&cluster_mutex_);
	auto it = cluster_nodes_.find(broker_id);
	if (it != cluster_nodes_.end()) {
		return it->second.network_mgr_addr;
	}
	return "";
}

int HeartBeatManager::GetNumBrokers () {
	if (is_head_node_) {
		return service_->GetNumBrokers();
	} else {
		return follower_->GetNumBrokers();
	}
}

void HeartBeatManager::RegisterCreateTopicEntryCallback(
		Embarcadero::CreateTopicEntryCallback callback) {

	if (is_head_node_) {
		service_->RegisterCreateTopicEntryCallback(callback);
	}
}

std::string HeartBeatManager::GetPID() {
	return std::to_string(getpid());
}

std::string HeartBeatManager::GenerateUniqueId() {
	// Get current timestamp
	auto now = std::chrono::system_clock::now();
	auto now_ms = std::chrono::time_point_cast<std::chrono::milliseconds>(now);
	auto value = now_ms.time_since_epoch();
	long long timestamp = value.count();

	// Generate a random number
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(0, 999999);
	int random_num = dis(gen);

	// Combine timestamp and random number
	std::stringstream ss;
	ss << std::hex << std::setfill('0')
		<< std::setw(12) << timestamp
		<< std::setw(6) << random_num;

	return ss.str();
}

std::string HeartBeatManager::GetAddress() {
	char hostbuffer[256];
	char *IPbuffer;
	struct hostent *host_entry;

	// Get hostname
	if (gethostname(hostbuffer, sizeof(hostbuffer)) == -1) {
		LOG(ERROR) << "Error getting hostname: " << strerror(errno);
		return "127.0.0.1";  // Return localhost as fallback
	}

	// Get host information
	host_entry = gethostbyname(hostbuffer);
	if (host_entry == nullptr) {
		LOG(ERROR) << "Error getting host information: " << strerror(h_errno);
		return "127.0.0.1";  // Return localhost as fallback
	}

	// Convert IP address to string
	IPbuffer = inet_ntoa(*((struct in_addr*)host_entry->h_addr_list[0]));
	if (IPbuffer == nullptr) {
		LOG(ERROR) << "Error converting IP address to string";
		return "127.0.0.1";  // Return localhost as fallback
	}

	return std::string(IPbuffer);
}

} // namespace heartbeat_system
