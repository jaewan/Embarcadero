// Deterministic C1a validation for the Corfu broker token proxy.
//
// The proxy is deliberately tested against a separate, fresh direct sequencer:
// identical logical request traces must receive byte-identical grants.  The
// remaining checks cover retry idempotence, conflicting retries, the bounded
// upstream deadline, and two distinct membership-derived ingress endpoints.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "corfu_sequencer.grpc.pb.h"
#include "corfu_token_proxy.grpc.pb.h"
#include "cxl_manager/corfu_sequencer_service.h"
#include "cxl_manager/corfu_mailbox_messages.h"
#include "cxl_manager/corfu_mailbox_sequencer.h"
#include "cxl_manager/corfu_token_proxy.h"
#include "cxl_transport/cxl_mailbox.h"

namespace {

using corfusequencer::CorfuSequencer;
using corfusequencer::TotalOrderRequest;
using corfusequencer::TotalOrderResponse;
using corfutokenproxy::CorfuTokenProxy;
using corfutokenproxy::TokenRequest;
using corfutokenproxy::TokenResponse;

int Fail(const char* message) {
  std::fprintf(stderr, "FAIL: %s\n", message);
  return 1;
}

std::unique_ptr<grpc::Server> StartServer(grpc::Service* service, int* port) {
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), port);
  builder.RegisterService(service);
  return builder.BuildAndStart();
}

grpc::Status DirectCall(const std::string& endpoint, const TokenRequest& request,
                        TokenResponse* out) {
  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  auto stub = CorfuSequencer::NewStub(channel);
  TotalOrderRequest forwarded;
  forwarded.set_client_id(request.client_id());
  forwarded.set_batchseq(request.batchseq());
  forwarded.set_num_msg(request.num_msg());
  forwarded.set_total_size(request.total_size());
  forwarded.set_broker_id(request.broker_id());
  TotalOrderResponse response;
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  grpc::Status status = stub->GetTotalOrder(&context, forwarded, &response);
  if (status.ok()) {
    out->set_total_order(response.total_order());
    out->set_log_idx(response.log_idx());
    out->set_broker_batch_seq(response.broker_batch_seq());
  }
  return status;
}

grpc::Status ProxyCall(const std::string& endpoint, const TokenRequest& request,
                       TokenResponse* out, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  auto stub = CorfuTokenProxy::NewStub(channel);
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + timeout);
  return stub->GetTotalOrder(&context, request, out);
}

bool SameGrant(const TokenResponse& lhs, const TokenResponse& rhs) {
  return lhs.total_order() == rhs.total_order() && lhs.log_idx() == rhs.log_idx() &&
         lhs.broker_batch_seq() == rhs.broker_batch_seq();
}

std::vector<TokenRequest> ValidTrace() {
  std::vector<TokenRequest> trace;
  for (uint64_t seq = 0; seq != 8; ++seq) {
    for (uint32_t broker = 0; broker != 2; ++broker) {
      TokenRequest request;
      request.set_client_id(100 + broker);
      request.set_broker_id(broker);
      request.set_batchseq(seq);
      request.set_num_msg(1 + ((seq + broker) % 3));
      request.set_total_size(64 * request.num_msg());
      trace.push_back(request);
    }
  }
  return trace;
}

