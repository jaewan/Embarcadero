#include "embarlet/bounded_reservation.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#define REQUIRE(x) do { if (!(x)) { std::cerr << "failed: " #x << " line " << __LINE__ << '\n'; std::abort(); } } while (0)

using namespace Embarcadero;

int main() {
  // Full, drain, wrap repeatedly through the same allocator used by Topic.
  std::atomic<PBRProducerState> state{{0, 0}};
  std::atomic<uint64_t> consumed{0};
  uint64_t modulo = 0;
  auto refresh = [&] { consumed.store(UnwrapPBRConsumed(consumed.load(), modulo, 4)); };
  uint64_t slot = 0, logical = 0, absolute = 0;
  for (uint64_t lap = 0; lap < 1000; ++lap) {
    const auto first = state.load().next_slot_seq;
    for (uint64_t i = 0; i < 3; ++i) {
      REQUIRE(TryReservePBR(state, consumed, 4, 2, slot, logical, refresh, &absolute));
      REQUIRE(slot == (first + i) % 4);
      REQUIRE(logical == (first + i) * 2);
      REQUIRE(absolute == first + i);
    }
    REQUIRE(!TryReservePBR(state, consumed, 4, 2, slot, logical, refresh));
    modulo = (first + 3) % 4;
    refresh();
    REQUIRE(consumed.load() == first + 3);
  }
  REQUIRE(!TryReservePBR(state, consumed, 1, 2, slot, logical, refresh));
  REQUIRE(!TryReservePBR(state, consumed, 4, 0, slot, logical, refresh));

  std::mutex fallback_mutex;
  std::atomic<PBRProducerState> fallback{{0, 0}};
  std::atomic<uint64_t> fallback_consumed{0};
  uint64_t fallback_modulo = 0;
  for (uint64_t n = 0; n < 1000; ++n) {
    std::lock_guard<std::mutex> lock(fallback_mutex);
    REQUIRE(TryReservePBR(fallback, fallback_consumed, 4, 3, slot, logical,
        [&] { fallback_consumed.store(UnwrapPBRConsumed(fallback_consumed.load(), fallback_modulo, 4)); }, &absolute));
    REQUIRE(absolute == n && slot == n % 4 && logical == n * 3);
    fallback_modulo = (n + 1) % 4;
  }

  // Concurrent producers and a real wrapping consumer. Publication is delayed
  // independently of reservation; a consumer never retires an unpublished slot.
  constexpr uint64_t slots = 8, count = 40000;
  std::atomic<PBRProducerState> concurrent{{0, 0}};
  std::atomic<uint64_t> retired{0}, consumer_modulo{0};
  std::atomic_flag observer = ATOMIC_FLAG_INIT;
  std::atomic<uint64_t> published[slots];
  for (auto& p : published) p.store(std::numeric_limits<uint64_t>::max());
  auto observe = [&] {
    if (observer.test_and_set(std::memory_order_acquire)) return;
    retired.store(UnwrapPBRConsumed(retired.load(), consumer_modulo.load(std::memory_order_acquire), slots), std::memory_order_release);
    observer.clear(std::memory_order_release);
  };
  std::thread consumer([&] {
    for (uint64_t n = 0; n < count; ++n) {
      while (published[n % slots].load(std::memory_order_acquire) != n) std::this_thread::yield();
      consumer_modulo.store((n + 1) % slots, std::memory_order_release);
    }
  });
  std::vector<std::thread> producers;
  for (int t = 0; t < 4; ++t) producers.emplace_back([&] {
    for (uint64_t i = 0; i < count / 4; ++i) {
      uint64_t s, l, a;
      while (!TryReservePBR(concurrent, retired, slots, 1, s, l, observe, &a)) std::this_thread::yield();
      REQUIRE(s == l % slots);
      REQUIRE(a == l);
      if (l % 7 == 0) std::this_thread::yield();
      published[s].store(l, std::memory_order_release);
    }
  });
  for (auto& t : producers) t.join();
  consumer.join();
  observe();
  REQUIRE(retired.load() == count);

  // Checked BLog reservation: exact fit, overflow, frozen rollover cursor,
  // and a stale segment snapshot paired with a new segment's cursor.
  constexpr uintptr_t base = 0x100000;
  constexpr size_t extent = 4096;
  std::atomic<unsigned long long> cursor{base + 64};
  uintptr_t address;
  REQUIRE(!TryReserveSegmentBytes(cursor, base, extent, 0, address));
  REQUIRE(!TryReserveSegmentBytes(cursor, base, extent, SIZE_MAX, address));
  REQUIRE(TryReserveSegmentBytes(cursor, base, extent, extent - 64, address));
  REQUIRE(address == base + 64);
  REQUIRE(!TryReserveSegmentBytes(cursor, base, extent, 1, address));
  cursor.store(0);
  REQUIRE(!TryReserveSegmentBytes(cursor, base, extent, 1, address));
  cursor.store(base + extent + 64);
  REQUIRE(!TryReserveSegmentBytes(cursor, base, extent, 1, address));
  REQUIRE(TryReserveSegmentBytes(cursor, base + extent, extent, 1, address));

  // Corfu offsets start after the initial 4KiB metadata reservation, not
  // the 64-byte rollover header. Exact final fit is allowed; overflow is not.
  REQUIRE(LocateAssignedPayload(base, base + 4096, 8192, 128, 64, address));
  REQUIRE(address == base + 4096 + 128);
  REQUIRE(LocateAssignedPayload(base, base + 4096, 8192, 4096 - 64, 64, address));
  REQUIRE(!LocateAssignedPayload(base, base + 4096, 8192, 4096 - 64, 65, address));
  REQUIRE(!LocateAssignedPayload(base, base + 4096, 8192, SIZE_MAX, 64, address));

  // Many concurrent reservations have neither overlap nor skipped capacity.
  constexpr size_t arena = 64 * 1024;
  cursor.store(base + 64);
  std::vector<uintptr_t> addresses;
  std::mutex results;
  producers.clear();
  for (int t = 0; t < 8; ++t) producers.emplace_back([&] {
    uintptr_t p;
    while (TryReserveSegmentBytes(cursor, base, arena, 64, p)) {
      std::lock_guard<std::mutex> lock(results);
      addresses.push_back(p);
    }
  });
  for (auto& t : producers) t.join();
  std::sort(addresses.begin(), addresses.end());
  REQUIRE(addresses.size() == (arena - 64) / 64);
  for (size_t i = 0; i < addresses.size(); ++i) REQUIRE(addresses[i] == base + 64 + i * 64);

  // Exercise the exact rollover transition used by Topic, concurrently with
  // reservation and delayed writes to retired segments. No address is reused.
  constexpr size_t segment_bytes = 512, segments = 128;
  std::vector<unsigned char> memory(segment_bytes * segments, 0);
  const auto origin = reinterpret_cast<uintptr_t>(memory.data());
  std::atomic<void*> live_segment{memory.data()};
  cursor.store(origin + 64);
  size_t allocated_segments = 1;
  std::mutex rollover;
  addresses.clear();
  producers.clear();
  for (int t = 0; t < 8; ++t) producers.emplace_back([&] {
    for (;;) {
      uintptr_t p;
      const auto observed = reinterpret_cast<uintptr_t>(live_segment.load(std::memory_order_acquire));
      if (TryReserveSegmentBytes(cursor, observed, segment_bytes, 64, p)) {
        std::this_thread::yield();  // Other threads can roll before this write.
        std::fill_n(reinterpret_cast<unsigned char*>(p), 64, 0x5a);
        std::lock_guard<std::mutex> lock(results);
        addresses.push_back(p);
        continue;
      }
      std::lock_guard<std::mutex> lock(rollover);
      if (!TryRolloverSegment(live_segment, cursor, segment_bytes, 64,
          [&](void*, uintptr_t) -> void* {
            if (allocated_segments == segments) return nullptr;
            return memory.data() + segment_bytes * allocated_segments++;
          })) break;
    }
  });
  for (auto& t : producers) t.join();
  std::sort(addresses.begin(), addresses.end());
  REQUIRE(addresses.size() == segments * (segment_bytes - 64) / 64);
  REQUIRE(allocated_segments == segments);
  REQUIRE(cursor.load() == origin + segments * segment_bytes);
  for (size_t i = 0; i < addresses.size(); ++i) {
    if (i) REQUIRE(addresses[i] >= addresses[i - 1] + 64);
    REQUIRE((addresses[i] - origin) % segment_bytes >= 64);
    for (size_t j = 0; j < 64; ++j)
      REQUIRE(reinterpret_cast<unsigned char*>(addresses[i])[j] == 0x5a);
  }

  // GOI exhaustion must not burn either message or GOI sequence space.
  std::atomic<uint64_t> goi{0};
  std::atomic<size_t> messages{0};
  uint64_t first_goi;
  size_t first_message;
  REQUIRE(TryReserveCommitRanges(goi, messages, 4, 3, 7, first_goi, first_message));
  REQUIRE(first_goi == 0 && first_message == 0);
  REQUIRE(!TryReserveCommitRanges(goi, messages, 4, 2, 9, first_goi, first_message));
  REQUIRE(goi.load() == 3 && messages.load() == 7);
  REQUIRE(TryReserveCommitRanges(goi, messages, 4, 1, 2, first_goi, first_message));
  REQUIRE(first_goi == 3 && first_message == 7);
  REQUIRE(!TryReserveCommitRanges(goi, messages, 4, 1, 1, first_goi, first_message));
  REQUIRE(goi.load() == 4 && messages.load() == 9);
  goi.store(0);
  messages.store(SIZE_MAX);
  REQUIRE(!TryReserveCommitRanges(goi, messages, 4, 1, 1, first_goi, first_message));
  REQUIRE(goi.load() == 0 && messages.load() == SIZE_MAX);
  std::cout << "bounded reservation tests passed\n";
}
