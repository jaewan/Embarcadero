#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>

// Serializes Corfu token acquisition in client seal order.  The gate is a
// terminal state machine: once poisoned, no waiter, current holder, or payload
// admission may proceed.  It is intentionally independent of Publisher so its
// concurrency and shutdown behavior can be tested deterministically.
class CorfuOrderedTokenGate {
 public:
  struct AcquireResult {
    bool acquired{false};
    bool abort_transition{false};
    uint64_t wait_ns{0};
    size_t next_ticket{0};
    std::string abort_reason;
  };

  struct Snapshot {
    size_t next_ticket{0};
    bool turn_held{false};
    size_t held_ticket{0};
    bool aborted{false};
    uint64_t abort_transitions{0};
    std::string abort_reason;
  };

  AcquireResult Acquire(size_t ticket, const std::atomic<bool>& shutdown) {
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lock(mu_);
    bool abort_transition = false;
    while (true) {
      if (shutdown.load(std::memory_order_acquire)) {
        abort_transition |= AbortLocked("publisher shutdown while waiting for Corfu token turn");
      }
      if (aborted_) {
        return {false, abort_transition, ElapsedNs(start), next_ticket_, abort_reason_};
      }
      if (next_ticket_ > ticket) {
        abort_transition |= AbortLocked("Corfu seal ticket re-entered after completion");
        return {false, abort_transition, ElapsedNs(start), next_ticket_, abort_reason_};
      }
      if (next_ticket_ == ticket) {
        if (turn_held_) {
          abort_transition |= AbortLocked("duplicate Corfu seal ticket entered while turn held");
          return {false, abort_transition, ElapsedNs(start), next_ticket_, abort_reason_};
        }
        turn_held_ = true;
        held_ticket_ = ticket;
        return {true, abort_transition, ElapsedNs(start), next_ticket_, {}};
      }
      // Some shutdown paths cannot take this mutex merely to notify.  A bounded
      // wait makes shutdown observable even if its notification races this wait.
      cv_.wait_for(lock, std::chrono::milliseconds(50));
    }
  }

  // Completes a successful token grant.  A grant that returns after shutdown
  // or a concurrent abort is burned: the caller must not admit its payload.
  bool Complete(size_t ticket, const std::atomic<bool>& shutdown) {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutdown.load(std::memory_order_acquire)) {
      AbortLocked("publisher shutdown while completing Corfu token turn");
      cv_.notify_all();
      return false;
    }
    if (aborted_) return false;
    if (!turn_held_ || held_ticket_ != ticket || next_ticket_ != ticket) {
      AbortLocked("Corfu token turn completion mismatch");
      cv_.notify_all();
      return false;
    }
    turn_held_ = false;
    next_ticket_ = ticket + 1;
    cv_.notify_all();
    return true;
  }

  // Linearization point for starting a payload send.  An abort that happens
  // after this returns concerns an already-admitted send; once abort is visible,
  // no later payload admission succeeds.
  bool AdmitPayload(const std::atomic<bool>& shutdown) {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutdown.load(std::memory_order_acquire)) {
      AbortLocked("publisher shutdown before Corfu payload admission");
      cv_.notify_all();
    }
    return !aborted_;
  }

  bool Abort(const std::string& reason) {
    bool transitioned = false;
    {
      std::lock_guard<std::mutex> lock(mu_);
      transitioned = AbortLocked(reason);
    }
    cv_.notify_all();
    return transitioned;
  }

  Snapshot GetSnapshot() const {
    std::lock_guard<std::mutex> lock(mu_);
    return {next_ticket_, turn_held_, held_ticket_, aborted_, abort_transitions_, abort_reason_};
  }

  bool IsAborted() const {
    std::lock_guard<std::mutex> lock(mu_);
    return aborted_;
  }

 private:
  static uint64_t ElapsedNs(const std::chrono::steady_clock::time_point& start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count());
  }

  bool AbortLocked(const std::string& reason) {
    if (aborted_) return false;
    aborted_ = true;
    abort_reason_ = reason;
    ++abort_transitions_;
    return true;
  }

  mutable std::mutex mu_;
  std::condition_variable cv_;
  size_t next_ticket_{0};
  bool turn_held_{false};
  size_t held_ticket_{0};
  bool aborted_{false};
  uint64_t abort_transitions_{0};
  std::string abort_reason_;
};