int RunTraceParityAndTwoIngressSmoke() {
  CorfuSequencerImpl direct_service;
  int direct_port = 0;
  auto direct_server = StartServer(&direct_service, &direct_port);
  if (!direct_server || direct_port <= 0) return Fail("direct sequencer did not bind");

  CorfuSequencerImpl proxied_service;
  int sequencer_port = 0;
  auto sequencer_server = StartServer(&proxied_service, &sequencer_port);
  if (!sequencer_server || sequencer_port <= 0) return Fail("proxied sequencer did not bind");
  const std::string sequencer_endpoint = "127.0.0.1:" + std::to_string(sequencer_port);

  // In production these are selected from broker membership host addresses plus
  // the broker-specific proxy port. Two independently bound services ensure the
  // test exercises both ingress roles rather than one shared local shortcut.
  Corfu::GrpcCorfuTokenProxy proxy0(sequencer_endpoint, 0);
  Corfu::GrpcCorfuTokenProxy proxy1(sequencer_endpoint, 1);
  int proxy0_port = 0;
  int proxy1_port = 0;
  auto proxy0_server = StartServer(&proxy0, &proxy0_port);
  auto proxy1_server = StartServer(&proxy1, &proxy1_port);
  if (!proxy0_server || !proxy1_server || proxy0_port <= 0 || proxy1_port <= 0) {
    return Fail("broker proxy did not bind");
  }
  const std::vector<std::string> membership_proxy_endpoints = {
      "127.0.0.1:" + std::to_string(proxy0_port),
      "127.0.0.1:" + std::to_string(proxy1_port)};
  const std::string direct_endpoint = "127.0.0.1:" + std::to_string(direct_port);

  for (const TokenRequest& request : ValidTrace()) {
    TokenResponse direct_grant;
    TokenResponse proxy_grant;
    if (!DirectCall(direct_endpoint, request, &direct_grant).ok()) {
      return Fail("direct trace request failed");
    }
    const std::string& ingress = membership_proxy_endpoints.at(request.broker_id());
    if (!ProxyCall(ingress, request, &proxy_grant).ok()) {
      return Fail("membership-derived proxy request failed");
    }
    if (!SameGrant(direct_grant, proxy_grant)) {
      return Fail("direct/proxy trace grants differ");
    }
  }
  // Endpoint selection is part of the protocol: broker 0's ingress must not
  // accept a request labelled for broker 1.
  TokenResponse misrouted_response;
  TokenRequest misrouted = ValidTrace().at(1);  // broker_id == 1
  if (ProxyCall(membership_proxy_endpoints.at(0), misrouted, &misrouted_response)
          .error_code() != grpc::StatusCode::INVALID_ARGUMENT) {
    return Fail("non-owner ingress accepted a misrouted broker token");
  }
  std::fprintf(stderr, "check: direct-proxy trace parity and two ingress endpoints: ok\n");
  return 0;
}

int RunRetryAndConflictTest() {
  CorfuSequencerImpl sequencer;
  int sequencer_port = 0;
  auto sequencer_server = StartServer(&sequencer, &sequencer_port);
  if (!sequencer_server) return Fail("retry sequencer did not bind");
  Corfu::GrpcCorfuTokenProxy proxy("127.0.0.1:" + std::to_string(sequencer_port));
  int proxy_port = 0;
  auto proxy_server = StartServer(&proxy, &proxy_port);
  if (!proxy_server) return Fail("retry proxy did not bind");
  const std::string endpoint = "127.0.0.1:" + std::to_string(proxy_port);

  TokenRequest request;
  request.set_client_id(77);
  request.set_broker_id(0);
  request.set_batchseq(0);
  request.set_num_msg(3);
  request.set_total_size(192);
  TokenResponse first;
  TokenResponse retry;
  if (!ProxyCall(endpoint, request, &first).ok() || !ProxyCall(endpoint, request, &retry).ok()) {
    return Fail("proxy retry request failed");
  }
  if (!SameGrant(first, retry)) return Fail("retry received a different grant");

  TokenRequest next = request;
  next.set_batchseq(1);
  TokenResponse next_grant;
  if (!ProxyCall(endpoint, next, &next_grant).ok() ||
      next_grant.total_order() != first.total_order() + request.num_msg()) {
    return Fail("retry consumed a second global token");
  }

  TokenRequest conflicting = request;
  conflicting.set_total_size(256);
  TokenResponse ignored;
  const grpc::Status conflict_status = ProxyCall(endpoint, conflicting, &ignored);
  if (conflict_status.error_code() != grpc::StatusCode::INVALID_ARGUMENT) {
    return Fail("conflicting retry was not rejected");
  }
  std::fprintf(stderr, "check: retry idempotence and conflicting retry rejection: ok\n");
  return 0;
}

