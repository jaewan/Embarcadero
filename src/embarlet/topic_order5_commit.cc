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

// GOI/visibility publication and contiguous committed-frontier advancement.
// State remains owned by Topic; this file adds no independent runtime state.

void Topic::AdvanceCVForSequencer(uint16_t broker_id, uint64_t pbr_index, uint64_t cumulative_msg_count) {
	// [[PHASE_2_CV_EXPORT]] ack_level=1: sequencer advances CV so export can proceed without waiting for replication.
	// IMPORTANT: topic-level ack_level is not a safe gate here (topics can outlive client ack-level mix).
	// Sequencer progress must not depend on a per-client ACK policy.

	// [[B0_ACK_FIX]] Advance on cumulative_msg_count (ACK offset), not pbr_index. Late-arriving batches
	// (e.g. seq=0 after seq=1,2 expired) have lower pbr_index but must still advance ACK so broker sees progress.
	// For replicated topics, the chain-replication tail exclusively owns completed_* durability fields.
	// Letting the sequencer also write completed_pbr_head races tail ACK2 updates on the same CXL line and
	// can publish a newer PBR head alongside an older durable logical frontier.
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	CompletionVectorEntry* entry = &cv[broker_id];
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
	const bool sequencer_owns_durable_cv = (replication_factor_ <= 1);
	// CV entries live in CXL memory and are shared with the replication tail. Refresh the full line
	// before modifying it so we do not flush a newer sequencer/pbr update alongside a stale durable
	// offset (or vice versa).
	CXL::flush_cacheline(entry);
	CXL::full_fence();
	// Advance sequencer logical offset (ACK1 frontier) when this batch extends the cumulative count
	for (;;) {
		uint64_t cur_offset = entry->sequencer_logical_offset.load(std::memory_order_acquire);
		if (cumulative_msg_count <= cur_offset) break;
		if (entry->sequencer_logical_offset.compare_exchange_strong(cur_offset, cumulative_msg_count, std::memory_order_release)) {
			if (sequencer_owns_durable_cv) {
				// Only unreplicated topics let the sequencer own the durable/export frontier directly.
				uint64_t cur_pbr = entry->completed_pbr_head.load(std::memory_order_acquire);
				if (cur_pbr == kNoProgress || pbr_index > cur_pbr) {
					entry->completed_pbr_head.store(pbr_index, std::memory_order_release);
				}
			}
			CXL::store_fence();
			CXL::flush_cacheline(entry);
			CXL::store_fence();
			break;
		}
	}
}

void Topic::AccumulateCVUpdate(
		uint16_t broker_id,
		uint64_t pbr_index,
		uint64_t cumulative_msg_count,
		std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
		std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index) {
	if (broker_id >= NUM_MAX_BROKERS) return;
	if (cumulative_msg_count > max_cumulative[broker_id]) {
		max_cumulative[broker_id] = cumulative_msg_count;
	}
	if (pbr_index > max_pbr_index[broker_id]) {
		max_pbr_index[broker_id] = pbr_index;
	}
}

void Topic::FlushAccumulatedCVLogicalOnly(
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index_plus_one) {
    if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
	const bool sequencer_owns_durable_cv = (replication_factor_ <= 1);

	for (int broker_id = 0; broker_id < NUM_MAX_BROKERS; ++broker_id) {
		const uint64_t cumulative = max_cumulative[broker_id];
		const uint64_t pbr_index_plus_one = max_pbr_index_plus_one[broker_id];
		if (cumulative == 0 && pbr_index_plus_one == 0) continue;
		const uint64_t pbr_index = (pbr_index_plus_one == 0) ? 0 : (pbr_index_plus_one - 1);

		CompletionVectorEntry* entry = &cv[broker_id];
		CXL::flush_cacheline(entry);
		// CompletionVector lives in CXL memory; CLFLUSHOPT is only ordered by SFENCE/MFENCE.
		// Using LFENCE here can re-read a stale line and republish a newer PBR head with an older
		// durable logical frontier.
		CXL::full_fence();

		const uint64_t prev_cur = entry->sequencer_logical_offset.load(std::memory_order_acquire);
		const uint64_t prev_durable = entry->completed_logical_offset.load(std::memory_order_acquire);
		const uint64_t prev_pbr = entry->completed_pbr_head.load(std::memory_order_acquire);
		bool advanced_logical = false;
		bool advanced_durable = false;
		bool advanced_pbr = false;
		uint64_t post_cur = prev_cur;
		uint64_t post_durable = prev_durable;
		uint64_t post_pbr = prev_pbr;
		if (cumulative > prev_cur) {
			uint64_t cur = prev_cur;
			while (cumulative > cur) {
				if (entry->sequencer_logical_offset.compare_exchange_strong(
				        cur, cumulative, std::memory_order_release, std::memory_order_acquire)) {
					advanced_logical = true;
					post_cur = cumulative;
					break;
				}
			}
			if (!advanced_logical) {
				post_cur = cur;
			}
		}
		if (sequencer_owns_durable_cv) {
			// ORDER=5 late/terminalized batches can bypass the normal commit accumulation path.
			// In the unreplicated case the sequencer therefore also owns durable/export visibility.
			if (cumulative > prev_durable) {
				uint64_t cur = prev_durable;
				while (cumulative > cur) {
					if (entry->completed_logical_offset.compare_exchange_strong(
					        cur, cumulative, std::memory_order_release, std::memory_order_acquire)) {
						advanced_durable = true;
						post_durable = cumulative;
						break;
					}
				}
				if (!advanced_durable) {
					post_durable = cur;
				}
			}
			if (pbr_index_plus_one != 0 && (prev_pbr == kNoProgress || pbr_index > prev_pbr)) {
				uint64_t cur = prev_pbr;
				while (cur == kNoProgress || pbr_index > cur) {
					if (entry->completed_pbr_head.compare_exchange_strong(
					        cur, pbr_index, std::memory_order_release, std::memory_order_acquire)) {
						advanced_pbr = true;
						post_pbr = pbr_index;
						break;
					}
				}
				if (!advanced_pbr) {
					post_pbr = cur;
				}
			}
		}
		if (!advanced_logical && !advanced_durable && !advanced_pbr) continue;
		RecordOrder5FlightEvent(
			kOrder5FlightCV,
			static_cast<uint32_t>(broker_id),
			prev_cur,
			post_cur,
			prev_pbr,
			post_pbr);
		if (ShouldEnableOrder5Trace() && order_ == 5) {
			LOG(INFO) << "[ORDER5_TRACE_CV_LOGICAL_ONLY B" << broker_id << "]"
			          << " cv_logical:" << prev_cur << "->"
			          << post_cur
			          << " cv_durable:" << prev_durable << "->"
			          << post_durable
			          << " cv_pbr:" << prev_pbr << "->"
			          << post_pbr;
		}

		CXL::store_fence();
		CXL::flush_cacheline(entry);
	}
	CXL::store_fence();
}

