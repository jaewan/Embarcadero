#ifndef INCLUDE_PEER_H_
#define INCLUDE_PEER_H_

#include <string>
#include <absl/container/flat_hash_map.h>
#include <grpcpp/grpcpp.h>
#include <peer.grpc.pb.h>
#include <thread>
#include <boost/asio.hpp>
#include "common/config.h"

namespace Embarcadero{

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

        /// Fetch the URL of the broker
        /// @return url as a string
        std::string GetUrl() {
            return address_ + ":" + port_;
        }

        /// Fetch the peer brokers of the broker
        /// @return peer brokers as a hashmap
        absl::flat_hash_map<std::string, std::string> GetPeerBrokers() {
            return peer_brokers_;
        }

        /// Create a new rpc client to communicate with a peer broker
        /// @param peer_url URL of the peer broker
        /// @return rpc client
        std::unique_ptr<Peer::Stub> GetRpcClient(std::string peer_url);

        /// Follower sends a request to the head to join cluster
        void JoinCluster();

        /// Initiate a health checker for a new peer that was added
        /// @param address Address of the new peer
        /// @param port Port used for new peer
        void InitiateHealthChecker(std::string address, std::string port);

        /// Fail the broker if health checking fails. 
        /// NOTE: For now shut down the system
        /// @param address IP address of the broker to fail
        /// @param port Port of the broker to fail
        void FailNode(std::string address, std::string port);

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

        /// A peer receives a health check request from another peer. If it is alive, it simply returns status ok. If not, the sender will detect it.
        /// @param context
        /// @param request Empty for now
        /// @param response Empty for now
        grpc::Status HandleHealthCheck(grpc::ServerContext* context, const HealthCheckRequest* request, HealthCheckResponse* response) override;

        /// Run the broker
        void Run();
    private:
        using Timer = boost::asio::deadline_timer;
        class HealthChecker {
            public:
                /// Constructor for HealthChecker that initiates the first health check
                /// @param address IP address of the broker we are health checking
                /// @param port Port of the broker we are health checking
                HealthChecker(PeerBroker *peer, std::string address, std::string port) 
                    : peer_(peer),
                      timer_(peer->io_service_),
                      health_check_remaining_(peer->failure_threshold_) {
                    health_check_address_ = address;
                    health_check_port_ = port;
                    initial_delay_ms_ = HEALTH_CHECK_INITIAL_DELAY_MS;
                    timeout_ms_ = HEALTH_CHECK_TIMEOUT_MS;
                    period_ms_ = HEALTH_CHECK_PERIOD_MS;

                    rpc_client_ = peer_->GetRpcClient(health_check_address_ + ":" + health_check_port_);

                    timer_.expires_from_now(
                        boost::posix_time::milliseconds(initial_delay_ms_));
                    timer_.async_wait([this](auto) { StartHealthCheck(); });
                }

                /// Starts the health check loop
                void StartHealthCheck();
            private:
                /// PeerBroker object
                PeerBroker *peer_;

                /// RPC client to communicate with the broker
                std::unique_ptr<Peer::Stub> rpc_client_;

                /// Timer used to perform async wait before the next healthcheck
                Timer timer_;

                /// IP address of the broker we are health checking
                std::string health_check_address_;

                /// Port of the broker we are health checking
                std::string health_check_port_;

                /// The delay for the first health check request.
                int64_t initial_delay_ms_;

                /// Timeout for each health check request.
                int64_t timeout_ms_;

                /// Intervals between two health check.
                int64_t period_ms_;

                /// Number of health checks remaining for this specific broker before it is considered dead
                int64_t health_check_remaining_;
        };

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

        /// Thread to run the io_service in a loop
        std::unique_ptr<std::thread> io_service_thread_;

        /// IO context that peers use to post tasks
        boost::asio::io_context io_service_;

        /// The number of failures before the node is considered as dead. 
        int64_t failure_threshold_;
};

} // End of namespace Embarcadero
#endif
