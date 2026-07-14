#pragma once

// CXL mailbox segment — a set of per-broker duplex ring pairs that the baseline
// control paths are ported onto. This is the single shared-memory RPC substrate the
// porting rule (docs/baselines/porting_rule.md §6) calls for: built from our own
// single-writer rings + clwb/sfence, not a borrowed RPC stack.
//
// Topology (one duplex channel per broker):
//     up(b)   : broker b  -> coordinator   (single writer = broker b)
//     down(b) : coordinator -> broker b     (single writer = coordinator)
//
// This one topology expresses every baseline's frozen protocol as a pure transport
// swap:
//   * Corfu  : broker posts TokenRequest on up(b); coordinator posts TokenGrant on
//              down(b). Request/response correlation rides inside the record (a POD the
//              transport never interprets) — same unary exchange, gRPC replaced.
//   * Scalog : local sequencer posts LocalCut on up(b); global sequencer broadcasts
//              GlobalCut by writing it to every down(b). Same per-epoch report/aggregate/
//              broadcast, gRPC stream replaced.
//   * LazyLog: local sequencer posts LocalProgress on up(b); coordinator broadcasts
//              GlobalBinding on down(b). Same binding round, gRPC stream replaced.
//
// The segment can be backed by POSIX shared memory (standalone global sequencers and
// tests) or attached in place over a region carved from the CXL segment (cxl_addr_ +
// offset). The bytes are identical either way — pure POD, offsets only, no pointers.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "cxl_transport/cxl_mailbox_ring.h"

namespace Embarcadero {
namespace cxl_transport {

inline constexpr uint32_t kMailboxSegmentMagic = 0x53424D45;  // 'EMBS'
// Version 2 adds Corfu mailbox session/key echoes.  Even though the fixed
// 512-byte ring slot and CXL offsets did not change, a v1 coordinator would
// not populate the new validation fields.  Attach must therefore reject mixed
// role binaries rather than silently accepting stale/aliased grants.
inline constexpr uint32_t kMailboxSegmentVersion = 2;

struct MailboxParams {
	uint32_t num_brokers = 0;
	uint32_t record_size = 512;    // max bytes per control message
	uint32_t up_capacity = 1024;   // slots per up ring (power of two)
	uint32_t down_capacity = 1024; // slots per down ring (power of two)
	// Per-ring spin bound for BroadcastDown's decoupled delivery (P1-3). A slow/full
	// broker ring is retried up to this many pause iterations before BroadcastDownNonBlocking
	// reports it WEDGED, so a single wedged ring never blocks delivery to the healthy ones.
	// Default: ~10x the down ring capacity — high enough that a merely-busy consumer is not
	// flagged spuriously, low enough that a genuinely stuck ring does not stall the broadcast.
	uint64_t broadcast_spin_bound = 10240;
};

// Result of a non-blocking broadcast (P1-3). Plain host-local metadata — never crosses
// the CXL link, no acquire/release semantics.
struct BroadcastStatus {
	uint32_t wedged_count = 0;            // rings that exhausted the spin bound while full
	uint32_t failed_count = 0;            // rings that rejected the record (len too large)
	std::vector<PerRingStatus> per_ring;  // status per broker (size == num_brokers)
};

struct alignas(kCacheLine) MailboxSegmentHeader {
	uint32_t magic;
	uint32_t version;
	uint32_t num_brokers;
	uint32_t record_size;
	uint32_t up_capacity;
	uint32_t down_capacity;
	uint64_t up_stride;      // bytes per up ring
	uint64_t down_stride;    // bytes per down ring
	uint64_t up_base;        // offset from segment base to up ring 0
	uint64_t down_base;      // offset from segment base to down ring 0
	uint64_t broadcast_spin_bound;  // per-ring spin bound for BroadcastDown (P1-3)
	// Fields now fill the cache line exactly (6*4 + 5*8 = 64 bytes); no padding needed.
};
static_assert(sizeof(MailboxSegmentHeader) == kCacheLine, "segment header is one cache line");

class MailboxSegment {
 public:
	~MailboxSegment();
	MailboxSegment(const MailboxSegment&) = delete;
	MailboxSegment& operator=(const MailboxSegment&) = delete;

