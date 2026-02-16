#ifndef INCLUDE_HEARTBEAT_H
#define INCLUDE_HEARTBEAT_H

#include <string>
#include <thread>
#include <random>
#include <iomanip>
#include <functional>
#include <chrono>
#include <memory>
#include <vector>
#include <atomic>

// System includes
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <string.h>

// Third-party libraries
#include <glog/logging.h>
#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/container/btree_set.h"
#include "absl/synchronization/mutex.h"
#include <grpcpp/grpcpp.h>
#include <grpcpp/alarm.h>

// Project includes
#include <heartbeat.grpc.pb.h>
#include "common/config.h"
#include "../cxl_manager/cxl_manager.h"

// Forward declarations
namespace Embarcadero {
    struct MessageHeader;
    struct TInode;
}

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

namespace heartbeat_system {

class HeartBeatServiceImpl final : public HeartBeat::Service {
	public:
		HeartBeatServiceImpl(std::string head_addr);
		~HeartBeatServiceImpl();

		Status RegisterNode(ServerContext* context, const NodeInfo* request,
				RegistrationStatus* reply) override;

		Status Heartbeat(ServerContext* context, const HeartbeatRequest* request,
				HeartbeatResponse* reply) override;

		Status SubscribeToCluster(ServerContext* context, const ClientInfo* request,
				grpc::ServerWriter<ClusterStatus>* writer) override;

		Status GetClusterStatus(ServerContext* context, const ClientInfo* request,
				ClusterStatus* reply) override;

		Status TerminateCluster(ServerContext* context, const google::protobuf::Empty* request,
				google::protobuf::Empty* response) override;

		Status KillBrokers(ServerContext* context, const KillBrokersRequest* request,
				KillBrokersResponse* reply) override;

		Status CreateNewTopic(ServerContext* context, const CreateTopicRequest* request,
				CreateTopicResponse* reply) override;

		void SetServer(std::shared_ptr<Server> server);
		void RegisterCreateTopicEntryCallback(Embarcadero::CreateTopicEntryCallback callback);
		int GetRegisteredBrokers(absl::btree_set<int> &registered_brokers,
				struct Embarcadero::MessageHeader** msg_to_order,
				struct Embarcadero::TInode *tinode);
		std::string GetNextBrokerAddr(int broker_id);
		int GetNumBrokers();

	private:
		// Renamed to avoid conflict with proto's NodeInfo
		struct NodeEntry {
			int broker_id;
			std::string address;
			std::string network_mgr_addr;
			std::chrono::steady_clock::time_point last_heartbeat;
			bool accepts_publishes;  // Always true (all brokers accept publishes)
		};

		void CheckHeartbeats();
		// Helper method to build cluster info response
		void FillClusterInfo(HeartbeatResponse* reply, bool force_full_update);


		absl::Mutex mutex_;
		absl::flat_hash_map<std::string, NodeEntry> nodes_;
		std::vector<std::shared_ptr<grpc::ServerWriter<ClusterStatus>>> subscribers_;
		absl::Mutex subscriber_mutex_;
		std::thread heartbeat_thread_;
		std::atomic<bool> shutdown_{false};
		std::shared_ptr<Server> server_;
		absl::Mutex cluster_mutex_;
		uint64_t cluster_version_{0} ABSL_GUARDED_BY(cluster_mutex_);  // Incremented when cluster changes
		Embarcadero::CreateTopicEntryCallback create_topic_entry_callback_;
};

class FollowerNodeClient {
	public:
		FollowerNodeClient(const std::string& node_id, const std::string& address,
				const std::shared_ptr<grpc::Channel>& channel);

		~FollowerNodeClient();
		void Wait();
		void RequestShutdown();
		int GetNumBrokers();
		bool IsHeadAlive() const { return head_alive_; }
		void SetHeadAlive(bool alive) { head_alive_ = alive; }
		int GetBrokerId() { return broker_id_; }
		std::string GetNodeId() const { return node_id_; }
		std::string GetAddress() const { return address_; }
		std::string GetNextBrokerAddr(int broker_id);

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
		// Helper method to process cluster info
		void ProcessClusterInfo(const HeartbeatResponse& reply);

		// Define the NodeEntry struct (same as in HeartBeatServiceImpl)
		struct NodeEntry {
			int broker_id;
			std::string address;
			std::string network_mgr_addr;
			bool accepts_publishes;
		};

		std::unique_ptr<HeartBeat::Stub> stub_;
		std::string node_id_;
		std::string address_;
		grpc::CompletionQueue cq_;
		std::atomic<bool> head_alive_{true};
		std::atomic<bool> wait_called_{false};
		int broker_id_{-1};
		std::thread heartbeat_thread_;
		std::atomic<bool> shutdown_{false};
		uint64_t cluster_version_{0};
		absl::Mutex cluster_mutex_;
		absl::flat_hash_map<int, NodeEntry> cluster_nodes_;
};

class HeartBeatManager {
	public:
		HeartBeatManager(bool is_head_node, std::string head_address);
		void Wait();
		void RequestShutdown();
		int GetBrokerId();
		int GetRegisteredBrokers(absl::btree_set<int> &registered_brokers,
				struct Embarcadero::MessageHeader** msg_to_order,
				struct Embarcadero::TInode *tinode);
		std::string GetNextBrokerAddr(int broker_id);
		int GetNumBrokers();
		void RegisterCreateTopicEntryCallback(Embarcadero::CreateTopicEntryCallback callback);

	private:
		bool is_head_node_;
		std::shared_ptr<Server> server_;
		std::unique_ptr<HeartBeatServiceImpl> service_;
		std::unique_ptr<FollowerNodeClient> follower_;

		std::string GetPID();
		std::string GenerateUniqueId();
		std::string GetAddress();
};

} // namespace heartbeat_system
#endif
