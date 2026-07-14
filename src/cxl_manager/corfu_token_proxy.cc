#include "cxl_manager/corfu_token_proxy.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <limits>
#include <sys/random.h>
#include <grpcpp/grpcpp.h>

#include "common/performance_utils.h"
#include "cxl_manager/corfu_mailbox_messages.h"
#include "cxl_transport/cxl_mailbox.h"

namespace Corfu {

namespace {

constexpr size_t kMaxMailboxPending = 65'536;

uint64_t NewMailboxProxySessionId() {
  uint64_t id = 0;
  // This is a transport incarnation identifier, not a logical request field.
  // Fail closed if the kernel cannot provide a fresh nonce: reusing a counter
  // after proxy restart could otherwise alias a late CXL grant.
  const ssize_t read = getrandom(&id, sizeof(id), 0);
  CHECK_EQ(read, static_cast<ssize_t>(sizeof(id)))
      << "cannot obtain Corfu mailbox proxy session nonce";
  CHECK_NE(id, 0u);
  return id;
}

}  // namespace

GrpcCorfuTokenProxy::GrpcCorfuTokenProxy(std::string global_sequencer_endpoint,
                                         int expected_broker_id)
    : expected_broker_id_(expected_broker_id),
      stub_(corfusequencer::CorfuSequencer::NewStub(
          grpc::CreateChannel(global_sequencer_endpoint, grpc::InsecureChannelCredentials()))) {}

grpc::Status GrpcCorfuTokenProxy::GetTotalOrder(
    grpc::ServerContext* context, const corfutokenproxy::TokenRequest* request,
    corfutokenproxy::TokenResponse* response) {
  // Publisher derives the endpoint from the selected broker's membership
  // address. Enforce that binding at ingress, so requests cannot silently use
  // a different broker's proxy while retaining their original broker id.
  if (expected_broker_id_ >= 0 &&
      request->broker_id() != static_cast<uint32_t>(expected_broker_id_)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "token request routed to non-owner Corfu ingress");
  }
  // Serialize cache miss handling. This keeps a retry from issuing a second
  // token for the same logical (client_id, broker_id, original batchseq) key.
  std::lock_guard<std::mutex> lock(cache_mu_);
  const auto now = std::chrono::steady_clock::now();
  for (auto it = grants_.begin(); it != grants_.end();) {
    if (now - it->second.created > std::chrono::seconds(60)) it = grants_.erase(it);
    else ++it;
  }
  const std::string key = CacheKey(*request);
  if (const auto found = grants_.find(key); found != grants_.end()) {
    if (found->second.num_msg != request->num_msg() ||
        found->second.total_size != request->total_size()) {
      return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                          "conflicting retry for an already granted token");
    }
    response->set_total_order(found->second.total_order);
    response->set_log_idx(found->second.log_idx);
    response->set_broker_batch_seq(found->second.broker_batch_seq);
    return grpc::Status::OK;
  }
  corfusequencer::TotalOrderRequest forwarded;
  forwarded.set_client_id(request->client_id());
  forwarded.set_batchseq(request->batchseq());
  forwarded.set_num_msg(request->num_msg());
  forwarded.set_total_size(request->total_size());
  forwarded.set_broker_id(request->broker_id());
  corfusequencer::TotalOrderResponse grant;
  grpc::ClientContext upstream;
  // A proxy must never turn a token-before-write phase into an unbounded
  // payload admission wait. Preserve a shorter client deadline, but impose a
  // hard 30-second cap when the caller has no deadline (or a longer one).
  const auto deadline_cap = std::chrono::system_clock::now() + std::chrono::seconds(30);
  upstream.set_deadline(std::min(context->deadline(), deadline_cap));
  const grpc::Status status = stub_->GetTotalOrder(&upstream, forwarded, &grant);
  if (!status.ok()) return status;
  response->set_total_order(grant.total_order());
  response->set_log_idx(grant.log_idx());
  response->set_broker_batch_seq(grant.broker_batch_seq());
  if (grants_.size() >= 65536) grants_.erase(grants_.begin());
  grants_.emplace(key, CachedGrant{grant.total_order(), grant.log_idx(),
      grant.broker_batch_seq(), request->num_msg(), request->total_size(), now});
  return grpc::Status::OK;
}

std::string GrpcCorfuTokenProxy::CacheKey(
    const corfutokenproxy::TokenRequest& request) const {
  return std::to_string(request.client_id()) + ":" +
      std::to_string(request.broker_id()) + ":" + std::to_string(request.batchseq());
}

MailboxCorfuTokenProxy::MailboxCorfuTokenProxy(
    Embarcadero::cxl_transport::MailboxSegment* segment, int expected_broker_id)
    : segment_(segment), expected_broker_id_(expected_broker_id),
      session_id_(NewMailboxProxySessionId()) {
  CHECK_NOTNULL(segment_);
  CHECK_GE(expected_broker_id_, 0);
  CHECK_LT(static_cast<uint32_t>(expected_broker_id_), segment_->num_brokers());
  CHECK_GE(segment_->record_size(), sizeof(Embarcadero::cxl_manager::CorfuTokenRequest));
  CHECK_GE(segment_->record_size(), sizeof(Embarcadero::cxl_manager::CorfuTokenGrant));
  dispatcher_ = std::thread(&MailboxCorfuTokenProxy::DispatchLoop, this);
  receiver_ = std::thread(&MailboxCorfuTokenProxy::ReceiveLoop, this);
}

