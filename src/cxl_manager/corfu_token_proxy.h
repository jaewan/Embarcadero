#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <thread>

#include <corfu_token_proxy.grpc.pb.h>
#include <corfu_sequencer.grpc.pb.h>

namespace grpc { class Server; }
namespace Embarcadero::cxl_transport { class MailboxSegment; }

namespace Corfu {

// C1a gRPC backend: the ingress broker forwards the unchanged logical token
// request to the existing global Corfu sequencer.  Mailbox C1b replaces only
// this backend, not the client-visible proxy service.
class GrpcCorfuTokenProxy final : public corfutokenproxy::CorfuTokenProxy::Service {
 public:
  // expected_broker_id binds this service instance to one broker-membership
  // ingress. A negative value is retained only for unbound protocol tests.
  explicit GrpcCorfuTokenProxy(std::string global_sequencer_endpoint,
                               int expected_broker_id = -1);
  grpc::Status GetTotalOrder(grpc::ServerContext* context,
      const corfutokenproxy::TokenRequest* request,
      corfutokenproxy::TokenResponse* response) override;

 private:
  struct CachedGrant {
    uint64_t total_order;
    uint64_t log_idx;
    uint64_t broker_batch_seq;
    // The logical request identity intentionally excludes the shape: a retry
    // must use the same shape as the request that consumed the global token.
    // Retaining it lets the proxy reject a conflicting reuse of a batch_seq
    // instead of silently returning a grant for different payload bytes.
    uint64_t num_msg;
    uint64_t total_size;
    std::chrono::steady_clock::time_point created;
  };
  std::string CacheKey(const corfutokenproxy::TokenRequest& request) const;
  const int expected_broker_id_;
  std::unique_ptr<corfusequencer::CorfuSequencer::Stub> stub_;
  std::mutex cache_mu_;
  std::unordered_map<std::string, CachedGrant> grants_;
};

// C1b: keeps the publisher-to-ingress gRPC protocol unchanged, but replaces the
// ingress-to-global-sequencer hop with the broker-owned CXL mailbox rings.  Calls
// owns an MPSC handoff plus one dispatcher and one receiver: those two threads
// are respectively the sole writer of up(b) and sole reader of down(b).
class MailboxCorfuTokenProxy final : public corfutokenproxy::CorfuTokenProxy::Service {
 public:
  MailboxCorfuTokenProxy(Embarcadero::cxl_transport::MailboxSegment* segment,
                         int expected_broker_id);
  ~MailboxCorfuTokenProxy() override;
  grpc::Status GetTotalOrder(grpc::ServerContext* context,
      const corfutokenproxy::TokenRequest* request,
      corfutokenproxy::TokenResponse* response) override;

 private:
  struct PendingGrant {
	uint64_t num_msg;
	uint64_t total_size;
	uint64_t client_id;
	uint64_t batch_seq;
	uint32_t broker_id;
	uint64_t session_id;
	bool request_sent{false};
	bool terminal{false};
	bool poisoned{false};
	uint32_t waiters{0};
    uint32_t status{0};
    uint64_t total_order{0};
    uint64_t log_idx{0};
    uint64_t broker_batch_seq{0};
	uint64_t correlation_id{0};
	std::chrono::steady_clock::time_point created;
	std::chrono::steady_clock::time_point last_access;
    std::condition_variable cv;
  };
  std::string CacheKey(const corfutokenproxy::TokenRequest& request) const;
  void DispatchLoop();
  void ReceiveLoop();
  grpc::Status GrantStatus(uint32_t status) const;

  Embarcadero::cxl_transport::MailboxSegment* const segment_;
  const int expected_broker_id_;
  std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<PendingGrant>> pending_;
  std::unordered_map<uint64_t, std::shared_ptr<PendingGrant>> by_correlation_;
  std::deque<std::pair<std::string, corfutokenproxy::TokenRequest>> requests_;
  std::condition_variable dispatch_cv_;
	std::atomic<bool> stop_{false};
	const uint64_t session_id_;
	std::atomic<uint64_t> next_correlation_{1};
  std::thread dispatcher_;
  std::thread receiver_;
};

class CorfuTokenProxyServer {
 public:
  CorfuTokenProxyServer(std::string listen_endpoint, std::string global_sequencer_endpoint,
                        int expected_broker_id,
                        Embarcadero::cxl_transport::MailboxSegment* mailbox_segment = nullptr);
  bool Start(std::string* error);
  void Shutdown();

 private:
  std::string listen_endpoint_;
  std::string global_sequencer_endpoint_;
  int expected_broker_id_;
  Embarcadero::cxl_transport::MailboxSegment* mailbox_segment_;
  std::unique_ptr<corfutokenproxy::CorfuTokenProxy::Service> service_;
  std::unique_ptr<grpc::Server> server_;
};

}  // namespace Corfu
