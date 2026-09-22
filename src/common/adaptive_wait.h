#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace Embarcadero {
// Cold event for predicates whose state is published before Notify(). There is
// no mutex acquisition when nobody waits. Sequentially consistent registration
// and generation operations close the check/register race: either the waiter
// observes the new generation, or the notifier observes its registration and
// takes the mutex before notifying. The predicate is always rechecked under the
// mutex; notification never substitutes for authoritative application state.
class AdaptiveWaitEvent {
 public:
  void Notify() {
    generation_.fetch_add(1, std::memory_order_seq_cst);
    if (waiters_.load(std::memory_order_seq_cst) == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    cv_.notify_all();
  }

  template <class Predicate>
  void WaitUntil(std::chrono::steady_clock::time_point deadline, Predicate ready) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto observed = generation_.load(std::memory_order_seq_cst);
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    struct Registration {
      std::atomic<unsigned>& count;
      ~Registration() { count.fetch_sub(1, std::memory_order_seq_cst); }
    } registration{waiters_};
    cv_.wait_until(lock, deadline, [&] {
      return ready() || generation_.load(std::memory_order_seq_cst) != observed;
    });
  }
 private:
  std::atomic<uint64_t> generation_{0};
  std::atomic<unsigned> waiters_{0};
  std::mutex mutex_;
  std::condition_variable cv_;
};
}  // namespace Embarcadero
