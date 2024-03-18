#include <iostream>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "peer.h"

PeerBroker::PeerBroker(bool is_head, std::string address, std::string port) 
    : io_service_thread_(std::make_unique<std::thread>([this] {
          // Keep io_service_ alive.
          boost::asio::io_service::work io_service_work_(io_service_);
          io_service_.run();
    })) {
    if (is_head) {
        this->is_head_ = true;
        this->address_ = GetAddress();
        this->port_ = "8080"; // Default port for now
        this->peer_brokers_ = absl::flat_hash_map<std::string, std::string>();
        this->current_head_address_ = this->address_;
        this->failure_threshold_ = HEALTH_CHECK_FAILURE_THRESHOLD;
    } else {
        this->is_head_ = false;
        this->address_ = GetAddress();
        this->port_ = port;
        this->peer_brokers_.emplace(address, port);
        this->current_head_address_ = address;
        this->failure_threshold_ = HEALTH_CHECK_FAILURE_THRESHOLD;
    }
}

std::string PeerBroker::GetAddress() {
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

std::unique_ptr<Peer::Stub> PeerBroker::GetRpcClient(std::string peer_url) {
    std::shared_ptr<grpc::Channel> channel = grpc::CreateChannel(peer_url, grpc::InsecureChannelCredentials());
    return Peer::NewStub(channel);
}

void PeerBroker::JoinCluster() {
    std::string head_address = current_head_address_;
    std::string head_port = port_;

    std::string head_url = head_address + ":" + head_port;
    auto rpc_client = GetRpcClient(head_url);

    JoinClusterRequest request;
    request.set_address(address_);
    request.set_port(port_);

    JoinClusterResponse response;
    grpc::ClientContext context;
    // This RPC call blocks until the new peer successfully joins the cluster
    grpc::Status status = rpc_client->HandleJoinCluster(&context, request, &response);

    if (!status.ok()) {
        std::cout << "Error sending join request to head " << head_address << ":" << head_port << std::endl;
        return;
    } else {
        std::cout << "Successfully joined the cluster" << std::endl;
    }

    // Start health check on head node
    InitiateHealthChecker(head_address, head_port);
}

grpc::Status PeerBroker::HandleJoinCluster(grpc::ServerContext* context, const JoinClusterRequest* request, JoinClusterResponse* response) {
    std::string new_peer_address = request->address();
    std::string new_peer_port = request->port();

    // If the peer is not in the hashmap, add it
    if (peer_brokers_.find(new_peer_address) == peer_brokers_.end()) {
        peer_brokers_.emplace(new_peer_address, new_peer_port);
        std::cout << "Peer " << new_peer_address << ":" << new_peer_port << " successfully joined the cluster" << std::endl;

        // Start health check
        InitiateHealthChecker(new_peer_address, new_peer_port);
    } else {
        std::cout << "Peer " << new_peer_address << ":" << new_peer_port << " already exists in the cluster" << std::endl;
        return grpc::Status::CANCELLED;
    }

    // Notify all other brokers of the new peer
    for (auto const& peer : peer_brokers_) {
        // Check if the peer is not the peer we just added
        if (peer.first != new_peer_address) {
            std::string peer_url = peer.first + ":" + peer.second;
            auto rpc_client = GetRpcClient(peer_url);

            ReportNewBrokerRequest request;
            request.set_address(new_peer_address);
            request.set_port(new_peer_port);

            ReportNewBrokerResponse response;
            grpc::ClientContext context;

            auto callback = [](grpc::Status status) {
                if (!status.ok()) {
                    std::cout << "Error sending new broker notification" << std::endl;
                }
            };

            // Async call to HandleReportNewBroker
            rpc_client->async()->HandleReportNewBroker(&context, &request, &response, callback);
        }
    }

    return grpc::Status::OK;
}

grpc::Status PeerBroker::HandleReportNewBroker(grpc::ServerContext* context, const ReportNewBrokerRequest* request, ReportNewBrokerResponse* response) {
    std::string new_peer_address = request->address();
    std::string new_peer_port = request->port();

    // If the new broker is not in the hashmap, add it
    if (peer_brokers_.find(new_peer_address) == peer_brokers_.end()) {
        peer_brokers_.emplace(new_peer_address, new_peer_port);
        
        std::cout << "New broker " << new_peer_address << ":" << new_peer_port << " successfully added to peer list of broker: " << address_ << std::endl;

        // Start health check
        InitiateHealthChecker(new_peer_address, new_peer_port);
    } else {
        std::cout << "New broker " << new_peer_address << ":" << new_peer_port << " already exists in peer list of broker: " << address_ << std::endl;
        return grpc::Status::CANCELLED;
    }

    return grpc::Status::OK;
}

void PeerBroker::InitiateHealthChecker(std::string address, std::string port) {
    io_service_.dispatch([this, address, port] {
        std::cout << "Initiating health check for " << address << ":" << port << std::endl;
        auto health_checker = new HealthChecker(this, address, port);
    });
}

grpc::Status PeerBroker::HandleHealthCheck(grpc::ServerContext* context, const HealthCheckRequest* request, HealthCheckResponse* response) {
    std::string sender_address = request->address();
    std::string sender_port = request->port();

    std::cout << "Received a health check from " << sender_address << ":" << sender_port << std::endl;

    // If the peer is not in the hashmap, add it and start a health check
    // Initially, this peer might not know of all other peers until it receives a health check from them
    // We are able to piggyback information about other peers through these health checks
    if (peer_brokers_.find(sender_address) == peer_brokers_.end()) {
        peer_brokers_.emplace(sender_address, sender_port);
        InitiateHealthChecker(sender_address, sender_port);
    }

    return grpc::Status::OK;
}

void PeerBroker::HealthChecker::StartHealthCheck() {
    grpc::ClientContext health_check_context;
    HealthCheckRequest request;
    HealthCheckResponse response;

    auto deadline =
        std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_);
    health_check_context.set_deadline(deadline);

    request.set_address(peer_->address_);
    request.set_port(peer_->port_);

    // Async call to HandleHealthCheck
    auto callback = [this](grpc::Status status) {
        if (status.ok()) {
            // Health check passed
            std::cout << "Health check passed for " << health_check_address_ << ":" << health_check_port_ << std::endl;

            health_check_remaining_ = peer_->failure_threshold_;
        } else {
            --health_check_remaining_;
            std::cout << "Health check failed for " << health_check_address_ << ":" << health_check_port_ << "," << health_check_remaining_ << " retries remaining" << std::endl;
        }

        // If number of health checks reaches 0 we consider the node as failed
        if (health_check_remaining_ == 0) {
            peer_->FailNode(health_check_address_, health_check_port_);
            delete this;
        } else {
            // Do another health check.
            std::cout << "Scheduling next health check for " << health_check_address_ << ":" << health_check_port_ << std::endl;

            timer_.expires_from_now(
                boost::posix_time::milliseconds(period_ms_));
            timer_.async_wait([this](auto) { StartHealthCheck(); });
        }
    };

    rpc_client_->async()->HandleHealthCheck(&health_check_context, &request, &response, callback);
}

void PeerBroker::FailNode(std::string address, std::string port) {
    std::cout << "Node " << address << ":" << port << " has failed, shutting down the system" << std::endl;
    // For now, shut down the system
    exit(1);
}

void PeerBroker::Run() {
    if (is_head_) {
        // Start the server
        std::string server_address = address_ + ":" + port_;

        // make insecure for now
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(this);

        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        std::cout << "Embarcadero head started. To connect to this node from another node, run: ./embarlet --follower='" << server_address << "'" << std::endl;
        server->Wait();
    } else {
        std::string server_address = address_ + ":" + port_;

        // make insecure for now
        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(this);

        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());

        // Join the cluster
        JoinCluster();
        
        std::cout << "Embarcadero follower started on " << server_address << std::endl;
        server->Wait();
    }
}