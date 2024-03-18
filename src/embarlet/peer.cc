#include <iostream>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "config.h"
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
        this->failure_threshold_ = health_check_failure_threshold;
    } else {
        this->is_head_ = false;
        this->address_ = GetAddress();
        this->port_ = port;
        this->peer_brokers_.emplace(address, port);
        this->current_head_address_ = address;
        this->failure_threshold_ = health_check_failure_threshold;
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

/// TODO: cover the case where new node that joins is aware of all other peers
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
    // This RPC call is blocking for now
    grpc::Status status = rpc_client->HandleJoinCluster(&context, request, &response);

    if (!status.ok()) {
        std::cout << "Error sending join request to head " << head_address << ":" << head_port << std::endl;
    } else {
        std::cout << "Successfully joined the cluster" << std::endl;
    }

    // Start health check on head node
    InitiateHealthChecker(head_address, head_port);
}

grpc::Status PeerBroker::HandleJoinCluster(grpc::ServerContext* context, const JoinClusterRequest* request, JoinClusterResponse* response) {
    std::string new_peer_address = request->address();
    std::string new_peer_port = request->port();

    // Notify all other brokers of the new peer
    for (auto const& peer : peer_brokers_) {
        std::string peer_url = peer.first + ":" + peer.second;
        auto rpc_client = GetRpcClient(peer_url);

        ReportNewBrokerRequest request;
        request.set_address(new_peer_address);
        request.set_port(new_peer_port);

        // This RPC call is synchronous for now
        ReportNewBrokerResponse response;
        grpc::ClientContext context;
        grpc::Status status = rpc_client->HandleReportNewBroker(&context, request, &response);

        if (!status.ok()) {
            std::cout << "Error sending new broker notification to " << peer_url << std::endl;
            return grpc::Status::CANCELLED;
        }

        /// TODO: Make sure this async rpc call is non blocking
        /// NOTE: These are different ways we could use async rpc calls
        // grpc::ClientContext context;
        // grpc::CompletionQueue cq;
        // grpc::Status status;
        // ReportNewBrokerResponse response;
        // // Create a unique_ptr to hold the response
        // std::unique_ptr<grpc::ClientAsyncResponseReader<ReportNewBrokerResponse>> rpc(
        //     rpc_client->AsyncHandleReportNewBroker(&context, request, &cq)
        // );

        // rpc->Finish(&response, &status, (void*)1);
        // void* got_tag;
        // bool ok = false;

        // cq.Next(&got_tag, &ok);

        // if (ok && got_tag == (void*)1) {
        //     if (!status.ok()) {
        //         std::cout << "Error sending new broker notification to " << peer_url << std::endl;
        //         return grpc::Status::CANCELLED;
        //     }
        // } else {
        //     std::cout << "Error sending new broker notification to " << peer_url << std::endl;
        //     return grpc::Status::CANCELLED;
        // }

        /// TODO: Modify this function later to use async rpc calls with callbacks
        /// NOTE: These are different ways we could use async rpc calls
        // rpc_client->AsyncHandleReportNewBroker(context, request, &response, [peer_url](grpc::Status status) {
        //     if (!status.ok()) {
        //         std::cout << "Error sending new broker notification to " << peer_url << std::endl;
        //     }
        // });

        return grpc::Status::OK;
    }

    // If the peer is not in the hashmap, add it
    if (peer_brokers_.find(new_peer_address) == peer_brokers_.end()) {
        peer_brokers_.emplace(new_peer_address, new_peer_port);
    }

    std::cout << "Peer " << new_peer_address << ":" << new_peer_port << " successfully joined the cluster" << std::endl;

    // Start health check
    InitiateHealthChecker(new_peer_address, new_peer_port);

    return grpc::Status::OK;
}

grpc::Status PeerBroker::HandleReportNewBroker(grpc::ServerContext* context, const ReportNewBrokerRequest* request, ReportNewBrokerResponse* response) {
    std::string new_peer_address = request->address();
    std::string new_peer_port = request->port();

    // If the new broker is not in the hashmap, add it
    if (peer_brokers_.find(new_peer_address) == peer_brokers_.end()) {
        peer_brokers_.emplace(new_peer_address, new_peer_port);
    }

    std::cout << "New broker " << new_peer_address << ":" << new_peer_port << " successfully added to peer list of broker: " << address_ << std::endl;

    // Start health check
    InitiateHealthChecker(new_peer_address, new_peer_port);

    return grpc::Status::OK;
}

void PeerBroker::InitiateHealthChecker(std::string address, std::string port) {
    io_service_.dispatch([this, address, port] {
        std::cout << "Initiating health check for " << address << ":" << port << std::endl;
        auto health_checker = new HealthChecker(this, address, port);
    });
}

grpc::Status PeerBroker::HandleHealthCheck(grpc::ServerContext* context, const HealthCheckRequest* request, HealthCheckResponse* response) {
    std::cout << "Received a health check" << std::endl;

    return grpc::Status::OK;
}

void PeerBroker::HealthChecker::StartHealthCheck() {
    grpc::ClientContext health_check_context;
    HealthCheckRequest request;
    HealthCheckResponse response;

    auto deadline =
        std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_);
    health_check_context.set_deadline(deadline);

    /// TODO: Figure out how to make this async
    grpc::Status status = rpc_client_->HandleHealthCheck(&health_check_context, request, &response);

    // Check if the deadline expired
    if (status.error_code() == grpc::DEADLINE_EXCEEDED) {
        --health_check_remaining_;
    } else if (status.ok()) {
        // Health check passed
        std::cout << "Health check passed for " << health_check_address_ << ":" << health_check_port_ << std::endl;

        health_check_remaining_ = peer_->failure_threshold_;
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
}

void PeerBroker::FailNode(std::string address, std::string port) {
    std::cout << "Node " << address << ":" << port << " has failed" << std::endl;
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
        std::cout << "Server listening on " << server_address << std::endl;
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
        
        std::cout << "Server listening on " << server_address << std::endl;
        server->Wait();
    }
}