void Topic::FlushAccumulatedCV(
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_cumulative,
		const std::array<uint64_t, NUM_MAX_BROKERS>& max_pbr_index,
		const std::array<bool, NUM_MAX_BROKERS>* touched) {
    if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
	// [PHASE-3] O(brokers) CXL writes + 1 fence (was O(batches) fences)
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	constexpr uint64_t kNoProgress = static_cast<uint64_t>(-1);
	const bool sequencer_owns_durable_cv = (replication_factor_ <= 1);

	for (int broker_id = 0; broker_id < NUM_MAX_BROKERS; ++broker_id) {
		// [[O5-3]] Skip untouched brokers to avoid a flush_cacheline + MFENCE per broker
		// every CommitEpoch. cv_max_* is populated only for touched brokers and only ever
		// increases, so an untouched broker's entry could not have advanced here and the
		// loop body would publish nothing for it anyway. Mirrors the both-zero early-continue
		// in FlushAccumulatedCVLogicalOnly. Only active when the caller supplies `touched`.
		if (touched != nullptr && !(*touched)[broker_id]) continue;
		CompletionVectorEntry* entry = &cv[broker_id];

		// [PANEL C3/R3] On non-coherent CXL, relaxed load can read from local cache (stale).
		// CLFLUSHOPT is only ordered by SFENCE/MFENCE, so use a full fence before reading shared CV
		// state that may also be advanced by the replication tail.
		CXL::flush_cacheline(entry);
		CXL::full_fence();

		// Do not gate sequencer progress on topic-level ack_level. Client ack policy is per-connection.
		uint64_t cur = entry->sequencer_logical_offset.load(std::memory_order_acquire);
		const uint64_t prev_cur = cur;
		uint64_t cur_pbr = entry->completed_pbr_head.load(std::memory_order_acquire);
		const uint64_t prev_pbr = cur_pbr;
		const uint64_t pbr_val = max_pbr_index[broker_id];
		const uint64_t cumulative = max_cumulative[broker_id];
		const uint64_t prev_durable =
			entry->completed_logical_offset.load(std::memory_order_acquire);
		bool advanced_logical = false;
		bool advanced_pbr = false;
		bool advanced_durable = false;
		uint64_t post_cumulative = prev_cur;
		uint64_t post_pbr = prev_pbr;
		uint64_t post_durable = prev_durable;
		if (cumulative > cur) {
			while (cumulative > cur) {
				if (entry->sequencer_logical_offset.compare_exchange_strong(
				        cur, cumulative, std::memory_order_release, std::memory_order_acquire)) {
					advanced_logical = true;
					post_cumulative = cumulative;
					break;
				}
			}
			if (!advanced_logical) {
				post_cumulative = cur;
			}
		}
		// [[ACK_LEVEL_2_RF1]] ORDER=5 ack_level=2 reads completed_logical_offset (NetworkManager).
		// Happy-path commits only fed FlushAccumulatedCV, not FlushAccumulatedCVLogicalOnly's
		// cv_logical_only_* maps (those are for late/terminalized batches), so durable lagged at 0.
		if (sequencer_owns_durable_cv && cumulative > prev_durable) {
			uint64_t cur_d = prev_durable;
			while (cumulative > cur_d) {
				if (entry->completed_logical_offset.compare_exchange_strong(
				        cur_d, cumulative, std::memory_order_release, std::memory_order_acquire)) {
					advanced_durable = true;
					post_durable = cumulative;
					break;
				}
			}
			if (!advanced_durable) {
				post_durable = cur_d;
			}
		}
		if (sequencer_owns_durable_cv &&
		    (cur_pbr == kNoProgress || pbr_val > cur_pbr)) {
			while (cur_pbr == kNoProgress || pbr_val > cur_pbr) {
				if (entry->completed_pbr_head.compare_exchange_strong(
				        cur_pbr, pbr_val, std::memory_order_release, std::memory_order_acquire)) {
					advanced_pbr = true;
					post_pbr = pbr_val;
					break;
				}
			}
			if (!advanced_pbr) {
				post_pbr = cur_pbr;
			}
		}
		if (order_ == 5 && post_pbr > prev_pbr && post_cumulative <= prev_cur) {
			order5_ack_order_violations_.fetch_add(1, std::memory_order_relaxed);
		}
		if (advanced_logical || advanced_pbr || advanced_durable) {
			RecordOrder5FlightEvent(
				kOrder5FlightCV,
				static_cast<uint32_t>(broker_id),
				prev_cur,
				post_cumulative,
				prev_pbr,
				post_pbr);
		}
		if (ShouldEnableOrder5Trace() && order_ == 5 &&
		    (advanced_logical || advanced_durable ||
		     (prev_pbr == kNoProgress || post_pbr > prev_pbr))) {
			LOG(INFO) << "[ORDER5_TRACE_CV B" << broker_id << "]"
			          << " cv_logical:" << prev_cur << "->"
			          << post_cumulative
			          << " cv_durable:" << prev_durable << "->"
			          << post_durable
			          << " cv_pbr_head:" << prev_pbr << "->"
			          << post_pbr;
		}

		CXL::store_fence();
		CXL::flush_cacheline(entry);
	}
	CXL::store_fence();
}

void Topic::ResetCompletedRangeQueue() {
	completed_ranges_head_.store(0, std::memory_order_release);
	completed_ranges_tail_.store(0, std::memory_order_release);
	completed_ranges_enqueue_retries_.store(0, std::memory_order_release);
	completed_ranges_enqueue_wait_ns_.store(0, std::memory_order_release);
	completed_ranges_max_depth_.store(0, std::memory_order_release);
	committed_updater_pending_peak_.store(0, std::memory_order_release);
	for (auto& slot : completed_ranges_ring_) {
		slot.ready.store(false, std::memory_order_relaxed);
	}
}

void Topic::EnqueueCompletedRange(uint64_t start, uint64_t end) {
	if (end <= start) return;
	auto wait_start = std::chrono::steady_clock::now();
	uint64_t spins = 0;
	static std::atomic<uint64_t> last_enqueue_trace_ns{0};
	while (!stop_threads_) {
		uint64_t tail = completed_ranges_tail_.load(std::memory_order_relaxed);
		uint64_t head = completed_ranges_head_.load(std::memory_order_acquire);
		uint64_t depth = tail - head;
		uint64_t prev_peak = completed_ranges_max_depth_.load(std::memory_order_relaxed);
		while (depth > prev_peak &&
		       !completed_ranges_max_depth_.compare_exchange_weak(prev_peak, depth, std::memory_order_relaxed)) {
		}
		if (depth < kCompletedRangesRingCap) {
			if (completed_ranges_tail_.compare_exchange_weak(
					tail, tail + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) {
				CompletedRangeSlot& slot = completed_ranges_ring_[tail & kCompletedRangesRingMask];
				slot.data.start = start;
				slot.data.end = end;
				slot.ready.store(true, std::memory_order_release);
				committed_seq_updater_cv_.notify_one();
				if (ShouldEnableOrder5Trace() && order_ == 5) {
					const uint64_t now_ns = SteadyNowNs();
					uint64_t last_ns = last_enqueue_trace_ns.load(std::memory_order_relaxed);
					if (now_ns - last_ns >= 1'000'000'000ULL &&
					    last_enqueue_trace_ns.compare_exchange_strong(
					        last_ns, now_ns, std::memory_order_relaxed)) {
						LOG(INFO) << "[ORDER5_TRACE_CR_ENQUEUE]"
						          << " range=[" << start << "," << end << ")"
						          << " depth=" << depth + 1
						          << " retries=" << completed_ranges_enqueue_retries_.load(std::memory_order_relaxed)
						          << " pending_peak=" << committed_updater_pending_peak_.load(std::memory_order_relaxed);
					}
				}
				if (spins > 0) {
					auto waited_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
						std::chrono::steady_clock::now() - wait_start).count();
					completed_ranges_enqueue_wait_ns_.fetch_add(static_cast<uint64_t>(waited_ns), std::memory_order_relaxed);
				}
				return;
			}
		} else {
			completed_ranges_enqueue_retries_.fetch_add(1, std::memory_order_relaxed);
			++spins;
				if ((spins & 0x3F) == 0) {
				std::this_thread::yield();
			} else {
				std::this_thread::sleep_for(std::chrono::microseconds(1));
			}
		}
	}
}

