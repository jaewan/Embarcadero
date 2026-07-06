#pragma once

// CXL mailbox ring — the single-writer, single-reader record ring that all three
// baseline control paths (Corfu token, Scalog cut/report, LazyLog coordinator) are
// ported onto. See docs/baselines/porting_rule.md: this is *transport only*. The ring
// carries whatever POD records a baseline's protocol already exchanges; it never
// interprets them.
//
// Correctness discipline (matches the Blog/PBR path in the rest of the stack — plain
// POD fields published with clwb+sfence, NOT std::atomic, because std::atomic's
// acquire/release does not cross a non-coherent CXL link):
//   * Single writer per ring (single-writer ownership).
//   * Monotonic cursors (write_index / read_index never move backward).
//   * Producer flushes the record body, fences, then stamps a per-slot sequence and
//     flushes that — the sequence stamp is the single publish point. A consumer that
//     observes slot.seq == expected is guaranteed the body is already resident in CXL.
//   * Reader forces a fresh fetch from CXL (invalidate + fence) before reading a value
//     another host may have written.
//   * Explicit back-pressure: a full ring blocks the producer, exactly as a full gRPC
//     flow-control window did before the port (porting_rule.md §6).
//
// Cross-CXL fields (the ones one host writes and another reads) are declared `volatile`
// so the compiler cannot reorder a plain store past the clwb/sfence that publishes it —
// clflushopt/sfence move bytes CPU->CXL but do not, by themselves, constrain compile-time
// store ordering under -O3/LTO. Volatile forces the store/load to happen exactly where
// written, which together with the fences preserves the seq-last publish point.
//
// The region is pure POD with no embedded pointers (only offsets/indices), so the same
// bytes are valid whether mapped at different virtual addresses in the broker process
// and the global-sequencer process, or on different hosts over CXL.
//
// SINGLE-WRITER OWNERSHIP CONTRACT (read before porting Corfu):
//   Exactly ONE thread may call TryProduce()/Produce()/TryProduceBounded() on a given
//   MailboxRing at any time. Concurrent producers on the same ring violate SPSC and WILL
//   corrupt the ring (lost/torn records). Baselines must enforce this at the application
//   layer via topology or pinning:
//     * Corfu: token requests from a single broker must be posted by exactly one thread
//       (per-thread-ring topology, or broker->thread pinning). Corfu's concurrent token
//       requests do NOT get one ring — they get one *writer thread* per ring.
//     * Scalog/LazyLog: each local sequencer posts to its own up(b) ring; keep the local
//       sequencer single-threaded per broker.
//   In Debug builds (#ifndef NDEBUG) TryProduce detects a second concurrent writer and
//   LOG(FATAL)s; in Release the detector compiles away entirely.

#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef NDEBUG
#include <thread>
#endif

#include "common/performance_utils.h"

namespace Embarcadero {
namespace cxl_transport {

inline constexpr uint32_t kMailboxRingMagic = 0x584D4245;  // little-endian ASCII "EBMX"
inline constexpr uint32_t kMailboxRingVersion = 1;
inline constexpr size_t kCacheLine = 64;

inline constexpr size_t AlignUp(size_t n, size_t a) { return (n + a - 1) & ~(a - 1); }

// Per-ring delivery outcome for the bounded / non-blocking producer path (P1-3). Plain
// POD status, host-local only — never published cross-CXL, never uses acquire/release.
enum class PerRingStatus : uint8_t {
	SUCCESS = 0,  // record enqueued
	FULL = 1,     // ring full (or len > record_size); nothing written
	WEDGED = 2,   // spin bound exceeded while full; nothing written
};

// Fixed header at the base of a ring region. Cursors live on their own cache lines so
// the producer's write_index and the consumer's read_index never share a line (false
// sharing across the CXL link would be a correctness hazard, not just a perf one).
struct alignas(kCacheLine) MailboxRingHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t record_size;   // max payload bytes per slot
	uint32_t capacity;      // number of slots (must be a power of two)
	uint32_t slot_stride;   // bytes per slot (payload + framing, cache-line aligned)
	uint32_t reserved0;
	uint64_t slots_offset;  // byte offset from header base to slot[0]
	uint8_t  pad_[kCacheLine - 32];

	// Producer-owned. Host-local progress hint only — advanced by the producer but NOT
	// published cross-CXL (P1-5b: the consumer gates on the per-slot seq, so flushing this
	// every message was a wasted flush+fence pair). It is never read by the consumer.
	alignas(kCacheLine) uint64_t write_index;
	uint8_t  pad_w_[kCacheLine - sizeof(uint64_t)];

