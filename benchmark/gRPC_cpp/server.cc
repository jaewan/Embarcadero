// benchmark_server.cpp
#include <iostream>
#include <string>
#include <grpcpp/grpcpp.h>
#include "benchmark.grpc.pb.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::Status;
using benchmark::BenchmarkRequest;
using benchmark::BenchmarkReply;
using benchmark::BenchmarkService;

// Implementation of the BenchmarkService
class BenchmarkServiceImpl final : public BenchmarkService::Service {
 public:
  Status SendMessage(ServerContext* context, const BenchmarkRequest* request,
                     BenchmarkReply* reply) override {
    reply->set_message("Ack");
    return Status::OK;
  }
};

void RunServer() {
  std::string server_address("0.0.0.0:50052");
  BenchmarkServiceImpl service;

  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "Server listening on " << server_address << std::endl;
  server->Wait();
}

int main() {
  RunServer();
  return 0;
}