int RunMailboxProxyTest() {
  // This is deliberately a separate in-process shared-memory segment: the
  // broker-facing API remains gRPC while the proxy's upstream hop is the exact
  // mailbox record/ring path used with a CXL in-place segment in production.
  Embarcadero::cxl_transport::MailboxParams params;
  params.num_brokers = 1;
  params.record_size = 512;
  params.up_capacity = 8;
  params.down_capacity = 8;
  const std::string name = "/corfu_proxy_mailbox_smoke_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  auto segment = Embarcadero::cxl_transport::MailboxSegment::CreateShm(name, params);
  if (!segment) return Fail("could not create mailbox segment");
  CorfuSequencerImpl sequencer;
  Embarcadero::cxl_manager::CorfuMailboxSequencer mailbox_sequencer(&sequencer, segment.get());
  mailbox_sequencer.StartThread();
  Corfu::MailboxCorfuTokenProxy proxy(segment.get(), 0);
  int proxy_port = 0;
  auto proxy_server = StartServer(&proxy, &proxy_port);
  if (!proxy_server) return Fail("mailbox proxy did not bind");
  const std::string endpoint = "127.0.0.1:" + std::to_string(proxy_port);

  // Fixed logical traces must yield the same grants through the direct gRPC
  // state machine and the mailbox state machine.  This is stronger than merely
  // checking that mailbox grants are monotonic.
  CorfuSequencerImpl direct_sequencer;
  int direct_port = 0;
  auto direct_server = StartServer(&direct_sequencer, &direct_port);
  if (!direct_server) return Fail("mailbox parity direct sequencer did not bind");
  const std::string direct_endpoint = "127.0.0.1:" + std::to_string(direct_port);
  for (const TokenRequest& trace_request : ValidTrace()) {
    if (trace_request.broker_id() != 0) continue;
    TokenResponse direct_grant;
    TokenResponse mailbox_grant;
    if (!DirectCall(direct_endpoint, trace_request, &direct_grant).ok() ||
        !ProxyCall(endpoint, trace_request, &mailbox_grant).ok() ||
        !SameGrant(direct_grant, mailbox_grant)) {
      return Fail("mailbox/direct fixed trace grants differ");
    }
  }

  TokenRequest request;
  request.set_client_id(4242);
  request.set_broker_id(0);
  request.set_batchseq(0);
  request.set_num_msg(2);
  request.set_total_size(128);
  TokenResponse first;
  TokenResponse retry;
  if (!ProxyCall(endpoint, request, &first).ok() || !ProxyCall(endpoint, request, &retry).ok()) {
    return Fail("mailbox proxy request failed");
  }
  if (!SameGrant(first, retry)) return Fail("mailbox proxy retry consumed a second token");
  TokenRequest next = request;
  next.set_batchseq(1);
  TokenResponse next_grant;
  if (!ProxyCall(endpoint, next, &next_grant).ok() ||
      next_grant.total_order() != first.total_order() + request.num_msg()) {
    return Fail("mailbox proxy did not preserve Corfu token order");
  }
  TokenRequest wrong_broker = request;
  wrong_broker.set_broker_id(1);
  TokenResponse ignored;
  if (ProxyCall(endpoint, wrong_broker, &ignored).error_code() != grpc::StatusCode::INVALID_ARGUMENT) {
    return Fail("mailbox proxy accepted a non-owner ingress request");
  }
  TokenRequest invalid = next;
  invalid.set_batchseq(2);
  invalid.set_num_msg(0);
  if (ProxyCall(endpoint, invalid, &ignored).error_code() != grpc::StatusCode::INVALID_ARGUMENT) {
    return Fail("mailbox proxy did not preserve sequencer invalid-request status");
  }
  // Concurrent gRPC handlers must not become concurrent SPSC producers.  The
  // proxy's dispatcher/receiver pair owns those rings; these requests exercise
  // independent waiter correlation through that handoff.
  std::vector<std::future<grpc::Status>> concurrent;
  std::vector<TokenResponse> concurrent_grants(8);
  for (uint64_t client = 0; client != concurrent_grants.size(); ++client) {
    TokenRequest r;
    r.set_client_id(9000 + client); r.set_broker_id(0); r.set_batchseq(0);
    r.set_num_msg(1); r.set_total_size(64);
    concurrent.emplace_back(std::async(std::launch::async, [&, r, client] {
      return ProxyCall(endpoint, r, &concurrent_grants[client]);
    }));
  }
  for (auto& result : concurrent) {
    if (!result.get().ok()) return Fail("concurrent mailbox proxy request failed");
  }
  proxy_server->Shutdown();
  mailbox_sequencer.Stop();
  mailbox_sequencer.Join();
  std::fprintf(stderr, "check: mailbox proxy transport and retry semantics: ok\n");
  return 0;
}

