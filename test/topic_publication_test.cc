// Bounded linked production regression: no brokers, TCP listeners, or shared-memory names.
#include "embarlet/topic.h"
#include "embarlet/topic_session_key.h"
#include "common/fault_injection.h"
#include "network_manager/session_snapshot.h"
#include <array>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#if EMBARCADERO_ENABLE_FAULT_INJECTION != 1
#error "This production-linked fixture requires fault injection in every linked Topic unit"
#endif
namespace Embarcadero {
class TopicPublicationTestAccess {
 public:
  using Shard = Topic::Level5ShardState;
  static std::vector<PendingBatch5> Classify(Topic& t, Shard& s, PendingBatch5 p) {
    std::vector<PendingBatch5> input{p}, ready;
    t.ProcessLevel5BatchesShard(s, input, ready);
    return ready;
  }
  static void Commit(Topic& t, std::vector<PendingBatch5>& ready) {
    std::vector<const PendingBatch5*> by_slot;
    std::array<size_t, NUM_MAX_BROKERS> consumed{};
    std::array<bool, NUM_MAX_BROKERS> seen{};
    std::array<uint64_t, NUM_MAX_BROKERS> cv{}, pbr{};
    std::vector<PendingBatch5> all = ready;
    t.CommitEpoch(ready, by_slot, consumed, seen, cv, pbr, all, false);
  }
  static void DrainCompleted(Topic& t) {
    // Run the real updater to completion on an already complete finite queue.
    t.committed_seq_updater_stop_.store(true);
    t.CommittedSeqUpdaterThread();
    t.committed_seq_updater_stop_.store(false);
  }
  static uint64_t Reserved(const Topic& t) { return t.global_batch_seq_.load(); }
  static bool Publish(Topic& t, uint32_t client, uint64_t expected, uint64_t hwm, bool fenced) {
    auto gate = t.session_publication_gate_.Lock();
    Topic::SessionPublishSnapshot snapshot{7, expected, hwm, hwm, fenced};
    return t.PublishSessionEntry(MakeSessionKey(client, 7), snapshot);
  }
};
}
namespace {
using namespace Embarcadero;
using namespace std::chrono_literals;
void Require(bool yes, const char* message) {
  if (!yes) throw std::runtime_error(message);
}
struct Control {
  int peer{-1};
  Control() {
    int fd[2]; Require(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, fd) == 0, "socketpair");
    peer = fd[0];
    setenv("EMBARCADERO_FAULT_CONTROL_FD", std::to_string(fd[1]).c_str(), 1);
    setenv("EMBARCADERO_FAULT_CONTROL_TOKEN", "topic-publication-test", 1);
    auto init = std::async(std::launch::async, [] { return fault::Pause("fixture.initialize"); });
    try {
      Require(Read() == "READY topic-publication-test", "controller ready");
      Send("START"); Require(Read() == "STARTED", "controller start");
      Require(init.get(), "controller initialized");
    } catch (...) { shutdown(peer, SHUT_RDWR); throw; }
  }
  ~Control() { if (peer >= 0) { shutdown(peer, SHUT_RDWR); close(peer); } }
  void Send(const std::string& text) {
    Require(send(peer, text.data(), text.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(text.size()), "controller send");
  }
  std::string Read() {
    pollfd p{peer, POLLIN, 0}; Require(poll(&p, 1, 3000) == 1, "controller event timeout");
    std::array<char, 2048> bytes{}; auto n = recv(peer, bytes.data(), bytes.size(), 0);
    Require(n > 0, "controller disconnected"); return {bytes.data(), static_cast<size_t>(n)};
  }
  void Arm(int id, const char* point, uint32_t client, const char* seq = "*") {
    Send("ARM " + std::to_string(id) + " " + point + " " + std::to_string(client) + " 7 " + seq + " 0");
    Require(Read() == "ARMED " + std::to_string(id), "arm response");
  }
  void Hit(int id, const char* point) {
    const auto prefix = "HIT " + std::to_string(id) + " " + point + " ";
    Require(Read().rfind(prefix, 0) == 0, "wrong fault event"); // Native TID may be appended.
  }
  void Release(int id) {
    Send("RELEASE " + std::to_string(id));
    Require(Read() == "RELEASED " + std::to_string(id), "release response");
  }
};
// Construct after each asynchronous operation: unwind releases pauses BEFORE
// the future destructor waits, preventing an assertion failure from hanging CTest.
struct CancelOnFailure {
  Control& control; int exceptions = std::uncaught_exceptions();
  ~CancelOnFailure() { if (std::uncaught_exceptions() > exceptions) shutdown(control.peer, SHUT_RDWR); }
};
struct Fixture {
  size_t bytes = BATCHHEADERS_SIZE + 65536;
  void* region = MAP_FAILED;
  std::unique_ptr<TInode> inode = std::make_unique<TInode>();
  std::unique_ptr<SessionEntry[]> sessions = std::make_unique<SessionEntry[]>(kMaxSessions);
  std::unique_ptr<Topic> topic;
  TopicPublicationTestAccess::Shard shard;
  uint32_t client;
  explicit Fixture(uint32_t id) : client(id) {
    Require(bytes < 64 * 1024 * 1024, "fixture ring exceeds 64MiB bound");
    region = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    Require(region != MAP_FAILED, "anonymous fixture mapping");
    auto* cb = new (region) ControlBlock{};
    cb->epoch.store(1); cb->committed_seq.store(UINT64_MAX);
    auto* cv = reinterpret_cast<CompletionVectorEntry*>(static_cast<uint8_t*>(region) + kCompletionVectorOffset);
    for (int i = 0; i < NUM_MAX_BROKERS; ++i) new (&cv[i]) CompletionVectorEntry{};
    for (int i = 0; i < 16; ++i) { new (&Goi()[i]) GOIEntry{}; Goi()[i].global_seq = UINT64_MAX; }
    inode->ack_level = 1; inode->replication_factor = 0;
    inode->offsets[0].log_offset = 32768;
    inode->offsets[0].batch_headers_offset = 65536;
    inode->offsets[0].batch_headers_consumed_through = BATCHHEADERS_SIZE;
    topic = std::make_unique<Topic>([]() -> void* { return nullptr; }, [] { return 1; },
        GetRegisteredBrokersCallback{}, inode.get(), nullptr, "linked-publication", 0, 5,
        heartbeat_system::SequencerType::EMBARCADERO, region, sessions.get(),
        static_cast<uint8_t*>(region) + 32768);
    Require(topic->TryAdmitSession(client, 7) == Topic::SessionAdmission::admitted, "initial OPEN admission");
  }
  ~Fixture() { topic.reset(); if (region != MAP_FAILED) munmap(region, bytes); }
  GOIEntry* Goi() { return reinterpret_cast<GOIEntry*>(static_cast<uint8_t*>(region) + kGOIOffset); }
  auto Snapshot() { return network::ReadSessionSnapshot(sessions.get(), Goi(), static_cast<ControlBlock*>(region), client, 7); }
  PendingBatch5 Batch(uint64_t seq) {
    PendingBatch5 p{}; p.client_id = client; p.session_epoch = 7; p.batch_seq = seq;
    p.num_msg = 1; p.cached_total_size = sizeof(BlogMessageHeader) + 8;
    p.cached_batch_id = static_cast<uint64_t>(client) * 100 + seq;
    p.cached_log_idx = 32768; p.cached_pbr_absolute_index = seq;
    p.slot_offset = (16 + seq) * sizeof(BatchHeader);
    p.hdr = new (static_cast<uint8_t*>(region) + 65536 + p.slot_offset) BatchHeader{};
    p.hdr->batch_id = p.cached_batch_id; p.hdr->client_id = client;
    p.hdr->num_msg = p.num_msg; p.hdr->total_size = p.cached_total_size;
    p.hdr->log_idx = p.cached_log_idx; p.hdr->batch_seq = seq;
    p.hdr->session_epoch32 = 7; p.hdr->session_epoch = 7;
    p.hdr->pbr_absolute_index = seq; p.hdr->publish_commit = seq;
    p.hdr->batch_complete = 1; p.hdr->flags = kBatchHeaderFlagClaimed | kBatchHeaderFlagValid;
    return p;
  }
  std::vector<PendingBatch5> Ready(uint64_t seq) {
    auto ready = TopicPublicationTestAccess::Classify(*topic, shard, Batch(seq));
    Require(ready.size() == 1 && !ready[0].skipped && !ready[0].is_held_marker, "real classifier ready");
    return ready;
  }
  void Seed() { auto ready = Ready(0); TopicPublicationTestAccess::Commit(*topic, ready); TopicPublicationTestAccess::DrainCompleted(*topic); }
  void Fence() {
    // A scanner's targeted timeout marker takes the production classifier's
    // record_fence path, including its real publication gate and notification.
    PendingBatch5 skip{}; skip.skipped = true; skip.client_id = client;
    skip.session_epoch = 7; skip.batch_seq = 2;
    (void)TopicPublicationTestAccess::Classify(*topic, shard, skip);
  }
};
void CommitWins(Control& control) {
  Fixture f(101); f.Seed(); auto ready = f.Ready(1);
  control.Arm(1, "session.after_expected_before_hwm", f.client, "1");
  auto commit = std::async(std::launch::async, [&] { TopicPublicationTestAccess::Commit(*f.topic, ready); });
  CancelOnFailure commit_cancel{control};
  control.Hit(1, "session.after_expected_before_hwm");
  Require(f.topic->TryAdmitSession(f.client, 7) == Topic::SessionAdmission::admitted, "OPEN during commit");
  auto middle = f.Snapshot();
  Require(middle.active && !middle.fenced && middle.expected_seq == 2 && middle.committed_hwm == 0,
          "must observe actual intermediate field publication");
  Require(middle.reconnect_committed_hwm == 0 && middle.goi_committed_hwm_found && middle.goi_committed_hwm == 0,
          "intermediate OPEN retains the prior contiguous prefix");
  control.Arm(2, "fence.before_publication_gate", f.client);
  auto fence = std::async(std::launch::async, [&] { f.Fence(); });
  CancelOnFailure fence_cancel{control};
  control.Hit(2, "fence.before_publication_gate"); control.Release(2);
  Require(fence.wait_for(30ms) == std::future_status::timeout, "fence must wait for commit gate");
  control.Release(1); commit.get(); fence.get();
  // GOI payload fields are non-atomic. The socket barrier controls the schedule
  // but is not a C++ happens-before edge; inspect this new entry only after join.
  Require(f.Goi()[1].global_seq == 1 && f.Goi()[1].client_seq == 1, "prefix has real GOI entry");
  TopicPublicationTestAccess::DrainCompleted(*f.topic);
  auto after = f.Snapshot();
  Require(after.goi_committed_hwm_found && after.goi_committed_hwm == 1 && after.reconnect_committed_hwm == 1, "production OPEN reader agrees with contiguous GOI updater");
  Require(after.fenced && after.expected_seq == 2 && after.committed_hwm == 1, "fence must retain committed winner");
  Topic::SessionFenceNotification note;
  Require(f.topic->TakeSessionFenceNotification(f.client, &note) && note.has_committed_prefix && note.committed_batch_seq == 1,
          "production fence notification reports exact committed prefix");
  Require(TopicPublicationTestAccess::Reserved(*f.topic) == 2, "exactly two GOI reservations");
}
void FenceWins(Control& control) {
  Fixture f(102); f.Seed(); auto ready = f.Ready(1);
  control.Arm(3, "session.after_expected_before_hwm", f.client, "0");
  auto fence = std::async(std::launch::async, [&] { f.Fence(); });
  CancelOnFailure fence_cancel{control}; control.Hit(3, "session.after_expected_before_hwm");
  control.Arm(4, "commit.before_publication_gate", f.client, "1");
  auto commit = std::async(std::launch::async, [&] { TopicPublicationTestAccess::Commit(*f.topic, ready); });
  CancelOnFailure commit_cancel{control};
  control.Hit(4, "commit.before_publication_gate"); control.Release(4);
  Require(commit.wait_for(30ms) == std::future_status::timeout, "commit must wait for fence gate");
  control.Release(3); fence.get(); commit.get();
  Require(ready.empty(), "actual CommitEpoch spatial guard must remove stale ready suffix");
  auto after = f.Snapshot();
  Require(after.fenced && after.expected_seq == 1 && after.committed_hwm == 0, "fence winner excludes uncommitted suffix");
  Require(f.topic->TryAdmitSession(f.client, 7) == Topic::SessionAdmission::fenced, "reopen rejected after fence");
  Require(TopicPublicationTestAccess::Reserved(*f.topic) == 1 && f.Goi()[1].global_seq == UINT64_MAX,
          "rejected suffix must reserve/write no GOI entry");
}
void RepeatedPublicationPreservesPrefixAndFence() {
  Fixture f(103);
  Require(TopicPublicationTestAccess::Publish(*f.topic, f.client, 2, 1, false), "first publication");
  Require(TopicPublicationTestAccess::Publish(*f.topic, f.client, 4, 3, false), "unchanged ACTIVE state publication");
  auto snapshot = f.Snapshot();
  Require(snapshot.active && !snapshot.fenced && snapshot.expected_seq == 4 &&
          snapshot.committed_hwm == 3 && snapshot.highest_sequenced == 3, "unchanged state still publishes new prefix");
  Require(TopicPublicationTestAccess::Publish(*f.topic, f.client, 4, 3, true), "fence publication");
  Require(TopicPublicationTestAccess::Publish(*f.topic, f.client, 1, 0, false), "stale normal publication");
  snapshot = f.Snapshot();
  Require(snapshot.fenced && snapshot.expected_seq == 4 && snapshot.committed_hwm == 3 &&
          snapshot.highest_sequenced == 3, "stale publication cannot lower prefix or clear fence");
  Require(f.topic->TryAdmitSession(f.client, 7) == Topic::SessionAdmission::fenced, "OPEN still rejects fenced identity");
}
}
void RunCxlManagerGeometryTest();
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--manager-geometry") {
      RunCxlManagerGeometryTest();
      return 0;
    }
    Require(argc == 1, "unknown fixture argument");
    Control control; CommitWins(control); FenceWins(control);
    RepeatedPublicationPreservesPrefixAndFence();
    std::cout << "PASS production Topic publication, OPEN snapshot, commit/fence winners\n";
    return 0;
  } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