MailboxCorfuTokenProxy::~MailboxCorfuTokenProxy() {
  stop_.store(true, std::memory_order_release);
  dispatch_cv_.notify_all();
  if (dispatcher_.joinable()) dispatcher_.join();
  if (receiver_.joinable()) receiver_.join();
}

std::string MailboxCorfuTokenProxy::CacheKey(
    const corfutokenproxy::TokenRequest& request) const {
  return std::to_string(request.client_id()) + ":" +
      std::to_string(request.broker_id()) + ":" + std::to_string(request.batchseq());
}

grpc::Status MailboxCorfuTokenProxy::GrantStatus(uint32_t status) const {
  switch (status) {
    case 0: return grpc::Status::OK;
    case 1: return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "global Corfu sequencer rejected token request");
    // Match CorfuSequencerImpl::GetTotalOrder's public gRPC mapping exactly;
    // the mailbox is a transport swap, not an opportunity to reinterpret these.
    case 2: return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "Batch sequence already processed");
    case 3: return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "Out of order batch sequence, please retry");
    default: return grpc::Status(grpc::StatusCode::DATA_LOSS,
                                 "invalid Corfu mailbox grant status");
  }
}

void MailboxCorfuTokenProxy::ReceiveLoop() {
  std::array<uint8_t, 512> buffer{};
  auto& down = segment_->down(static_cast<uint32_t>(expected_broker_id_));
  while (!stop_.load(std::memory_order_acquire)) {
    uint32_t len = 0;
    if (!down.TryConsume(buffer.data(), buffer.size(), &len)) {
      Embarcadero::CXL::cpu_pause();
      continue;
    }
    if (len != sizeof(Embarcadero::cxl_manager::CorfuTokenGrant)) {
      LOG(ERROR) << "discarding malformed Corfu mailbox grant: len=" << len;
      continue;
    }
    const auto* grant = reinterpret_cast<const Embarcadero::cxl_manager::CorfuTokenGrant*>(buffer.data());
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = by_correlation_.find(grant->correlation_id);
    if (it == by_correlation_.end()) {
      LOG(ERROR) << "discarding uncorrelated Corfu mailbox grant id=" << grant->correlation_id;
      continue;
    }
    auto& pending = *it->second;
    // A matching counter is insufficient after a proxy restart.  Validate the
    // session and the complete logical request context echoed by the
    // sequencer.  On any mismatch, poison the entry permanently rather than
    // allowing a retry to submit a second token for an uncertain first one.
    if (grant->session_id != pending.session_id ||
        grant->client_id != pending.client_id ||
        grant->batch_seq != pending.batch_seq ||
        grant->broker_id != pending.broker_id) {
      LOG(ERROR) << "Corfu mailbox grant context mismatch for correlation="
                 << grant->correlation_id << "; failing closed";
      pending.poisoned = true;
      pending.terminal = true;
      pending.status = std::numeric_limits<uint32_t>::max();
      pending.cv.notify_all();
      continue;
    }
    pending.terminal = true;
    pending.status = grant->status;
    pending.total_order = grant->total_order;
    pending.log_idx = grant->log_idx;
    pending.broker_batch_seq = grant->broker_batch_seq;
    pending.cv.notify_all();
  }
}

