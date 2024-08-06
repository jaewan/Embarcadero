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

#include <glog/logging.h>
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/btree_set.h"
#include "absl/synchronization/mutex.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/alarm.h>
#include <heartbeat.grpc.pb.h>
#include "common/config.h"
#include <scalog_sequencer.grpc.pb.h>
#include "../cxl_manager/cxl_manager.h"

namespace Embarcadero{

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
using heartbeat_system::CreateTopicRequest;
using heartbeat_system::CreateTopicResponse;

struct LocalNodeInfo {
	int broker_id;
	std::string address;
	std::string network_mgr_addr;
	std::chrono::steady_clock::time_point last_heartbeat;
};

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
			LOG(INFO) << "[HeartBeatServiceImpl] Destructing";
			shutdown_ = true;
			if (heartbeat_thread_.joinable()) {
				heartbeat_thread_.join();
			}
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
			}
			return Status::OK;
		}

		Status Heartbeat(ServerContext* context, const HeartbeatRequest* request,
				HeartbeatResponse* reply) override {
			absl::MutexLock lock(&mutex_);
			auto it = nodes_.find(request->node_id());
			if (it != nodes_.end()) {
				it->second.last_heartbeat = std::chrono::steady_clock::now();
				reply->set_alive(true);
			} else {
				reply->set_alive(false);
			}
			return Status::OK;
		}

		Status GetClusterStatus(ServerContext* context, const ClientInfo* request,
														ClusterStatus* reply) override {
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

		Status CreateNewTopic(ServerContext* context, const CreateTopicRequest* request,
				CreateTopicResponse* reply){
				char topic[TOPIC_NAME_SIZE] = {0};
				memcpy(topic, request->topic().data(), request->topic().size());
				reply->set_success(
					create_topic_entry_callback_(topic, (int)request->order(), (int)request->seq_type()));
				return Status::OK;
		}

		void RegisterCreateTopicEntryCallback(Embarcadero::CreateTopicEntryCallback callback){
			create_topic_entry_callback_ = callback;
		}

		int GetNumBrokers() {
			absl::MutexLock lock(&mutex_);
			return nodes_.size();
		}

		void GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
														struct Embarcadero::MessageHeader** msg_to_order, struct Embarcadero::TInode *tinode){
			// absl::flat_hash_set<int> copy_set(registered_brokers.begin(), registered_brokers.end());
			// {
			// 	absl::MutexLock lock(&mutex_);
			// 	for(const auto &node:nodes_){
			// 		int broker_id = node.second.broker_id;
			// 		if(registered_brokers.find(broker_id) == registered_brokers.end()){
			// 			registered_brokers.insert(broker_id);
			// 			while(tinode->offsets[broker_id].log_offset == 0){
			// 				_mm_pause(); //yield can cause deadlock in grpc. Instead of yield, make it spin
			// 			}
			// 			msg_to_order[broker_id] = ((struct Embarcadero::MessageHeader*)((uint8_t*)msg_to_order[broker_id] + tinode->offsets[broker_id].log_offset));
			// 		}else{
			// 			copy_set.erase(broker_id);
			// 		}
			// 	}
			// }
			// for(auto broker_id : copy_set){
			// 	registered_brokers.erase(broker_id);
			// 	msg_to_order[broker_id] = nullptr;
			// }
		}

		absl::flat_hash_map<std::string, LocalNodeInfo> GetPeerBrokers() {
			absl::MutexLock lock(&mutex_);
			absl::flat_hash_map<std::string, LocalNodeInfo> peer_nodes = nodes_;
			peer_nodes.erase("0");
			return peer_nodes;
		}

	private:
		void CheckHeartbeats();

		absl::Mutex mutex_;
		absl::flat_hash_map<std::string, LocalNodeInfo> nodes_;
		std::thread heartbeat_thread_;
		bool shutdown_ = false;
		Embarcadero::CreateTopicEntryCallback create_topic_entry_callback_;
};

class FollowerNodeClient {
	public:
		FollowerNodeClient(const std::string& node_id, const std::string& address,
				const std::shared_ptr<grpc::Channel>& heartbeat_channel, const std::shared_ptr<grpc::Channel>& scalog_channel);

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

		std::unique_ptr<ScalogSequencer::Stub> GetScalogStub() {
			return std::move(scalog_stub_);
		}

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
		void CheckHeartBeatReply();
		void HeartBeatLoop();

		std::unique_ptr<HeartBeat::Stub> stub_;
		std::unique_ptr<ScalogSequencer::Stub> scalog_stub_;
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
				}else{
					follower_ = std::make_unique<FollowerNodeClient>(GenerateUniqueId(), GetAddress(), 
							grpc::CreateChannel(head_address, grpc::InsecureChannelCredentials()),
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
		void GetRegisteredBrokers(absl::btree_set<int> &registered_brokers, 
														struct Embarcadero::MessageHeader** msg_to_order, struct Embarcadero::TInode *tinode){
			if(is_head_node_){
				service_->GetRegisteredBrokers(registered_brokers, msg_to_order, tinode);
			}else{
				LOG(ERROR) << "GetRegisteredBrokers should not be called from non-head brokers, this it for sequencer";
			}
		}

		void RegisterCreateTopicEntryCallback(Embarcadero::CreateTopicEntryCallback callback){
			if(is_head_node_){
				service_->RegisterCreateTopicEntryCallback(callback);
			}
			return;
		}

		std::string GetBrokerUrl(){
			if (is_head_node_) {
				return GetAddress()+":"+std::to_string(0+PORT);
			}
			return GetAddress()+":"+std::to_string(follower_->GetBrokerId()+PORT);
		}

		std::unique_ptr<ScalogSequencer::Stub> GetScalogStub(){
			if (is_head_node_) {
				return nullptr;
			}
			return follower_->GetScalogStub();
		}

		int GetNumBrokers() {
			return service_->GetNumBrokers();
		}

		absl::flat_hash_map<std::string, LocalNodeInfo> GetPeerBrokers() {
			return service_->GetPeerBrokers();
		}

	private:
		bool is_head_node_;
		std::unique_ptr<Server> server_;
		std::unique_ptr<HeartBeatServiceImpl> service_;
		std::unique_ptr<FollowerNodeClient> follower_;

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

} // End of namespace Embarcadero
#endif
