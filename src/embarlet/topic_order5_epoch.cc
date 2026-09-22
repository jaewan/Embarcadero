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

// Epoch lifecycle, scanner/shard worker orchestration, and sealed-epoch sequencing.
// State remains owned by Topic; this file adds no independent runtime state.

// Sequencer 5: Batch-level sequencer (Phase 1b: epoch pipeline + Level 5 hold buffer)
void Topic::Sequencer5() {
	LOG(INFO) << "Starting Sequencer5 (Phase 1b epoch pipeline) for topic: " << topic_name_;
	commit_order_last_seq_.clear();  // [[W1.2]] fresh per-session commit-order tracking each run
	order5_commit_order_violations_.store(0, std::memory_order_relaxed);
	ResetCompletedRangeQueue();
	order5_recovery_complete_.store(false, std::memory_order_release);
	const uint64_t recovered_next_goi = RecoverSequencer5State();
	global_batch_seq_.store(recovered_next_goi, std::memory_order_release);
	{
		ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
		// Preserve the recovered committed prefix. Wiping to UINT64_MAX forced
		// CommittedSeqUpdaterThread to restart at 0 with a permanent gap.
		const uint64_t recovered_committed =
			(recovered_next_goi == 0) ? UINT64_MAX : (recovered_next_goi - 1);
		control_block->committed_seq.store(recovered_committed, std::memory_order_release);
		CXL::store_fence();
		CXL::flush_cacheline(control_block);
		CXL::store_fence();
	}
	committed_seq_updater_stop_.store(false, std::memory_order_release);
	committed_seq_updater_thread_ = std::thread(&Topic::CommittedSeqUpdaterThread, this);
	InitLevel5Shards();
	{
		absl::MutexLock lock(&per_client_mu_);
		for (const auto& [client_id, count] : recovered_order5_msg_counts_) {
			per_client_ordered_[client_id] = std::max(per_client_ordered_[client_id], count);
		}
	}
	absl::btree_set<int> registered_brokers;
	GetRegisteredBrokerSet(registered_brokers);

	// Epoch increment on sequencer start (§4.2)
	// New sequencer writes epoch+1 to ControlBlock so zombies see new epoch and replicas reject stale entries
	ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
	CXL::flush_cacheline(control_block);
	CXL::load_fence();
	uint64_t prev_epoch = control_block->epoch.load(std::memory_order_acquire);
	uint64_t new_epoch = prev_epoch + 1;
	control_block->epoch.store(new_epoch, std::memory_order_release);
	CXL::store_fence();
	CXL::flush_cacheline(control_block);
	CXL::store_fence();
	cached_epoch_.store(new_epoch, std::memory_order_release);
	last_control_epoch_refresh_ns_.store(SteadyNowNs(), std::memory_order_release);
	RestampRecoveredOrderedEpochs(new_epoch);
	LOG(INFO) << "Sequencer5: ControlBlock.epoch advanced " << prev_epoch << " -> " << new_epoch << " (zombie fencing)";

	global_seq_.store(recovered_global_seq_, std::memory_order_relaxed);
	epoch_index_.store(0, std::memory_order_relaxed);
	last_sequenced_epoch_.store(0, std::memory_order_relaxed);
	epoch_driver_done_.store(false, std::memory_order_release);
	current_epoch_for_hold_.store(0, std::memory_order_release);
	for (int i = 0; i < 3; i++) {
		epoch_buffers_[i].state.store(EpochBuffer5::State::IDLE, std::memory_order_relaxed);
	}
	CHECK(epoch_buffers_[0].reset_and_start());

	// Init per-broker export chain cursors (array: O(1) access in commit path)
	export_cursor_by_broker_.fill(nullptr);
	export_sequence_by_broker_.fill(0);
	for (int broker_id : registered_brokers) {
		InitExportCursorForBroker(broker_id);
	}
	RebuildOrder5ExportDescriptorsFromGOI(recovered_next_goi);

	epoch_driver_thread_ = std::thread(&Topic::EpochDriverThread, this);
	std::thread epoch_sequencer_thread(&Topic::EpochSequencerThread, this);

	// [[FIX: B3=0 ACKs]] Use dynamic scanner management instead of fixed threads
	// This allows late-registering brokers to be scanned
	{
		absl::MutexLock lock(&scanner_management_mu_);
		for (int broker_id : registered_brokers) {
			scanner_shutdown_drained_[broker_id].store(false, std::memory_order_release);
			brokers_with_scanners_.insert(broker_id);
			scanner_threads_.emplace_back(&Topic::BrokerScannerWorker5, this, broker_id);
			LOG(INFO) << "Sequencer5: Started initial BrokerScannerWorker5 for broker " << broker_id;
		}
	}

	epoch_sequencer_thread.join();

	// Join all scanner threads (including any dynamically added ones)
	{
		absl::MutexLock lock(&scanner_management_mu_);
		for (std::thread& t : scanner_threads_) {
			if (t.joinable()) t.join();
		}
	}
	if (epoch_driver_thread_.joinable()) {
		epoch_driver_thread_.join();
	}
	committed_seq_updater_stop_.store(true, std::memory_order_release);
	committed_seq_updater_cv_.notify_all();
	if (committed_seq_updater_thread_.joinable()) {
		committed_seq_updater_thread_.join();
	}
	// [[TR_TRACE]] All ORDER=5 producer threads have joined; write the trace CSV.
	Order5TrTrace::Instance().Flush();
}