	// Consumer-owned, published cross-CXL (the producer reads it fresh to detect a full
	// ring). Volatile so the read_index store is not reordered past its flush.
	alignas(kCacheLine) volatile uint64_t read_index;
	uint8_t  pad_r_[kCacheLine - sizeof(uint64_t)];
};
static_assert(sizeof(MailboxRingHeader) == 3 * kCacheLine, "ring header must be 3 cache lines");

// Per-slot framing. `seq` is written and flushed last; it is the publish point.
// seq == index+1 for the slot's current generation (nonzero, distinguishes wraps).
// seq and len are cross-CXL published fields, hence volatile. When the header + payload
// fit in a single cache line (record_size <= kCacheLine - sizeof(header)), the payload is
// folded directly behind this header so producer/consumer touch ONE line per message
// (P1-5d). Larger records spill onto subsequent lines.
struct alignas(kCacheLine) MailboxSlotHeader {
	volatile uint64_t seq;
	volatile uint32_t len;
	uint32_t reserved;
	// payload bytes follow immediately (record_size bytes), then padding to slot_stride
};

// Non-owning view over a ring laid out in a caller-provided region (CXL shm, POSIX
// shm, or an anonymous shared mmap for tests). Construct with Create() once by whoever
// owns the segment, then Attach() from every participant.
//
// Base-address alignment: the region base MUST be 64-byte (cache-line) aligned. Both
// Create() and Attach() CHECK this. A misaligned base would push every alignas(64) field
// off its cache line, so a clflush on seq could touch two lines and a reader could observe
// a torn publish point — that breaks the single-publish invariant.
class MailboxRing {
 public:
	MailboxRing() = default;

	// MailboxRing is a non-owning view (base_/h_ are borrowed) and is moved into the
	// segment's ring vectors by value. In Debug builds the concurrent-writer detector adds
	// std::atomic members, which would delete the implicit move ops, so we define moves
	// explicitly. The detector state is transient per-instance bookkeeping (no in-flight
	// producer can be mid-call during a move), so it is simply reset on the moved-to object.
	MailboxRing(MailboxRing&& o) noexcept { MoveFrom(o); }
	MailboxRing& operator=(MailboxRing&& o) noexcept {
		if (this != &o) MoveFrom(o);
		return *this;
	}
	MailboxRing(const MailboxRing&) = delete;
	MailboxRing& operator=(const MailboxRing&) = delete;

	static size_t BytesNeeded(uint32_t record_size, uint32_t capacity) {
		const uint32_t stride = SlotStride(record_size);
		return sizeof(MailboxRingHeader) + static_cast<size_t>(stride) * capacity;
	}

	// Initialize a fresh ring in `base` (must be >= BytesNeeded). Zeroes the region and
	// writes the header. Call exactly once, before any Attach() elsewhere.
	static MailboxRing Create(void* base, size_t region_bytes, uint32_t record_size, uint32_t capacity) {
		CHECK(base != nullptr);
		// Enforce 64-byte base alignment (cache-line boundary). See class comment.
		CHECK_EQ(reinterpret_cast<uintptr_t>(base) % kCacheLine, 0UL)
		    << "MailboxRing base must be 64-byte aligned; got " << base;
		CHECK((capacity & (capacity - 1)) == 0) << "capacity must be a power of two";
		CHECK_GE(region_bytes, BytesNeeded(record_size, capacity));
		std::memset(base, 0, BytesNeeded(record_size, capacity));
		auto* h = reinterpret_cast<MailboxRingHeader*>(base);
		h->magic = kMailboxRingMagic;
		h->version = kMailboxRingVersion;
		h->record_size = record_size;
		h->capacity = capacity;
		h->slot_stride = SlotStride(record_size);
		h->slots_offset = sizeof(MailboxRingHeader);
		h->write_index = 0;                 // host-local hint, not published
		h->read_index = 0;                  // volatile store
		// Publish the whole header region to CXL.
		for (size_t off = 0; off < sizeof(MailboxRingHeader); off += kCacheLine) {
			CXL::flush_cacheline(reinterpret_cast<uint8_t*>(base) + off);
		}
		CXL::store_fence();
		return Attach(base, region_bytes);
	}

