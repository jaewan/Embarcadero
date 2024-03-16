#ifndef INCLUDE_PEER_H_
#define INCLUDE_PEER_H_

#include <string>
#include <absl/container/flat_hash_map.h>
#include <grpcpp/grpcpp.h>
#include <peer.grpc.pb.h>

/// Class for a single broker
class PeerBroker : public Peer::Service {
    public:
        /// Constructor for PeerBroker
        /// address and port are only NULL if the broker is the leader
        /// @param address IP address of the head broker
        /// @param port Port of the head broker
        /// @param is_head bool to indicate if broker is the leader
        PeerBroker(bool is_head, std::string address = "", std::string port = "");

        /// Fetch the ip address of the host broker
        /// @return ip address as a string
        std::string GetAddress();

        /// Create a new rpc client to communicate with a peer broker
        /// @param peer_url URL of the peer broker
        /// @return rpc client
        std::unique_ptr<Peer::Stub> GetRpcClient(std::string peer_url);

        /// Follower sends a request to the head to join cluster
        void JoinCluster();

        /// Head handles join request from follower
        /// @param context 
        /// @param request Includes the address and port of the follower
        /// @param response Empty for now
        grpc::Status HandleJoinCluster(grpc::ServerContext* context, const JoinClusterRequest* request, JoinClusterResponse* response) override;

        /// Head sends a notification to all other brokers to update their peer list
        /// @param context 
        /// @param request Includes the address and port of the new broker
        /// @param response Empty for now
        grpc::Status HandleReportNewBroker(grpc::ServerContext* context, const ReportNewBrokerRequest* request, ReportNewBrokerResponse* response) override;

        /// Run the broker
        void Run();
    private:
        /// IP address of the broker
        std::string address_;

        /// Port of the broker
        std::string port_;

        /// Bool to indicate if broker is the leader
        bool is_head_;

        /// IP address of the current head broker
        std::string current_head_address_;

        /// Have hashmap of other peer brokers
        absl::flat_hash_map<std::string, std::string> peer_brokers_;
};

#endif