void Topic::EpochDriverThread() {
	unsigned epoch_us = kEpochUs;
	if (const char* env = std::getenv("EMBAR_ORDER5_EPOCH_US")) {
		char* end = nullptr;
		unsigned long parsed = std::strtoul(env, &end, 10);
		if (end != env && *end == '\0' && parsed >= 100 && parsed <= 5000) {
			epoch_us = static_cast<unsigned>(parsed);
		} else {
			LOG(WARNING) << "Ignoring invalid EMBAR_ORDER5_EPOCH_US='" << env
			             << "' (expected integer in [100, 5000]); using default " << kEpochUs;
		}
	}
	LOG(INFO) << "EpochDriverThread started (epoch_us=" << epoch_us << ")";
	constexpr uint64_t kNewBrokerCheckInterval = 20000;  // Check every 20000 epochs (~10 s at 500 µs/epoch)
	const auto epoch_duration = std::chrono::microseconds(epoch_us);
	auto next_seal_deadline = std::chrono::steady_clock::now() + epoch_duration;
	uint64_t epoch_count = 0;
	uint64_t cadence_epochs = 0;
	uint64_t cadence_force_epochs = 0;
	auto last_cadence_diag = std::chrono::steady_clock::now();
	const bool order5_phase_diag = (order_ == 5 && ShouldEnableOrder5PhaseDiag());
	auto last_driver_diag = std::chrono::steady_clock::now();
	while (!stop_threads_) {
		// Pace sealing to the configured epoch duration to preserve batching efficiency.
		auto now = std::chrono::steady_clock::now();
		const bool disconnect_drain_active =
			SteadyNowNs() < force_expire_hold_until_ns_.load(std::memory_order_acquire);
		if (!disconnect_drain_active && now < next_seal_deadline) {
			auto remaining = next_seal_deadline - now;
			if (remaining > std::chrono::microseconds(50)) {
				std::this_thread::sleep_for(remaining - std::chrono::microseconds(25));
			} else {
				CXL::cpu_pause();
			}
			continue;
		}
		++cadence_epochs;
		if (disconnect_drain_active) {
			++cadence_force_epochs;
		}
		if (ShouldEnableFrontierTrace()) {
			const auto cadence_now = std::chrono::steady_clock::now();
			if (cadence_now - last_cadence_diag >= std::chrono::seconds(1)) {
				LOG(INFO) << "[FRONTIER_TRACE_EPOCH_CADENCE]"
				          << " configured_epoch_us=" << epoch_us
				          << " epochs=" << cadence_epochs
				          << " force_epochs=" << cadence_force_epochs
				          << " force_deadline_ns="
				          << force_expire_hold_until_ns_.load(std::memory_order_acquire);
				cadence_epochs = 0;
				cadence_force_epochs = 0;
				last_cadence_diag = cadence_now;
			}
		}
		if (disconnect_drain_active) {
			RecordOrder5FlightEvent(
				kOrder5FlightDriver,
				static_cast<uint32_t>(broker_id_),
				epoch_index_.load(std::memory_order_acquire),
				last_sequenced_epoch_.load(std::memory_order_acquire),
				force_expire_hold_until_ns_.load(std::memory_order_acquire),
				1);
		}
		if (disconnect_drain_active) {
			// Disconnect-time tail drain is latency-sensitive: keep retrying seal/advance instead
			// of waiting for the normal epoch cadence. Re-anchor the next normal deadline so we
			// do not run a catch-up burst once the drain window closes.
			next_seal_deadline = now + epoch_duration;
		} else {
			next_seal_deadline += epoch_duration;
			if (now - next_seal_deadline > epoch_duration) {
				// If delayed (e.g., scheduler pause), re-anchor deadline to avoid catch-up bursts.
				next_seal_deadline = now + epoch_duration;
			}
		}

		constexpr int kMaxIterations = 50000; // Prevent infinite spinning
		int iterations = 0;
		while (!stop_threads_ && iterations < kMaxIterations) {
			uint64_t cur = epoch_index_.load(std::memory_order_acquire);
			EpochBuffer5& cur_buf = epoch_buffers_[cur % 3];
			uint64_t last_seq = last_sequenced_epoch_.load(std::memory_order_acquire);
			EpochBuffer5::State cur_state = cur_buf.state.load(std::memory_order_acquire);
			// Recovery path: epoch_index must always point at a collectable epoch. If it points at
			// an IDLE buffer, scanners have nowhere to publish and the sequencer can stall forever.
			if (cur_state == EpochBuffer5::State::IDLE) {
				if (cur_buf.reset_and_start()) {
					break;
				}
				cur_state = cur_buf.state.load(std::memory_order_acquire);
				if (cur_state == EpochBuffer5::State::COLLECTING) {
					break;
				}
			}
			// Recovery path: if current epoch is already SEALED and no index advance occurred,
			// re-establish a COLLECTING buffer so scanners cannot livelock on sealed/idle states.
				if (cur_state == EpochBuffer5::State::SEALED) {
					uint64_t next = cur + 1;
					EpochBuffer5& next_buf = epoch_buffers_[next % 3];
					EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
					if (next_state == EpochBuffer5::State::SEALED && last_seq >= cur) {
						// Tail case: scanners can populate and a prior driver pass can seal the successor
						// before epoch_index advances. Move forward so the sequencer can consume it.
						epoch_index_.store(next, std::memory_order_release);
						break;
					}
					if (next_state == EpochBuffer5::State::COLLECTING) {
						epoch_index_.store(next, std::memory_order_release);
						break;
					}
				if (next_state == EpochBuffer5::State::IDLE && last_seq >= cur) {
					if (!next_buf.reset_and_start()) {
						++iterations;
						continue;
					}
					epoch_index_.store(next, std::memory_order_release);
					break;
				}
			}
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
            if (!fault::Pause("epoch.before_seal", {UINT64_MAX, UINT64_MAX, cur}, &stop_threads_)) {
                // Controller cancellation is a stop request, not permission to
                // bypass final sealing and the epoch_driver_done_ publication.
                stop_threads_.store(true, std::memory_order_release);
                break;
            }
#endif
			if (cur_buf.seal()) {
				uint64_t next = cur + 1;
				// [[TR_TRACE]] Driver seal event -> tau (seal/commit period) distribution.
				Order5TrTrace::Instance().RecordSeal(next, SteadyNowNs());
				EpochBuffer5& next_buf = epoch_buffers_[next % 3];
				// Critical invariant: after sealing current epoch, ensure there is always a
				// COLLECTING successor before returning. Waiting only for IDLE can deadlock
				// if the successor is already COLLECTING.
				int wait_iterations = 0;
					while (!stop_threads_) {
						EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
						if (next_state == EpochBuffer5::State::SEALED) {
							epoch_index_.store(next, std::memory_order_release);
							break;
						}
						if (next_state == EpochBuffer5::State::COLLECTING) {
							epoch_index_.store(next, std::memory_order_release);
							break;
						}
					if (next_state == EpochBuffer5::State::IDLE) {
						if (!next_buf.reset_and_start()) {
							CXL::cpu_pause();
							++wait_iterations;
							if ((wait_iterations % 8192) == 0) {
								std::this_thread::yield();
							}
							continue;
						}
						// [[FAST-SEAL]] Stamp collection start for fast-seal floor check.
						next_buf.epoch_collection_start_ns.store(
							SteadyNowNs(), std::memory_order_relaxed);
						epoch_index_.store(next, std::memory_order_release);
						break;
					}
					CXL::cpu_pause();
					++wait_iterations;
					if ((wait_iterations % 8192) == 0) {
						std::this_thread::yield();
					}
				}
				if (stop_threads_) break;
				break; // Successfully processed epoch
			}
			CXL::cpu_pause();
			iterations++;
		}

			// Yield occasionally to prevent starvation, but much more frequently than 500us
			if (iterations >= kMaxIterations) {
				if (order5_phase_diag) {
					auto now = std::chrono::steady_clock::now();
					if (now - last_driver_diag >= std::chrono::seconds(1)) {
						last_driver_diag = now;
						uint64_t cur = epoch_index_.load(std::memory_order_acquire);
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
						auto active_count = [&](size_t idx) {
							int active = 0;
							for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
								if (epoch_buffers_[idx].broker_active[i].load(std::memory_order_acquire)) {
									++active;
								}
							}
							return active;
						};
						EpochBuffer5::State s0 = epoch_buffers_[0].state.load(std::memory_order_acquire);
						EpochBuffer5::State s1 = epoch_buffers_[1].state.load(std::memory_order_acquire);
						EpochBuffer5::State s2 = epoch_buffers_[2].state.load(std::memory_order_acquire);
						LOG(INFO) << "[ORDER5_PHASE_DIAG driver]"
						          << " cur=" << cur
						          << " last_sequenced=" << last_seq
						          << " state0=" << state_to_cstr(s0) << "(active=" << active_count(0) << ")"
						          << " state1=" << state_to_cstr(s1) << "(active=" << active_count(1) << ")"
						          << " state2=" << state_to_cstr(s2) << "(active=" << active_count(2) << ")";
					}
				}
				std::this_thread::yield();
			}

		// Periodically check for newly registered brokers and spawn scanners
		if (++epoch_count % kNewBrokerCheckInterval == 0) {
			CheckAndSpawnNewScanners();
		}
	}

	// [[TAIL_STALL_FIX]] Seal final epoch on shutdown so batches in COLLECTING state are processed.
	// Without this, when publisher ACK timeout sets stop_threads_, we exit without sealing and
	// ~7,708 messages (last epoch(s)) are never sequenced → ACK shortfall.
	LOG(INFO) << "EpochDriverThread: Sealing final epoch before exit";
	uint64_t final_epoch = epoch_index_.load(std::memory_order_acquire);
	LOG(INFO) << "EpochDriverThread: final_epoch=" << final_epoch
	          << " last_sequenced=" << last_sequenced_epoch_.load(std::memory_order_acquire);
	EpochBuffer5& final_buf = epoch_buffers_[final_epoch % 3];
	if (final_buf.seal()) {
		// After sealing the final steady-state epoch, keep sealing a small bounded number
		// of shutdown collection epochs so batches that become visible during disconnect/drain
		// are not stranded in a trailing COLLECTING buffer.
		LOG(INFO) << "EpochDriverThread: Resetting trailing buffers for late-arriving batches";
		const auto kFinalCollectionBudget =
			(replication_factor_ > 0) ? std::chrono::seconds(12) : std::chrono::milliseconds(1800);
		const auto kFinalSequencerBudget =
			(replication_factor_ > 0) ? std::chrono::seconds(12) : std::chrono::milliseconds(1800);
		const int kMaxTrailingEpochs = (replication_factor_ > 0) ? 64 : 3;
		uint64_t last_sealed_epoch = final_epoch;
		auto collect_deadline = std::chrono::steady_clock::now() + kFinalCollectionBudget;

		for (int step = 0; step < kMaxTrailingEpochs; ++step) {
			uint64_t next_epoch = last_sealed_epoch + 1;
			EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];
			auto buf_avail_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
			while (!next_buf.is_available() &&
			       std::chrono::steady_clock::now() < buf_avail_deadline) {
				std::this_thread::sleep_for(std::chrono::microseconds(10));
			}
			if (!next_buf.is_available()) {
				break;
			}

			if (!next_buf.reset_and_start()) {
				break;
			}
			// [[FAST-SEAL]] Record when this epoch started collecting so scanners can
			// enforce the kFastSealFloorNs minimum age before attempting a fast seal.
			next_buf.epoch_collection_start_ns.store(SteadyNowNs(), std::memory_order_relaxed);
			epoch_index_.store(next_epoch, std::memory_order_release);
			LOG(INFO) << "EpochDriverThread: Trailing epoch " << next_epoch << " ready for collection";

			int quiescent_checks = 0;
			bool saw_activity = false;
			while (std::chrono::steady_clock::now() < collect_deadline) {
				bool any_active = false;
				for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
					if (next_buf.broker_active[i].load(std::memory_order_acquire)) {
						any_active = true;
						saw_activity = true;
						break;
					}
				}
				if (!any_active) {
					if (++quiescent_checks >= 20) break;  // ~2ms quiescent
				} else {
					quiescent_checks = 0;
				}
				std::this_thread::sleep_for(std::chrono::microseconds(100));
			}

			if (!next_buf.seal()) {
				break;
			}

			size_t buffered_batches = 0;
            for (auto& q : next_buf.per_broker) {
                std::lock_guard<std::mutex> lock(q.mu);
                buffered_batches += q.batches.size();
            }
			const size_t held_batches = GetTotalHoldBufferSize();
			LOG(INFO) << "EpochDriverThread: Sealed trailing epoch " << next_epoch
			          << " (buffered_batches=" << buffered_batches
			          << ", held_batches=" << held_batches
			          << ", saw_activity=" << (saw_activity ? "yes" : "no") << ")";
			last_sealed_epoch = next_epoch;

			if (!saw_activity && buffered_batches == 0 && held_batches == 0) {
				break;
			}
		}

		// last_sequenced_epoch_ is the next buffer to extract, not an inclusive
        // commit frontier. Wait past the last successful seal; the sequencer
        // thread is separately joined before Topic teardown completes.
		auto deadline = std::chrono::steady_clock::now() + kFinalSequencerBudget;
		while (last_sequenced_epoch_.load(std::memory_order_acquire) <= last_sealed_epoch &&
		       std::chrono::steady_clock::now() < deadline) {
			std::this_thread::sleep_for(std::chrono::microseconds(100));
		}
		LOG(INFO) << "EpochDriverThread: Final epochs sealed, last_sequenced="
		          << last_sequenced_epoch_.load(std::memory_order_acquire)
                  << " last_sealed=" << last_sealed_epoch;
	} else {
		LOG(WARNING) << "EpochDriverThread: Failed to seal final epoch " << final_epoch;
	}
	epoch_driver_done_.store(true, std::memory_order_release);
}