void MailboxCorfuTokenProxy::DispatchLoop() {
  auto& up = segment_->up(static_cast<uint32_t>(expected_broker_id_));
  while (true) {
    std::pair<std::string, corfutokenproxy::TokenRequest> queued;
    std::shared_ptr<PendingGrant> pending;
    {
      std::unique_lock<std::mutex> lock(mu_);
      dispatch_cv_.wait(lock, [this] { return stop_.load(std::memory_order_acquire) || !requests_.empty(); });
      if (stop_.load(std::memory_order_acquire) && requests_.empty()) return;
      queued = std::move(requests_.front());
      requests_.pop_front();
      pending = pending_.at(queued.first);
    }
    Embarcadero::cxl_manager::CorfuTokenRequest wire{};
    wire.client_id = queued.second.client_id(); wire.batch_seq = queued.second.batchseq();
    wire.num_msg = queued.second.num_msg(); wire.total_size = queued.second.total_size();
    wire.session_id = pending->session_id;
    wire.correlation_id = pending->correlation_id; wire.broker_id = queued.second.broker_id();
    while (!stop_.load(std::memory_order_acquire) && !up.TryProduce(&wire, sizeof(wire))) {
      Embarcadero::CXL::cpu_pause();
    }
    if (stop_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(mu_);
    pending->request_sent = true;
  }
}

grpc::Status MailboxCorfuTokenProxy::GetTotalOrder(
    grpc::ServerContext* context, const corfutokenproxy::TokenRequest* request,
    corfutokenproxy::TokenResponse* response) {
  if (request->broker_id() != static_cast<uint32_t>(expected_broker_id_)) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "token request routed to non-owner Corfu ingress");
  }
  std::unique_lock<std::mutex> lock(mu_);
  const auto now = std::chrono::steady_clock::now();
  // Completed grants are retained for retry idempotence. Pending entries are
  // deliberately not expired: deleting one after it entered up(b) could permit
  // a later retry to issue a second global token. Bound admission instead: a
  // stalled sequencer cannot turn unbounded RPC retries into unbounded memory.
  for (auto it = pending_.begin(); it != pending_.end();) {
    if (it->second->terminal && it->second->status == 0 &&
        it->second->waiters == 0 && now - it->second->created > std::chrono::seconds(60)) {
      by_correlation_.erase(it->second->correlation_id);
      it = pending_.erase(it);
    } else {
      ++it;
    }
  }
  const std::string key = CacheKey(*request);
  auto [it, inserted] = pending_.try_emplace(key, nullptr);
  if (inserted) {
    if (pending_.size() > kMaxMailboxPending) {
      // First make room using the least-recently-used completed successful
      // grant. Never evict a pending/failed/poisoned entry: its token outcome
      // is uncertain and re-admission could duplicate global order.
      auto victim = pending_.end();
      for (auto candidate = pending_.begin(); candidate != pending_.end(); ++candidate) {
        const auto& value = candidate->second;
        if (candidate == it || !value->terminal || value->status != 0 || value->waiters != 0) continue;
        if (victim == pending_.end() || value->last_access < victim->second->last_access) victim = candidate;
      }
      if (victim == pending_.end()) {
        pending_.erase(it);
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "Corfu mailbox proxy pending-grant limit reached");
      }
      by_correlation_.erase(victim->second->correlation_id);
      pending_.erase(victim);
    }
    auto p = std::make_shared<PendingGrant>();
    p->num_msg = request->num_msg(); p->total_size = request->total_size();
    p->client_id = request->client_id(); p->batch_seq = request->batchseq();
    p->broker_id = request->broker_id(); p->session_id = session_id_;
    p->created = now; p->last_access = now;
    p->correlation_id = next_correlation_.fetch_add(1, std::memory_order_relaxed);
    it->second = p; by_correlation_.emplace(p->correlation_id, p);
    requests_.emplace_back(key, *request); dispatch_cv_.notify_one();
  }
  auto pending = it->second;
  if (!inserted && (pending->num_msg != request->num_msg() || pending->total_size != request->total_size())) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                        "conflicting retry for an already submitted token request");
  }
  pending->last_access = now;
  ++pending->waiters;
  const auto deadline = std::min(context->deadline(),
      std::chrono::system_clock::now() + std::chrono::seconds(30));
  while (!pending->terminal && !context->IsCancelled() && std::chrono::system_clock::now() < deadline) {
    pending->cv.wait_until(lock, deadline);
  }
  --pending->waiters;
  if (!pending->terminal) return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED,
                                                "Corfu mailbox token grant timed out after submission");
  if (pending->poisoned) {
    return grpc::Status(grpc::StatusCode::DATA_LOSS,
                        "Corfu mailbox grant context mismatch; retry is unsafe");
  }
  if (pending->status != 0) {
    const grpc::Status status = GrantStatus(pending->status);
    by_correlation_.erase(pending->correlation_id); pending_.erase(key);
    return status;
  }
  response->set_total_order(pending->total_order);
  response->set_log_idx(pending->log_idx);
  response->set_broker_batch_seq(pending->broker_batch_seq);
  return grpc::Status::OK;
}

CorfuTokenProxyServer::CorfuTokenProxyServer(std::string listen_endpoint,
    std::string global_sequencer_endpoint, int expected_broker_id,
    Embarcadero::cxl_transport::MailboxSegment* mailbox_segment)
    : listen_endpoint_(std::move(listen_endpoint)),
      global_sequencer_endpoint_(std::move(global_sequencer_endpoint)),
      expected_broker_id_(expected_broker_id), mailbox_segment_(mailbox_segment) {}

bool CorfuTokenProxyServer::Start(std::string* error) {
  if (mailbox_segment_) {
    service_ = std::make_unique<MailboxCorfuTokenProxy>(mailbox_segment_, expected_broker_id_);
  } else {
    service_ = std::make_unique<GrpcCorfuTokenProxy>(global_sequencer_endpoint_,
                                                      expected_broker_id_);
  }
  grpc::ServerBuilder builder;
  builder.AddListeningPort(listen_endpoint_, grpc::InsecureServerCredentials());
  builder.RegisterService(service_.get());
  server_ = builder.BuildAndStart();
  if (!server_) {
    if (error) *error = "could not listen on " + listen_endpoint_;
    service_.reset();
    return false;
  }
  return true;
}

void CorfuTokenProxyServer::Shutdown() {
  if (server_) server_->Shutdown();
  server_.reset();
  service_.reset();
}

}  // namespace Corfu