void Topic::CommittedSeqUpdaterThread() {
	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::flush_cacheline(control_block);
	CXL::load_fence();
	uint64_t cur = control_block->committed_seq.load(std::memory_order_acquire);
	uint64_t next_expected_start = (cur == UINT64_MAX) ? 0 : (cur + 1);
	uint64_t last_trace_ns = SteadyNowNs();

	std::priority_queue<CompletedRange, std::vector<CompletedRange>, std::greater<CompletedRange>> pending;
	uint64_t idle_spins = 0;

	while (true) {
		bool drained_any = false;
		while (true) {
			uint64_t head = completed_ranges_head_.load(std::memory_order_relaxed);
			uint64_t tail = completed_ranges_tail_.load(std::memory_order_acquire);
			if (head == tail) break;
			CompletedRangeSlot& slot = completed_ranges_ring_[head & kCompletedRangesRingMask];
			if (!slot.ready.load(std::memory_order_acquire)) break;
			pending.push(slot.data);
			slot.ready.store(false, std::memory_order_release);
			completed_ranges_head_.store(head + 1, std::memory_order_release);
			drained_any = true;
		}
		if (drained_any) {
			uint64_t pending_sz = static_cast<uint64_t>(pending.size());
			uint64_t prev_peak = committed_updater_pending_peak_.load(std::memory_order_relaxed);
			while (pending_sz > prev_peak &&
			       !committed_updater_pending_peak_.compare_exchange_weak(prev_peak, pending_sz, std::memory_order_relaxed)) {
			}
		}

		bool advanced = false;
		while (!pending.empty()) {
			const CompletedRange r = pending.top();
			if (r.end <= next_expected_start) {
				// Fully stale/duplicate range.
				pending.pop();
				continue;
			}
			if (r.start <= next_expected_start) {
				// Overlap or exact continuation.
				next_expected_start = r.end;
				pending.pop();
				advanced = true;
				continue;
			}
			// True gap at the head: cannot advance further yet.
			break;
		}

		if (advanced && next_expected_start > 0) {
			uint64_t new_committed = next_expected_start - 1;
			control_block->committed_seq.store(new_committed, std::memory_order_release);
			CXL::store_fence();
			CXL::flush_cacheline(control_block);
			CXL::store_fence();
			if (ShouldEnableOrder5Trace() && order_ == 5) {
				const uint64_t now_ns = SteadyNowNs();
				if (now_ns - last_trace_ns >= 1'000'000'000ULL) {
					LOG(INFO) << "[ORDER5_TRACE_COMMITTED_UPDATER]"
					          << " committed_seq=" << new_committed
					          << " next_expected_start=" << next_expected_start
					          << " pending_size=" << pending.size()
					          << " ring_depth="
					          << (completed_ranges_tail_.load(std::memory_order_relaxed) -
					              completed_ranges_head_.load(std::memory_order_relaxed))
					          << " pending_peak=" << committed_updater_pending_peak_.load(std::memory_order_relaxed);
					last_trace_ns = now_ns;
				}
			}
			idle_spins = 0;
		}

		if (ShouldEnableOrder5Trace() && order_ == 5 && !advanced) {
			const uint64_t now_ns = SteadyNowNs();
			if (now_ns - last_trace_ns >= 1'000'000'000ULL) {
				const uint64_t ring_depth =
					completed_ranges_tail_.load(std::memory_order_relaxed) -
					completed_ranges_head_.load(std::memory_order_relaxed);
				if (!pending.empty()) {
					const CompletedRange r = pending.top();
					LOG(INFO) << "[ORDER5_TRACE_COMMITTED_STALL]"
					          << " next_expected_start=" << next_expected_start
					          << " head_range=[" << r.start << "," << r.end << ")"
					          << " pending_size=" << pending.size()
					          << " ring_depth=" << ring_depth
					          << " stop=" << committed_seq_updater_stop_.load(std::memory_order_relaxed);
				} else if (ring_depth > 0) {
					LOG(INFO) << "[ORDER5_TRACE_COMMITTED_WAIT]"
					          << " next_expected_start=" << next_expected_start
					          << " ring_depth=" << ring_depth
					          << " pending_size=0";
				}
				last_trace_ns = now_ns;
			}
		}

		if (committed_seq_updater_stop_.load(std::memory_order_acquire)) {
			uint64_t head = completed_ranges_head_.load(std::memory_order_acquire);
			uint64_t tail = completed_ranges_tail_.load(std::memory_order_acquire);
			if (head == tail && pending.empty()) break;
		}

		if (!advanced && !drained_any) {
			if (++idle_spins < 64) {
				std::this_thread::yield();
			} else {
				std::unique_lock<std::mutex> lock(committed_seq_updater_mu_);
				committed_seq_updater_cv_.wait_for(lock, std::chrono::microseconds(200), [this] {
					return committed_seq_updater_stop_.load(std::memory_order_acquire) ||
					       (completed_ranges_head_.load(std::memory_order_acquire) !=
					        completed_ranges_tail_.load(std::memory_order_acquire));
				});
				idle_spins = 0;
			}
		}
	}
}

void Topic::StopForCapacityExhaustion() {
  // Call without the publication gate: shard workers acquire shard -> gate.
  order5_capacity_exhausted_.store(true, std::memory_order_release);
  stop_threads_.store(true, std::memory_order_release);
  for (auto& shard : level5_shards_) {
    if (!shard) continue;
    {
      std::lock_guard<std::mutex> lock(shard->mu);
      shard->stop = true;
    }
    shard->cv.notify_all();
  }
}

/**
 * CommitEpoch: Unified commit logic for both main loop and drain loop
 * Handles GOI writing, export chain setup, CV accumulation, and consumed_through advancement
 */
