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

// Published-ring scanning and processed-slot frontier advancement.
// State remains owned by Topic; this file adds no independent runtime state.

void Topic::BrokerScannerWorker5(int broker_id) {
	scanner_shutdown_drained_[broker_id].store(false, std::memory_order_release);
	if (TopicDiagnosticsEnabled()) {
		LOG(INFO) << "BrokerScannerWorker5 starting for broker " << broker_id;
	}
	// Wait until tinode of the broker is initialized
	while(tinode_->offsets[broker_id].log_offset == 0){
		std::this_thread::yield();
	}
	if (TopicDiagnosticsEnabled()) {
		LOG(INFO) << "BrokerScannerWorker5 broker " << broker_id << " initialized, starting scan loop";
	}

	BatchHeader* ring_start_default = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id].batch_headers_offset);
	BatchHeader* current_batch_header = ring_start_default;
	uint64_t scanner_slot_seq = 0;

	// [[CRITICAL FIX: Ring Buffer Boundary]] - Calculate ring end to prevent out-of-bounds access
	// Each broker gets BATCHHEADERS_SIZE bytes per topic for batch headers
	BatchHeader* ring_end = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(ring_start_default) + BATCHHEADERS_SIZE);
	// Start scanner from consumed_through (next expected slot), not ring start.
	{
		const void* consumed_addr = const_cast<const void*>(
			reinterpret_cast<const volatile void*>(
				&tinode_->offsets[broker_id].batch_headers_consumed_through));
		CXL::flush_cacheline(consumed_addr);
		CXL::load_fence();
		size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
		if (consumed == BATCHHEADERS_SIZE) consumed = 0;
		if (consumed >= BATCHHEADERS_SIZE || (consumed % sizeof(BatchHeader)) != 0) {
			consumed = 0;
		}
		scanner_slot_seq = static_cast<uint64_t>(consumed / sizeof(BatchHeader));
		current_batch_header = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(ring_start_default) + consumed);
	}

	size_t total_batches_processed = 0;
	auto last_log_time = std::chrono::steady_clock::now();
	auto scanner_heartbeat_time = last_log_time;
	size_t idle_cycles = 0;
	size_t heartbeat_iteration_count = 0;  // [[PERF_OPTIMIZATION]] Counter-based heartbeat to avoid timestamp overhead

	// Scanner only skips truly stuck CLAIMED slots; empty/non-claimed slots are treated as tail.
	// Empty-slot skip without an authoritative producer tail can leap ahead of real writes
	// and permanently miss batches under startup skew.
	const uint64_t kMaxClaimedWaitUs = GetOrder5ClaimedWaitUs();
	uint64_t hole_wait_start_ns = 0;
	size_t holes_skipped = 0;  // Diagnostic: track how many slots were skipped after timeout

	// [[PEER_REVIEW #10]] Named constants for scanner timing
	constexpr size_t kIdleCyclesThreshold = 1024;    // Idle cycles before sleeping
	constexpr size_t kHeartbeatIterations = 1000000; // [[PERF_OPTIMIZATION]] ~1M iterations at 1M/sec = ~1s heartbeat
	// [[Phase 2.1]] Bounded CLAIMED wait: prevent permanent liveness failure if broker dies mid-write
	constexpr uint64_t kClaimedHealthCheckIntervalUs = 1ULL * 1000 * 1000;  // Check every 1s
	const size_t slot_count = BATCHHEADERS_SIZE / sizeof(BatchHeader);
	constexpr size_t kMaxScannerLeadSlots = 256;
	std::vector<uint64_t> last_pushed_pbr_index_per_slot(slot_count, std::numeric_limits<uint64_t>::max());
	// [[SCANNER_RETIRED_SKIP]] Bound CPU/CXL cost if the ring ever reads fully retired
	// (possible only after a complete wrap): after a full lap of retired skips, yield.
	size_t retired_skip_streak = 0;
	uint64_t tr_scan_iter = 0;  // [[TR_TRACE]] scanner observation-pass sampler


	while (!stop_threads_) {
		// [[TR_TRACE]] Sample the scanner observation-pass rate (P). One sample per
		// 4096 loop inspections keeps volume bounded; period P = dt/4096 offline.
		if ((++tr_scan_iter & 0xFFF) == 0) {
			Order5TrTrace::Instance().RecordScanPass(
				static_cast<uint32_t>(broker_id), tr_scan_iter, SteadyNowNs());
		}
		// If sequencer has already advanced the authoritative consumed frontier beyond our current
		// scanner cursor, jump forward. Otherwise the scanner can keep replaying an already-processed
		// prefix whose slots still look locally readable.
		{
			const void* consumed_addr = const_cast<const void*>(
				reinterpret_cast<const volatile void*>(
					&tinode_->offsets[broker_id].batch_headers_consumed_through));
			CXL::flush_cacheline(consumed_addr);
			CXL::load_fence();
			size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
			if (consumed == BATCHHEADERS_SIZE) consumed = 0;
			if (consumed < BATCHHEADERS_SIZE && (consumed % sizeof(BatchHeader)) == 0) {
				const size_t consumed_slot = consumed / sizeof(BatchHeader);
				const size_t current_slot = static_cast<size_t>(scanner_slot_seq % slot_count);
				const size_t forward = (consumed_slot + slot_count - current_slot) % slot_count;
				if (forward > 0 && forward < (slot_count / 2)) {
					current_batch_header = reinterpret_cast<BatchHeader*>(
						reinterpret_cast<uint8_t*>(ring_start_default) + consumed);
					scanner_slot_seq += forward;
					hole_wait_start_ns = 0;
					idle_cycles = 0;
				}
			}
		}

		// [[PERF_CRITICAL]] Conservative prefetching to hide CXL latency (200-500ns)
		// Prefetch 2 slots ahead (256 bytes) - matches original tuning for optimal CXL performance
		{
			BatchHeader* prefetch_target = current_batch_header + 2;
			if (prefetch_target >= ring_end) {
				prefetch_target = ring_start_default + (prefetch_target - ring_end);
			}
			__builtin_prefetch(prefetch_target, 0, 1);  // Read prefetch
		}

		const void* current_batch_header_second_line =
			reinterpret_cast<const uint8_t*>(current_batch_header) + 64;
		CXL::flush_cacheline(current_batch_header_second_line);
		CXL::load_fence();
		uint64_t publish_commit = current_batch_header->publish_commit;

		CXL::flush_cacheline(current_batch_header);
		CXL::load_fence();

		// CLAIMED/VALID are lifecycle hints; publish_commit is the authoritative readiness barrier.
		uint32_t flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
		bool is_claimed = (flags & kBatchHeaderFlagClaimed) != 0;
		bool is_valid = (flags & kBatchHeaderFlagValid) != 0;
		uint64_t pbr_absolute_index = current_batch_header->pbr_absolute_index;
		bool publish_ready = BatchHeaderPublishCommitted(*current_batch_header);

		// num_msg is uint32_t in BatchHeader, so read as volatile uint32_t for type safety.
		// For non-coherent CXL: volatile prevents compiler caching; ACQUIRE doesn't help cache coherence.
		volatile uint32_t num_msg_check = 0;
		if (publish_ready) {
			num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
		}

		// Max reasonable: 2MB batch / 64B min message = ~32k messages, use 100k as safety limit
		constexpr uint32_t MAX_REASONABLE_NUM_MSG = 100000;
		// ORDER=5 only treats a slot as ready after the receiver has published the full header
		// and set batch_complete=1. This avoids exposing partially received payloads to the
		// epoch sequencer now that per-message paddedSize polling is gone from the hot path.
		volatile int batch_complete_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->batch_complete;
		bool batch_ready = publish_ready &&
			(batch_complete_check == 1) &&
			(num_msg_check > 0 && num_msg_check <= MAX_REASONABLE_NUM_MSG);

		// [[PERF_OPTIMIZATION]] Lazy timestamp collection - only when needed for hole timing or heartbeat
		// Removed: std::chrono::steady_clock::now() call from every iteration (~25ns overhead)

		// Per-scanner heartbeat for observability; use iteration counter to avoid timestamp overhead
		if (TopicDiagnosticsEnabled() && ++heartbeat_iteration_count >= kHeartbeatIterations) {
			auto heartbeat_now = std::chrono::steady_clock::now();
			const char* state_str = batch_ready ? "READY" : "EMPTY";
			VLOG(1) << "[Scanner B" << broker_id << "] slot=" << std::hex << current_batch_header << std::dec
				<< " num_msg=" << num_msg_check << " batch_complete=" << batch_complete_check
				<< " flags=0x" << std::hex << flags << std::dec
				<< " publish_commit=" << publish_commit
				<< " pbr_abs=" << pbr_absolute_index
				<< " valid=" << is_valid << " claimed=" << is_claimed
				<< " state=" << state_str << " batches_collected_total=" << total_batches_processed
				<< " holes_skipped=" << holes_skipped;
			scanner_heartbeat_time = heartbeat_now;
			heartbeat_iteration_count = 0;
		}

		if (!batch_ready) {
			// [[SCANNER_RETIRED_SKIP]] A retired slot (sequencer cleared it after commit or
			// hold-insert) never becomes publishable again until ring wrap. Waiting on it
			// deadlocks the scanner while published batches sit unscanned in later slots;
			// its batch is already owned by the sequencer, so passing it is always safe.
			if (!publish_ready && (flags & kBatchHeaderFlagRetired) != 0) {
				BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
					reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
				if (next_batch_header >= ring_end) next_batch_header = ring_start_default;
				current_batch_header = next_batch_header;
				++scanner_slot_seq;
				hole_wait_start_ns = 0;
				idle_cycles = 0;
				if (++retired_skip_streak >= slot_count) {
					retired_skip_streak = 0;
					std::this_thread::yield();
				}
				continue;
			}
			retired_skip_streak = 0;
			// Only truly empty slots (!CLAIMED && not publish-committed) are tail.
			// Anything else is treated as an in-flight or partially retired slot and follows bounded wait.
			if (!is_claimed && !publish_ready) {
				// Re-sync to the authoritative consumed frontier when the scanner lands on an
				// already-processed empty slot after wrap/skip/drain progress elsewhere.
				// [[SCANNER_FORWARD_ONLY_RESYNC]] Jump forward only (same window rule as the
				// top-of-loop jump). consumed_through can point at a slot the sequencer has
				// invalidated but not yet consumed (hold-insert); jumping backward onto it
				// parked the scanner forever and stranded the rest of the ring.
				const void* consumed_addr = const_cast<const void*>(
					reinterpret_cast<const volatile void*>(
						&tinode_->offsets[broker_id].batch_headers_consumed_through));
				CXL::flush_cacheline(consumed_addr);
				CXL::load_fence();
				size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
				if (consumed == BATCHHEADERS_SIZE) consumed = 0;
				if (consumed < BATCHHEADERS_SIZE && (consumed % sizeof(BatchHeader)) == 0) {
					const size_t consumed_slot = consumed / sizeof(BatchHeader);
					const size_t current_slot = static_cast<size_t>(scanner_slot_seq % slot_count);
					const size_t forward = (consumed_slot + slot_count - current_slot) % slot_count;
					if (forward > 0 && forward < (slot_count / 2)) {
						current_batch_header = reinterpret_cast<BatchHeader*>(
							reinterpret_cast<uint8_t*>(ring_start_default) + consumed);
						scanner_slot_seq += forward;
						hole_wait_start_ns = 0;
						idle_cycles = 0;
						continue;
					}
				}
			hole_wait_start_ns = 0;
			++idle_cycles;
			if (idle_cycles >= kIdleCyclesThreshold) {
				std::this_thread::yield();
				idle_cycles = 0;
			} else {
				CXL::cpu_pause();
			}
			continue;
			}

			// CLAIMED but not VALID: bounded wait, then skip only on stuck/failed producer.
			if (hole_wait_start_ns == 0) {
				// [[PERF_OPTIMIZATION]] Lazy timestamp: only call now() when entering wait state
				auto claimed_hole_now = std::chrono::steady_clock::now();
				hole_wait_start_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
					claimed_hole_now.time_since_epoch()).count();
			}

			// [[PERF_OPTIMIZATION]] Lazy timestamp: only call now() when checking timeout
			auto claimed_check_now = std::chrono::steady_clock::now();
			uint64_t claimed_check_now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
				claimed_check_now.time_since_epoch()).count();
			uint64_t waited_us = (claimed_check_now_ns - hole_wait_start_ns) / 1000;
			if (waited_us < kMaxClaimedWaitUs) {
				++idle_cycles;
				if (idle_cycles >= kIdleCyclesThreshold) {
					// [[FIX_CLAIMED_YIELD]] Use yield() for CLAIMED slot waits - allows producer to make progress.
					// CLAIMED slots indicate producer is working, so yield to give producer CPU time.
					std::this_thread::yield();
					idle_cycles = 0;
				} else {
					CXL::cpu_pause();
				}
				continue;
			}

			{
				// [[PERF_OPTIMIZATION]] Reuse the timestamp from timeout check above
				uint64_t claimed_waited_us = (claimed_check_now_ns - hole_wait_start_ns) / 1000;
				if (claimed_waited_us > kClaimedHealthCheckIntervalUs) {
					absl::btree_set<int> alive_brokers;
					GetRegisteredBrokerSet(alive_brokers);
					if (alive_brokers.find(broker_id) == alive_brokers.end()) {
						LOG(ERROR) << "[Scanner B" << broker_id
							<< "] Broker DEAD but slot still CLAIMED — forcing skip (no clear)";
					}
				}
				// [[B0_ACK_FIX]] Same-process re-check before CLAIMED skip: re-read up to 5×50ms.
				if (broker_id == 0) {
					bool recheck_ready = false;
					for (int recheck = 0; recheck < 5 && !recheck_ready; ++recheck) {
						std::this_thread::sleep_for(std::chrono::milliseconds(50));
						// [PHASE-0 FIX] Both cache lines for re-check
						CXL::invalidate_cacheline_for_read(current_batch_header);
						CXL::invalidate_cacheline_for_read(
							reinterpret_cast<const uint8_t*>(current_batch_header) + 64);
						CXL::full_fence();
						uint32_t re_flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
						volatile uint32_t re_num = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
						volatile int re_complete =
							reinterpret_cast<volatile BatchHeader*>(current_batch_header)->batch_complete;
						if ((re_flags & kBatchHeaderFlagClaimed) &&
						    BatchHeaderPublishCommitted(*current_batch_header) &&
						    re_complete == 1 &&
						    re_num > 0 &&
						    re_num <= MAX_REASONABLE_NUM_MSG) {
							recheck_ready = true;
						}
					}
					if (recheck_ready) {
						hole_wait_start_ns = 0;
						continue;  // Next iteration will see ready and process
					}
				}
				// Phase 2: Ultimate timeout or broker dead — force skip to prevent permanent stall.
				// Do NOT clear CLAIMED: if we did, a late producer could write VALID and we'd process
				// the same batch twice on next wrap (Order 2 has no dedup). Skip + advance only; on
				// next wrap slot is still CLAIMED (skip again), or VALID (process), or empty.
				//
				// Re-check the slot before committing to skip: the producer may have completed
				// the write during the final health-check / B0-recheck window above.
				CXL::flush_cacheline(current_batch_header);
				CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(current_batch_header) + 64);
				CXL::load_fence();
				{
					uint32_t re_flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
					volatile uint32_t re_num = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
					volatile int re_complete = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->batch_complete;
					if ((re_flags & kBatchHeaderFlagClaimed) &&
					    BatchHeaderPublishCommitted(*current_batch_header) &&
					    re_complete == 1 &&
					    re_num > 0 && re_num <= MAX_REASONABLE_NUM_MSG) {
						hole_wait_start_ns = 0;
						continue;
					}
				}

				LOG(ERROR) << "[Scanner B" << broker_id << "] CLAIMED slot timeout ("
					<< (kMaxClaimedWaitUs / 1000000) << "s), forcing skip. DATA MAY BE LOST for this batch.";
				LOG(ERROR) << "[ORDER5_CLAIMED_TIMEOUT]"
				           << " broker=" << broker_id
				           << " slot_seq=" << scanner_slot_seq
				           << " slot_off=" << (reinterpret_cast<uint8_t*>(current_batch_header) -
				                               reinterpret_cast<uint8_t*>(ring_start_default))
				           << " client=" << current_batch_header->client_id
				           << " seq=" << current_batch_header->batch_seq
				           << " flags=0x" << std::hex << __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE)
				           << std::dec
				           << " publish_commit=" << current_batch_header->publish_commit
				           << " pbr_abs=" << current_batch_header->pbr_absolute_index
				           << " num_msg=" << current_batch_header->num_msg
				           << " batch_complete=" << current_batch_header->batch_complete;

				// Push SKIP marker with the same robust retry as normal batch push.
				// Timer reset and counter increment are deferred until the push succeeds;
				// on failure the scanner re-enters the timeout branch immediately on the
				// next iteration (no redundant 10s wait) and also re-reads the slot so a
				// late producer completion is not missed.
				{
					size_t slot_offset = reinterpret_cast<uint8_t*>(current_batch_header) -
						reinterpret_cast<uint8_t*>(ring_start_default);
					PendingBatch5 skip_marker;
					skip_marker.hdr = current_batch_header;
					skip_marker.broker_id = broker_id;
					skip_marker.num_msg = 0;
					skip_marker.client_id = current_batch_header->client_id;
					skip_marker.batch_seq = current_batch_header->batch_seq;
					skip_marker.session_epoch = current_batch_header->session_epoch32 != 0
						? current_batch_header->session_epoch32
						: static_cast<uint32_t>(current_batch_header->session_epoch);
					skip_marker.slot_offset = slot_offset;
					skip_marker.epoch_created = static_cast<uint16_t>(current_batch_header->epoch_created);
					skip_marker.cached_batch_id = current_batch_header->batch_id;
					skip_marker.cached_pbr_absolute_index = current_batch_header->pbr_absolute_index;
					skip_marker.cached_log_idx = current_batch_header->log_idx;
					skip_marker.cached_total_size = current_batch_header->total_size;
					skip_marker.cached_start_logical_offset = current_batch_header->start_logical_offset;
					skip_marker.skipped = true;

					bool pushed = false;
					int skip_retry = 0;
					constexpr int kSkipMaxRetries = 10000;
					while (!pushed && skip_retry < kSkipMaxRetries && !stop_threads_) {
						uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
						EpochBuffer5& cur_buf = epoch_buffers_[epoch % 3];
						if (cur_buf.enter_collection(broker_id)) {
							{
								// [[OPT-B]] per-broker lock: no cross-scanner contention
								std::lock_guard<std::mutex> data_lock(cur_buf.per_broker[broker_id].mu);
								cur_buf.per_broker[broker_id].batches.push_back(skip_marker);
							}
							cur_buf.exit_collection(broker_id);
							pushed = true;
						} else {
							EpochBuffer5& next_buf = epoch_buffers_[(epoch + 1) % 3];
							if (next_buf.enter_collection(broker_id)) {
								{
									std::lock_guard<std::mutex> data_lock(next_buf.per_broker[broker_id].mu);
									next_buf.per_broker[broker_id].batches.push_back(skip_marker);
								}
								next_buf.exit_collection(broker_id);
								pushed = true;
							}
						}
						if (pushed) break;
						if ((skip_retry % 256) == 0) {
							uint64_t cur_epoch = epoch_index_.load(std::memory_order_acquire);
							EpochBuffer5& rb = epoch_buffers_[cur_epoch % 3];
							EpochBuffer5::State st = rb.state.load(std::memory_order_acquire);
							if (st == EpochBuffer5::State::IDLE) {
								rb.reset_and_start();
							}
						}
						CXL::cpu_pause();
						++skip_retry;
						if (skip_retry % 100 == 0) {
							std::this_thread::sleep_for(std::chrono::microseconds(10));
						}
					}

						if (pushed) {
							if (ShouldEnableOrder5SessionTestTrace()) {
								LOG(WARNING) << "[ORDER5_TEST_SKIP_MARKER_PUSHED]"
								             << " broker=" << broker_id
								             << " client=" << skip_marker.client_id
								             << " session_epoch=" << skip_marker.session_epoch
								             << " batch_seq=" << skip_marker.batch_seq
								             << " pbr=" << skip_marker.cached_pbr_absolute_index
								             << " num_msg=" << skip_marker.num_msg;
							}
							hole_wait_start_ns = 0;
						++holes_skipped;
						order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
						order5_scanner_timeout_skips_.fetch_add(1, std::memory_order_relaxed);
						BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
							reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
						if (next_batch_header >= ring_end) next_batch_header = ring_start_default;
						current_batch_header = next_batch_header;
						++scanner_slot_seq;
						idle_cycles = 0;
					}
					// On push failure: hole_wait_start_ns stays set, so the next
					// iteration re-enters the timeout branch immediately and re-tries.
				}
				continue;
			}
		}

	// Batch is ready - reset hole wait state
	hole_wait_start_ns = 0;

		// [[PHASE_1B]] Batch ready: push to epoch buffer; EpochSequencerThread will assign order and commit
		VLOG(3) << "BrokerScannerWorker5 [B" << broker_id << "]: Collecting batch with "
		        << num_msg_check << " messages, batch_seq=" << current_batch_header->batch_seq
		        << ", client_id=" << current_batch_header->client_id;
	size_t slot_offset = reinterpret_cast<uint8_t*>(current_batch_header) -
		reinterpret_cast<uint8_t*>(ring_start_default);
	PendingBatch5 pending;
	pending.hdr = current_batch_header;
	pending.broker_id = broker_id;
	pending.num_msg = num_msg_check;
	pending.client_id = current_batch_header->client_id;
	pending.batch_seq = current_batch_header->batch_seq;
	pending.session_epoch = current_batch_header->session_epoch32 != 0
		? current_batch_header->session_epoch32
		: static_cast<uint32_t>(current_batch_header->session_epoch);
	pending.slot_offset = slot_offset;
	pending.epoch_created = static_cast<uint16_t>(current_batch_header->epoch_created);
	// [PHASE-5] Cache metadata while in L1 (just invalidated both cache lines above)
	pending.cached_log_idx = current_batch_header->log_idx;
	pending.cached_total_size = current_batch_header->total_size;
	pending.cached_batch_id = current_batch_header->batch_id;
	pending.cached_pbr_absolute_index = current_batch_header->pbr_absolute_index;
	pending.cached_start_logical_offset = current_batch_header->start_logical_offset;

	// A valid slot stays readable until the sequencer clears it. Under backlog the scanner can
	// wrap and revisit that same header before consumed_through catches up, which re-enqueues the
	// exact same batch many times. Suppress scanner-side duplicates using the published
	// per-broker absolute PBR index recorded in the slot.
	const size_t slot_index = slot_offset / sizeof(BatchHeader);
	if (slot_index < last_pushed_pbr_index_per_slot.size() &&
	    last_pushed_pbr_index_per_slot[slot_index] == pending.cached_pbr_absolute_index) {
		const void* consumed_addr = const_cast<const void*>(
			reinterpret_cast<const volatile void*>(
				&tinode_->offsets[broker_id].batch_headers_consumed_through));
		CXL::flush_cacheline(consumed_addr);
		CXL::load_fence();
		size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
		if (consumed == BATCHHEADERS_SIZE) consumed = 0;
		if (consumed < BATCHHEADERS_SIZE && (consumed % sizeof(BatchHeader)) == 0) {
			current_batch_header = reinterpret_cast<BatchHeader*>(
				reinterpret_cast<uint8_t*>(ring_start_default) + consumed);
			scanner_slot_seq = static_cast<uint64_t>(consumed / sizeof(BatchHeader));
		}
		std::this_thread::yield();
		continue;
	}

	// [[COMPLETE_DATA_LOSS_FIX]] Once batch is ready, it MUST be pushed to epoch buffer
	// before advancing the scanner position. This ensures no batches are lost during shutdown.
	// Spin-wait until we successfully push to an available epoch buffer.
	{
		bool pushed = false;
		int retry_count = 0;
		constexpr int kMaxRetries = 10000;  // ~1 second of retries with cpu_pause

		while (!pushed && retry_count < kMaxRetries) {
			uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
			EpochBuffer5& cur_buf = epoch_buffers_[epoch % 3];
			if (cur_buf.enter_collection(broker_id)) {
				{
					// [[OPT-B]] per-broker lock: no cross-scanner contention
					std::lock_guard<std::mutex> data_lock(cur_buf.per_broker[broker_id].mu);
					cur_buf.per_broker[broker_id].batches.push_back(pending);
				}
				cur_buf.exit_collection(broker_id);
				pushed = true;
			} else {
				// epoch_index_ can move between the scanner's load and enter_collection(). Allow a
				// successor collecting epoch, but do not spray ready batches arbitrarily two epochs ahead.
				EpochBuffer5& next_buf = epoch_buffers_[(epoch + 1) % 3];
				if (next_buf.enter_collection(broker_id)) {
					{
						std::lock_guard<std::mutex> data_lock(next_buf.per_broker[broker_id].mu);
						next_buf.per_broker[broker_id].batches.push_back(pending);
					}
					next_buf.exit_collection(broker_id);
					pushed = true;
				}
			}
			if (pushed) break;
			// Recovery: if no buffer is collecting and current epoch is sealed/caught-up,
			// move epoch_index to an IDLE successor and start collection there.
			if ((retry_count % 256) == 0) {
				uint64_t cur_epoch = epoch_index_.load(std::memory_order_acquire);
				EpochBuffer5& cur_buf = epoch_buffers_[cur_epoch % 3];
				EpochBuffer5::State cur_state = cur_buf.state.load(std::memory_order_acquire);
				if (cur_state == EpochBuffer5::State::IDLE) {
					cur_buf.reset_and_start();
				} else if (cur_state == EpochBuffer5::State::SEALED) {
					uint64_t last_seq = last_sequenced_epoch_.load(std::memory_order_acquire);
					if (last_seq < cur_epoch) {
						// Sequencer has not yet consumed the sealed epoch; do not skip ahead.
						CXL::cpu_pause();
						++retry_count;
						if (retry_count % 100 == 0) {
							std::this_thread::sleep_for(std::chrono::microseconds(10));
						}
						continue;
					}
					for (int step = 1; step <= 2; ++step) {
						uint64_t cand_epoch = cur_epoch + static_cast<uint64_t>(step);
						EpochBuffer5& cand = epoch_buffers_[cand_epoch % 3];
						if (cand.state.load(std::memory_order_acquire) != EpochBuffer5::State::IDLE) continue;
						if (!cand.reset_and_start()) {
							continue;
						}
						uint64_t expected = cur_epoch;
						// [[EPOCH_BUFFER_LOSS_FIX]] On CAS failure do NOT roll the candidate back to
						// IDLE. Between reset_and_start() (state=COLLECTING) and a rollback store,
						// another scanner can enter_collection() and push batches; marking the buffer
						// IDLE strands them, and the next reset_and_start() silently wipes them
						// (observed as a client stream freezing at the wiped batch's seq with the
						// entire tail piling into the hold buffer). Leaving it COLLECTING is safe:
						// buffers are not epoch-tagged, and the driver's rotation seals and the
						// sequencer consumes it within <=2 epochs.
						epoch_index_.compare_exchange_strong(expected, cand_epoch, std::memory_order_release);
						break;
					}
				}
			}
			// No buffer currently collecting; pause/retry.
			CXL::cpu_pause();
			++retry_count;
			// Occasional micro-sleep to prevent busy-wait during shutdown
			if (retry_count % 100 == 0) {
				std::this_thread::sleep_for(std::chrono::microseconds(10));
			}
		}

		if (!pushed) {
			// [PANEL C5] Do not advance: retry same slot next iteration (Property 5: Progress).
			// Advancing would lose the batch (PBR slot never sequenced). Sleep then retry.
			if (TopicDiagnosticsEnabled()) {
				uint64_t cur_epoch = epoch_index_.load(std::memory_order_acquire);
				uint64_t last_seq = last_sequenced_epoch_.load(std::memory_order_acquire);
				auto state_to_cstr = [](EpochBuffer5::State s) {
					switch (s) {
						case EpochBuffer5::State::IDLE: return "IDLE";
						case EpochBuffer5::State::RESETTING: return "RESETTING";
						case EpochBuffer5::State::COLLECTING: return "COLLECTING";
						case EpochBuffer5::State::SEALED: return "SEALED";
					}
					return "UNKNOWN";
				};
				EpochBuffer5::State s0 = epoch_buffers_[0].state.load(std::memory_order_acquire);
				EpochBuffer5::State s1 = epoch_buffers_[1].state.load(std::memory_order_acquire);
				EpochBuffer5::State s2 = epoch_buffers_[2].state.load(std::memory_order_acquire);
				LOG(ERROR) << "[PUSH_FAILURE] BrokerScannerWorker5 [B" << broker_id
				           << "] Failed to push batch after " << kMaxRetries << " retries; will retry slot (batch_seq="
				           << pending.batch_seq << "). Do not advancing scanner."
				           << " epoch_index=" << cur_epoch
				           << " last_sequenced_epoch=" << last_seq
				           << " states=[" << state_to_cstr(s0) << ","
				           << state_to_cstr(s1) << ","
				           << state_to_cstr(s2) << "]";
				static std::atomic<uint64_t> push_retry_count{0};
				push_retry_count.fetch_add(1, std::memory_order_relaxed);
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}
	}
	total_batches_processed++;
	scanner_pushed_batches_[broker_id].fetch_add(1, std::memory_order_relaxed);
	scanner_pushed_msgs_[broker_id].fetch_add(static_cast<uint64_t>(pending.num_msg), std::memory_order_relaxed);

	// [[FAST-SEAL]] Steady-state early-seal: if the system has no pending holds/gaps
	// and the epoch has been open long enough, attempt to seal immediately rather than
	// waiting for the 500µs EpochDriverThread timer. Reduces average ACK latency from
	// epoch/2 (~250µs) to single-digit µs on the co-located steady-state path.
	//
	// Trigger conditions (all must be true):
	//   1. order5_steady_state_ == true (sequencer says no active holds or gaps)
	//   2. Epoch has been COLLECTING for >= kFastSealFloorNs (avoids sealing on first push
	//      before other brokers have had a chance to push their batches)
	//   3. The epoch is still in COLLECTING state (not already sealed by driver)
	//
	// Correctness:
	//   - seal() is a CAS on state COLLECTING→SEALED; if the driver wins, this is a no-op.
	//   - All ordering guarantees (PerSessionPrefix, NoDupCommit, etc.) are enforced by
	//     CommitEpoch + classify_one, which run AFTER seal() quiesces — both unchanged.
	//   - Sealing an epoch early is safe: batches from the same session that land in the
	//     next epoch are still committed in sequence via ClientState5::next_expected.
	//   - If seal() succeeds, we advance epoch_index_ to the successor, unblocking the
	//     EpochSequencerThread busy-spin and triggering CommitEpoch immediately.
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
    // Deterministic batching uses the timer driver's reached seal barrier.
    // This immutable opt-in exists only in fault builds; no production branch.
    static const bool fault_disable_fast_seal = [] {
        const char* value = std::getenv("EMBARCADERO_FAULT_DISABLE_FAST_SEAL");
        return value && std::string(value) == "1";
    }();
    const bool fault_collection_cancelled = !fault::Pause("scanner.after_collect",
            {pending.client_id, pending.session_epoch, pending.batch_seq,
             epoch_index_.load(std::memory_order_acquire), pending.cached_pbr_absolute_index},
            &stop_threads_);
    if (fault_collection_cancelled) stop_threads_.store(true, std::memory_order_release);
    // The batch is already collected. Finish this slot's bookkeeping before
    // entering the normal drain and publishing scanner_shutdown_drained_.
    if (!fault_disable_fast_seal && !fault_collection_cancelled)
#endif
	if (order5_steady_state_.load(std::memory_order_acquire)) {
		const uint64_t pushed_epoch = epoch_index_.load(std::memory_order_acquire);
		EpochBuffer5& pushed_buf = epoch_buffers_[pushed_epoch % 3];
		const uint64_t start_ns =
			pushed_buf.epoch_collection_start_ns.load(std::memory_order_relaxed);
		const uint64_t age_ns = (start_ns > 0) ? (SteadyNowNs() - start_ns) : 0;
		if (age_ns >= kFastSealFloorNs &&
		    pushed_buf.state.load(std::memory_order_acquire) == EpochBuffer5::State::COLLECTING) {
			if (pushed_buf.seal()) {
				// Seal succeeded — advance epoch_index_ to the successor so the
				// EpochSequencerThread picks it up on its next busy-spin iteration.
				const uint64_t next_epoch = pushed_epoch + 1;
				// [[TR_TRACE]] Fast-seal event (steady state only; disabled during a hold).
				Order5TrTrace::Instance().RecordSeal(next_epoch, SteadyNowNs());
				EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];
				if (next_buf.state.load(std::memory_order_acquire) == EpochBuffer5::State::IDLE) {
					next_buf.epoch_collection_start_ns.store(
						SteadyNowNs(), std::memory_order_relaxed);
					next_buf.reset_and_start();
				}
				// CAS epoch_index_ from pushed_epoch → next_epoch (driver may race, that's fine).
				uint64_t expected = pushed_epoch;
				epoch_index_.compare_exchange_strong(
					expected, next_epoch, std::memory_order_release, std::memory_order_relaxed);
				order5_fast_seal_count_.fetch_add(1, std::memory_order_relaxed);
			}
		}
	}
	if (slot_index < last_pushed_pbr_index_per_slot.size()) {
		last_pushed_pbr_index_per_slot[slot_index] = pending.cached_pbr_absolute_index;
	}
	{
		const void* consumed_addr = const_cast<const void*>(
			reinterpret_cast<const volatile void*>(
				&tinode_->offsets[broker_id].batch_headers_consumed_through));
		CXL::flush_cacheline(consumed_addr);
		CXL::load_fence();
		size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
		if (consumed == BATCHHEADERS_SIZE) consumed = 0;
		if (consumed < BATCHHEADERS_SIZE && (consumed % sizeof(BatchHeader)) == 0) {
			const size_t consumed_slot = consumed / sizeof(BatchHeader);
			const size_t next_slot = static_cast<size_t>(scanner_slot_seq % slot_count);
			const size_t lead = (next_slot + slot_count - consumed_slot) % slot_count;
			if (lead > kMaxScannerLeadSlots) {
				std::this_thread::yield();
			}
		}
	}
	idle_cycles = 0;

	// Periodic status logging (VLOG to avoid hot-path overhead during throughput tests)
	auto now = std::chrono::steady_clock::now();
		if (TopicDiagnosticsEnabled() && std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 5) {
			VLOG(2) << "BrokerScannerWorker5 [B" << broker_id << "]: Processed " << total_batches_processed
			        << " batches, current tinode.ordered=" << tinode_->offsets[broker_id].ordered;
			last_log_time = now;
		}

	// Advance to next batch header
	BatchHeader* next_batch_header = reinterpret_cast<BatchHeader*>(
		reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));

	// Ring Buffer Boundary Check
		if (next_batch_header >= ring_end) {
			next_batch_header = ring_start_default;
		}

		current_batch_header = next_batch_header;
		++scanner_slot_seq;
	}

	// [[B0_ACK_FIX]] Drain phase: after stop_threads_, process any remaining ready batches so the
	// last B0 batch (e.g. ~1,927 msgs) that became VALID just before shutdown is not lost.
	// Without this, we exit the main loop without re-checking the current slot; if NetworkManager
	// wrote VALID after our last check, that batch would never be pushed → B0 ordered short.
	constexpr uint64_t kDrainDurationMs = 2000;
	constexpr uint32_t kMaxReasonableNumMsg = 100000;
	auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kDrainDurationMs);
	size_t drain_pushed = 0;
	LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: Starting drain (up to " << kDrainDurationMs << " ms)";

	while (std::chrono::steady_clock::now() < drain_deadline) {
		// [PHASE-0 FIX] Invalidate both cache lines in drain phase
		CXL::flush_cacheline(current_batch_header);
		CXL::flush_cacheline(
			reinterpret_cast<const uint8_t*>(current_batch_header) + 64);
		CXL::load_fence();

		// [[RACE_FIX]] Read flags with ACQUIRE first
		uint32_t flags = __atomic_load_n(&current_batch_header->flags, __ATOMIC_ACQUIRE);
		bool is_valid = (flags & kBatchHeaderFlagValid) != 0;

		volatile uint32_t num_msg_check = 0;
		if (is_valid) {
			num_msg_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->num_msg;
		}

		volatile int batch_complete_check = reinterpret_cast<volatile BatchHeader*>(current_batch_header)->batch_complete;
		bool batch_ready = is_valid &&
			(batch_complete_check == 1) &&
			(num_msg_check > 0 && num_msg_check <= kMaxReasonableNumMsg);

		if (batch_ready) {
			size_t slot_offset = reinterpret_cast<uint8_t*>(current_batch_header) -
				reinterpret_cast<uint8_t*>(ring_start_default);
			PendingBatch5 pending;
			pending.hdr = const_cast<BatchHeader*>(current_batch_header);
			pending.broker_id = broker_id;
			pending.num_msg = num_msg_check;
			pending.client_id = current_batch_header->client_id;
			pending.batch_seq = current_batch_header->batch_seq;
			pending.session_epoch = current_batch_header->session_epoch32 != 0
				? current_batch_header->session_epoch32
				: static_cast<uint32_t>(current_batch_header->session_epoch);
			pending.slot_offset = slot_offset;
			pending.epoch_created = static_cast<uint16_t>(current_batch_header->epoch_created);
			pending.cached_log_idx = current_batch_header->log_idx;
			pending.cached_total_size = current_batch_header->total_size;
			pending.cached_batch_id = current_batch_header->batch_id;
			pending.cached_pbr_absolute_index = current_batch_header->pbr_absolute_index;
			pending.cached_start_logical_offset = current_batch_header->start_logical_offset;

			bool pushed = false;
			for (int r = 0; r < 5000 && !pushed; ++r) {
				uint64_t epoch = epoch_index_.load(std::memory_order_acquire);
				EpochBuffer5& buf = epoch_buffers_[epoch % 3];
				if (buf.enter_collection(broker_id)) {
					{
						// [[OPT-B]] per-broker lock: no cross-scanner contention
						std::lock_guard<std::mutex> data_lock(buf.per_broker[broker_id].mu);
						buf.per_broker[broker_id].batches.push_back(pending);
					}
					buf.exit_collection(broker_id);
					pushed = true;
					drain_pushed++;
					total_batches_processed++;
					scanner_pushed_batches_[broker_id].fetch_add(1, std::memory_order_relaxed);
					scanner_pushed_msgs_[broker_id].fetch_add(static_cast<uint64_t>(pending.num_msg), std::memory_order_relaxed);
				} else {
					std::this_thread::sleep_for(std::chrono::microseconds(10));
				}
			}
			if (!pushed) {
				// [PANEL C5] Do not advance: retry same slot next iteration (Property 5: Progress).
				LOG(WARNING) << "BrokerScannerWorker5 [B" << broker_id << "] drain: failed to push batch_seq="
				             << current_batch_header->batch_seq << " after retries; will retry slot";
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
		}

		BatchHeader* next_batch_header_drain = reinterpret_cast<BatchHeader*>(
			reinterpret_cast<uint8_t*>(current_batch_header) + sizeof(BatchHeader));
		if (next_batch_header_drain >= ring_end) next_batch_header_drain = ring_start_default;
		current_batch_header = next_batch_header_drain;
		++scanner_slot_seq;

		if (!batch_ready) {
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}

	if (drain_pushed > 0) {
		LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: Drain complete, pushed " << drain_pushed
		          << " batches (total_batches_processed=" << total_batches_processed << ")";
	}
	scanner_shutdown_drained_[broker_id].store(true, std::memory_order_release);
}

// Helper method to advance consumed_through for processed slots
// [[WRAP_FIX]] Handle ring wrap: when next_expected==BATCHHEADERS_SIZE, slot 0 is the next expected.
void Topic::AdvanceConsumedThroughForProcessedSlots(
    const std::vector<PendingBatch5>& batch_list,
    std::array<size_t, NUM_MAX_BROKERS>& contiguous_consumed_per_broker,
    const std::array<bool, NUM_MAX_BROKERS>& broker_seen_in_epoch,
    const std::array<uint64_t, NUM_MAX_BROKERS>* cv_max_cumulative,
    const std::array<uint64_t, NUM_MAX_BROKERS>* cv_max_pbr_index) {
    if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
	if (batch_list.empty()) {
		if (cv_max_cumulative && cv_max_pbr_index) {
			FlushAccumulatedCV(*cv_max_cumulative, *cv_max_pbr_index);
		}
		CXL::store_fence();
		return;
	}

	const size_t slot_size = sizeof(BatchHeader);
	const size_t slot_count = BATCHHEADERS_SIZE / slot_size;
	// [E3] Reuse the member scratch instead of allocating a fresh array each call.
	// Safe without a lock: this function runs only on the single EpochSequencerThread
	// (see processed_slots_scratch_ declaration in topic.h). resize() grows only on the
	// first call (slot_count is constant per run); std::fill re-zeroes the seen brokers'
	// regions so the marking loop below sees zeroed slots for every seen broker.
	std::array<std::vector<uint8_t>, NUM_MAX_BROKERS>& processed_slots = processed_slots_scratch_;
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (broker_seen_in_epoch[b]) {
			if (processed_slots[b].size() != slot_count) {
				processed_slots[b].resize(slot_count);
			}
			std::fill(processed_slots[b].begin(), processed_slots[b].end(), static_cast<uint8_t>(0));
		}
	}
	// [[HELD_SLOT_KEYED]] Held-slot detection: small deferred/backpressured lists are swept
	// once per call; the (potentially large) hold_buffer is probed per batch by session key
	// (see IsOrder5SlotHeldInHoldBuffer). The previous full sweep was O(hold_depth) per epoch
	// and dominated the sequencer at 2-proc saturation (~900 ms/s at hold_depth ≈ 27K).
	static const bool heldcheck_validate =
		ReadEnvBoolLenient("EMBARCADERO_ORDER5_HELDCHECK_VALIDATE", false);
	absl::flat_hash_set<std::tuple<int, size_t, uint64_t>> held_slots;
	absl::flat_hash_set<std::tuple<int, size_t, uint64_t>> full_held_slots_for_validation;
	{
		const bool sweep_profile = ShouldEnableOrder5CommitProfile();
		const auto sweep_t = sweep_profile ? std::chrono::steady_clock::now()
		                                   : std::chrono::steady_clock::time_point{};
		CollectOrder5DeferredSlotIdentities(held_slots);
		if (sweep_profile) {
			order5_commit_heldsweep_ns_.fetch_add(
				static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
					std::chrono::steady_clock::now() - sweep_t).count()),
				std::memory_order_relaxed);
		}
	}
	if (heldcheck_validate) {
		CollectOrder5HeldSlotIdentities(full_held_slots_for_validation);
	}
	for (const PendingBatch5& p : batch_list) {
		int b = p.broker_id;
		if (b < 0 || b >= NUM_MAX_BROKERS || !broker_seen_in_epoch[b]) continue;
		if (p.slot_offset >= BATCHHEADERS_SIZE) continue;
		if ((p.slot_offset % slot_size) != 0) continue;
		const bool held =
			held_slots.contains({b, p.slot_offset, p.cached_pbr_absolute_index}) ||
			IsOrder5SlotHeldInHoldBuffer(p);
		if (heldcheck_validate) {
			const bool full_held = full_held_slots_for_validation.contains(
				{b, p.slot_offset, p.cached_pbr_absolute_index});
			if (held != full_held) {
				LOG(ERROR) << "[ORDER5_HELDCHECK_MISMATCH]"
				           << " keyed=" << held << " full=" << full_held
				           << " client=" << p.client_id
				           << " session_epoch=" << p.session_epoch
				           << " batch_seq=" << p.batch_seq
				           << " broker=" << b
				           << " slot_offset=" << p.slot_offset
				           << " pbr=" << p.cached_pbr_absolute_index
				           << " skipped=" << p.skipped
				           << " held_marker=" << p.is_held_marker
				           << " from_hold=" << p.from_hold;
			}
		}
		if (held) continue;
		processed_slots[b][p.slot_offset / slot_size] = 1;
	}
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (!broker_seen_in_epoch[b]) continue;
		size_t next_expected = contiguous_consumed_per_broker[b];
		if (next_expected == BATCHHEADERS_SIZE || next_expected >= BATCHHEADERS_SIZE) {
			next_expected = 0;
		}
		if ((next_expected % slot_size) != 0) continue;
		size_t next_slot = next_expected / slot_size;
		size_t advanced = 0;
		while (advanced < slot_count && processed_slots[b][next_slot]) {
			next_slot = (next_slot + 1) % slot_count;
			advanced++;
		}
		contiguous_consumed_per_broker[b] =
			(next_slot == 0) ? BATCHHEADERS_SIZE : (next_slot * slot_size);
	}
	// Write back consumed_through advances
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (!broker_seen_in_epoch[b]) continue;
		size_t val = contiguous_consumed_per_broker[b];
		tinode_->offsets[b].batch_headers_consumed_through = val;
		CXL::store_fence();
		CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].batch_headers_consumed_through));
	}
	// [BUG_FIX] Flush accumulated CV if provided (for late-arriving/skipped L5 batches)
	if (cv_max_cumulative && cv_max_pbr_index) {
		FlushAccumulatedCV(*cv_max_cumulative, *cv_max_pbr_index);
	}
	CXL::store_fence();
}

}  // namespace Embarcadero
