#ifndef INCLUDE_PEER_H_
#define INCLUDE_PEER_H_

#include <string>
#include <absl/container/flat_hash_map.h>
#include <grpcpp/grpcpp.h>
#include <protobuf/peer.grpc.pb.h>

/// Class for a single broker
class PeerBroker : public Peer::Service {
    public:
        /// Constructor for PeerBroker
        /// address and port are only NULL if the broker is the leader
        /// @param address IP address of the head broker
        /// @param port Port of the head broker
        /// @param is_head bool to indicate if broker is the leader
        PeerBroker( bool is_head, std::string address = NULL, std::string port = NULL);

        /// Fetch the ip address of the host broker
        /// @return ip address as a string
        std::string GetAddress();

        /// Create a new rpc client to communicate with a peer broker
        /// @param peer_url URL of the peer broker
        /// @return rpc client
        std::unique_ptr<Peer::Stub> GetRpcClient(std::string peer_url);

        /// Send a heartbeat to all peer brokers
        void BroadcastHeartbeat();

        /// Handle heartbeat request from a peer broker
        /// @param context 
        /// @param request  Includes the address and port of the sender
        /// @param response indicate that the broker is still alive
        grpc::Status HandleBroadcastHeartbeat(grpc::ServerContext* context, const BroadcastHeartbeatRequest* request, BroadcastHeartbeatResponse* response) override;

        /// Run the broker
        void Run();
    private:
        /// IP address of the broker
        std::string address;

        /// Port of the broker
        std::string port;

        /// Bool to indicate if broker is the leader
        bool is_head;

        /// Have hashmap of other peer brokers
        absl::flat_hash_map<std::string, std::string> peer_brokers;
};

#endif