void Topic::CommitEpoch(
		std::vector<PendingBatch5>& ready,
		std::vector<const PendingBatch5*>& by_slot,
		std::array<size_t, NUM_MAX_BROKERS>& contiguous_consumed_per_broker,
		std::array<bool, NUM_MAX_BROKERS>& broker_seen_in_epoch,
		std::array<uint64_t, NUM_MAX_BROKERS>& cv_max_cumulative,
		std::array<uint64_t, NUM_MAX_BROKERS>& cv_max_pbr_index,
		std::vector<PendingBatch5>& batch_list,
		bool is_drain_mode) {

#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
    (void)fault::Pause("commit.before_publication_gate",
        {ready.empty() ? UINT64_MAX : ready.front().client_id,
         ready.empty() ? UINT64_MAX : ready.front().session_epoch,
         ready.empty() ? UINT64_MAX : ready.front().batch_seq}, &stop_threads_);
#endif
    auto publication_lock = session_publication_gate_.Lock();
    if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;

	const bool commit_profile = ShouldEnableOrder5CommitProfile();
	const auto commit_t_start = commit_profile ? std::chrono::steady_clock::now()
	                                            : std::chrono::steady_clock::time_point{};
	// [[ORDER5_COMMIT_PROFILE_V2]] Scoped-phase helper; ~zero cost when profiling is off.
	auto phase_ns = [commit_profile](std::atomic<uint64_t>& counter,
	                                 std::chrono::steady_clock::time_point start) {
		if (commit_profile) {
			counter.fetch_add(
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - start).count()),
				std::memory_order_relaxed);
		}
	};
	auto phase_now = [commit_profile]() {
		return commit_profile ? std::chrono::steady_clock::now()
		                      : std::chrono::steady_clock::time_point{};
	};

    bool admission_failed = false;
	auto spatial_guard_reject = [&](PendingBatch5& p) {
		if (p.skipped || p.is_held_marker) return false;
		const uint32_t session_epoch = p.from_hold ? p.hold_meta.session_epoch : p.session_epoch;
		if (session_epoch == 0) return false;
		const size_t client_id = p.from_hold ? p.hold_meta.client_id : p.client_id;
		const uint64_t batch_seq = p.from_hold ? p.hold_meta.batch_seq : p.batch_seq;
        const uint64_t session_key = MakeSessionKey(client_id, session_epoch);
        SessionEntry* entry = FindSessionEntry(session_key);
        if (!entry) {
            entry = FindOrCreateSessionEntry(session_key);
            if (!entry) { admission_failed = true; return false; }
            CXL::invalidate_cacheline_for_read(entry);
            CXL::load_fence();
        }
        // A matching FindSessionEntry already refreshed this never-reused key.
        // Keep every authoritative guard load under the publication gate.
		const uint64_t state_word = entry->state_word.load(std::memory_order_acquire);
		const uint64_t flags = state_word & 0xFFFFFFFFULL;
		if ((flags & kSessionEntryFlagActive) == 0) return false;
		const uint64_t durable_expected = entry->expected_seq.load(std::memory_order_acquire);
		const bool reject = ShouldRejectOrder5SpatialGuard(
			(flags & kSessionEntryFlagActive) != 0,
			(flags & kSessionEntryFlagFenced) != 0,
			batch_seq,
			durable_expected);
		if (!reject) return false;

		order5_spatial_guard_rejects_.fetch_add(1, std::memory_order_relaxed);
		if (p.hdr != nullptr) {
			ClearOrder5PublishState(p.hdr);
			CXL::store_fence();
			CXL::flush_cacheline(p.hdr);
			CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(p.hdr) + 64);
			CXL::store_fence();
		}
		LOG_EVERY_N(WARNING, 100)
			<< "[ORDER5_SPATIAL_GUARD_REJECT]"
			<< " client=" << client_id
			<< " session_epoch=" << session_epoch
			<< " batch_seq=" << batch_seq
			<< " durable_expected=" << durable_expected
			<< " fenced=" << ((flags & kSessionEntryFlagFenced) != 0);
		return true;
	};
    // Classification and commit are different phases. Check authoritative
    // state every epoch, while the fence/commit publication gate is held.
    {
      const auto guard_t = phase_now();
      ready.erase(std::remove_if(ready.begin(), ready.end(), spatial_guard_reject), ready.end());
      phase_ns(order5_commit_guard_ns_, guard_t);
    }

    if (admission_failed) {
      publication_lock.unlock();
      StopForCapacityExhaustion();
      LOG(ERROR) << "Authoritative session table full; refusing commit topic=" << topic_name_;
      return;
    }
	if (ready.empty()) {
        publication_lock.unlock();
		const auto advance_t = phase_now();
		AdvanceConsumedThroughForProcessedSlots(
			batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch,
			&cv_max_cumulative, &cv_max_pbr_index);
		phase_ns(order5_commit_advance_ns_, advance_t);
		return;
	}

	// [[NAMING]] total_order = message-level sequence (subscribers); GOI index = slot in GOI array (design §2.4).
	// One atomic per epoch (§3.2): reserve message-order range for this epoch.
	// [[TOTAL_ORDER_SKIP_GAP_FIX]] Must exclude skipped/held entries exactly like the GOI-count
	// loop below (num_goi_order5) and the per-batch assignment loop that advances next_order --
	// otherwise a batch aged out of the hold buffer (skipped=true, e.g. stale-epoch drop at
	// kMaxEpochAge) still has its num_msg counted here, reserving total_order space via
	// global_seq_.fetch_add that the assignment loop then correctly never fills (it skips the
	// same entry). That mismatch permanently strands exactly that batch's message count as a
	// gap no future epoch can ever close, wedging every subscriber's strict total-order
	// consumption forever (see docs/experiments/YCSB_DISTRIBUTED_KV_PLAN.md Sec 6f). A
	// held-but-not-yet-ready entry (is_held_marker) must also be excluded: its real num_msg is
	// counted later, when it is actually delivered via the p.from_hold branch in a future epoch.
    size_t total_msg = 0;
    size_t num_goi_order5 = 0;
    bool count_overflow = false;
    for (const PendingBatch5& p : ready) {
      if (p.skipped || p.is_held_marker) continue;
      if ((!p.from_hold && p.hdr == nullptr) ||
          p.num_msg > std::numeric_limits<size_t>::max() - total_msg) {
        count_overflow = true;
        break;
      }
      total_msg += p.num_msg;
      ++num_goi_order5;
    }
    uint64_t base_batch_index_order5 = 0;
    size_t base_order = 0;
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
    // Physical layout stays unchanged. An immutable logical budget exercises
    // the production all-or-nothing range reservation with bounded workloads.
    static const uint64_t kGOICapacity = []() -> uint64_t {
        constexpr uint64_t physical = 256ULL * 1024ULL * 1024ULL;
        const char* value = std::getenv("EMBARCADERO_FAULT_GOI_CAPACITY");
        if (!value) return physical;
        char* end = nullptr;
        errno = 0;
        const auto parsed = std::strtoull(value, &end, 10);
        if (errno || end == value || *end || !parsed || parsed > physical)
            throw std::invalid_argument("invalid fault GOI budget");
        return parsed;
    }();
#else
    constexpr uint64_t kGOICapacity = 256ULL * 1024ULL * 1024ULL;