void Topic::CheckAndSpawnNewScanners() {
	// Get current registered brokers
	absl::btree_set<int> current_brokers;
	GetRegisteredBrokerSet(current_brokers);

	// Check for new brokers and spawn scanners
	absl::MutexLock lock(&scanner_management_mu_);
	for (int broker_id : current_brokers) {
		if (brokers_with_scanners_.find(broker_id) == brokers_with_scanners_.end()) {
			// New broker found - spawn a scanner!
			LOG(INFO) << "[DYNAMIC_SCANNER] Detected newly registered broker " << broker_id
			         << ", spawning BrokerScannerWorker5";

			// Initialize per-broker export cursor for the new broker
			InitExportCursorForBroker(broker_id);
			scanner_shutdown_drained_[broker_id].store(false, std::memory_order_release);

			brokers_with_scanners_.insert(broker_id);
			scanner_threads_.emplace_back(&Topic::BrokerScannerWorker5, this, broker_id);

			LOG(INFO) << "[DYNAMIC_SCANNER] Started BrokerScannerWorker5 for broker " << broker_id
			         << ", total scanners now: " << brokers_with_scanners_.size();
		}
	}
}

bool Topic::HaveAllScannerDrainsCompleted() {
	absl::MutexLock lock(&scanner_management_mu_);
	for (int broker_id : brokers_with_scanners_) {
		if (!scanner_shutdown_drained_[broker_id].load(std::memory_order_acquire)) {
			return false;
		}
	}
	return true;
}

