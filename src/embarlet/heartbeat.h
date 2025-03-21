#ifndef INCLUDE_HEATBEAT_H
#define INCLUDE_HEATBEAT_H

#include <string>
#include <thread>
#include <random>
#include <iomanip>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>

#include <glog/logging.h>
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/btree_set.h"
#include "absl/synchronization/mutex.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/alarm.h>
#include <heartbeat.grpc.pb.h>
#include "common/config.h"
#include "../cxl_manager/cxl_manager.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using heartbeat_system::HeartBeat;
using heartbeat_system::NodeInfo;
using heartbeat_system::ClientInfo;
using heartbeat_system::ClusterStatus;
using heartbeat_system::RegistrationStatus;
using heartbeat_system::HeartbeatRequest;
using heartbeat_system::HeartbeatResponse;
using heartbeat_system::KillBrokersRequest;
using heartbeat_system::KillBrokersResponse;
using heartbeat_system::CreateTopicRequest;
using heartbeat_system::CreateTopicResponse;

class HeartBeatServiceImpl final : public HeartBeat::Service {
	public:
		HeartBeatServiceImpl(std::string head_addr) {
			// Inserting head node to the nodes_
			nodes_["0"] = {0, head_addr, head_addr+":"+std::to_string(PORT + 0), std::chrono::steady_clock::now()};
			// Start a thread to check follower node heartbeats
			heartbeat_thread_ = std::thread([this]() {
					this->CheckHeartbeats();
					});
		}

		~HeartBeatServiceImpl() {
			shutdown_ = true;
			if (heartbeat_thread_.joinable()) {
				heartbeat_thread_.join();
			}
			LOG(INFO) << "[HeartBeatServiceImpl] Destructed";
		}

		Status RegisterNode(ServerContext* context, const NodeInfo* request,
				RegistrationStatus* reply) override {
			absl::MutexLock lock(&mutex_);
			auto nodes_it = nodes_.find(request->node_id());
			int broker_id = (int)nodes_.size();
			if ( nodes_it != nodes_.end() || broker_id >= NUM_MAX_BROKERS) {
				reply->set_success(false);
				reply->set_broker_id(broker_id);
				if (broker_id < NUM_MAX_BROKERS)
					reply->set_message("Node already registered");
				else
					reply->set_message("Trying to Register too many brokers. Increase NUM_MAX_BROKERS");
			} else {
				VLOG(3) << "Registering node:" << request->address() << " broker:" << broker_id;
				nodes_[request->node_id()] = {broker_id, request->address(), request->address()+":"+std::to_string(broker_id+PORT), std::chrono::steady_clock::now()};
				reply->set_success(true);
				reply->set_broker_id(broker_id);
				reply->set_message("Node registered successfully");
				{
					ClusterStatus cluster_status;
					cluster_status.add_new_nodes(request->address()+":"+std::to_string(broker_id+PORT));
					absl::MutexLock lock(&subscriber_mutex_);
					for (auto& writer : subscribers_) {
            if (writer) {
							writer->Write(cluster_status);  // Push update to each connected client
            }
					}
				}
			}
			return Status::OK;
		}

		Status Heartbeat(ServerContext* context, const HeartbeatRequest* request,
				HeartbeatResponse* reply) override {
			absl::MutexLock lock(&mutex_);
			auto it = nodes_.find(request->node_id());
			reply->set_shutdown(false);
			if (it != nodes_.end()) {
				it->second.last_heartbeat = std::chrono::steady_clock::now();
				if(shutdown_){
					reply->set_shutdown(true);
					nodes_.erase(it);
				}
				reply->set_alive(true);
			} else {
				reply->set_alive(false);
			}
			return Status::OK;
		}

		// gRPC Stream to update client of cluster changes. This  is for dynamic broker addition benchmark
		// It will alert the client faster than client polling
		// This does not alert removed nodes
		Status SubscribeToCluster(ServerContext* context, const ClientInfo* request,
				grpc::ServerWriter<ClusterStatus>* writer) override {
			ClusterStatus initial_status;
			absl::flat_hash_set<int32_t> s(request->nodes_info().begin(), request->nodes_info().end());
			{
				absl::MutexLock lock(&mutex_);
				for (const auto &it : nodes_){
					if(s.contains(it.second.broker_id)){
						s.erase(it.second.broker_id);
					}else{
						initial_status.add_new_nodes(it.second.network_mgr_addr);
					}
				}
			}
			writer->Write(initial_status);
			{
				absl::MutexLock lock(&subscriber_mutex_);
				subscribers_.push_back(std::make_shared<grpc::ServerWriter<ClusterStatus>>(*writer));
			}

			// Keep the stream open and active for the client (blocking call)
			while (true) {
				// Simulate keeping the stream alive, handling disconnection, etc.
				if (context->IsCancelled()) {
					break;
				}
				std::this_thread::yield();
			}

			// Cleanup the subscriber on disconnection
			{
				absl::MutexLock lock(&subscriber_mutex_);
				subscribers_.erase(std::remove_if(subscribers_.begin(), subscribers_.end(),
							[writer](const std::shared_ptr<grpc::ServerWriter<ClusterStatus>>& w) {
							return w.get() == writer;
							}),
						subscribers_.end());
			}

			return Status::OK;
		}

		Status GetClusterStatus(ServerContext* context, const ClientInfo* request,
				ClusterStatus* reply) override;

		Status TerminateCluster(ServerContext* context, const google::protobuf::Empty* request,
				google::protobuf::Empty* response) override;

		Status KillBrokers(ServerContext* context, const KillBrokersRequest* request,
				KillBrokersResponse* reply) override;

