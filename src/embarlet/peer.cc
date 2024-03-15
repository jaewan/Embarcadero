#include <iostream>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include "config.h"
#include "peer.h"

PeerBroker::PeerBroker( bool is_head, std::string address, std::string port) {
    if (is_head) {
        this->is_head = true;
        this->address = GetAddress();
        this->port = "8080"; // Default port for now
        this->peer_brokers = absl::flat_hash_map<std::string, std::string>();
    } else {
        this->is_head = false;
        this->address = GetAddress();
        this->port = port;
        this->peer_brokers.emplace(address, port);
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
    grpc::SslCredentialsOptions ssl_opts;
    std::shared_ptr<grpc::ChannelCredentials> creds = grpc::SslCredentials(ssl_opts);

    return Peer::NewStub(grpc::CreateChannel(peer_url, creds));
}

void PeerBroker::BroadcastHeartbeat() {
    // For each peer in the hashmap, send a heartbeat request
    for (auto const& peer : peer_brokers) {
        std::string peer_address = peer.first;
        std::string peer_port = peer.second;

        std::string peer_url = peer_address + ":" + peer_port;
        auto rpc_client = GetRpcClient(peer_url);

        BroadcastHeartbeatRequest request;
        request.set_address(address);
        request.set_port(port);

        BroadcastHeartbeatResponse response;

        grpc::ClientContext context;
        grpc::Status status = rpc_client->HandleBroadcastHeartbeat(&context, request, &response);

        if (!status.ok()) {
            std::cout << "Error sending heartbeat to peer " << peer_address << ":" << peer_port << std::endl;
        }
    }
}

grpc::Status PeerBroker::HandleBroadcastHeartbeat(grpc::ServerContext* context, const BroadcastHeartbeatRequest* request, BroadcastHeartbeatResponse* response) {
    std::string peer_address = request->address();
    std::string peer_port = request->port();

    // If the peer is not in the hashmap, add it
    if (peer_brokers.find(peer_address) == peer_brokers.end()) {
        peer_brokers.emplace(peer_address, peer_port);
    }

    std::cout << "Peer " << peer_address << ":" << peer_port << " is alive" << std::endl;

    return grpc::Status::OK;
}

void PeerBroker::Run() {
    if (is_head) {
        // Start the server
        std::string server_address = address + ":" + port;
        grpc::SslServerCredentialsOptions ssl_opts;
        std::shared_ptr<grpc::ServerCredentials> creds = grpc::SslServerCredentials(ssl_opts);

        grpc::ServerBuilder builder;
        builder.AddListeningPort(server_address, creds);
        builder.RegisterService(this);

        std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
        std::cout << "Server listening on " << server_address << std::endl;

        server->Wait();
    } else {
        // Start the client
        while (true) {
            BroadcastHeartbeat();
            sleep(5);
        }
    }
}