void Topic::EpochSequencerThread() {
	LOG(INFO) << "EpochSequencerThread started for topic: " << topic_name_;
	// [[P0.3]] Reuse epoch working vectors to avoid per-iteration allocations.
	std::vector<PendingBatch5> batch_list;
	std::vector<PendingBatch5> level0, level5, ready_level5, ready;
	std::vector<const PendingBatch5*> by_slot;
	uint64_t last_idle_hold_tick_ns = 0;
	const bool order5_phase_diag = (order_ == 5 && ShouldEnableOrder5PhaseDiag());
	auto last_phase_diag = std::chrono::steady_clock::now();
	auto emit_phase_diag = [&](const char* tag) {
		if (!order5_phase_diag) return;
		auto now = std::chrono::steady_clock::now();
		if (now - last_phase_diag < std::chrono::seconds(1)) return;
		last_phase_diag = now;
		auto state_to_cstr = [](EpochBuffer5::State s) {
			switch (s) {
				case EpochBuffer5::State::IDLE: return "IDLE";
				case EpochBuffer5::State::RESETTING: return "RESETTING";
				case EpochBuffer5::State::COLLECTING: return "COLLECTING";
				case EpochBuffer5::State::SEALED: return "SEALED";
			}
			return "UNKNOWN";
		};
		auto active_count = [&](size_t idx) {
			int active = 0;
			for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
				if (epoch_buffers_[idx].broker_active[i].load(std::memory_order_acquire)) {
					++active;
				}
			}
			return active;
		};
		EpochBuffer5::State s0 = epoch_buffers_[0].state.load(std::memory_order_acquire);
		EpochBuffer5::State s1 = epoch_buffers_[1].state.load(std::memory_order_acquire);
		EpochBuffer5::State s2 = epoch_buffers_[2].state.load(std::memory_order_acquire);
		LOG(INFO) << "[ORDER5_PHASE_DIAG " << tag << "]"
		          << " epoch_index=" << epoch_index_.load(std::memory_order_relaxed)
		          << " last_sequenced=" << last_sequenced_epoch_.load(std::memory_order_relaxed)
		          << " state0=" << state_to_cstr(s0) << "(active=" << active_count(0) << ")"
		          << " state1=" << state_to_cstr(s1) << "(active=" << active_count(1) << ")"
		          << " state2=" << state_to_cstr(s2) << "(active=" << active_count(2) << ")"
		          << " B0(push_b=" << scanner_pushed_batches_[0].load(std::memory_order_relaxed)
		          << ",push_m=" << scanner_pushed_msgs_[0].load(std::memory_order_relaxed)
		          << ",commit_b=" << sequencer_committed_batches_[0].load(std::memory_order_relaxed)
		          << ",commit_m=" << sequencer_committed_msgs_[0].load(std::memory_order_relaxed)
		          << ",ordered=" << tinode_->offsets[0].ordered
		          << ",consumed=" << tinode_->offsets[0].batch_headers_consumed_through << ")"
		          << " B1(push_b=" << scanner_pushed_batches_[1].load(std::memory_order_relaxed)
		          << ",commit_b=" << sequencer_committed_batches_[1].load(std::memory_order_relaxed)
		          << ",ordered=" << tinode_->offsets[1].ordered << ")"
		          << " B2(push_b=" << scanner_pushed_batches_[2].load(std::memory_order_relaxed)
		          << ",commit_b=" << sequencer_committed_batches_[2].load(std::memory_order_relaxed)
		          << ",ordered=" << tinode_->offsets[2].ordered << ")"
		          << " B3(push_b=" << scanner_pushed_batches_[3].load(std::memory_order_relaxed)
		          << ",commit_b=" << sequencer_committed_batches_[3].load(std::memory_order_relaxed)
		          << ",ordered=" << tinode_->offsets[3].ordered << ")";
		// [[TMP_O5_STALL_DIAG]] Per-client hold state so a frozen stream's stuck point is visible:
		// next_expected vs min held seq tells whether the gap batch vanished (min > expected) or
		// the drain is broken (min == expected). Remove before final diff review if too chatty.
		for (const auto& shard_ptr : level5_shards_) {
			if (!shard_ptr) continue;
			std::lock_guard<std::mutex> shard_lock(shard_ptr->mu);
			for (const auto& [cid, cmap] : shard_ptr->hold_buffer) {
				if (cmap.empty()) continue;
				const auto st_it = shard_ptr->client_state.find(cid);
				const auto em_it = shard_ptr->client_emitted_tracker.find(cid);
				LOG(INFO) << "[ORDER5_HOLD_DIAG] client=" << cid
				          << " held=" << cmap.size()
				          << " min_held_seq=" << cmap.begin()->first
				          << " max_held_seq=" << cmap.rbegin()->first
				          << " next_expected="
				          << (st_it != shard_ptr->client_state.end() ? st_it->second.next_expected : 0)
				          << " highest_sequenced="
				          << (st_it != shard_ptr->client_state.end() ? st_it->second.highest_sequenced : 0)
				          << " emitted_contig_max="
				          << (em_it != shard_ptr->client_emitted_tracker.end()
				                  ? em_it->second.contiguous_max : UINT64_MAX)
				          << " deferred=" << shard_ptr->deferred_level5.size();
			}
		}
	};
	// [[ORDER5_COMMIT_PROFILE]] Periodic (1s) breakdown of CommitEpoch's internal wall-clock
	// cost, to find why ACKed throughput can trail raw publish throughput on the ORDER=5
	// EpochSequencerThread's single-threaded commit path. Deltas since the last print, so the
	// numbers reflect recent load rather than a lifetime average that hides transients.
	if (ShouldBypassOrder5SessionFIFOForAblation()) {
		LOG(WARNING)
			<< "[ORDER5_SESSION_FIFO_ABLATION] session FIFO is DISABLED; "
			<< "this run provides global order only and is invalid for correctness/failure claims";
	}
	const bool commit_profile_enabled = (order_ == 5) && ShouldEnableOrder5CommitProfile();
	auto last_commit_profile = std::chrono::steady_clock::now();
	uint64_t last_cp_calls = 0, last_cp_batches = 0, last_cp_msgs = 0;
	uint64_t last_cp_total_ns = 0, last_cp_goi_ns = 0, last_cp_export_ns = 0;
	uint64_t last_cp_metadata_ns = 0, last_cp_cv_ns = 0;
	uint64_t last_cp_guard_ns = 0, last_cp_lock_ns = 0, last_cp_advance_ns = 0;
	uint64_t last_cp_heldsweep_ns = 0, last_cp_enqueue_ns = 0, last_cp_tailflush_ns = 0;
	auto emit_commit_profile = [&]() {
		if (!commit_profile_enabled) return;
		auto now = std::chrono::steady_clock::now();
		if (now - last_commit_profile < std::chrono::seconds(1)) return;
		last_commit_profile = now;
		const uint64_t calls = order5_commit_calls_.load(std::memory_order_relaxed);
		const uint64_t batches = order5_commit_batches_.load(std::memory_order_relaxed);
		const uint64_t msgs = order5_commit_msgs_.load(std::memory_order_relaxed);
		const uint64_t total_ns = order5_commit_total_ns_.load(std::memory_order_relaxed);
		const uint64_t goi_ns = order5_commit_goi_ns_.load(std::memory_order_relaxed);
		const uint64_t export_ns = order5_commit_export_ns_.load(std::memory_order_relaxed);
		const uint64_t metadata_ns = order5_commit_metadata_ns_.load(std::memory_order_relaxed);
		const uint64_t cv_ns = order5_commit_cv_flush_ns_.load(std::memory_order_relaxed);
		const uint64_t guard_ns = order5_commit_guard_ns_.load(std::memory_order_relaxed);
		const uint64_t lock_ns = order5_commit_lock_ns_.load(std::memory_order_relaxed);
		const uint64_t advance_ns = order5_commit_advance_ns_.load(std::memory_order_relaxed);
		const uint64_t heldsweep_ns = order5_commit_heldsweep_ns_.load(std::memory_order_relaxed);
		const uint64_t enqueue_ns = order5_commit_enqueue_ns_.load(std::memory_order_relaxed);
		const uint64_t tailflush_ns = order5_commit_tailflush_ns_.load(std::memory_order_relaxed);
		const uint64_t d_calls = calls - last_cp_calls;
		const uint64_t d_batches = batches - last_cp_batches;
		const uint64_t d_msgs = msgs - last_cp_msgs;
		const uint64_t d_total_ns = total_ns - last_cp_total_ns;
		const uint64_t d_goi_ns = goi_ns - last_cp_goi_ns;
		const uint64_t d_export_ns = export_ns - last_cp_export_ns;
		const uint64_t d_metadata_ns = metadata_ns - last_cp_metadata_ns;
		const uint64_t d_cv_ns = cv_ns - last_cp_cv_ns;
		const uint64_t d_guard_ns = guard_ns - last_cp_guard_ns;
		const uint64_t d_lock_ns = lock_ns - last_cp_lock_ns;
		const uint64_t d_advance_ns = advance_ns - last_cp_advance_ns;
		const uint64_t d_heldsweep_ns = heldsweep_ns - last_cp_heldsweep_ns;
		const uint64_t d_enqueue_ns = enqueue_ns - last_cp_enqueue_ns;
		const uint64_t d_tailflush_ns = tailflush_ns - last_cp_tailflush_ns;
		last_cp_calls = calls; last_cp_batches = batches; last_cp_msgs = msgs;
		last_cp_total_ns = total_ns; last_cp_goi_ns = goi_ns; last_cp_export_ns = export_ns;
		last_cp_metadata_ns = metadata_ns; last_cp_cv_ns = cv_ns;
		last_cp_guard_ns = guard_ns; last_cp_lock_ns = lock_ns; last_cp_advance_ns = advance_ns;
		last_cp_heldsweep_ns = heldsweep_ns; last_cp_enqueue_ns = enqueue_ns;
		last_cp_tailflush_ns = tailflush_ns;
		if (d_calls == 0) return;
		LOG(INFO) << "[ORDER5_COMMIT_PROFILE]"
		          << " calls=" << d_calls
		          << " batches=" << d_batches
		          << " msgs=" << d_msgs
		          << " avg_batch_sz=" << (d_batches ? (d_msgs / d_batches) : 0)
		          << " total_ms=" << (d_total_ns / 1e6)
		          << " goi_ms=" << (d_goi_ns / 1e6)
		          << " export_ms=" << (d_export_ns / 1e6)
		          << " metadata_ms=" << (d_metadata_ns / 1e6)
		          << " cv_flush_ms=" << (d_cv_ns / 1e6)
		          << " guard_ms=" << (d_guard_ns / 1e6)
		          << " lock_ms=" << (d_lock_ns / 1e6)
		          << " advance_ms=" << (d_advance_ns / 1e6)
		          << " heldsweep_ms=" << (d_heldsweep_ns / 1e6)
		          << " enqueue_ms=" << (d_enqueue_ns / 1e6)
		          << " tailflush_ms=" << (d_tailflush_ns / 1e6)
		          << " hold_depth=" << GetTotalHoldBufferSize();
	};
	uint64_t last_idle_diag_ns = 0;
	// last_idle_progress_epoch removed: epoch advancement is not real progress
	std::array<uint64_t, NUM_MAX_BROKERS> last_idle_scanner_pushed_batches{};
	std::array<uint64_t, NUM_MAX_BROKERS> last_idle_sequencer_committed_batches{};
	std::array<uint64_t, NUM_MAX_BROKERS> last_idle_progress_ns{};
	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		last_idle_scanner_pushed_batches[b] =
			scanner_pushed_batches_[b].load(std::memory_order_relaxed);
		last_idle_sequencer_committed_batches[b] =
			sequencer_committed_batches_[b].load(std::memory_order_relaxed);
		if (last_idle_scanner_pushed_batches[b] != 0 ||
		    last_idle_sequencer_committed_batches[b] != 0) {
			last_idle_progress_ns[b] = SteadyNowNs();
		}
	}
	// [[EXPIRY_ARMING_LIVENESS_FIX]] allow_inline_drain=false runs only the progress/stall
	// tracking, force-expiry arming, and diagnostics — callable from the BUSY loop. The
	// inline empty-epoch drain stays idle-only. Previously this tick ran exclusively in the
	// idle branches; a sequencer kept permanently busy (e.g., a large hold buffer makes each
	// epoch pass as slow as the 500 µs seal cadence) could never arm force-expiry, so a
	// gapped stream's hold never drained and the client stalled to ACK timeout.
	auto run_idle_hold_tick = [&](bool allow_inline_drain) {
		if (!Embarcadero::UsesEpochSequencerPath(order_) || seq_type_ != EMBARCADERO) return;
		const uint64_t now_ns = SteadyNowNs();
		if (now_ns - last_idle_hold_tick_ns < 1'000'000ULL) return;
		const bool force_expire_active =
			now_ns < force_expire_hold_until_ns_.load(std::memory_order_acquire);
		const size_t hold_before = GetTotalHoldBufferSize();
		size_t deferred_before = 0;
		for (const auto& shard_ptr : level5_shards_) {
			if (!shard_ptr) continue;
			std::lock_guard<std::mutex> lock(shard_ptr->mu);
			deferred_before += shard_ptr->deferred_level5.size();
		}
		const bool has_held_work = (hold_before > 0 || deferred_before > 0);
		bool observed_progress = false;
		bool has_active_stalled_broker = false;
		uint64_t longest_stall_ns = 0;
		// NOTE: Do NOT count last_sequenced_epoch_ advancement as progress.
		// The epoch driver seals empty epochs every 500µs, so epoch advancement
		// is not evidence of data flow. Only scanner pushes and sequencer commits
		// indicate real progress; otherwise the idle stall timer never fires.
		//
		// [[HOLD_GAP_STALL_FIX 2026-07-13]] When the hold buffer is non-empty,
		// scanner *pushes* alone must not reset the stall timer. Overnight N=2
		// and the instrumented repro showed push_b climbing while commit_b=0 and
		// HOLD_DIAG next_expected=0 with min_held_seq>0 — every late push into the
		// gapped hold reset progress, force_expire stayed 0, and ACKs never moved.
		// With held work, only commits count as progress for expiry arming.
		for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
			const uint64_t pushed = scanner_pushed_batches_[b].load(std::memory_order_relaxed);
			const uint64_t committed =
				sequencer_committed_batches_[b].load(std::memory_order_relaxed);
			const bool commit_moved = (committed != last_idle_sequencer_committed_batches[b]);
			const bool push_moved = (pushed != last_idle_scanner_pushed_batches[b]);
			if (commit_moved || (push_moved && !has_held_work)) {
				observed_progress = true;
				last_idle_progress_ns[b] = now_ns;
			}
			const bool broker_active = (pushed != 0 || committed != 0);
			if (broker_active && last_idle_progress_ns[b] == 0) {
				last_idle_progress_ns[b] = now_ns;
			}
			if (has_held_work && broker_active && last_idle_progress_ns[b] != 0) {
				const uint64_t stalled_ns = now_ns - last_idle_progress_ns[b];
				if (stalled_ns > longest_stall_ns) {
					longest_stall_ns = stalled_ns;
				}
				if (stalled_ns >= GetOrder5IdleForceExpireTriggerNs(replication_factor_ > 0)) {
					has_active_stalled_broker = true;
				}
			}
			last_idle_scanner_pushed_batches[b] = pushed;
			last_idle_sequencer_committed_batches[b] = committed;
		}
		if (has_held_work && has_active_stalled_broker) {
			// Re-arm while any broker remains stalled so the force-expire fence
			// clock stays live until held gaps resolve (B0/B1 progress must not
			// let the window expire out from under B2/B3).
			ArmOrder5ForceExpiryWindow(
				GetOrder5IdleForceExpireWindowNs(replication_factor_ > 0));
		}
		if (has_held_work &&
		    (now_ns - last_idle_diag_ns) > 5'000'000'000ULL) {
			last_idle_diag_ns = now_ns;
			CompletionVectorEntry* cv = reinterpret_cast<CompletionVectorEntry*>(
				reinterpret_cast<uint8_t*>(cxl_addr_) + kCompletionVectorOffset);
			LOG(ERROR) << "[SEQ5_IDLE_DIAG] hold=" << hold_before
			           << " deferred=" << deferred_before
			           << " force_expire=" << (force_expire_active ? 1 : 0)
			           << " progress=" << (observed_progress ? 1 : 0)
			           << " stalled_broker=" << (has_active_stalled_broker ? 1 : 0)
			           << " longest_stall_ms=" << (longest_stall_ns / 1'000'000.0)
			           << " epoch_idx=" << epoch_index_.load(std::memory_order_relaxed)
			           << " last_seq=" << last_sequenced_epoch_.load(std::memory_order_relaxed);
			for (int b = 0; b < 4; ++b) {
				CXL::flush_cacheline(&cv[b]);
				CXL::full_fence();
				uint64_t slo = cv[b].sequencer_logical_offset.load(std::memory_order_acquire);
				LOG(ERROR) << "[SEQ5_IDLE_DIAG] B" << b
				           << " seq_logical=" << slo
				           << " ordered=" << tinode_->offsets[b].ordered
				           << " consumed=" << tinode_->offsets[b].batch_headers_consumed_through
				           << " scan_push_b=" << scanner_pushed_batches_[b].load(std::memory_order_relaxed)
				           << " scan_push_m=" << scanner_pushed_msgs_[b].load(std::memory_order_relaxed)
				           << " seq_commit_b=" << sequencer_committed_batches_[b].load(std::memory_order_relaxed)
				           << " seq_commit_m=" << sequencer_committed_msgs_[b].load(std::memory_order_relaxed);
			}
		}
		if (!has_held_work && !force_expire_active) {
			// Update the gate so the busy-loop caller pays this scan at most once per ms.
			last_idle_hold_tick_ns = now_ns;
			return;
		}
		if (!allow_inline_drain) {
			last_idle_hold_tick_ns = now_ns;
			return;
		}
		std::vector<PendingBatch5> idle_level5_empty;
		std::vector<PendingBatch5> idle_ready_level5;
		ProcessLevel5Batches(idle_level5_empty, idle_ready_level5);
		if (!idle_ready_level5.empty()) {
			std::array<size_t, NUM_MAX_BROKERS> idle_contiguous_consumed{};
			std::array<bool, NUM_MAX_BROKERS> idle_broker_seen{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_max_cumulative{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_max_pbr_index{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_logical_only_cumulative{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_logical_only_pbr_index{};
			for (auto& shard_ptr : level5_shards_) {
				if (!shard_ptr) continue;
				for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS && cum > idle_cv_logical_only_cumulative[bid]) {
						idle_cv_logical_only_cumulative[bid] = cum;
					}
				}
				for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS && pbr > idle_cv_logical_only_pbr_index[bid]) {
						idle_cv_logical_only_pbr_index[bid] = pbr;
					}
				}
			}
			std::vector<const PendingBatch5*> idle_by_slot;
			std::vector<PendingBatch5> idle_batch_list;
			CommitEpoch(idle_ready_level5, idle_by_slot, idle_contiguous_consumed, idle_broker_seen,
			           idle_cv_max_cumulative, idle_cv_max_pbr_index, idle_batch_list,
			           /*is_drain_mode=*/false);
        if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
			FlushAccumulatedCVLogicalOnly(idle_cv_logical_only_cumulative, idle_cv_logical_only_pbr_index);
		} else {
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_logical_only_cumulative{};
			std::array<uint64_t, NUM_MAX_BROKERS> idle_cv_logical_only_pbr_index{};
			for (auto& shard_ptr : level5_shards_) {
				if (!shard_ptr) continue;
				for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS && cum > idle_cv_logical_only_cumulative[bid]) {
						idle_cv_logical_only_cumulative[bid] = cum;
					}
				}
				for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS && pbr > idle_cv_logical_only_pbr_index[bid]) {
						idle_cv_logical_only_pbr_index[bid] = pbr;
					}
				}
			}
			FlushAccumulatedCVLogicalOnly(idle_cv_logical_only_cumulative, idle_cv_logical_only_pbr_index);
		}
		last_idle_hold_tick_ns = now_ns;
	};

	while (!stop_threads_) {
		emit_phase_diag("loop");
		emit_commit_profile();
		// [[EXPIRY_ARMING_LIVENESS_FIX]] Arm/track even while busy (internally 1 ms-gated).
		run_idle_hold_tick(/*allow_inline_drain=*/false);
		uint64_t last = last_sequenced_epoch_.load(std::memory_order_acquire);
		uint64_t current = epoch_index_.load(std::memory_order_acquire);
		if (last >= current) {
			run_idle_hold_tick(/*allow_inline_drain=*/true);
			// [[FIX_SEQUENCER_WAIT_PAUSE]] Replace sleep_for with cpu_pause for epoch waiting.
			CXL::cpu_pause();
			CXL::cpu_pause();
			continue;
		}
		size_t buffer_idx = last % 3;
		EpochBuffer5& buf = epoch_buffers_[buffer_idx];
		if (buf.state.load(std::memory_order_acquire) != EpochBuffer5::State::SEALED) {
			// Tail-progress tick: when no epoch is sealed, still age/expire hold-buffer entries
			// so final batches do not wait indefinitely for new input traffic.
			run_idle_hold_tick(/*allow_inline_drain=*/true);
			// [[FIX_BUFFER_STATE_PAUSE]] Replace sleep_for with cpu_pause for buffer state waiting.
			CXL::cpu_pause();
			CXL::cpu_pause();
			continue;
		}
		// [[EPOCH_BUFFER_LOSS_FIX]] seal() publishes SEALED *before* its collector quiesce
		// wait completes, so a scanner that already passed enter_collection() may still be
		// about to push. Draining before those collectors exit loses their batches (the
		// buffer is drained, the late push lands in it, and the next reset_and_start()
		// wipes it). Consume only once no collector is active; new collectors cannot enter
		// a SEALED buffer, and stragglers hold broker_active until after their push.
		{
			bool collectors_active = false;
			for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
				if (buf.broker_active[i].load(std::memory_order_acquire)) {
					collectors_active = true;
					break;
				}
			}
			if (collectors_active) {
				CXL::cpu_pause();
				continue;
			}
		}
		// Clear only when we have work; avoids redundant clear() in idle loop.
		batch_list.clear();
		level0.clear(); level5.clear(); ready_level5.clear(); ready.clear();
		by_slot.clear();
		// [PHASE-10a] Pre-allocate batch_list
		// [[OPT-B]] Drain each broker queue under its own lock (no shared data_mu).
		{
			size_t total = 0;
			for (auto& pbq : buf.per_broker) {
				std::lock_guard<std::mutex> lk(pbq.mu);
				total += pbq.batches.size();
			}
			batch_list.reserve(total);
		}
		{
			for (auto& pbq : buf.per_broker) {
				std::lock_guard<std::mutex> lk(pbq.mu);
				batch_list.insert(batch_list.end(),
					std::make_move_iterator(pbq.batches.begin()),
					std::make_move_iterator(pbq.batches.end()));
				pbq.batches.clear();
			}
		}

		// [[PERF_OPTIMIZATION]] Minimal prefetching for batch_list processing
		// Only prefetch if batch_list is large enough to benefit (> 1000 entries)
		if (batch_list.size() > 1000) {
			constexpr size_t kPrefetchAhead = 32;  // Look ahead 32 entries (conservative)
			if (kPrefetchAhead < batch_list.size()) {
				__builtin_prefetch(&batch_list[kPrefetchAhead], 0, 1);
			}
		}

		buf.state.store(EpochBuffer5::State::IDLE, std::memory_order_release);
		last_sequenced_epoch_.store(last + 1, std::memory_order_release);
		current_epoch_for_hold_.store(last + 1, std::memory_order_release);

		// [PHASE-2B] Update cached_epoch_ for sequencer use (avoids CXL read)
		{
			ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
			CXL::flush_cacheline(control_block);
			CXL::load_fence();
			cached_epoch_.store(control_block->epoch.load(std::memory_order_acquire), std::memory_order_release);
		}

		// [[DEADLOCK_FIX]] Do not skip when batch_list empty: run ProcessLevel5Batches so hold
		// buffer ageing/expiry runs every epoch. Otherwise held batches never expire.

		// [[PHASE_1A_EPOCH_FENCING]] Sequencer-side epoch validation (§4.2)
		// Reject batches with stale epoch_created so replicas never see zombie-sequencer data
		uint64_t current_epoch = cached_epoch_.load(std::memory_order_acquire);
		if (current_epoch == 0) {
			ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
			CXL::flush_cacheline(control_block);
			CXL::load_fence();
			current_epoch = control_block->epoch.load(std::memory_order_acquire);
			cached_epoch_.store(current_epoch, std::memory_order_release);
		}
		constexpr uint16_t kMaxEpochAge = 2000;  // Design §4.2: reject if entry.epoch < current_epoch - MAX_EPOCH_AGE

		// [PANEL FIX] Do NOT erase stale batches from batch_list! If we erase them, they never reach
		// the consumed_through update logic, so the PBR slots are leaked forever.
		// Instead, mark them as skipped so they flow through to ready and trigger consumed_through advance.
		for (PendingBatch5& p : batch_list) {
			if (p.skipped) continue;
			uint16_t cur = static_cast<uint16_t>(current_epoch & 0xFFFF);
			uint16_t created = p.epoch_created;
			uint16_t age = (cur - created) & 0xFFFF;
			if (age > kMaxEpochAge) {
				LOG_EVERY_N(WARNING, 100) << "Dropping stale batch: age=" << age << " > max=" << kMaxEpochAge
					<< " batch_seq=" << p.batch_seq << " broker=" << p.broker_id;
				p.skipped = true; // Treat as skipped slot (advances consumed_through, no sequencing)
				order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
				order5_stale_epoch_skips_.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// [[DEADLOCK_FIX]] Do not skip when batch_list empty after fencing: run hold buffer processing so expiry advances.

		// [[CONSUMED_THROUGH_FIX]] Advance consumed_through for ALL batches in epoch buffer before processing.
		// ProcessLevel5Batches may hold batches due to gaps, but scanner pushed them to epoch buffer,
		// so sequencer must advance consumed_through to allow ring to drain and prevent deadlock.
		// [PHASE-3] Replace hash maps with arrays for better performance
		std::array<size_t, NUM_MAX_BROKERS> contiguous_consumed_per_broker{};
		std::array<bool, NUM_MAX_BROKERS> broker_seen_in_epoch{};
		for (const PendingBatch5& p : batch_list) {
			int b = p.broker_id;
			if (b < 0 || b >= NUM_MAX_BROKERS) continue;
			if (broker_seen_in_epoch[b]) continue;
			const volatile void* addr = reinterpret_cast<const volatile void*>(&tinode_->offsets[b].batch_headers_consumed_through);
			CXL::flush_cacheline(const_cast<const void*>(addr));
			CXL::load_fence();
			size_t initial = tinode_->offsets[b].batch_headers_consumed_through;
			// [[SENTINEL]] BATCHHEADERS_SIZE means "all slots free" (topic_manager.cc); for min-contiguous
			// we need "next expected = slot 0", so convert sentinel to 0. Otherwise slot_offset (0,64,...)
			// never equals BATCHHEADERS_SIZE and consumed_through would never advance → no backpressure.
			if (initial == BATCHHEADERS_SIZE) {
				initial = 0;
			}
			contiguous_consumed_per_broker[b] = initial;
			broker_seen_in_epoch[b] = true;
		}

		// Partition Level 0 (client_id==0) vs Level 5 (client_id!=0)
		// Skipped markers now carry real client_id/batch_seq and go through level5 so hold buffer can advance (§3.2)
		for (PendingBatch5& p : batch_list) {
			if (p.client_id == 0) {
				level0.push_back(std::move(p));
			} else {
				level5.push_back(std::move(p));
			}
		}

			// Process Level 5: hold buffer + gap timeout (§3.2)
			ProcessLevel5Batches(level5, ready_level5);

		// Merge Level 0 + Level 5 ready; preserve order for stable commit (we sort by broker+slot later)
		if (level5.empty() && ready_level5.empty()) {
			ready = std::move(level0);
		} else {
			ready.reserve(level0.size() + ready_level5.size());
			for (PendingBatch5& p : level0) ready.push_back(std::move(p));
			for (PendingBatch5& p : ready_level5) ready.push_back(std::move(p));
		}

		// [PHASE-3] Accumulate CV updates locally; merge Level 5 shard CV updates
		std::array<uint64_t, NUM_MAX_BROKERS> cv_max_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> cv_max_pbr_index{};
		std::array<uint64_t, NUM_MAX_BROKERS> cv_logical_only_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> cv_logical_only_pbr_index{};

		for (auto& shard_ptr : level5_shards_) {
			if (!shard_ptr) continue;
			for (const auto& [bid, cum] : shard_ptr->cv_max_cumulative) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (cum > cv_max_cumulative[bid]) cv_max_cumulative[bid] = cum;
				}
			}
			for (const auto& [bid, pbr] : shard_ptr->cv_max_pbr_index) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (pbr > cv_max_pbr_index[bid]) cv_max_pbr_index[bid] = pbr;
				}
			}
			for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (cum > cv_logical_only_cumulative[bid]) cv_logical_only_cumulative[bid] = cum;
				}
			}
			for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (pbr > cv_logical_only_pbr_index[bid]) cv_logical_only_pbr_index[bid] = pbr;
				}
			}
		}

		if (ready.empty()) {
			// No ready batches, but we still need to advance consumed_through for all processed slots
			// to prevent ring deadlock. Advance consumed_through for all slots in batch_list.
			AdvanceConsumedThroughForProcessedSlots(batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch, &cv_max_cumulative, &cv_max_pbr_index);
			FlushAccumulatedCVLogicalOnly(cv_logical_only_cumulative, cv_logical_only_pbr_index);
			continue;
		}

		// Call unified commit logic
		CommitEpoch(ready, by_slot, contiguous_consumed_per_broker, broker_seen_in_epoch,
		           cv_max_cumulative, cv_max_pbr_index, batch_list, /*is_drain_mode=*/false);
        if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
		FlushAccumulatedCVLogicalOnly(cv_logical_only_cumulative, cv_logical_only_pbr_index);

		// [[FAST-SEAL]] After a clean commit, update the steady-state flag.
		// Uses the lock-free order5_total_hold_size_ counter rather than acquiring
		// any shard mutex — avoids contention with ProcessLevel5BatchesShard.
		// Steady-state = no held batches anywhere across all shards.
		order5_steady_state_.store(
			order5_total_hold_size_.load(std::memory_order_acquire) == 0,
			std::memory_order_release);
		}

	// [[TAIL_STALL_FIX]] Drain remaining sealed epochs before exit.
	LOG(INFO) << "EpochSequencerThread: Draining remaining sealed epochs before exit";
	// [PANEL FIX] Increase deadline to 15s to outlive EpochDriverThread's 6s wait + sealing time
	auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
	while (std::chrono::steady_clock::now() < drain_deadline) {
		uint64_t last = last_sequenced_epoch_.load(std::memory_order_acquire);
		uint64_t current = epoch_index_.load(std::memory_order_acquire);
        // Shutdown can seal the current epoch without opening a successor.
        // Consume that SEALED buffer before waiting for driver_done, otherwise
        // driver and sequencer wait on each other until the shutdown budget.
        if (!epoch_buffers_[current % 3].CanDrainAt(last, current)) {
			// [PANEL FIX] Only exit if EpochDriverThread has finished sealing everything.
			// Otherwise, wait for it to seal the final late-arriving epoch.
			if (epoch_driver_done_.load(std::memory_order_acquire)) {
				EpochBuffer5& cur_buf = epoch_buffers_[current % 3];
                // Consuming a sealed current epoch leaves it IDLE. Keep a
                // collection destination available until scanner drain ends.
                if (last == current + 1 && cur_buf.is_available() &&
                    !HaveAllScannerDrainsCompleted()) {
                    EpochBuffer5& next_buf = epoch_buffers_[last % 3];
                    if (next_buf.reset_and_start()) {
                        next_buf.epoch_collection_start_ns.store(SteadyNowNs(), std::memory_order_relaxed);
                        epoch_index_.store(last, std::memory_order_release);
                    }
                    continue;
                }
                if (cur_buf.state.load(std::memory_order_acquire) == EpochBuffer5::State::COLLECTING) {
                    if (!cur_buf.seal()) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                        continue;
                    }
                    // CanDrainAt consumes the sealed current directly. Do not
                    // create an empty successor here: the IDLE branch opens one
                    // only while scanners still need a collection destination.
                    if (ShouldEnableOrder5Trace() && order_ == 5) {
                        LOG(INFO) << "[ORDER5_TRACE_DRAIN_SEAL]"
                                  << " sealed_epoch=" << current
                                  << " scanners_shutdown_pending="
                                  << (!HaveAllScannerDrainsCompleted() ? 1 : 0);
                    }
                    continue;
                }
				if (!HaveAllScannerDrainsCompleted()) {
					std::this_thread::sleep_for(std::chrono::microseconds(100));
					continue;
				}
				// [[B0_TAIL_FIX]] Force one last hold/deferred drain so shutdown does not strand
				// tail batches after scanners have already stopped producing new input.
				size_t hb = GetTotalHoldBufferSize();
				bool has_deferred_level5 = false;
				for (auto& shard_ptr : level5_shards_) {
					if (!shard_ptr) continue;
					std::lock_guard<std::mutex> lock(shard_ptr->mu);
					if (!shard_ptr->deferred_level5.empty()) {
						has_deferred_level5 = true;
						break;
					}
				}
					if (hb > 0 || has_deferred_level5) {
						LOG(INFO) << "EpochSequencerThread: Final hold/deferred drain (hold_size="
						          << hb << ", has_deferred=" << (has_deferred_level5 ? 1 : 0)
						          << ") before exit";
						force_expire_hold_until_ns_.store(
							SteadyNowNs() + 20'000'000ULL, std::memory_order_release);
						std::vector<PendingBatch5> level5_empty, ready_level5;
						ProcessLevel5Batches(level5_empty, ready_level5);
						if (!ready_level5.empty()) {
							std::array<size_t, NUM_MAX_BROKERS> drain_contiguous_consumed{};
							std::array<bool, NUM_MAX_BROKERS> drain_broker_seen{};
							for (const PendingBatch5& p : ready_level5) {
								int b = p.from_hold ? p.hold_meta.broker_id : p.broker_id;
								if (b < 0 || b >= NUM_MAX_BROKERS || drain_broker_seen[b]) continue;
								const volatile void* addr = reinterpret_cast<const volatile void*>(
									&tinode_->offsets[b].batch_headers_consumed_through);
								CXL::flush_cacheline(const_cast<const void*>(addr));
								CXL::load_fence();
								size_t initial = tinode_->offsets[b].batch_headers_consumed_through;
								if (initial == BATCHHEADERS_SIZE) initial = 0;
								drain_contiguous_consumed[b] = initial;
								drain_broker_seen[b] = true;
							}
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_cumulative{};
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_pbr_index{};
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_cumulative{};
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_pbr_index{};
							for (auto& shard_ptr : level5_shards_) {
								if (!shard_ptr) continue;
								for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
									if (bid >= 0 && bid < NUM_MAX_BROKERS &&
									    cum > drain_cv_logical_only_cumulative[bid]) {
										drain_cv_logical_only_cumulative[bid] = cum;
									}
								}
								for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
									if (bid >= 0 && bid < NUM_MAX_BROKERS &&
									    pbr > drain_cv_logical_only_pbr_index[bid]) {
										drain_cv_logical_only_pbr_index[bid] = pbr;
									}
								}
							}
							std::vector<const PendingBatch5*> drain_by_slot;
							std::vector<PendingBatch5> empty_batch_list;
							CommitEpoch(ready_level5, drain_by_slot, drain_contiguous_consumed, drain_broker_seen,
							           drain_cv_max_cumulative, drain_cv_max_pbr_index,
							           empty_batch_list, /*is_drain_mode=*/true);
        if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
							FlushAccumulatedCVLogicalOnly(
								drain_cv_logical_only_cumulative, drain_cv_logical_only_pbr_index);
						}
						else {
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_cumulative{};
							std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_pbr_index{};
							for (auto& shard_ptr : level5_shards_) {
								if (!shard_ptr) continue;
								for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
									if (bid >= 0 && bid < NUM_MAX_BROKERS &&
									    cum > drain_cv_logical_only_cumulative[bid]) {
										drain_cv_logical_only_cumulative[bid] = cum;
									}
								}
								for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
									if (bid >= 0 && bid < NUM_MAX_BROKERS &&
									    pbr > drain_cv_logical_only_pbr_index[bid]) {
										drain_cv_logical_only_pbr_index[bid] = pbr;
									}
								}
							}
							FlushAccumulatedCVLogicalOnly(
								drain_cv_logical_only_cumulative, drain_cv_logical_only_pbr_index);
						}
						}
				LOG(INFO) << "EpochSequencerThread: Caught up (last=" << last << " current=" << current
				          << ") and driver done, exiting drain loop";
				break;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			continue;
		}
		size_t buffer_idx = last % 3;
		std::vector<PendingBatch5> batch_list;
		EpochBuffer5& buf = epoch_buffers_[buffer_idx];
        // seal() may still be waiting or rolling SEALED back to COLLECTING.
        // Serialize shutdown extraction with that decision so rollback cannot
        // resurrect a buffer after its records have been moved and it is IDLE.
        std::unique_lock<std::mutex> sealed_ownership(buf.seal_mutex);
		if (buf.state.load(std::memory_order_acquire) != EpochBuffer5::State::SEALED) {
			std::this_thread::sleep_for(std::chrono::microseconds(100));
			continue;
		}
		// [[EPOCH_BUFFER_LOSS_FIX]] Same quiesce requirement as the main consume loop:
		// do not drain a SEALED buffer while a collector is still active (its push would
		// land in a drained buffer and be wiped by the next reset_and_start()).
		{
			bool collectors_active = false;
			for (int i = 0; i < NUM_MAX_BROKERS; ++i) {
				if (buf.broker_active[i].load(std::memory_order_acquire)) {
					collectors_active = true;
					break;
				}
			}
			if (collectors_active) {
				std::this_thread::sleep_for(std::chrono::microseconds(10));
				continue;
			}
		}
		// [[OPT-B]] Drain each broker queue under its own lock.
		{
			size_t total = 0;
			for (auto& pbq : buf.per_broker) {
				std::lock_guard<std::mutex> lk(pbq.mu);
				total += pbq.batches.size();
			}
			batch_list.reserve(total);
		}
		LOG(INFO) << "EpochSequencerThread Drain: Processing epoch " << last << " (buffer " << buffer_idx << ")";
		{
			for (auto& pbq : buf.per_broker) {
				std::lock_guard<std::mutex> lk(pbq.mu);
				batch_list.insert(batch_list.end(),
					std::make_move_iterator(pbq.batches.begin()),
					std::make_move_iterator(pbq.batches.end()));
				pbq.batches.clear();
			}
		}
		buf.state.store(EpochBuffer5::State::IDLE, std::memory_order_release);
		last_sequenced_epoch_.store(last + 1, std::memory_order_release);
		current_epoch_for_hold_.store(last + 1, std::memory_order_release);
        sealed_ownership.unlock();

		ControlBlock* control_block = reinterpret_cast<ControlBlock*>(cxl_addr_);
		CXL::flush_cacheline(control_block);
		CXL::load_fence();
		uint64_t current_epoch = control_block->epoch.load(std::memory_order_acquire);
		constexpr uint16_t kMaxEpochAge = 2000;
		// [[FIX_DRAIN_EPOCH_FENCING]] Mark stale batches as skipped instead of erasing them.
		// Erasing causes PBR slot leaks since slots are never freed. Use skip logic like main loop.
		for (PendingBatch5& p : batch_list) {
			if (p.skipped) continue;
			uint16_t cur = static_cast<uint16_t>(current_epoch & 0xFFFF);
			uint16_t created = p.epoch_created;
			uint16_t age = (cur - created) & 0xFFFF;
			if (age > kMaxEpochAge) {
				LOG_EVERY_N(WARNING, 100) << "Drain: Marking stale batch as skipped: age=" << age << " > max=" << kMaxEpochAge
					<< " batch_seq=" << p.batch_seq << " broker=" << p.broker_id;
				p.skipped = true;
				order5_skipped_batches_.fetch_add(1, std::memory_order_relaxed);
				order5_stale_epoch_skips_.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// [PHASE-3] Replace hash maps with arrays for better performance
		std::array<size_t, NUM_MAX_BROKERS> contiguous_consumed_per_broker{};
		std::array<bool, NUM_MAX_BROKERS> broker_seen_in_epoch{};
		for (const PendingBatch5& p : batch_list) {
			int b = p.broker_id;
			if (b < 0 || b >= NUM_MAX_BROKERS) continue;
			if (broker_seen_in_epoch[b]) continue;
			const volatile void* addr = reinterpret_cast<const volatile void*>(&tinode_->offsets[b].batch_headers_consumed_through);
			CXL::flush_cacheline(const_cast<const void*>(addr));
			CXL::load_fence();
			size_t initial = tinode_->offsets[b].batch_headers_consumed_through;
			if (initial == BATCHHEADERS_SIZE) {
				initial = 0;
			}
			contiguous_consumed_per_broker[b] = initial;
			broker_seen_in_epoch[b] = true;
		}

		std::vector<PendingBatch5> level0, level5;
		for (PendingBatch5& p : batch_list) {
			if (p.client_id == 0) {
				level0.push_back(std::move(p));
			} else {
				level5.push_back(std::move(p));
			}
		}

			std::vector<PendingBatch5> ready_level5;
			ProcessLevel5Batches(level5, ready_level5);

		std::vector<PendingBatch5> ready;
		for (PendingBatch5& p : level0) ready.push_back(std::move(p));
		for (PendingBatch5& p : ready_level5) ready.push_back(std::move(p));

		if (ready.empty()) {
			// No ready batches, but we still need to advance consumed_through for all processed slots
			// to prevent ring deadlock. Advance consumed_through for all slots in batch_list.
			AdvanceConsumedThroughForProcessedSlots(batch_list, contiguous_consumed_per_broker, broker_seen_in_epoch, nullptr, nullptr);
			std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_cumulative{};
			std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_pbr_index{};
			for (auto& shard_ptr : level5_shards_) {
				if (!shard_ptr) continue;
				for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS &&
					    cum > drain_cv_logical_only_cumulative[bid]) {
						drain_cv_logical_only_cumulative[bid] = cum;
					}
				}
				for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
					if (bid >= 0 && bid < NUM_MAX_BROKERS &&
					    pbr > drain_cv_logical_only_pbr_index[bid]) {
						drain_cv_logical_only_pbr_index[bid] = pbr;
					}
				}
			}
			FlushAccumulatedCVLogicalOnly(
				drain_cv_logical_only_cumulative, drain_cv_logical_only_pbr_index);
			continue;
		}

		// Call unified commit logic
		// [[DRAIN_MODE_COMMIT]] Use drain-specific CV accumulator arrays
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_max_pbr_index{};
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_cumulative{};
		std::array<uint64_t, NUM_MAX_BROKERS> drain_cv_logical_only_pbr_index{};
		for (auto& shard_ptr : level5_shards_) {
			if (!shard_ptr) continue;
			for (const auto& [bid, cum] : shard_ptr->cv_max_cumulative) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (cum > drain_cv_max_cumulative[bid]) drain_cv_max_cumulative[bid] = cum;
				}
			}
			for (const auto& [bid, pbr] : shard_ptr->cv_max_pbr_index) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (pbr > drain_cv_max_pbr_index[bid]) drain_cv_max_pbr_index[bid] = pbr;
				}
			}
			for (const auto& [bid, cum] : shard_ptr->cv_logical_only_cumulative) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (cum > drain_cv_logical_only_cumulative[bid]) {
						drain_cv_logical_only_cumulative[bid] = cum;
					}
				}
			}
			for (const auto& [bid, pbr] : shard_ptr->cv_logical_only_pbr_index) {
				if (bid >= 0 && bid < NUM_MAX_BROKERS) {
					if (pbr > drain_cv_logical_only_pbr_index[bid]) {
						drain_cv_logical_only_pbr_index[bid] = pbr;
					}
				}
			}
		}
		CommitEpoch(ready, by_slot, contiguous_consumed_per_broker, broker_seen_in_epoch,
		           drain_cv_max_cumulative, drain_cv_max_pbr_index, batch_list, /*is_drain_mode=*/true);
        if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
		FlushAccumulatedCVLogicalOnly(
			drain_cv_logical_only_cumulative, drain_cv_logical_only_pbr_index);
	}

	// [[SHARD_SHUTDOWN]] Signal Level 5 workers to exit
	for (auto& shard : level5_shards_) {
		if (!shard) continue;
		{
			std::lock_guard<std::mutex> lock(shard->mu);
			shard->stop = true;
		}
		shard->cv.notify_one();
	}
}