int RunMailboxStaleGrantFailClosedTest() {
  Embarcadero::cxl_transport::MailboxParams params;
  params.num_brokers = 1;
  params.record_size = 512;
  params.up_capacity = 8;
  params.down_capacity = 8;
  const std::string name = "/corfu_proxy_stale_grant_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  auto segment = Embarcadero::cxl_transport::MailboxSegment::CreateShm(name, params);
  if (!segment) return Fail("could not create stale-grant mailbox segment");
  // Intentionally do not start a sequencer.  The test acts as a stale mailbox
  // writer after observing the queued request and verifies the proxy refuses a
  // grant whose session/key context does not match that request.
  Corfu::MailboxCorfuTokenProxy proxy(segment.get(), 0);
  int proxy_port = 0;
  auto proxy_server = StartServer(&proxy, &proxy_port);
  if (!proxy_server) return Fail("stale-grant proxy did not bind");
  TokenRequest request;
  request.set_client_id(777); request.set_broker_id(0); request.set_batchseq(0);
  request.set_num_msg(1); request.set_total_size(64);
  TokenResponse response;
  auto future = std::async(std::launch::async, [&] {
    return ProxyCall("127.0.0.1:" + std::to_string(proxy_port), request, &response,
                     std::chrono::seconds(3));
  });
  Embarcadero::cxl_manager::CorfuTokenRequest queued{};
  uint32_t len = 0;
  bool consumed = false;
  for (int attempt = 0; attempt != 1000; ++attempt) {
    if (segment->up(0).TryConsume(&queued, sizeof(queued), &len)) {
      consumed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  if (!consumed || len != sizeof(queued)) return Fail("proxy did not enqueue stale-grant request");
  Embarcadero::cxl_manager::CorfuTokenGrant forged{};
  forged.client_id = queued.client_id;
  forged.batch_seq = queued.batch_seq;
  forged.broker_id = queued.broker_id;
  forged.correlation_id = queued.correlation_id;
  forged.session_id = queued.session_id + 1;  // deliberate prior-session alias
  forged.status = 0;
  if (!segment->down(0).TryProduce(&forged, sizeof(forged))) {
    return Fail("could not inject forged stale grant");
  }
  if (future.get().error_code() != grpc::StatusCode::DATA_LOSS) {
    return Fail("stale mailbox grant was not failed closed");
  }
  proxy_server->Shutdown();
  std::fprintf(stderr, "check: stale mailbox grant session/key mismatch fails closed: ok\n");
  return 0;
}

int RunMailboxTimeoutRetryNoDuplicateTest() {
  Embarcadero::cxl_transport::MailboxParams params;
  params.num_brokers = 1;
  params.record_size = 512;
  params.up_capacity = 8;
  params.down_capacity = 8;
  const std::string name = "/corfu_proxy_timeout_retry_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
  auto segment = Embarcadero::cxl_transport::MailboxSegment::CreateShm(name, params);
  if (!segment) return Fail("could not create timeout-retry mailbox segment");
  // No mailbox sequencer: this models a stopped/unavailable control service.
  Corfu::MailboxCorfuTokenProxy proxy(segment.get(), 0);
  int proxy_port = 0;
  auto proxy_server = StartServer(&proxy, &proxy_port);
  if (!proxy_server) return Fail("timeout-retry proxy did not bind");
  const std::string endpoint = "127.0.0.1:" + std::to_string(proxy_port);
  TokenRequest request;
  request.set_client_id(888); request.set_broker_id(0); request.set_batchseq(0);
  request.set_num_msg(1); request.set_total_size(64);
  TokenResponse first;
  if (ProxyCall(endpoint, request, &first, std::chrono::milliseconds(100)).error_code() !=
      grpc::StatusCode::DEADLINE_EXCEEDED) {
    return Fail("stopped mailbox sequencer did not time out token phase");
  }
  Embarcadero::cxl_manager::CorfuTokenRequest posted{};
  uint32_t len = 0;
  if (!segment->up(0).TryConsume(&posted, sizeof(posted), &len) || len != sizeof(posted)) {
    return Fail("first timeout did not submit exactly one mailbox request");
  }
  TokenResponse retry;
  if (ProxyCall(endpoint, request, &retry, std::chrono::milliseconds(100)).error_code() !=
      grpc::StatusCode::DEADLINE_EXCEEDED) {
    return Fail("retry of pending mailbox token did not retain timeout state");
  }
  if (segment->up(0).TryConsume(&posted, sizeof(posted), &len)) {
    return Fail("timed-out mailbox token retry submitted a second request");
  }
  proxy_server->Shutdown();
  std::fprintf(stderr, "check: stopped mailbox timeout suppresses grant/payload phase and retry duplicate: ok\n");
  return 0;
}

class DeadlineObservingSequencer final : public CorfuSequencer::Service {
 public:
  grpc::Status GetTotalOrder(grpc::ServerContext* context, const TotalOrderRequest*,
                             TotalOrderResponse*) override {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        context->deadline() - std::chrono::system_clock::now());
    remaining_ms_.store(remaining.count(), std::memory_order_release);
    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "intentional upstream failure");
  }

  int64_t remaining_ms() const { return remaining_ms_.load(std::memory_order_acquire); }

 private:
  std::atomic<int64_t> remaining_ms_{-1};
};