	// Attach to a ring another participant already Create()d.
	static MailboxRing Attach(void* base, size_t region_bytes) {
		CHECK(base != nullptr);
		// Attach must also reject a misaligned base: a broker may map the region at a
		// different virtual address than the creator, and the alignment contract must hold
		// there too.
		CHECK_EQ(reinterpret_cast<uintptr_t>(base) % kCacheLine, 0UL)
		    << "MailboxRing base must be 64-byte aligned; got " << base;
		auto* h = reinterpret_cast<MailboxRingHeader*>(base);
		CXL::invalidate_cacheline_for_read(h);
		CXL::full_fence();
		CHECK_EQ(h->magic, kMailboxRingMagic) << "not a mailbox ring / uninitialized region";
		CHECK_EQ(h->version, kMailboxRingVersion) << "mailbox ring version mismatch";
		CHECK_GE(region_bytes, BytesNeeded(h->record_size, h->capacity));
		MailboxRing r;
		r.base_ = reinterpret_cast<uint8_t*>(base);
		r.h_ = h;
		// Seed the producer's shadow of read_index from the current published value so a
		// fresh Attach (broker re-attaching to a partially-drained ring) does not falsely
		// see the ring as full on its first produce. Consumer-only attachers ignore this.
		r.cached_read_index_ = ReadFreshCursor(&h->read_index);
		return r;
	}

	bool valid() const { return h_ != nullptr; }
	uint32_t record_size() const { return h_->record_size; }
	uint32_t capacity() const { return h_->capacity; }

	// ---- Producer side (single writer) ----

	// Try to enqueue one record. Returns false if the ring is full or len exceeds
	// record_size. Non-blocking.
	//
	// SINGLE-WRITER ONLY: must be called by exactly one thread per ring (see class
	// comment). Concurrent callers corrupt the ring.
	bool TryProduce(const void* record, uint32_t len) {
#ifndef NDEBUG
		DebugEnterWriter();
#endif
		if (len > h_->record_size) {
#ifndef NDEBUG
			DebugExitWriter();
#endif
			return false;
		}
		const uint64_t w = h_->write_index;  // producer owns it; local read is fine
		// Hot path: check against the producer-owned shadow of read_index (no fence). Only
		// on an apparent-full do we pay the mfence for a fresh cross-CXL read (P1-5a).
		uint64_t r = cached_read_index_;
		if (w - r >= h_->capacity) {
			r = ReadFreshCursor(&h_->read_index);
			cached_read_index_ = r;
			if (w - r >= h_->capacity) {  // truly full — caller applies back-pressure
#ifndef NDEBUG
				DebugExitWriter();
#endif
				return false;
			}
		}
		PublishSlot(w, record, len);
		h_->write_index = w + 1;  // host-local hint only; NOT flushed cross-CXL
#ifndef NDEBUG
		DebugExitWriter();
#endif
		return true;
	}

	// Enqueue one record, spinning with a pause hint until space frees (faithful
	// realization of the gRPC flow-control window; see porting_rule.md §6).
	// Returns false only if len exceeds record_size.
	//
	// SINGLE-WRITER ONLY: same contract as TryProduce.
	bool Produce(const void* record, uint32_t len) {
		if (len > h_->record_size) return false;
		while (!TryProduce(record, len)) CXL::cpu_pause();
		return true;
	}

	// Non-blocking produce with a bounded spin under back-pressure (P1-3). Returns:
	//   SUCCESS — record enqueued (identical cross-CXL publish to TryProduce/Produce);
	//   FULL    — len > record_size (invalid); nothing written;
	//   WEDGED  — ring stayed full for max_spins iterations; nothing written.
	// Never partially writes a slot and never advances write_index on FULL/WEDGED, so the
	// SPSC invariant holds. Used by BroadcastDownNonBlocking so one slow/full broker ring
	// cannot indefinitely block delivery to the others.
	//
	// SINGLE-WRITER ONLY: same contract as TryProduce.
	PerRingStatus TryProduceBounded(const void* record, uint32_t len, uint64_t max_spins) {
		if (len > h_->record_size) return PerRingStatus::FULL;
#ifndef NDEBUG
		DebugEnterWriter();
#endif
		const uint64_t w = h_->write_index;  // producer owns it
		for (uint64_t spin = 0;; ++spin) {
			uint64_t r = cached_read_index_;
			if (w - r >= h_->capacity) {
				r = ReadFreshCursor(&h_->read_index);
				cached_read_index_ = r;
			}
			if (w - r < h_->capacity) break;   // space available
			if (spin >= max_spins) {
#ifndef NDEBUG
				DebugExitWriter();
#endif
				return PerRingStatus::WEDGED;
			}
			CXL::cpu_pause();
		}
		PublishSlot(w, record, len);
		h_->write_index = w + 1;  // host-local hint only; NOT flushed cross-CXL
#ifndef NDEBUG
		DebugExitWriter();
#endif
		return PerRingStatus::SUCCESS;
	}