void Topic::Level5ShardWorker(size_t shard_id) {
	Level5ShardState& shard = *level5_shards_[shard_id];
	while (true) {
		std::vector<PendingBatch5> input;
		{
			std::unique_lock<std::mutex> lock(shard.mu);
			// [PANEL FIX] Ignore stop_threads_: worker must stay alive until EpochSequencerThread
			// finishes drain and sets shard.stop. Otherwise workers exit early and drain fails.
			shard.cv.wait(lock, [&]() { return shard.has_work || shard.stop; });
			if (shard.stop && !shard.has_work) {
				break;
			}
			shard.has_work = false;
			input.swap(shard.input);
		}

		shard.ready.clear();
		ProcessLevel5BatchesShard(shard, input, shard.ready);

		{
			std::lock_guard<std::mutex> lock(shard.mu);
			shard.done = true;
		}
		shard.cv.notify_one();
	}
}

void Topic::InitLevel5Shards() {
	if (level5_shards_started_.exchange(true)) {
		return;
	}
	size_t shard_count = 1;
	if (const char* env = std::getenv("EMBAR_LEVEL5_SHARDS")) {
		int parsed = std::atoi(env);
		if (parsed > 0) {
			shard_count = static_cast<size_t>(parsed);
		}
	}
	if (shard_count > 32) shard_count = 32;
	level5_num_shards_ = shard_count;
	level5_shards_.resize(level5_num_shards_);
	for (size_t i = 0; i < level5_num_shards_; ++i) {
		level5_shards_[i] = std::make_unique<Level5ShardState>();
	}

	if (level5_num_shards_ > 1) {
		level5_shard_threads_.reserve(level5_num_shards_);
		for (size_t i = 0; i < level5_num_shards_; ++i) {
			level5_shard_threads_.emplace_back(&Topic::Level5ShardWorker, this, i);
		}
		LOG(INFO) << "Sequencer5: Level5 shards enabled, shards=" << level5_num_shards_;
	} else {
		LOG(INFO) << "Sequencer5: Level5 sharding disabled (single shard).";
	}
}

}  // namespace Embarcadero
