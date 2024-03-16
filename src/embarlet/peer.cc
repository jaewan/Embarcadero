#include <iostream>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "config.h"
#include "peer.h"

PeerBroker::PeerBroker(bool is_head, std::string address, std::string port) {
    if (is_head) {
        this->is_head_ = true;
        this->address_ = GetAddress();
        this->port_ = "8080"; // Default port for now
        this->peer_brokers_ = absl::flat_hash_map<std::string, std::string>();
        this->current_head_address_ = this->address_;
    } else {
        this->is_head_ = false;
        this->address_ = GetAddress();
        this->port_ = port;
        this->peer_brokers_.emplace(address, port);
        this->current_head_address_ = address;
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
    }
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

    // Start health check with new thread

    return grpc::Status::OK;
}

grpc::Status PeerBroker::HandleReportNewBroker(grpc::ServerContext* context, const ReportNewBrokerRequest* request, ReportNewBrokerResponse* response) {
    std::string new_broker_address = request->address();
    std::string new_broker_port = request->port();

    // If the new broker is not in the hashmap, add it
    if (peer_brokers_.find(new_broker_address) == peer_brokers_.end()) {
        peer_brokers_.emplace(new_broker_address, new_broker_port);
    }

    std::cout << "New broker " << new_broker_address << ":" << new_broker_port << " successfully added to peer list of broker: " << address_ << std::endl;

    // Start health check with new thread

    return grpc::Status::OK;
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
        // Start the client
        JoinCluster();
    }
}