#include "embarlet/session_admission.h"
#include "embarlet/sequencer_utils.h"

#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { std::cerr << "failed: " #x << " line " << __LINE__ << '\n'; std::abort(); } } while (0)
using namespace Embarcadero;
struct Entry {
  std::atomic<uint64_t> session_key{0};
  std::atomic<uint64_t> state_word{0};
  std::atomic<uint64_t> expected_seq{0};
};

int main() {
  Entry table[4];
  auto observe = [](Entry*) {};
  // Force collisions and concurrent claims of the same four identities.
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) threads.emplace_back([&, t] {
    for (int i = 0; i < 1000; ++i) {
      const uint64_t key = 1 + (i + t) % 4;
      Entry* p = FindOrClaimSession(table, 4, key, 0, observe);
      REQUIRE(p && p->session_key.load() == key);
    }
  });
  for (auto& thread : threads) thread.join();
  REQUIRE(!FindOrClaimSession(table, 4, 5, 0, observe));
  REQUIRE(!FindOrClaimSession(table, 4, 0, 0, observe));
  for (uint64_t key = 1; key <= 4; ++key)
    REQUIRE(FindOrClaimSession(table, 4, key, 0, observe));

  Entry entry;
  SessionPublicationGate gate;
  entry.state_word.store(MergeSessionStateWord(0, 7, false));
  // Classification is ready, then fencing wins before commit. The same guard
  // used by CommitEpoch must reject even if no prior rejection ever occurred.
  std::promise<void> ready, fenced;
  auto ready_future = ready.get_future();
  auto fenced_future = fenced.get_future();
  bool committed = false;
  std::thread committer([&] {
    ready.set_value();
    fenced_future.wait();
    auto lock = gate.Lock();
    const auto state = entry.state_word.load();
    if (!ShouldRejectOrder5SpatialGuard(state & 2, state & 1, 0, entry.expected_seq.load())) {
      StoreSessionMaximum(entry.expected_seq, 1);
      committed = true;
    }
  });
  ready_future.wait();
  {
    auto lock = gate.Lock();
    entry.state_word.store(MergeSessionStateWord(entry.state_word.load(), 7, true));
  }
  fenced.set_value();
  committer.join();
  REQUIRE(!committed && entry.expected_seq.load() == 0);
  // A stale normal publication cannot reactivate this session.
  REQUIRE(MergeSessionStateWord(entry.state_word.load(), 7, false) & 1);

  // Reverse ordering: publication wins the gate. The fence must observe that
  // exact committed prefix, not a classifier's larger speculative cursor.
  Entry second;
  second.state_word.store(MergeSessionStateWord(0, 8, false));
  std::promise<void> commit_locked, fence_attempting;
  auto commit_locked_future = commit_locked.get_future();
  auto fence_attempting_future = fence_attempting.get_future();
  uint64_t fence_prefix = 0;
  std::thread commit_first([&] {
    auto lock = gate.Lock();
    commit_locked.set_value();
    fence_attempting_future.wait();
    StoreSessionMaximum(second.expected_seq, 3);
  });
  commit_locked_future.wait();
  std::thread fence_second([&] {
    fence_attempting.set_value();
    auto lock = gate.Lock();
    fence_prefix = second.expected_seq.load();
    second.state_word.store(MergeSessionStateWord(second.state_word.load(), 8, true));
  });
  commit_first.join();
  fence_second.join();
  REQUIRE(fence_prefix == 3);
  StoreSessionMaximum(second.expected_seq, 1);
  REQUIRE(second.expected_seq.load() == 3);
  REQUIRE(second.state_word.load() & 1);
  std::cout << "session admission and publication tests passed\n";
}