	// Producer-side check: is there room for at least one more record? Uses the private
	// cached read_index shadow (cheap; issues a fresh cross-CXL read only when the cache says
	// full), exactly like TryProduce's own space check. SINGLE-WRITER ONLY.
	//
	// Guarantee for a sole, single-threaded writer: if HasSpace() returns true, the very next
	// TryProduce on this ring is guaranteed to succeed, because free space can only GROW (the
	// consumer never produces). Callers that must not consume/commit work unless the result can
	// be delivered (e.g. a sequencer that irreversibly assigns a token before granting) should
	// gate on HasSpace() first.
	bool HasSpace() {
		const uint64_t w = h_->write_index;
		uint64_t r = cached_read_index_;
		if (w - r >= h_->capacity) {
			r = ReadFreshCursor(&h_->read_index);
			cached_read_index_ = r;
		}
		return (w - r) < h_->capacity;
	}

	// ---- Consumer side (single reader) ----

	// Try to dequeue one record into out (capacity out_cap). On success sets *out_len
	// and returns true. Non-blocking; false if empty.
	//
	// The output buffer MUST be large enough for the record: TryConsume CHECK-fails if
	// out_cap < len rather than silently truncating (P2-1). Size out_cap >= record_size.
	bool TryConsume(void* out, uint32_t out_cap, uint32_t* out_len) {
		const uint64_t r = h_->read_index;  // consumer owns it (local read is fine)
		MailboxSlotHeader* slot = SlotAt(r);
		const uint64_t expected = r + 1;
		// Single fresh fetch of the slot-header line — brings in BOTH seq and len, which
		// share this cache line (P1-5c: no second invalidate/fetch of the same line).
		const uint64_t seq = ReadFreshCursor(&slot->seq);
		if (seq != expected) return false;  // not yet published (or empty)
		// Body is guaranteed resident: the producer flushed+fenced it before stamping seq.
		// len is already fresh (same line just fetched by ReadFreshCursor).
		const uint32_t len = slot->len;
		CHECK_LE(len, out_cap)
		    << "TryConsume output buffer too small: record is " << len
		    << " bytes, buffer capacity is " << out_cap;
		if (out_len) *out_len = len;
		if (len) {
			uint8_t* payload = reinterpret_cast<uint8_t*>(slot) + sizeof(MailboxSlotHeader);
			// If the payload fits in the slot-header line it was already fetched above.
			// Only when it spills past that line do we invalidate+fetch the overflow.
			const uint8_t* line_end = reinterpret_cast<const uint8_t*>(
			    (reinterpret_cast<uintptr_t>(slot) & ~(kCacheLine - 1)) + kCacheLine);
			const uint8_t* payload_end = payload + len;
			if (payload_end > line_end) {
				InvalidateRange(line_end, static_cast<size_t>(payload_end - line_end));
				// full_fence (mfence), NOT load_fence (lfence): clflush is weakly ordered and
				// lfence does not order it (see performance_utils.h full_fence docs), so only
				// mfence guarantees the invalidation completes before the memcpy reads the
				// overflow payload — otherwise the reader could copy a stale cached line even
				// after observing the publish point.
				CXL::full_fence();
			}
			std::memcpy(out, payload, len);  // len <= out_cap, checked above
		}
		h_->read_index = r + 1;  // volatile store
		CXL::flush_cacheline(CXL::ToFlushable(&h_->read_index));
		CXL::store_fence();
		return true;
	}

	// Approximate number of records available to the consumer (may be stale). Uses the
	// producer-owned host-local write_index (no longer published cross-CXL, P1-5b) minus a
	// fresh read of read_index. Meaningful only when called by the producer; primarily a
	// test/debug aid.
	uint64_t SizeApprox() const {
		const uint64_t w = h_->write_index;  // producer-local hint
		const uint64_t r = ReadFreshCursor(&h_->read_index);
		return w - r;
	}

 private:
	// Move borrowed view + producer shadow; reset the (transient) debug detector state.
	void MoveFrom(MailboxRing& o) {
		base_ = o.base_;
		h_ = o.h_;
		cached_read_index_ = o.cached_read_index_;
		o.base_ = nullptr;
		o.h_ = nullptr;
#ifndef NDEBUG
		in_progress_.store(false, std::memory_order_relaxed);
		owner_thread_id_.store(0, std::memory_order_relaxed);
#endif
	}

