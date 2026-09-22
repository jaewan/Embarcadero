#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace Embarcadero {

// No deletion: a full authoritative table rejects new identities. Recycling
// requires an independent, proven session-retention protocol.
template <typename Entry, typename Observe>
Entry* FindOrClaimSession(Entry* table, size_t capacity, uint64_t key,
                          size_t start, Observe&& observe, bool* claimed = nullptr) {
  if (claimed) *claimed = false;
  if (!table || !capacity || !key || start >= capacity) return nullptr;
  for (size_t probe = 0; probe < capacity; ++probe) {
    Entry* entry = &table[(start + probe) % capacity];
    observe(entry);
    auto seen = entry->session_key.load(std::memory_order_acquire);
    if (seen == key) return entry;
    if (seen == 0) {
      if (entry->session_key.compare_exchange_strong(
              seen, key, std::memory_order_acq_rel, std::memory_order_acquire)) {
        if (claimed) *claimed = true;
        return entry;
      }
      if (seen == key) return entry;
    }
  }
  return nullptr;
}

inline void StoreSessionMaximum(std::atomic<uint64_t>& field, uint64_t value) {
  auto current = field.load(std::memory_order_relaxed);
  while (current < value && !field.compare_exchange_weak(
      current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

inline uint64_t MergeSessionStateWord(uint64_t current, uint32_t epoch, bool fenced) {
  constexpr uint64_t active = 2, terminal_fence = 1;
  // A session key contains the full epoch. Existing fencing is irreversible.
  return (static_cast<uint64_t>(epoch) << 32) | active |
         (current & terminal_fence) | (fenced ? terminal_fence : 0);
}

// ACTIVE is published only after the prefix/key flush has completed. An
// inactive same-key match may be another claimant's not-yet-flushed CAS, so
// callers still help publish it. Keys are never recycled.
inline bool SessionClaimNeedsFlush(bool claimed, uint64_t state_word) {
  return claimed || (state_word & 2ULL) == 0;
}

// This only merges the atomic state. Callers must still finish their prefix
// publication and cache-visibility protocol, including when it returns false.
inline bool MergeSessionState(std::atomic<uint64_t>& state, uint32_t epoch, bool fenced) {
  auto current = state.load(std::memory_order_acquire);
  for (;;) {
    const auto next = MergeSessionStateWord(current, epoch, fenced);
    if (next == current) return false;
    if (state.compare_exchange_weak(current, next, std::memory_order_release,
                                    std::memory_order_acquire)) return true;
  }
}

// The supported runtime has one authoritative sequencer process per topic.
// Serialize its commit publication and fence publication, once per epoch/fence.
// This is not distributed locking or physical fencing of another host.
class SessionPublicationGate {
 public:
  std::unique_lock<std::mutex> Lock() { return std::unique_lock<std::mutex>(mutex_); }
 private:
  std::mutex mutex_;
};

}  // namespace Embarcadero