#endif
    if (count_overflow || !TryReserveCommitRanges(global_batch_seq_, global_seq_,
          kGOICapacity, num_goi_order5, total_msg, base_batch_index_order5, base_order)) {
      // Classification may already have advanced volatile session state. This is
      // terminal for this topic: never retry with a changed identity, publish an
      // ACK, or retire these slots. Existing committed prefixes remain readable.
      publication_lock.unlock();
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
      (void)fault::Pause("capacity.commit_rejected",
              {UINT64_MAX, UINT64_MAX, num_goi_order5, global_batch_seq_.load(), global_seq_.load()},
              &stop_threads_);  // Cancellation must still latch the real capacity failure.
#endif
      StopForCapacityExhaustion();
      LOG(ERROR) << "ORDER5 commit capacity exhausted; topic stopped without publishing epoch topic="
                 << topic_name_;
      return;
    }

	// [PHASE-2D] Fast-path: ready vector is already sorted by broker+slot before calling CommitEpoch
	// Sorting is done in main loop and drain loop to preserve consumed_through contiguity.
	// [PHASE-4] Accumulate per-broker tinode updates
	// Replace hash maps with arrays for better performance
	// [PHASE-7] Single-pass commit: hold lock for entire epoch; export chain inline with GOI/CV/tinode.
	const auto lock_t = phase_now();
	absl::MutexLock header_lock(&export_cursor_mu_);
	phase_ns(order5_commit_lock_ns_, lock_t);

	size_t next_order = base_order;  // message order (total_order) for next batch
	GOIEntry* goi = reinterpret_cast<GOIEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kGOIOffset);

	// [PANEL C2/P1] O(1) atomics per epoch: reserve GOI indices once (§3.2)

	size_t goi_idx_order5 = 0;
	std::array<uint64_t, NUM_MAX_BROKERS> goi_cumulative_tracker{};
	std::array<uint64_t, NUM_MAX_BROKERS> cv_cumulative_tracker{};
	std::array<bool, NUM_MAX_BROKERS> goi_tracker_initialized{};
	std::array<bool, NUM_MAX_BROKERS> cv_tracker_initialized{};
	CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
	auto ensure_goi_tracker = [&](int broker_id) {
		if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS || goi_tracker_initialized[broker_id]) return;
		goi_cumulative_tracker[broker_id] = tinode_->offsets[broker_id].ordered;
		goi_tracker_initialized[broker_id] = true;
	};
	auto ensure_cv_tracker = [&](int broker_id) {
		if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS || cv_tracker_initialized[broker_id]) return;
		const uint64_t base = tinode_->offsets[broker_id].ordered;
		CompletionVectorEntry* entry = &cv[broker_id];
		CXL::flush_cacheline(entry);
		CXL::full_fence();
		const uint64_t cv_logical =
			entry->sequencer_logical_offset.load(std::memory_order_acquire);
		const uint64_t cv_durable =
			entry->completed_logical_offset.load(std::memory_order_acquire);
		cv_cumulative_tracker[broker_id] = std::max(base, std::max(cv_logical, cv_durable));
		cv_tracker_initialized[broker_id] = true;
	};

	// [COMMIT_DIAG] Count batches committed per broker this epoch
	std::array<size_t, NUM_MAX_BROKERS> committed_this_epoch{};

    const auto goi_loop_t_start =
        commit_profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // [PHASE-3D] Regroup flushes for better pipeline utilization
    // 1. Flush all GOI entries
    for (PendingBatch5& p : ready) {
        if (p.skipped || p.is_held_marker)
            continue;
        uint64_t batch_index = base_batch_index_order5 + goi_idx_order5++;
        GOIEntry* entry = &goi[batch_index];

        if (p.from_hold) {
            const HoldBatchMetadata& m = p.hold_meta;
            const int owner_broker = m.broker_id;
            ensure_goi_tracker(owner_broker);
            const uint64_t cumulative_msg_count =
                (owner_broker >= 0 && owner_broker < NUM_MAX_BROKERS)
                    ? (goi_cumulative_tracker[owner_broker] += m.num_msg)
                    : static_cast<uint64_t>(m.num_msg);
            entry->total_order = next_order;
            entry->batch_id = m.batch_id;
            entry->broker_id = static_cast<uint16_t>(m.broker_id);
            entry->epoch_sequenced = m.epoch_created;
            entry->blog_offset = m.log_idx;
            entry->payload_size = static_cast<uint32_t>(m.total_size);
            entry->message_count = static_cast<uint32_t>(m.num_msg);
            entry->num_replicated.store(0, std::memory_order_release);
            entry->client_id = m.client_id;
            entry->client_seq = m.batch_seq;
            entry->pbr_index = m.pbr_absolute_index;
            entry->cumulative_message_count = cumulative_msg_count;
            entry->session_epoch = m.session_epoch;
            // Publish GOI index last so readers never treat a partially-written entry as valid.
            entry->global_seq = batch_index;
            if (ShouldEnableOrder5SessionTestTrace()) {
                const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                LOG(WARNING) << "[ORDER5_TEST_GOI_COMMIT]" << " client=" << m.client_id
                             << " session_epoch=" << m.session_epoch << " batch_seq=" << m.batch_seq
                             << " goi=" << batch_index << " pbr=" << m.pbr_absolute_index
                             << " from_hold=1 wall_ms=" << wall_ms;
            }
            next_order += m.num_msg;
        } else if (p.hdr != nullptr) {
            const int owner_broker = p.broker_id;
            ensure_goi_tracker(owner_broker);
            const uint64_t cumulative_msg_count =
                (owner_broker >= 0 && owner_broker < NUM_MAX_BROKERS)
                    ? (goi_cumulative_tracker[owner_broker] += p.num_msg)
                    : static_cast<uint64_t>(p.num_msg);
            p.hdr->total_order = next_order;
            entry->total_order = next_order;
            entry->batch_id = p.cached_batch_id;
            entry->broker_id = static_cast<uint16_t>(p.broker_id);
            entry->epoch_sequenced = p.epoch_created;
            entry->blog_offset = p.cached_log_idx;
            entry->payload_size = static_cast<uint32_t>(p.cached_total_size);
            entry->message_count = static_cast<uint32_t>(p.num_msg);
            entry->num_replicated.store(0, std::memory_order_release);
            entry->client_id = p.client_id;
            entry->client_seq = p.hdr->batch_seq;
            entry->pbr_index = p.cached_pbr_absolute_index;
            entry->cumulative_message_count = cumulative_msg_count;
            entry->session_epoch = p.session_epoch;
            // Publish GOI index last so readers never treat a partially-written entry as valid.
            entry->global_seq = batch_index;
            if (ShouldEnableOrder5SessionTestTrace()) {
                const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                LOG(WARNING) << "[ORDER5_TEST_GOI_COMMIT]" << " client=" << p.client_id
                             << " session_epoch=" << p.session_epoch
                             << " batch_seq=" << p.hdr->batch_seq << " goi=" << batch_index
                             << " pbr=" << p.cached_pbr_absolute_index
                             << " from_hold=0 wall_ms=" << wall_ms;
            }
            next_order += p.num_msg;
        }
        // [[W1.2]] Empirical per-session in-order commit invariant (W1 item #2): across epoch
        // seals the collector must commit a client's batches in strictly increasing client_seq.
        // A LOWER client_seq committed after a higher one for the same session is a genuine
        // collector reorder and must be surfaced immediately under EMBAR_ASSERT_COMMIT_ORDER.
        // [[OPT-W12]] The commit_order_last_seq_ map update (flat_hash_map find+insert per batch)
        // is purely a debug invariant used by ShouldAssertCommitOrder(). Gate the entire block
        // behind the assert flag so production runs pay zero cost (no map write, no map lookup).
        if (ShouldAssertCommitOrder() && order_ == kOrderStrong && (p.from_hold || p.hdr != nullptr)) {
            const uint64_t w12_cid = entry->client_id;
            const uint64_t w12_seq = entry->client_seq;
            const uint32_t w12_session_epoch =
                p.from_hold ? p.hold_meta.session_epoch : p.session_epoch;
            const uint64_t w12_session_key = MakeSessionKey(w12_cid, w12_session_epoch);
            auto w12_it = commit_order_last_seq_.find(w12_session_key);
            if (w12_it != commit_order_last_seq_.end() && w12_seq <= w12_it->second) {
                order5_commit_order_violations_.fetch_add(1, std::memory_order_relaxed);
                LOG(ERROR) << "[W1.2_COMMIT_ORDER_VIOLATION] client=" << w12_cid
                           << " session_epoch=" << w12_session_epoch
                           << " committed client_seq=" << w12_seq << " <= last=" << w12_it->second
                           << " broker=" << entry->broker_id << " goi_index=" << batch_index;
                CHECK_GT(w12_seq, w12_it->second)
                    << "[W1.2] per-session commit-order invariant violated";
                commit_order_last_seq_[w12_session_key] =
                    std::max<uint64_t>(w12_seq, w12_it->second);
            } else {
                commit_order_last_seq_[w12_session_key] = w12_seq;
            }
        }
        // [[OPT-GOI-FENCE]] LINE1 flush deferred to Pass 2 below.
        // (No per-batch SFENCE here — see Pass 2 comment for the invariant argument.)
        (void)entry;  // suppress unused-variable warning; entry used in Pass 2 via index
    }

    // [[OPT-GOI-FENCE Pass 2]] Batched fence sequence for the entire epoch.
    // [[CXL-1]] requires: all LINE1 fields durable BEFORE LINE0 sentinel. Original code:
    //   per-batch: store_fence → flush_LINE1 → store_fence → flush_LINE0  (2×B SFENCEs)
    // Optimized:  store_fence → flush_all_LINE1 → store_fence → flush_all_LINE0 → store_fence
    //   → 3 total SFENCEs per epoch regardless of batch count.
    // Correctness argument:
    //   (a) The first store_fence drains all field stores from Pass 1 across all entries.
    //       Since entries are GOI array slots written sequentially by one thread, all stores
    //       are already in the store buffer by this point; the SFENCE drains them all at once.
    //   (b) All flush_LINE1 calls (clflushopt, weakly ordered) are then issued. Because SFENCE
    //       precedes them, the lines they evict contain committed data.
    //   (c) The second store_fence serializes all LINE1 clflushopt above against LINE0 below,
    //       preserving the [[CXL-1]] LINE1-before-LINE0 durability ordering globally.
    //   (d) All flush_LINE0 calls (sentinel, includes global_seq) are issued.
    //   (e) Final store_fence ensures LINE0 sentinels are visible before any reader can act.
    // Note: the sentinel (global_seq) being written in Pass 1 and its LINE0 flush deferred
    // to here is safe because no reader outside this thread can observe a partial GOI entry:
    // the sentinel is LINE0[0:8] and is only meaningful after the LINE1 fields are durable.
    CXL::store_fence();  // (a) drain all field stores from Pass 1
    {
        size_t goi_pass2_idx = 0;
        for (const PendingBatch5& p : ready) {
            if (p.skipped || p.is_held_marker) continue;
            const GOIEntry* entry = &goi[base_batch_index_order5 + goi_pass2_idx++];
            CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(entry) + 64);  // (b) LINE1
        }
    }
    CXL::store_fence();  // (c) order all LINE1 evictions before LINE0
    {
        size_t goi_pass2_idx = 0;
        for (const PendingBatch5& p : ready) {
            if (p.skipped || p.is_held_marker) continue;
            GOIEntry* entry = &goi[base_batch_index_order5 + goi_pass2_idx++];
            CXL::flush_cacheline(entry);  // (d) LINE0 (global_seq sentinel) last
        }
    }
    CXL::store_fence();  // (e) sentinels durable before any reader

    if (commit_profile) {
        order5_commit_goi_ns_.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - goi_loop_t_start)
                                      .count()),
            std::memory_order_relaxed);
    }
    const auto export_loop_t_start =
        commit_profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    // 2. Flush shared immutable export descriptors into the per-broker export ring.
    // Non-head brokers must be able to export committed ORDER=5 batches without access to
    // the head sequencer's process-local hold/export map, so every committed batch gets a
    // descriptor written into shared memory keyed by compact export sequence.
    size_t export_next_order = base_order;
    // [[REMOVED: per-batch payload recompute]] This used to re-walk every message's header
    // (RecomputeOrder5BatchSizeFromPayload) to defend against a stale/zero-initialized read of
    // the cached total_size; that walk dominated CommitEpoch before the v2 CXL visibility fix.
    // Now the cached total_size is sourced from the publisher stride computation and flushed as
    // part of the sentinel-gated BatchHeader, so export does not re-derive it.
    for (PendingBatch5& p : ready) {
        if (p.skipped || p.is_held_marker)
            continue;
        const bool from_hold = p.from_hold;
        const HoldBatchMetadata* hold = from_hold ? &p.hold_meta : nullptr;
        const int b = from_hold ? hold->broker_id : p.broker_id;
        const size_t log_idx = from_hold ? hold->log_idx : p.cached_log_idx;
        const uint32_t num_msg = from_hold ? hold->num_msg : p.num_msg;
        const uint64_t pbr_abs = from_hold ? hold->pbr_absolute_index : p.cached_pbr_absolute_index;
        const size_t batch_total_size = from_hold ? hold->total_size : p.cached_total_size;
        const uint64_t batch_id = from_hold ? hold->batch_id : p.cached_batch_id;
        const size_t original_batch_seq = from_hold ? hold->batch_seq : p.batch_seq;
        const size_t client_id = from_hold ? hold->client_id : p.client_id;
        const uint32_t session_epoch = from_hold ? hold->session_epoch : p.session_epoch;
        const uint16_t epoch_created = from_hold ? hold->epoch_created : p.epoch_created;
        const size_t start_logical_offset =
            from_hold ? hold->start_logical_offset : p.cached_start_logical_offset;

        if (b >= 0 && b < NUM_MAX_BROKERS) {
            const size_t batch_headers_offset = tinode_->offsets[b].batch_headers_offset;
            if (batch_headers_offset != 0) {
                BatchHeader* ring_start = reinterpret_cast<BatchHeader*>(
                    reinterpret_cast<uint8_t*>(cxl_addr_) + batch_headers_offset);
                BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
                    reinterpret_cast<uint8_t*>(ring_start) + BATCHHEADERS_SIZE);
                BatchHeader* cur = export_cursor_by_broker_[b];

                // Late topic creation on non-head brokers can leave an old/null cursor.
                // Rebind to ring_start once batch_headers_offset is published.
                if (cur == nullptr || cur < ring_start || cur >= ring_end) {
                    cur = ring_start;
                    export_cursor_by_broker_[b] = cur;
                }

                const uint64_t export_seq = export_sequence_by_broker_[b]++;
                // This slot is an export descriptor, not a producer-published PBR slot.
                // Keep publish_commit uncommitted and flags RETIRED so BrokerScannerWorker5
                // will not re-ingest compacted descriptors when PBR has holes.
                cur->batch_seq = export_seq;
                cur->client_id = static_cast<uint32_t>(client_id);
                cur->broker_id = static_cast<uint32_t>(b);
                cur->epoch_created = epoch_created;
                cur->session_epoch = static_cast<uint16_t>(session_epoch & 0xFFFFu);
                cur->session_epoch32 = session_epoch;
                cur->batch_id = batch_id;
                cur->pbr_absolute_index = pbr_abs;
                cur->start_logical_offset = start_logical_offset;
                cur->batch_off_to_export = 0;
                cur->log_idx = log_idx;
                cur->total_size = static_cast<uint32_t>(batch_total_size);
                cur->num_msg = num_msg;
                cur->total_order = export_next_order;
                cur->ordered = 1;
                cur->batch_complete = 0;
                cur->publish_commit = kBatchHeaderPublishUncommitted;
                cur->flags = kBatchHeaderFlagRetired;
                CXL::store_fence();
                CXL::flush_cacheline(cur);
                CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(cur) + 64);

                BatchHeader* next_cursor = reinterpret_cast<BatchHeader*>(
                    reinterpret_cast<uint8_t*>(cur) + sizeof(BatchHeader));
                if (next_cursor >= ring_end)
                    next_cursor = ring_start;
                export_cursor_by_broker_[b] = next_cursor;
                if (ShouldEnableOrder5Trace() && order_ == 5) {
                    LOG(INFO) << "[ORDER5_TRACE_EXPORT_DESCRIPTOR B" << b << "]"
                              << " export_seq=" << export_seq
                              << " original_batch_seq=" << original_batch_seq << " pbr=" << pbr_abs
                              << " total_order=" << export_next_order << " num_msg=" << num_msg;
                }
            }
        }
        if (p.hdr != nullptr) {
            ClearOrder5PublishState(p.hdr);
            CXL::store_fence();
            CXL::flush_cacheline(p.hdr);
            CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(p.hdr) + 64);
        }
        export_next_order += num_msg;
    }
    if (commit_profile) {
        order5_commit_export_ns_.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - export_loop_t_start)
                                      .count()),
            std::memory_order_relaxed);
    }
    const auto metadata_loop_t_start =
        commit_profile ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

    // 3. Metadata updates (local accumulation)
    goi_idx_order5 = 0;
    next_order = base_order;
    std::array<size_t, NUM_MAX_BROKERS> ordered_increment{};
    std::array<size_t, NUM_MAX_BROKERS> last_ordered_offset{};
    std::array<bool, NUM_MAX_BROKERS> ordered_broker_seen{};
    // Per-client delta accumulated here; flushed to per_client_ordered_ after [PHASE-4].
    absl::flat_hash_map<uint32_t, uint64_t> per_client_delta_epoch;
	absl::flat_hash_map<uint64_t, SessionPublishSnapshot> session_publish_epoch;
	auto record_session_publish = [&](size_t client_id, uint32_t session_epoch, uint64_t batch_seq) {
		if (session_epoch == 0) return;
		const uint64_t session_key = MakeSessionKey(client_id, session_epoch);
		SessionPublishSnapshot& snapshot = session_publish_epoch[session_key];
		snapshot.session_epoch = session_epoch;
		snapshot.expected_seq = std::max(snapshot.expected_seq, batch_seq + 1);
		snapshot.committed_hwm = std::max(snapshot.committed_hwm, batch_seq);
		snapshot.highest_sequenced = std::max(snapshot.highest_sequenced, batch_seq);
	};

	for (PendingBatch5& p : ready) {
		if (p.skipped || p.is_held_marker) continue;
		uint64_t batch_index = base_batch_index_order5 + goi_idx_order5++;

		if (p.from_hold) {
			const HoldBatchMetadata& m = p.hold_meta;
				const int owner_broker = m.broker_id;
				ensure_cv_tracker(owner_broker);
				const uint64_t cumulative_msg_count =
					(owner_broker >= 0 && owner_broker < NUM_MAX_BROKERS)
						? (cv_cumulative_tracker[owner_broker] += m.num_msg)
						: static_cast<uint64_t>(m.num_msg);
				AccumulateCVUpdate(
					static_cast<uint16_t>(m.broker_id), m.pbr_absolute_index,
					cumulative_msg_count, cv_max_cumulative, cv_max_pbr_index);
				if (replication_factor_ > 0) {
					size_t ring_pos =
						goi_timestamp_write_pos_.fetch_add(1, std::memory_order_relaxed) %
						kGOITimestampRingSize;
					goi_timestamps_[ring_pos].goi_index.store(batch_index, std::memory_order_relaxed);
					goi_timestamps_[ring_pos].timestamp_ns.store(SteadyNowNs(), std::memory_order_release);
				}

				int b = m.broker_id;
				if (b >= 0 && b < NUM_MAX_BROKERS) {
					ordered_increment[b] += m.num_msg;
					last_ordered_offset[b] = m.log_idx;
					ordered_broker_seen[b] = true;
					size_t& next_expected = contiguous_consumed_per_broker[b];
					if (m.slot_offset == next_expected ||
					    (next_expected == BATCHHEADERS_SIZE && m.slot_offset == 0)) {
						next_expected = m.slot_offset + sizeof(BatchHeader);
						if (next_expected >= BATCHHEADERS_SIZE) next_expected = BATCHHEADERS_SIZE;
					}
					if (b < static_cast<int>(committed_this_epoch.size())) {
						committed_this_epoch[b]++;
					}
				}
				per_client_delta_epoch[static_cast<uint32_t>(m.client_id)] += m.num_msg;
				record_session_publish(m.client_id, m.session_epoch, m.batch_seq);
				next_order += m.num_msg;
		} else {
			const int owner_broker = p.broker_id;
				ensure_cv_tracker(owner_broker);
				const uint64_t cumulative_msg_count =
					(owner_broker >= 0 && owner_broker < NUM_MAX_BROKERS)
						? (cv_cumulative_tracker[owner_broker] += p.num_msg)
						: static_cast<uint64_t>(p.num_msg);
				AccumulateCVUpdate(
					static_cast<uint16_t>(p.broker_id), p.cached_pbr_absolute_index,
					cumulative_msg_count, cv_max_cumulative, cv_max_pbr_index);
				if (replication_factor_ > 0) {
					size_t ring_pos =
						goi_timestamp_write_pos_.fetch_add(1, std::memory_order_relaxed) %
						kGOITimestampRingSize;
					goi_timestamps_[ring_pos].goi_index.store(batch_index, std::memory_order_relaxed);
					goi_timestamps_[ring_pos].timestamp_ns.store(SteadyNowNs(), std::memory_order_release);
				}

				next_order += p.num_msg;
				int b = p.broker_id;
				per_client_delta_epoch[static_cast<uint32_t>(p.client_id)] += p.num_msg;
				record_session_publish(p.client_id, p.session_epoch, p.batch_seq);
				if (b >= 0 && b < static_cast<int>(committed_this_epoch.size())) {
					ordered_increment[b] += p.num_msg;
					last_ordered_offset[b] = static_cast<size_t>(
						reinterpret_cast<uint8_t*>(p.hdr) - reinterpret_cast<uint8_t*>(cxl_addr_));
					ordered_broker_seen[b] = true;
					size_t& next_expected = contiguous_consumed_per_broker[b];
					if (p.slot_offset == next_expected ||
					    (next_expected == BATCHHEADERS_SIZE && p.slot_offset == 0)) {
						next_expected = p.slot_offset + sizeof(BatchHeader);
						if (next_expected >= BATCHHEADERS_SIZE) next_expected = BATCHHEADERS_SIZE;
					}
					committed_this_epoch[b]++;
				}
		}
	}

	// [PHASE-4] Write accumulated tinode updates
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (!ordered_broker_seen[b]) continue;
		size_t inc = ordered_increment[b];
		tinode_->offsets[b].ordered += inc;
		tinode_->offsets[b].ordered_offset = last_ordered_offset[b];
		sequencer_committed_batches_[b].fetch_add(static_cast<uint64_t>(committed_this_epoch[b]), std::memory_order_relaxed);
		sequencer_committed_msgs_[b].fetch_add(static_cast<uint64_t>(inc), std::memory_order_relaxed);
		CXL::store_fence();
		CXL::flush_cacheline(const_cast<const void*>(
			reinterpret_cast<const volatile void*>(&tinode_->offsets[b].ordered)));
		CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].ordered_offset));
	}
	// Per-client ordered update (one lock acquisition per epoch, not per batch).
	if (!per_client_delta_epoch.empty()) {
		absl::MutexLock lock(&per_client_mu_);
		const uint64_t producing_epoch = cached_epoch_.load(std::memory_order_acquire);
		for (auto& [cid, cnt] : per_client_delta_epoch) {
			per_client_ordered_[cid] += cnt;
			per_client_ordered_epoch_[cid] = producing_epoch;
		}
	}
	for (const auto& [session_key, snapshot] : session_publish_epoch) {
        if (!PublishSessionEntry(session_key, snapshot)) {
          publication_lock.unlock();
          StopForCapacityExhaustion();
          return;
        }
	}
	if (commit_profile) {
		order5_commit_metadata_ns_.fetch_add(
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - metadata_loop_t_start).count()),
			std::memory_order_relaxed);
	}
	if (ShouldEnableFrontierTrace() && order_ == 5) {
		static thread_local uint64_t last_frontier_log_ns = 0;
		const uint64_t now_ns = SteadyNowNs();
		if (now_ns - last_frontier_log_ns >= 1'000'000'000ULL) {
			for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
				if (!ordered_broker_seen[b]) continue;
				LOG(INFO) << "[FRONTIER_TRACE_COMMIT B" << b << "]"
				          << " ordered=" << tinode_->offsets[b].ordered
				          << " ordered_inc=" << ordered_increment[b]
				          << " cv_target_logical=" << cv_max_cumulative[b]
				          << " cv_target_pbr=" << cv_max_pbr_index[b]
				          << " consumed_through=" << contiguous_consumed_per_broker[b];
			}
			last_frontier_log_ns = now_ns;
		}
	}
	if (ShouldEnableOrder5Trace() && order_ == 5 && num_goi_order5 > 0) {
		for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
			if (!ordered_broker_seen[b]) continue;
			LOG(INFO) << "[ORDER5_TRACE_COMMIT B" << b << "]"
			          << " epoch_base_order=" << base_order
			          << " total_msg=" << total_msg
			          << " goi_range=[" << base_batch_index_order5 << ","
			          << (base_batch_index_order5 + num_goi_order5) << ")"
			          << " ordered_inc=" << ordered_increment[b]
			          << " ordered_after=" << tinode_->offsets[b].ordered
			          << " consumed_through=" << contiguous_consumed_per_broker[b]
			          << " committed_batches=" << committed_this_epoch[b];
		}
	}
	if (order_ == 5 && num_goi_order5 > 0) {
		RecordOrder5FlightEvent(
			kOrder5FlightCommit,
			static_cast<uint32_t>(broker_id_),
			base_order,
			total_msg,
			base_batch_index_order5,
			num_goi_order5);
	}
	const auto cv_flush_t_start = commit_profile ? std::chrono::steady_clock::now()
	                                              : std::chrono::steady_clock::time_point{};
	// [PHASE-3] Single fence for all CV updates
	// [[O5-3]] ordered_broker_seen[b] is set under the same broker-range guard that gates
	// AccumulateCVUpdate into cv_max_*, so any broker with a non-zero cv_max_* here is also
	// marked seen; passing it lets FlushAccumulatedCV skip the per-broker flush+fence for
	// untouched brokers.
	FlushAccumulatedCV(cv_max_cumulative, cv_max_pbr_index, &ordered_broker_seen);
	if (commit_profile) {
		order5_commit_cv_flush_ns_.fetch_add(
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - cv_flush_t_start).count()),
			std::memory_order_relaxed);
	}

    // GOI and authoritative session publication are complete. Release before
    // acquiring shard locks during retirement (fence paths hold shard -> gate).
    publication_lock.unlock();
	// Advance consumed_through for all remaining slots that were processed but not ready
	{
		const auto advance_t = phase_now();
		AdvanceConsumedThroughForProcessedSlots(batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch, nullptr, nullptr);
		phase_ns(order5_commit_advance_ns_, advance_t);
	}

	if (num_goi_order5 > 0) {
		const auto enqueue_t = phase_now();
		EnqueueCompletedRange(base_batch_index_order5, base_batch_index_order5 + num_goi_order5);
		StageTrace::Record(
			StageTrace::Stage::GoiCommit,
			0,
			base_batch_index_order5 + num_goi_order5 - 1,
			SteadyNowNs(),
			num_goi_order5);
		phase_ns(order5_commit_enqueue_ns_, enqueue_t);
	}

	{
		const auto tail_t = phase_now();
		for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
			if (!broker_seen_in_epoch[b]) continue;
			size_t val = contiguous_consumed_per_broker[b];
			tinode_->offsets[b].batch_headers_consumed_through = val;
			CXL::store_fence();
			CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].batch_headers_consumed_through));
		}
		CXL::store_fence();
		phase_ns(order5_commit_tailflush_ns_, tail_t);
	}

	if (commit_profile) {
		order5_commit_calls_.fetch_add(1, std::memory_order_relaxed);
		order5_commit_batches_.fetch_add(ready.size(), std::memory_order_relaxed);
		order5_commit_msgs_.fetch_add(total_msg, std::memory_order_relaxed);
		order5_commit_total_ns_.fetch_add(
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now() - commit_t_start).count()),
			std::memory_order_relaxed);
	}
}

