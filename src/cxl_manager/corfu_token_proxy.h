#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <chrono>

#include <corfu_token_proxy.grpc.pb.h>
#include <corfu_sequencer.grpc.pb.h>

namespace grpc { class Server; }

namespace Corfu {

// C1a gRPC backend: the ingress broker forwards the unchanged logical token
// request to the existing global Corfu sequencer.  Mailbox C1b replaces only
// this backend, not the client-visible proxy service.
class GrpcCorfuTokenProxy final : public corfutokenproxy::CorfuTokenProxy::Service {
 public:
  explicit GrpcCorfuTokenProxy(std::string global_sequencer_endpoint);
  grpc::Status GetTotalOrder(grpc::ServerContext* context,
      const corfutokenproxy::TokenRequest* request,
      corfutokenproxy::TokenResponse* response) override;

 private:
  struct CachedGrant {
    uint64_t total_order;
    uint64_t log_idx;
    uint64_t broker_batch_seq;
    std::chrono::steady_clock::time_point created;
  };
  std::string CacheKey(const corfutokenproxy::TokenRequest& request) const;
  std::unique_ptr<corfusequencer::CorfuSequencer::Stub> stub_;
  std::mutex cache_mu_;
  std::unordered_map<std::string, CachedGrant> grants_;
};

class CorfuTokenProxyServer {
 public:
  CorfuTokenProxyServer(std::string listen_endpoint, std::string global_sequencer_endpoint);
  bool Start(std::string* error);
  void Shutdown();

 private:
  std::string listen_endpoint_;
  std::string global_sequencer_endpoint_;
  std::unique_ptr<GrpcCorfuTokenProxy> service_;
  std::unique_ptr<grpc::Server> server_;
};

}  // namespace Corfu