	static uint32_t SlotStride(uint32_t record_size) {
		// Fold header + payload into a single cache line when they fit (P1-5d): halves the
		// flush/invalidate traffic for small control records. Otherwise align to 64 as before.
		if (sizeof(MailboxSlotHeader) + record_size <= kCacheLine) return kCacheLine;
		return static_cast<uint32_t>(AlignUp(sizeof(MailboxSlotHeader) + record_size, kCacheLine));
	}

	MailboxSlotHeader* SlotAt(uint64_t index) const {
		const uint32_t slot = static_cast<uint32_t>(index & (h_->capacity - 1));
		return reinterpret_cast<MailboxSlotHeader*>(base_ + h_->slots_offset +
		                                            static_cast<size_t>(slot) * h_->slot_stride);
	}

	// Write one slot's body and publish the seq stamp. Shared by TryProduce and
	// TryProduceBounded so the cross-CXL publish sequence is identical in both paths:
	// body written -> flushed -> store_fence -> seq stamped -> flushed -> store_fence.
	// Caller must already have confirmed space and hold single-writer ownership.
	void PublishSlot(uint64_t w, const void* record, uint32_t len) {
		MailboxSlotHeader* slot = SlotAt(w);
		uint8_t* payload = reinterpret_cast<uint8_t*>(slot) + sizeof(MailboxSlotHeader);
		slot->len = len;       // volatile store
		slot->reserved = 0;
		if (len) std::memcpy(payload, record, len);
		// Flush body (slot header + payload) before publishing the sequence stamp.
		FlushRange(slot, sizeof(MailboxSlotHeader) + len);
		CXL::store_fence();
		slot->seq = w + 1;     // publish point (volatile store)
		CXL::flush_cacheline(slot);
		CXL::store_fence();
	}

	// Force a fresh read of a single cache-line-resident value another host may have
	// written. clflush evicts the local line; the fence orders the eviction before the
	// load so the load fetches from CXL. Accepts volatile-qualified cursors (seq,
	// read_index) directly.
	static uint64_t ReadFreshCursor(const volatile uint64_t* p) {
		CXL::invalidate_cacheline_for_read(const_cast<const uint64_t*>(p));
		CXL::full_fence();
		return *p;
	}

	static void FlushRange(const void* addr, size_t bytes) {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
		const uint8_t* end = p + bytes;
		p = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(p) & ~(kCacheLine - 1));
		for (; p < end; p += kCacheLine) CXL::flush_cacheline(p);
	}

	static void InvalidateRange(const void* addr, size_t bytes) {
		const uint8_t* p = reinterpret_cast<const uint8_t*>(addr);
		const uint8_t* end = p + bytes;
		p = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(p) & ~(kCacheLine - 1));
		for (; p < end; p += kCacheLine) CXL::invalidate_cacheline_for_read(p);
	}

#ifndef NDEBUG
	// Debug-only concurrent-writer detector. Host-local (never in CXL, never flushed); the
	// relaxed atomics only coordinate host threads and compile away entirely in Release.
	void DebugEnterWriter() {
		const uint64_t self = std::hash<std::thread::id>{}(std::this_thread::get_id());
		const bool busy = in_progress_.load(std::memory_order_relaxed);
		const uint64_t owner = owner_thread_id_.load(std::memory_order_relaxed);
		if (busy && owner != self) {
			LOG(FATAL) << "SPSC VIOLATION: concurrent producers on one MailboxRing: owner="
			           << owner << " current=" << self;
		}
		owner_thread_id_.store(self, std::memory_order_relaxed);
		in_progress_.store(true, std::memory_order_relaxed);
	}
	void DebugExitWriter() { in_progress_.store(false, std::memory_order_relaxed); }
#endif

	uint8_t* base_ = nullptr;
	MailboxRingHeader* h_ = nullptr;
	// Producer-owned shadow of read_index (P1-5a). Host-local ONLY: never flushed to CXL,
	// never read/written by the consumer side. Monotonically catches up to the true
	// read_index on back-pressure, so a stale value can only cause a (harmless) false-full,
	// never a false-empty.
	uint64_t cached_read_index_ = 0;
#ifndef NDEBUG
	std::atomic<bool> in_progress_{false};
	std::atomic<uint64_t> owner_thread_id_{0};
#endif
};

}  // namespace cxl_transport
}  // namespace Embarcadero
