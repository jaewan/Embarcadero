#pragma once
// Shared production OPEN snapshot reader; used directly by bounded fault fixtures.
#include "cxl_manager/cxl_datastructure.h"
#include "common/performance_utils.h"
#include "embarlet/sequencer_utils.h"
#include <glog/logging.h>
namespace Embarcadero::network {
inline constexpr uint64_t kSnapshotSessionFenced = 1ULL << 0;
inline constexpr uint64_t kSnapshotSessionActive = 1ULL << 1;
inline uint64_t Mix64Network(uint64_t x) {
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33;
	x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return x;
}

inline uint64_t MakeSessionKeyNetwork(uint32_t client_id, uint32_t session_epoch) {
	return SessionKeyFor(client_id, session_epoch);
}

struct DurableSessionSnapshot {
	bool active{false};
	bool fenced{false};
	uint32_t session_epoch{0};
	uint64_t expected_seq{0};
	uint64_t committed_hwm{0};
	uint64_t highest_sequenced{0};
	uint64_t goi_committed_hwm{0};
	bool goi_committed_hwm_found{false};
	uint64_t reconnect_committed_hwm{0};
};

inline bool ReadSessionEntrySnapshot(
		SessionEntry* table,
		uint32_t client_id,
		uint32_t session_epoch,
		DurableSessionSnapshot* out) {
	if (table == nullptr || session_epoch == 0 || out == nullptr) return false;
	const uint64_t session_key = MakeSessionKeyNetwork(client_id, session_epoch);
	const size_t start = static_cast<size_t>(Mix64Network(session_key) % kMaxSessions);
	for (size_t probe = 0; probe < kMaxSessions; ++probe) {
		SessionEntry* entry = &table[(start + probe) % kMaxSessions];
		CXL::invalidate_cacheline_for_read(entry);
		CXL::load_fence();
		const uint64_t observed = entry->session_key.load(std::memory_order_acquire);
		if (observed == 0) return false;
		if (observed != session_key) continue;

		const uint64_t state_word = entry->state_word.load(std::memory_order_acquire);
		const uint64_t flags = state_word & 0xFFFFFFFFULL;
		out->active = (flags & kSnapshotSessionActive) != 0;
		out->fenced = (flags & kSnapshotSessionFenced) != 0;
		out->session_epoch = static_cast<uint32_t>(state_word >> 32);
		out->expected_seq = entry->expected_seq.load(std::memory_order_acquire);
		out->committed_hwm = entry->committed_hwm.load(std::memory_order_acquire);
		out->highest_sequenced = entry->highest_sequenced.load(std::memory_order_acquire);
		out->reconnect_committed_hwm = out->committed_hwm;
		return out->active;
	}
	return false;
}

inline bool ScanGOICommittedHwm(
		GOIEntry* goi,
		ControlBlock* control_block,
		uint32_t client_id,
		uint32_t session_epoch,
		uint64_t* out_hwm) {
	if (out_hwm != nullptr) *out_hwm = 0;
	if (goi == nullptr || control_block == nullptr || session_epoch == 0 || out_hwm == nullptr) return false;
	CXL::invalidate_cacheline_for_read(control_block);
	CXL::load_fence();
	const uint64_t committed_seq = control_block->committed_seq.load(std::memory_order_acquire);
	if (committed_seq == UINT64_MAX) return false;
	static constexpr uint64_t kMaxGOIEntries = 256ULL * 1024ULL * 1024ULL;
	CHECK_LT(committed_seq, kMaxGOIEntries) << "ControlBlock.committed_seq exceeds GOI capacity";
	const uint64_t limit = committed_seq + 1;
	// Scan BACKWARD and stop at the first match: the W1.2 commit-order invariant keeps a
	// session's client_seq strictly increasing in GOI index order, so the highest-index match
	// is the hwm. The previous forward full scan was O(committed_seq) with a serializing
	// clflush per 128B entry — multi-second per call mid-run, and every retransmit reconnect
	// (one connection per resent batch) triggered two of them, starving the recv pool during
	// exactly the ACK stalls that caused the retransmits (measured 2026-07-12, run 8).
	for (uint64_t i = limit; i-- > 0;) {
		GOIEntry* entry = &goi[i];
		CXL::invalidate_cacheline_for_read(entry);
        CXL::invalidate_cacheline_for_read(reinterpret_cast<const uint8_t*>(entry) + 64);
		CXL::load_fence();
		if (entry->global_seq != i) continue;
		if (entry->client_id == client_id && entry->session_epoch == session_epoch) {
			*out_hwm = entry->client_seq;
			return true;
		}
	}
	return false;
}


inline DurableSessionSnapshot ReadSessionSnapshot(SessionEntry* table, GOIEntry* goi,
        ControlBlock* control, uint32_t client_id, uint32_t session_epoch) {
    DurableSessionSnapshot snapshot;
    ReadSessionEntrySnapshot(table, client_id, session_epoch, &snapshot);
    snapshot.goi_committed_hwm_found = ScanGOICommittedHwm(
        goi, control, client_id, session_epoch, &snapshot.goi_committed_hwm);
    snapshot.reconnect_committed_hwm = ReconnectAnswerHwm(snapshot.committed_hwm,
        snapshot.goi_committed_hwm_found, snapshot.goi_committed_hwm);
    return snapshot;
}
}  // namespace Embarcadero::network
