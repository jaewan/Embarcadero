// [[PHASE_3]] Sequencer-driven recovery implementation (§4.2.2)
// Scan-based approach - no hot-path overhead, no queue limits.

#include "topic.h"
#include "../common/performance_utils.h"
#include <chrono>
#include <glog/logging.h>

namespace Embarcadero {

/**
 * GOIRecoveryThread monitors GOI entries for stalled chain replication.
 * Scans recent GOI writes (tracked in lock-free ring buffer) and recovers stalled chains.
 *
 * Design:
 * - Sequencer writes GOI entry, then records (goi_index, timestamp) in lock-free ring
 * - Recovery thread scans ring every 1ms looking for entries older than timeout
 * - If num_replicated < target after timeout, increment to unblock chain
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
 * 1. Failure detection: Scan ring for entries older than 10ms with num_replicated < target
 * 2. Fault identification: Read num_replicated; if value is k, replica R_k failed
 * 3. Recovery: Increment num_replicated from k to k+1 (monotonic CXL write + flush)
 * 4. Membership notification: Report failed replica (TODO: integrate with membership service)
 */
void Topic::GOIRecoveryThread() {
	LOG(INFO) << "GOIRecoveryThread started for topic " << topic_name_;

	// Get GOI pointer (same as sequencer uses)
	GOIEntry* goi = reinterpret_cast<GOIEntry*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + Embarcadero::kGOIOffset);

	// Recovery statistics
	uint64_t total_scans = 0;
	uint64_t total_recoveries = 0;
	uint64_t last_stats_log_time_ns = 0;

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
			CXL::load_fence();
			uint32_t current_replicated = goi_entry->num_replicated.load(std::memory_order_acquire);

			// Check if replication is stuck
			if (current_replicated < static_cast<uint32_t>(replication_factor_)) {
				// STALLED CHAIN DETECTED
				// Replica R_k (where k = current_replicated) failed to increment
				uint32_t failed_replica_id = current_replicated;

				LOG(WARNING) << "GOI[" << goi_idx << "] stalled: num_replicated=" << current_replicated
				             << " (expected " << replication_factor_ << ") after "
				             << (elapsed_ns / 1'000'000) << "ms. Replica R_" << failed_replica_id
				             << " failed. Unblocking chain...";

				// RECOVERY: Increment num_replicated to unblock next replica
				// This is safe because:
				// 1. Replica R_k is dead (timeout exceeded)
				// 2. Data is already copied by R_k (it died AFTER copy, BEFORE increment)
				// 3. Monotonic increment: current_replicated → current_replicated + 1
				uint32_t new_value = current_replicated + 1;
				goi_entry->num_replicated.store(new_value, std::memory_order_release);
				CXL::flush_cacheline(goi_entry);
				CXL::store_fence();

				LOG(INFO) << "GOI[" << goi_idx << "] recovered: num_replicated incremented to "
				          << new_value << ". Replica R_" << (new_value) << " unblocked.";

				// TODO: Report failed replica to membership service
				// Example: membership_service_->ReportReplicaFailure(failed_replica_id, goi_idx);
				// For now, just log it. Membership integration is future work.
				LOG(ERROR) << "Replica R_" << failed_replica_id << " declared FAILED "
				           << "(membership notification not implemented yet)";

				total_recoveries++;
			}

			// Clear timestamp only if slot wasn't reused by sequencer (race-safe)
			// Sequencer can overwrite this ring slot with a new (goi_index, timestamp) before we clear.
			// CAS: clear only if value still matches; if sequencer overwrote, CAS fails and we skip.
			uint64_t expected = timestamp_ns;
			goi_timestamps_[ring_pos].timestamp_ns.compare_exchange_strong(
				expected, 0, std::memory_order_release, std::memory_order_relaxed);
		}

		total_scans++;

		// Log statistics every 10 seconds
		if (now_ns - last_stats_log_time_ns > 10'000'000'000ULL) {
			LOG(INFO) << "GOIRecoveryThread stats: " << total_scans << " scans, "
			          << total_recoveries << " recoveries since start";
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
	          << " (total_scans=" << total_scans << ", total_recoveries=" << total_recoveries << ")";
}

} // namespace Embarcadero