int RunFailedTokenDeadlineTest() {
  DeadlineObservingSequencer sequencer;
  int sequencer_port = 0;
  auto sequencer_server = StartServer(&sequencer, &sequencer_port);
  if (!sequencer_server) return Fail("deadline sequencer did not bind");
  Corfu::GrpcCorfuTokenProxy proxy("127.0.0.1:" + std::to_string(sequencer_port));
  int proxy_port = 0;
  auto proxy_server = StartServer(&proxy, &proxy_port);
  if (!proxy_server) return Fail("deadline proxy did not bind");

  TokenRequest request;
  request.set_client_id(9);
  request.set_broker_id(0);
  request.set_batchseq(0);
  request.set_num_msg(1);
  request.set_total_size(64);
  TokenResponse response;
  const auto start = std::chrono::steady_clock::now();
  const grpc::Status status = ProxyCall("127.0.0.1:" + std::to_string(proxy_port), request,
                                        &response, std::chrono::seconds(40));
  const auto elapsed = std::chrono::steady_clock::now() - start;
  if (status.error_code() != grpc::StatusCode::UNAVAILABLE ||
      elapsed >= std::chrono::seconds(30)) {
    return Fail("failed token phase was not bounded before grant");
  }
  // Allow scheduling/network jitter but require that the proxy forwarded no
  // more than its documented 30-second admission deadline.
  // gRPC's wire deadline has millisecond rounding/propagation slack; permit
  // one second for that representation but never a caller-supplied 40-second
  // deadline.
  if (sequencer.remaining_ms() < 25'000 || sequencer.remaining_ms() > 31'000) {
    std::fprintf(stderr, "observed upstream deadline remaining_ms=%lld\n",
                 static_cast<long long>(sequencer.remaining_ms()));
    return Fail("proxy failed to cap upstream token deadline at 30 seconds");
  }
  std::fprintf(stderr, "check: failed token phase returns before payload admission; 30s cap: ok\n");
  return 0;
}

}  // namespace

int main() {
  if (RunTraceParityAndTwoIngressSmoke() != 0) return 1;
  if (RunRetryAndConflictTest() != 0) return 1;
  if (RunMailboxProxyTest() != 0) return 1;
  if (RunMailboxStaleGrantFailClosedTest() != 0) return 1;
  if (RunMailboxTimeoutRetryNoDuplicateTest() != 0) return 1;
  if (RunFailedTokenDeadlineTest() != 0) return 1;
  std::fprintf(stderr, "ALL CORFU TOKEN PROXY CHECKS PASSED\n");
  return 0;
}
