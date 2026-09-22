#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace Embarcadero {

struct alignas(16) PBRProducerState {
  uint64_t next_slot_seq;
  uint64_t logical_offset;
};
static_assert(sizeof(PBRProducerState) == 16);

// A modulo consumer cannot advance a full lap without the producer observing it:
// at most slots-1 reservations are outstanding. Callers serialize observations
// (not producers), otherwise a delayed modulo sample can look like another lap.
inline uint64_t UnwrapPBRConsumed(uint64_t previous, uint64_t modulo,
                                  uint64_t slots) {
  if (slots < 2 || modulo >= slots) return previous;
  const uint64_t old_modulo = previous % slots;
  const uint64_t delta = modulo >= old_modulo ? modulo - old_modulo
                                             : slots - old_modulo + modulo;
  if (delta > std::numeric_limits<uint64_t>::max() - previous) return previous;
  return previous + delta;
}

template <typename Refresh>
bool TryReservePBR(std::atomic<PBRProducerState>& state,
                   std::atomic<uint64_t>& consumed, uint64_t slots,
                   uint32_t messages, uint64_t& slot, uint64_t& logical,
                   Refresh&& refresh, uint64_t* absolute = nullptr) {
  if (slots < 2 || messages == 0) return false;
  refresh();
  auto current = state.load(std::memory_order_acquire);
  for (;;) {
    auto retired = consumed.load(std::memory_order_acquire);
    // A producer snapshot may precede a concurrent producer and consumer.
    if (retired > current.next_slot_seq) {
      current = state.load(std::memory_order_acquire);
      continue;
    }
    if (current.next_slot_seq - retired >= slots - 1) {
      refresh();
      retired = consumed.load(std::memory_order_acquire);
      if (retired > current.next_slot_seq) {
        current = state.load(std::memory_order_acquire);
        continue;
      }
      if (current.next_slot_seq - retired >= slots - 1) return false;
    }
    if (current.next_slot_seq == std::numeric_limits<uint64_t>::max() ||
        messages > std::numeric_limits<uint64_t>::max() - current.logical_offset)
      return false;
    PBRProducerState next{current.next_slot_seq + 1,
                          current.logical_offset + messages};
    if (state.compare_exchange_weak(current, next, std::memory_order_acq_rel,
                                   std::memory_order_acquire)) {
      slot = current.next_slot_seq % slots;
      if (absolute) *absolute = current.next_slot_seq;
      logical = current.logical_offset;
      return true;
    }
  }
}

// External sequencers assign byte offsets from the configured first payload
// address, which can reserve more metadata than subsequent segment headers.
inline bool LocateAssignedPayload(uintptr_t segment, uintptr_t first_payload,
                                   size_t extent, size_t offset, size_t bytes,
                                   uintptr_t& result) {
  if (!segment || first_payload < segment || !bytes ||
      segment > std::numeric_limits<uintptr_t>::max() - extent) return false;
  const size_t header = first_payload - segment;
  if (header >= extent || offset > extent - header ||
      bytes > extent - header - offset) return false;
  result = first_payload + offset;
  return true;
}

// Shared by the real BLog receive reservation and its concurrency tests. Zero
// freezes the cursor during rollover. Segment addresses are never reused.
inline bool TryReserveSegmentBytes(std::atomic<unsigned long long>& cursor,
                                  uintptr_t segment, size_t extent, size_t bytes,
                                  uintptr_t& result) {
  if (segment == 0 || extent <= 64 || bytes == 0 || bytes > extent - 64 ||
      segment > std::numeric_limits<uintptr_t>::max() - extent) return false;
  auto current = cursor.load(std::memory_order_acquire);
  for (;;) {
    if (current < segment + 64 || current > segment + extent ||
        bytes > segment + extent - current) return false;
    if (cursor.compare_exchange_weak(current, current + bytes,
                                    std::memory_order_acq_rel,
                                    std::memory_order_acquire)) {
      result = current;
      return true;
    }
  }
}

// Caller holds its rollover mutex. Ordinary reservations never acquire it.
// The initializer may allocate/initialize a fresh segment but must never recycle
// a segment address. Zero prevents a stale CAS from crossing publication.
template <typename Initialize>
bool TryRolloverSegment(std::atomic<void*>& segment,
                        std::atomic<unsigned long long>& cursor,
                        size_t extent, size_t bytes, Initialize&& initialize) {
  void* old = segment.load(std::memory_order_acquire);
  const uintptr_t base = reinterpret_cast<uintptr_t>(old);
  if (!base || extent <= 64 || !bytes || bytes > extent - 64 ||
      base > std::numeric_limits<uintptr_t>::max() - extent) return false;
  auto current = cursor.load(std::memory_order_acquire);
  for (;;) {
    if (current < base + 64 || current > base + extent) return false;
    if (bytes <= base + extent - current) return true;
    if (cursor.compare_exchange_weak(current, 0, std::memory_order_acq_rel,
                                     std::memory_order_acquire)) break;
  }
  void* next;
  try {
    next = initialize(old, current);
  } catch (...) {
    cursor.store(current, std::memory_order_release);
    throw;
  }
  const uintptr_t next_base = reinterpret_cast<uintptr_t>(next);
  if (!next || next == old || next_base % extent != base % extent ||
      next_base > std::numeric_limits<uintptr_t>::max() - extent) {
    cursor.store(current, std::memory_order_release);
    return false;
  }
  segment.store(next, std::memory_order_release);
  cursor.store(next_base + 64, std::memory_order_release);
  return true;
}

// CommitEpoch is single-writer. Check both irreversible sequence reservations
// together before advancing either; failure leaves both frontiers unchanged.
inline bool TryReserveCommitRanges(std::atomic<uint64_t>& goi,
                                  std::atomic<size_t>& messages,
                                  uint64_t capacity, uint64_t batches,
                                  size_t count, uint64_t& first_goi,
                                  size_t& first_message) {
  const auto g = goi.load(std::memory_order_relaxed);
  const auto m = messages.load(std::memory_order_relaxed);
  if (g > capacity || batches > capacity - g ||
      count > std::numeric_limits<size_t>::max() - m) return false;
  first_goi = g;
  first_message = m;
  goi.store(g + batches, std::memory_order_relaxed);
  messages.store(m + count, std::memory_order_relaxed);
  return true;
}

}  // namespace Embarcadero
