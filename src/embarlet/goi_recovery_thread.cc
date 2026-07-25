// [[PHASE_3]] Sequencer-driven recovery implementation (§4.2.2)
// Scan-based approach - no hot-path overhead, no queue limits.

#include "topic.h"
#include "common/performance_utils.h"
#include <chrono>
#include <unordered_map>
#include <glog/logging.h>

namespace Embarcadero {

/**
 * GOIRecoveryThread monitors GOI entries for stalled chain replication.
 * Scans recent GOI writes (tracked in lock-free ring buffer) and reports/recovers stalled chains.
 *
 * Design:
 * - Sequencer writes GOI entry, then records (goi_index, timestamp) in lock-free ring
 * - Recovery thread scans ring every 1ms looking for entries older than timeout
 * - If num_replicated < target after timeout, classify as stalled and handle per policy
 *
 * Performance:
 * - NO mutex on sequencer hot path (lock-free ring write)
 * - NO queue size limits (ring wraps, old entries overwritten)
 * - Scan cost: ~60K entries/ms = 60ns per entry (acceptable)
 *
 * @threading Single recovery thread per Topic (sequencer)
 * @ownership Topic::goi_recovery_thread_
 * @paper_ref Design §4.2.2 Sequencer-Driven Replica Recovery (Stalled Chain)
 *
 * Protocol:
 * 1. Failure detection: Scan ring for entries older than timeout with num_replicated < target
 * 2. Stability check: Require unchanged num_replicated across multiple scans before classifying stalled
 * 3. Policy: fail closed. Report the stall but never mutate num_replicated;
 *    timeout is not evidence that a payload reached durable media.
 * 4. Membership notification/re-replication: TODO (not implemented). A future
 *    reconfiguration may advance the token only after a replacement replica
 *    supplies a fresh durable-completion proof.
 */
void Topic::GOIRecoveryThread() {
	LOG(INFO) << "GOIRecoveryThread started for topic " << topic_name_;

	// Recovery monitoring is meaningful only for a multi-replica chain.
	if (replication_factor_ <= 1) {
		LOG(INFO) << "GOIRecoveryThread disabled for topic " << topic_name_
		          << " (replication_factor=" << replication_factor_ << ")";
		return;
	}

	// Get GOI pointer (same as sequencer uses)
	GOIEntry* goi = reinterpret_cast<GOIEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kGOIOffset);

	// Recovery statistics
	uint64_t total_scans = 0;
	uint64_t total_stalls = 0;
	uint64_t last_stats_log_time_ns = 0;
	struct StallState {
		uint32_t last_replicated{0};
		uint64_t first_stalled_ns{0};
		bool reported{false};
	};
	std::unordered_map<uint64_t, StallState> stall_state;

	while (!stop_threads_) {
		auto scan_start = std::chrono::steady_clock::now();
		uint64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
			std::chrono::steady_clock::now().time_since_epoch()).count();

		// Scan the timestamp ring for entries that need checking
		// We scan the entire ring (or up to current write position)
		uint64_t write_pos = goi_timestamp_write_pos_.load(std::memory_order_acquire);
		uint64_t scan_start_pos = (write_pos > kGOITimestampRingSize) ?
		                           (write_pos - kGOITimestampRingSize) : 0;

		for (uint64_t i = scan_start_pos; i < write_pos; i++) {
			size_t ring_pos = i % kGOITimestampRingSize;

			// Read timestamp and GOI index (lock-free)
			uint64_t timestamp_ns = goi_timestamps_[ring_pos].timestamp_ns.load(std::memory_order_acquire);
			uint64_t goi_idx = goi_timestamps_[ring_pos].goi_index.load(std::memory_order_acquire);

			// Skip if entry not initialized (timestamp == 0)
			if (timestamp_ns == 0) continue;

			// Check if entry has timed out
			uint64_t elapsed_ns = now_ns - timestamp_ns;
			if (elapsed_ns < kChainReplicationTimeoutNs) {
				// Not old enough yet
				continue;
			}

			// Entry timed out - check replication progress
			GOIEntry* goi_entry = &goi[goi_idx];

			// Read current num_replicated (replicas increment this after copying data)
			CXL::flush_cacheline(goi_entry);
			CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(goi_entry) + 64);
			CXL::load_fence();
			if (goi_entry->global_seq != goi_idx ||
			    goi_entry->payload_size == 0 ||
			    goi_entry->message_count == 0) {
				// Entry not fully published yet; keep timestamp for next scan.
				continue;
			}
			uint32_t current_replicated = goi_entry->num_replicated.load(std::memory_order_acquire);

			// Check if replication is stuck
			if (current_replicated < static_cast<uint32_t>(replication_factor_)) {
				StallState& st = stall_state[goi_idx];
				if (st.first_stalled_ns == 0 || st.last_replicated != current_replicated) {
					st.last_replicated = current_replicated;
					st.first_stalled_ns = now_ns;
					st.reported = false;
					continue;
				}
				const uint64_t stalled_ns = now_ns - st.first_stalled_ns;
				if (stalled_ns < kChainReplicationTimeoutNs) {
					continue;
				}

				if (!st.reported) {
					st.reported = true;
					total_stalls++;
					LOG(ERROR) << "GOI[" << goi_idx << "] replication stalled at "
					           << current_replicated << "/" << replication_factor_
					           << "; refusing to advance durability without a durable proof";
				}
			} else {
				stall_state.erase(goi_idx);
			}

			// Only a genuine replica completion clears the scan entry.
			if (current_replicated >= static_cast<uint32_t>(replication_factor_)) {
				uint64_t expected = timestamp_ns;
				goi_timestamps_[ring_pos].timestamp_ns.compare_exchange_strong(
					expected, 0, std::memory_order_release, std::memory_order_relaxed);
			}
		}

		total_scans++;

		// Log statistics every 10 seconds
		if (now_ns - last_stats_log_time_ns > 10'000'000'000ULL) {
			LOG(INFO) << "GOIRecoveryThread stats: " << total_scans << " scans, "
			          << total_stalls << " fail-closed stalls since start";
			last_stats_log_time_ns = now_ns;
		}

		// Sleep until next scan interval
		auto elapsed = std::chrono::steady_clock::now() - scan_start;
		auto sleep_duration = std::chrono::nanoseconds(kRecoveryScanIntervalNs) - elapsed;
		if (sleep_duration.count() > 0) {
			std::this_thread::sleep_for(sleep_duration);
		}
	}

	LOG(INFO) << "GOIRecoveryThread stopped for topic " << topic_name_
	          << " (total_scans=" << total_scans
	          << ", total_stalls=" << total_stalls << ")";
}

} // namespace Embarcadero