void Topic::AssignOrder5(BatchHeader* batch_to_order, size_t start_total_order, BatchHeader*& header_for_sub) {
	int broker = batch_to_order->broker_id;

	size_t num_messages = batch_to_order->num_msg;
	if (num_messages == 0) {
		LOG(WARNING) << "!!!! Orderer5: Dequeued batch with zero messages. Skipping !!!";
		return;
	}

	// Pure batch-level ordering - set only batch total_order, no message-level processing
	batch_to_order->total_order = start_total_order;

	// [[BLOG_HEADER: Batch-level ordering only for ORDER=5]]
	// With BlogMessageHeader emission at publisher, messages already have proper header format.
	// Subscriber reconstructs per-message total_order logically using BatchMetadata.
	// NO per-message CXL writes needed - this eliminates the performance bottleneck.
	//
	// RATIONALE:
	// - Publisher emits BlogMessageHeader with proper format directly
	// - ORDER=5 means all messages in batch get same range [start_total_order, start_total_order + num_msg)
	// - Subscriber uses wire::BatchMetadata to assign per-message total_order logically
	// - Eliminates per-message flush_blog_sequencer_region() that was causing ~50% slowdown
	//
	// DISABLED CODE (was causing performance regression):
	// if (HeaderUtils::ShouldUseBlogHeader() && batch_to_order->num_msg > 0) {
	//     for (size_t i = 0; i < num_messages; ++i) {
	//         msg_hdr->total_order = current_order;
	//         CXL::flush_blog_sequencer_region(msg_hdr);
	//     }
	// }

	// Update ordered count by the number of messages in the batch
	tinode_->offsets[broker].ordered = tinode_->offsets[broker].ordered + num_messages;

	// [[DEVIATION_004]] - Update TInode.offset_entry.ordered_offset (Stage 3: Global Ordering)
	// Paper §3.3 - Sequencer updates ordered_offset after assigning total_order
	// This signals to Stage 4 (Replication) that the batch is ordered
	// Using TInode.offset_entry.ordered_offset instead of Bmeta.seq.ordered_ptr
	size_t ordered_offset = static_cast<size_t>(
		reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(cxl_addr_));
	tinode_->offsets[broker].ordered_offset = ordered_offset;

	const void* seq_region = const_cast<const void*>(static_cast<const volatile void*>(&tinode_->offsets[broker].ordered));
	CXL::store_fence();
	CXL::flush_cacheline(seq_region);

	// [[LIFECYCLE]] Clear flags and batch_complete so scanner skips slot (no VALID); keep num_msg so export can read metadata
	ClearOrder5PublishState(batch_to_order);

	// BatchHeader is 128B (2 cachelines); flush both for non-coherent CXL visibility
	CXL::store_fence();
	CXL::flush_cacheline(batch_to_order);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(batch_to_order) + 64);

	// Single fence for all flushes - reduces fence overhead
	CXL::store_fence();

	// Self-contained export descriptor (same contract as CommitEpoch fast path).
	// Do not set batch_off_to_export -> batch_to_order: that slot is live producer
	// memory and can be reinitialized before the export thread reads it.
	header_for_sub->total_order = start_total_order;
	header_for_sub->num_msg = static_cast<uint32_t>(num_messages);
	header_for_sub->total_size = static_cast<uint32_t>(batch_to_order->total_size);
	header_for_sub->log_idx = batch_to_order->log_idx;
	header_for_sub->pbr_absolute_index = batch_to_order->pbr_absolute_index;
	header_for_sub->batch_id = batch_to_order->batch_id;
	header_for_sub->client_id = batch_to_order->client_id;
	header_for_sub->batch_seq = batch_to_order->batch_seq;
	header_for_sub->broker_id = batch_to_order->broker_id;
	header_for_sub->epoch_created = batch_to_order->epoch_created;
	header_for_sub->batch_off_to_export = 0;
	header_for_sub->ordered = 1;
	header_for_sub->batch_complete = 1;
	header_for_sub->publish_commit = batch_to_order->pbr_absolute_index;
	header_for_sub->flags = kBatchHeaderFlagClaimed | kBatchHeaderFlagValid;
	CXL::store_fence();
	CXL::flush_cacheline(header_for_sub);
	CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(header_for_sub) + 64);
	CXL::store_fence();

	header_for_sub = reinterpret_cast<BatchHeader*>(reinterpret_cast<uint8_t*>(header_for_sub) + sizeof(BatchHeader));

	VLOG(3) << "Orderer5: Assigned batch-level order " << start_total_order
			<< " to batch with " << num_messages << " messages from broker " << broker;
}

}  // namespace Embarcadero
