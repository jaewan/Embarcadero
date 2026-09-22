#include "topic.h"
#include "topic_order5_internal.h"
#include "common/ack_rf_policy.h"
#include "common/performance_utils.h"
#include "common/stage_trace.h"
#include "common/wire_formats.h"
#include "common/order_level.h"
#include "common/env_flags.h"
#include "common/fault_injection.h"
#include "order5_tr_trace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>

namespace Embarcadero {
using namespace topic_internal;

// Recovery of ordering frontiers and bounded diagnostic flight recording.
// State remains owned by Topic; this file adds no independent runtime state.

void Topic::RecordOrder5FlightEvent(
		uint32_t kind, uint32_t broker, uint64_t a, uint64_t b, uint64_t c, uint64_t d) {
	if (!ShouldCaptureOrder5Flight(order_, broker_id_)) return;
	const uint64_t pos = order5_flight_write_pos_.fetch_add(1, std::memory_order_relaxed);
	Order5FlightEvent& slot = order5_flight_ring_[pos % kOrder5FlightRingSize];
	slot.ts_ns = SteadyNowNs();
	slot.kind = kind;
	slot.broker = broker;
	slot.a = a;
	slot.b = b;
	slot.c = c;
	slot.d = d;
}

void Topic::ReconstructClientStateFromSessionEntry(uint64_t session_key, ClientState5& state) {
	ApplyRecoveredSequencer5State(session_key, state);
	SessionEntry* entry = FindSessionEntry(session_key);
	if (entry == nullptr) return;

	CXL::invalidate_cacheline_for_read(entry);
	CXL::load_fence();
	const uint64_t state_word = entry->state_word.load(std::memory_order_acquire);
	const uint64_t flags = state_word & 0xFFFFFFFFULL;
	if ((flags & kSessionEntryFlagActive) == 0) return;

	const uint32_t durable_epoch = static_cast<uint32_t>(state_word >> 32);
	const uint64_t durable_expected = entry->expected_seq.load(std::memory_order_acquire);
	const uint64_t durable_committed = entry->committed_hwm.load(std::memory_order_acquire);
	const uint64_t durable_highest = entry->highest_sequenced.load(std::memory_order_acquire);

	state.session_epoch = std::max(state.session_epoch, durable_epoch);
	if (durable_expected > state.next_expected) {
		state.set_next_expected(durable_expected);
	}
	state.committed_hwm = std::max(state.committed_hwm, durable_committed);
	state.highest_sequenced = std::max(state.highest_sequenced, durable_highest);
	if ((flags & kSessionEntryFlagFenced) != 0) {
		state.fenced = true;
	}
	ApplyRecoveredSequencer5State(session_key, state);
}

void Topic::ApplyRecoveredSequencer5State(uint64_t session_key, ClientState5& state) {
	if (!order5_recovery_complete_.load(std::memory_order_acquire)) return;
	auto it = recovered_order5_next_expected_.find(session_key);
	if (it == recovered_order5_next_expected_.end()) return;
	if (it->second > state.next_expected) {
		state.set_next_expected(it->second);
	}
	if (it->second > 0) {
		state.committed_hwm = std::max<uint64_t>(state.committed_hwm, it->second - 1);
	}
}

uint64_t Topic::RecoverSequencer5State() {
	const uint64_t start_ns = SteadyNowNs();
	recovered_order5_next_expected_.clear();
	recovered_order5_msg_counts_.clear();
	recovered_global_seq_ = 0;
	uint64_t next_goi_index = 0;

	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	GOIEntry* goi = cxl_addr_ == nullptr
		? nullptr
		: reinterpret_cast<GOIEntry*>(reinterpret_cast<uint8_t*>(cxl_addr_) + kGOIOffset);
	if (control_block != nullptr && goi != nullptr) {
		CXL::invalidate_cacheline_for_read(control_block);
		CXL::load_fence();
		const uint64_t committed_seq = control_block->committed_seq.load(std::memory_order_acquire);
		if (committed_seq != UINT64_MAX) {
			static constexpr uint64_t kMaxGOIEntries = 256ULL * 1024ULL * 1024ULL;
			CHECK_LT(committed_seq, kMaxGOIEntries)
				<< "ControlBlock.committed_seq exceeds GOI capacity";
			const uint64_t limit = committed_seq + 1;
			bool saw_valid_goi = false;
			for (uint64_t i = 0; i < limit; ++i) {
				GOIEntry* entry = &goi[i];
				// [[CXL-1]] Invalidate BOTH cache lines before reading global_seq (line0) AND the
				// session fields (line1) below — the old single-line invalidate could read a fresh
				// global_seq with a stale line1, corrupting the recovered per-session frontier.
				ReadGOIEntryFresh(entry);
				if (entry->global_seq != i) continue;
				saw_valid_goi = true;
				next_goi_index = i + 1;
				const uint64_t batch_end_order =
					entry->total_order + static_cast<uint64_t>(entry->message_count);
				if (batch_end_order > recovered_global_seq_) {
					recovered_global_seq_ = batch_end_order;
				}
				if (entry->session_epoch == 0) continue;
				const uint64_t session_key =
					MakeSessionKey(entry->client_id, entry->session_epoch);
				uint64_t& next_expected = recovered_order5_next_expected_[session_key];
				next_expected = RecoveredNextExpected(next_expected, entry->client_seq + 1);
				recovered_order5_msg_counts_[entry->client_id] += entry->message_count;
			}
			if (!saw_valid_goi) {
				next_goi_index = 0;
			}
		}
	}

	if (session_table_ != nullptr) {
		for (size_t i = 0; i < kMaxSessions; ++i) {
			SessionEntry* entry = &session_table_[i];
			CXL::invalidate_cacheline_for_read(entry);
			CXL::load_fence();
			const uint64_t session_key = entry->session_key.load(std::memory_order_acquire);
			if (session_key == 0) continue;
			const uint64_t state_word = entry->state_word.load(std::memory_order_acquire);
			const uint64_t flags = state_word & 0xFFFFFFFFULL;
			if ((flags & kSessionEntryFlagActive) == 0) continue;
			const uint64_t expected = entry->expected_seq.load(std::memory_order_acquire);
			uint64_t& next_expected = recovered_order5_next_expected_[session_key];
			next_expected = RecoveredNextExpected(next_expected, expected);
		}
	}

	order5_recovery_complete_.store(true, std::memory_order_release);
	const uint64_t elapsed = SteadyNowNs() - start_ns;
	const uint64_t lease = GetSessionLeaseNs(replication_factor_ > 0);
	if (elapsed >= lease) {
		LOG(ERROR) << "ORDER5 recovery reconstruction exceeded session lease; "
		           << "ACK relay will withhold/terminal-fence stale uncommitted frontiers"
		           << " elapsed_ns=" << elapsed
		           << " lease_ns=" << lease;
	}
	LOG(INFO) << "RecoverSequencer5State: sessions=" << recovered_order5_next_expected_.size()
	          << " next_goi_index=" << next_goi_index
	          << " recovered_global_seq=" << recovered_global_seq_
	          << " elapsed_ns=" << elapsed
	          << " lease_ns=" << lease;
	return next_goi_index;
}

void Topic::DumpOrder5FlightRecorder(const char* reason) {
	if (!ShouldCaptureOrder5Flight(order_, broker_id_)) return;
	if (order5_flight_dumped_.test_and_set(std::memory_order_acq_rel)) return;

	const uint64_t end = order5_flight_write_pos_.load(std::memory_order_acquire);
	const uint64_t begin = (end > kOrder5FlightRingSize) ? (end - kOrder5FlightRingSize) : 0;
	LOG(ERROR) << "[ORDER5_FLIGHT_DUMP_BEGIN]"
	           << " reason=" << (reason ? reason : "unknown")
	           << " topic=" << topic_name_
	           << " broker=" << broker_id_
	           << " events=" << (end - begin)
	           << " epoch_index=" << epoch_index_.load(std::memory_order_acquire)
	           << " last_sequenced=" << last_sequenced_epoch_.load(std::memory_order_acquire)
	           << " hold_entries=" << GetTotalHoldBufferSize()
	           << " force_expire_until_ns=" << force_expire_hold_until_ns_.load(std::memory_order_acquire);
	for (uint64_t i = begin; i < end; ++i) {
		const Order5FlightEvent& ev = order5_flight_ring_[i % kOrder5FlightRingSize];
		LOG(ERROR) << "[ORDER5_FLIGHT]"
		           << " idx=" << i
		           << " kind=" << Order5FlightKindToString(ev.kind)
		           << " ts_ns=" << ev.ts_ns
		           << " broker=" << ev.broker
		           << " a=" << ev.a
		           << " b=" << ev.b
		           << " c=" << ev.c
		           << " d=" << ev.d;
	}
	LOG(ERROR) << "[ORDER5_FLIGHT_DUMP_END]"
	           << " reason=" << (reason ? reason : "unknown");
}

}  // namespace Embarcadero