	// --- POSIX shared memory backing (standalone processes) ---
	// Coordinator creates+owns the named segment (unlinks any stale one first).
	static std::unique_ptr<MailboxSegment> CreateShm(const std::string& name, const MailboxParams& p);
	// Broker attaches to a segment the coordinator created.
	static std::unique_ptr<MailboxSegment> AttachShm(const std::string& name);

	// --- In-place backing (region carved from the CXL segment) ---
	static std::unique_ptr<MailboxSegment> CreateInPlace(void* base, size_t bytes, const MailboxParams& p);
	static std::unique_ptr<MailboxSegment> AttachInPlace(void* base, size_t bytes);

	static size_t BytesNeeded(const MailboxParams& p);

	// Broker b's upstream ring (broker b -> coordinator).
	// SINGLE-WRITER: exactly one thread may call TryProduce/Produce on the returned ring.
	// Corfu and the other baselines must guarantee this via topology or thread pinning —
	// concurrent producers on one up(b) ring violate SPSC (see cxl_mailbox_ring.h).
	MailboxRing& up(uint32_t broker_id) { return up_[broker_id]; }
	// Broker b's downstream ring (coordinator -> broker b).
	// The coordinator (a single thread in every baseline) is the sole writer of all down(b)
	// rings, so the single-writer invariant is satisfied automatically here.
	MailboxRing& down(uint32_t broker_id) { return down_[broker_id]; }
	uint32_t num_brokers() const { return num_brokers_; }
	uint32_t record_size() const { return record_size_; }
	uint32_t up_capacity() const { return up_.empty() ? 0 : up_.front().capacity(); }
	uint32_t down_capacity() const { return down_.empty() ? 0 : down_.front().capacity(); }

	// Broadcast a record to every broker's down ring (Scalog GlobalCut / LazyLog
	// GlobalBinding).
	//
	// Back-pressure semantics (P1-3): delivery to each down(b) is INDEPENDENT. One broker
	// whose ring is full or slow no longer performs head-of-line blocking on delivery to
	// the others — each ring is attempted with a bounded spin. This models per-stream gRPC
	// flow control (porting_rule.md §6) rather than the sequential artifact of retrying one
	// ring to completion before touching the next.
	//
	// BroadcastDownNonBlocking makes one bounded pass and returns per-ring status; the
	// caller (e.g. Scalog's global sequencer) owns the policy: retry only the healthy/wedged
	// rings, drop, or apply application back-pressure. Cross-CXL publish correctness and
	// SPSC are preserved on every ring (same body-flush-then-seq-stamp as Produce).
	BroadcastStatus BroadcastDownNonBlocking(const void* record, uint32_t len,
	                                         uint64_t spin_bound_per_ring);

	// Convenience broadcast using the segment's stored broadcast_spin_bound. Makes ONE
	// decoupled, bounded pass and ALWAYS returns per-ring status — it does NOT spin until
	// every broker has the record, because a dead broker would then hang the coordinator and
	// stall all ordering (the P1-3 hazard). WEDGED rings in the returned status were not
	// delivered; the caller owns the policy (retry next epoch, or fail the broker via the
	// lease layer). CHECK-fails only on FULL (len > record_size, a caller error).
	BroadcastStatus BroadcastDown(const void* record, uint32_t len);

 private:
	MailboxSegment() = default;
	void BindRings(void* base, bool create, const MailboxParams* create_params);

	void* base_ = nullptr;
	size_t bytes_ = 0;
	int shm_fd_ = -1;
	std::string shm_name_;  // non-empty iff this instance owns a shm segment to unlink
	bool owns_mapping_ = false;  // true iff we mmap'd (shm paths); false for in-place
	uint32_t num_brokers_ = 0;
	uint32_t record_size_ = 0;
	uint64_t broadcast_spin_bound_ = 0;  // cached from the segment header (P1-3)
	std::vector<MailboxRing> up_;
	std::vector<MailboxRing> down_;
};

}  // namespace cxl_transport
}  // namespace Embarcadero