		Status CreateNewTopic(ServerContext* context, const CreateTopicRequest* request,
				CreateTopicResponse* reply) override;

		void SetServer(std::shared_ptr<grpc::Server> server) {
			server_ = server;
		}

		void RegisterCreateTopicEntryCallback(Embarcadero::CreateTopicEntryCallback callback){
			create_topic_entry_callback_ = callback;
		}

		int GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
				struct Embarcadero::MessageHeader** msg_to_order, struct Embarcadero::TInode *tinode);

	private:
		struct NodeInfo {
			int broker_id;
			std::string address;
			std::string network_mgr_addr;
			std::chrono::steady_clock::time_point last_heartbeat;
		};

		void CheckHeartbeats();

		absl::Mutex mutex_;
		absl::flat_hash_map<std::string, NodeInfo> nodes_;
		// This is for stream communication with clients. Remove this if stream is not used
		std::vector<std::shared_ptr<grpc::ServerWriter<ClusterStatus>>> subscribers_;
		absl::Mutex subscriber_mutex_;
		std::thread heartbeat_thread_;
		bool shutdown_ = false;
		std::shared_ptr<grpc::Server> server_;
		Embarcadero::CreateTopicEntryCallback create_topic_entry_callback_;
};

class FollowerNodeClient {
	public:
		FollowerNodeClient(const std::string& node_id, const std::string& address,
				const std::shared_ptr<grpc::Channel>& channel);

		~FollowerNodeClient() {
			shutdown_ = true;
			cq_.Shutdown();
			if (!wait_called_ && heartbeat_thread_.joinable()) {
				heartbeat_thread_.join();
			}
		}

		void Wait() {
			wait_called_ = true;
			heartbeat_thread_.join();
			return;
		}

		bool IsHeadAlive() const { return head_alive_; }
		void SetHeadAlive(bool alive) { head_alive_ = alive; }
		int GetBrokerId() { return broker_id_; }

		std::string GetNodeId() const { return node_id_; }
		std::string GetAddress() const { return address_; }

	private:
		struct AsyncClientCall {
			HeartbeatResponse reply;
			grpc::ClientContext context;
			Status status;
			std::unique_ptr<grpc::ClientAsyncResponseReader<HeartbeatResponse>> response_reader;
			grpc::Alarm alarm;
			~AsyncClientCall() {
				context.TryCancel();
				alarm.Cancel();
			}
		};

		void Register();
		void SendHeartbeat();
		bool CheckHeartBeatReply();
		void HeartBeatLoop();

		std::unique_ptr<HeartBeat::Stub> stub_;
		std::string node_id_;
		std::string address_;
		grpc::CompletionQueue cq_;
		bool head_alive_;
		bool wait_called_;
		int broker_id_;
		std::thread heartbeat_thread_;
		bool shutdown_ = false;
};

class HeartBeatManager{
	public:
		// param head_address should be the ipadress:port
		HeartBeatManager(bool is_head_node, std::string head_address)
			:is_head_node_(is_head_node){
				if(is_head_node){
					service_ = std::make_unique<HeartBeatServiceImpl>(GetAddress());
					ServerBuilder builder;
					builder.AddListeningPort(head_address, grpc::InsecureServerCredentials());
					builder.RegisterService(service_.get());
					server_ = builder.BuildAndStart();
					service_->SetServer(server_);
				}else{
					follower_ = std::make_unique<FollowerNodeClient>(GetPID(), GetAddress(), 
							grpc::CreateChannel(head_address, grpc::InsecureChannelCredentials()));
				}
			}

		void Wait(){
			if(is_head_node_){
				server_->Wait();
			}else{
				follower_->Wait();
			}
		}

		int GetBrokerId(){
			if (is_head_node_){
				return 0;
			}
			return follower_->GetBrokerId();
		}
		int GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
				struct Embarcadero::MessageHeader** msg_to_order, struct Embarcadero::TInode *tinode){
			if(is_head_node_){
				return service_->GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			}else{
				LOG(ERROR) << "GetRegisteredBrokers should not be called from non-head brokers, this it for sequencer";
				return 0;
			}
		}

		void RegisterCreateTopicEntryCallback(Embarcadero::CreateTopicEntryCallback callback){
			if(is_head_node_){
				service_->RegisterCreateTopicEntryCallback(callback);
			}
			return;
		}

	private:
		bool is_head_node_;
		std::shared_ptr<Server> server_;
		std::unique_ptr<HeartBeatServiceImpl> service_;
		std::unique_ptr<FollowerNodeClient> follower_;

		std::string GetPID() {
			pid_t pid = getpid();
			std::string pid_str = std::to_string(pid);
			return pid_str;
		}
		// We do not use IP address as node identifier b/c multiple brokers could run on a single node
		std::string GenerateUniqueId() {
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

		std::string GetAddress() {
			char hostbuffer[256];
			char *IPbuffer;
			struct hostent *host_entry;
			int hostname;

			// Get hostname
			hostname = gethostname(hostbuffer, sizeof(hostbuffer));
			if (hostname == -1) {
				perror("Error getting hostname");
				return "";
			}

			// Get host information
			host_entry = gethostbyname(hostbuffer);
			if (host_entry == NULL) {
				perror("Error getting host information");
				return "";
			}

			// Convert IP address to string
			IPbuffer = inet_ntoa(*((struct in_addr *)host_entry->h_addr_list[0]));
			if (IPbuffer == NULL) {
				perror("Error converting IP address to string");
				return "";
			}

			return std::string(IPbuffer);
		}
};
#endif
