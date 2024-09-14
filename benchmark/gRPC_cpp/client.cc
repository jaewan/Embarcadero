// benchmark_client.cpp
#include <iostream>
#include <chrono>
#include <grpcpp/grpcpp.h>
#include "benchmark.grpc.pb.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::Status;
using benchmark::BenchmarkService;
using benchmark::BenchmarkRequest;
using benchmark::BenchmarkReply;

class BenchmarkClient {
 public:
  BenchmarkClient(std::shared_ptr<Channel> channel)
      : stub_(BenchmarkService::NewStub(channel)) {}

  void BenchmarkThroughput(int message_count, const std::string& message, int message_size) {
    BenchmarkRequest request;
    request.set_message(message);
    BenchmarkReply reply;

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < message_count; ++i) {
      ClientContext context;
      Status status = stub_->SendMessage(&context, request, &reply);
      if (!status.ok()) {
        std::cerr << "gRPC failed." << std::endl;
        return;
      }
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double elapsed_seconds = elapsed_ns * 1e-9;
    double throughput = (message_count * message_size) / elapsed_seconds;

    std::cout << "Sent " << message_count << " messages of size " << message_size << " bytes." << std::endl;
    std::cout << "Throughput: " << throughput / (1024 * 1024) << " MB/s." << std::endl;
    std::cout << "Elapsed time: " << elapsed_ns << " nanoseconds." << std::endl;
  }

 private:
  std::unique_ptr<BenchmarkService::Stub> stub_;
};

int main(int argc, char** argv) {
  std::string target_str = "0.0.0.0:50052";
  BenchmarkClient client(grpc::CreateChannel(target_str, grpc::InsecureChannelCredentials()));

  int message_count = 1000;  // Number of messages to send
  int message_size = 1024;

  // Generate 1KB message
  std::string message(message_size, 'A'); 

  client.BenchmarkThroughput(message_count, message, message_size);

  return 0;
}
