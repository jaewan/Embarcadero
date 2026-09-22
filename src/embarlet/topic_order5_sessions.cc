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

// Session classification, duplicate suppression, held suffixes, and fencing decisions.
// State remains owned by Topic; this file adds no independent runtime state.

void Topic::ArmOrder5ForceExpiryWindow(uint64_t duration_ns) {
	if (duration_ns == 0) return;
	const uint64_t now_ns = SteadyNowNs();
	if (now_ns < force_expire_rearm_cooldown_until_ns_.load(std::memory_order_acquire)) {
		return;
	}
	const uint64_t new_deadline = now_ns + duration_ns;
	uint64_t cur_deadline = force_expire_hold_until_ns_.load(std::memory_order_acquire);
	while (cur_deadline < new_deadline &&
	       !force_expire_hold_until_ns_.compare_exchange_weak(
	           cur_deadline, new_deadline, std::memory_order_release, std::memory_order_acquire)) {
	}
}

void Topic::RequestOrder5HoldExpiryOnce() {
	if (seq_type_ == EMBARCADERO && Embarcadero::UsesEpochSequencerPath(order_)) {
		const uint64_t kDisconnectForceExpireWindowNs =
			(replication_factor_ > 0) ? 90'000'000'000ULL : 2'500'000'000ULL;
		ArmOrder5ForceExpiryWindow(kDisconnectForceExpireWindowNs);
		const uint64_t new_deadline = force_expire_hold_until_ns_.load(std::memory_order_acquire);

		// A disconnect often means publishers have finished sending and the last useful work
		// is sitting in the current COLLECTING epoch. Nudge the epoch state machine forward so
		// tail batches do not wait for a later shutdown-only drain path.
		uint64_t cur_epoch = epoch_index_.load(std::memory_order_acquire);
		EpochBuffer5& cur_buf = epoch_buffers_[cur_epoch % 3];
		if (ShouldEnableOrder5Trace()) {
			LOG(INFO) << "[ORDER5_TRACE_DISCONNECT_DRAIN_REQUEST]"
			          << " deadline_ns=" << new_deadline
			          << " cur_epoch=" << cur_epoch
			          << " cur_state=" << static_cast<int>(cur_buf.state.load(std::memory_order_acquire));
		}
		RecordOrder5FlightEvent(
			kOrder5FlightDisconnect,
			static_cast<uint32_t>(broker_id_),
			new_deadline,
			cur_epoch,
			static_cast<uint64_t>(cur_buf.state.load(std::memory_order_acquire)),
			force_expire_hold_until_ns_.load(std::memory_order_acquire));
		const EpochBuffer5::State cur_state = cur_buf.state.load(std::memory_order_acquire);
		auto advance_to_successor_epoch = [&](uint64_t base_epoch) {
			const uint64_t next_epoch = base_epoch + 1;
			EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];
			EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
			if (next_state == EpochBuffer5::State::IDLE) {
				next_buf.reset_and_start();
				next_state = next_buf.state.load(std::memory_order_acquire);
			}
			if (next_state == EpochBuffer5::State::COLLECTING ||
			    next_state == EpochBuffer5::State::SEALED) {
				uint64_t expected = base_epoch;
				epoch_index_.compare_exchange_strong(
					expected, next_epoch, std::memory_order_release, std::memory_order_acquire);
			}
			return next_state;
		};
		if (cur_state == EpochBuffer5::State::IDLE) {
			// ACK-stall disconnect nudges can arrive after ingress has quiesced and the epoch driver has
			// already fallen back to an IDLE buffer. In that state no new sealed epoch is produced, so
			// hold/deferred-only work never gets another sequencer pass. Bootstrap an empty epoch here so
			// the sequencer can run ProcessLevel5Batches() and drain held tail state without new ingress.
			if (cur_buf.reset_and_start() && cur_buf.seal()) {
				const EpochBuffer5::State next_state = advance_to_successor_epoch(cur_epoch);
				if (ShouldEnableOrder5Trace()) {
					LOG(INFO) << "[ORDER5_TRACE_DISCONNECT_IDLE_BOOTSTRAP]"
					          << " sealed_epoch=" << cur_epoch
					          << " next_epoch=" << (cur_epoch + 1)
					          << " next_state=" << static_cast<int>(next_state);
				}
			}
			return;
		}
		if (cur_state == EpochBuffer5::State::SEALED) {
			const EpochBuffer5::State next_state = advance_to_successor_epoch(cur_epoch);
			if (ShouldEnableOrder5Trace()) {
				LOG(INFO) << "[ORDER5_TRACE_DISCONNECT_SEALED_ADVANCE]"
				          << " sealed_epoch=" << cur_epoch
				          << " next_epoch=" << (cur_epoch + 1)
				          << " next_state=" << static_cast<int>(next_state);
			}
			return;
		}
		if (cur_state == EpochBuffer5::State::COLLECTING &&
		    cur_buf.seal()) {
			const uint64_t next_epoch = cur_epoch + 1;
			EpochBuffer5& next_buf = epoch_buffers_[next_epoch % 3];
			EpochBuffer5::State next_state = next_buf.state.load(std::memory_order_acquire);
			if (next_state == EpochBuffer5::State::IDLE) {
				next_buf.reset_and_start();
				next_state = next_buf.state.load(std::memory_order_acquire);
			}
			if (next_state == EpochBuffer5::State::COLLECTING ||
			    next_state == EpochBuffer5::State::SEALED) {
				uint64_t expected = cur_epoch;
				epoch_index_.compare_exchange_strong(
					expected, next_epoch, std::memory_order_release, std::memory_order_acquire);
			}
			if (ShouldEnableOrder5Trace()) {
				LOG(INFO) << "[ORDER5_TRACE_DISCONNECT_DRAIN]"
				          << " sealed_epoch=" << cur_epoch
				          << " next_epoch=" << next_epoch
				          << " next_state=" << static_cast<int>(next_state);
			}
		}
	}
}

bool Topic::CheckAndInsertBatchId(Level5ShardState& shard, uint64_t batch_id) {
	// Use FastDeduplicator for high-throughput deduplication
	return shard.dedup.check_and_insert(batch_id);
}

void Topic::ClientGc(Level5ShardState& shard) {
	uint64_t epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
	if ((epoch & (kClientGcEpochInterval - 1)) != 0) {
		return;
	}
	std::vector<size_t> evict;
	evict.reserve(shard.client_state.size());
	for (const auto& kv : shard.client_state) {
		const size_t cid = kv.first;
		const ClientState5& st = kv.second;
		auto hold_it = shard.hold_buffer.find(cid);
		if (hold_it != shard.hold_buffer.end() && !hold_it->second.empty()) {
			continue;
		}
		if (shard.clients_with_held_batches.contains(cid)) {
			continue;
		}
		if (!shard.deferred_level5.empty()) {
			continue;
		}
		if (epoch > st.last_epoch && epoch - st.last_epoch > kClientTtlEpochs) {
			evict.push_back(cid);
		}
	}
	for (size_t cid : evict) {
		auto hold_it = shard.hold_buffer.find(cid);
		if (hold_it != shard.hold_buffer.end()) {
			const size_t n = hold_it->second.size();
			shard.hold_buffer_size -= n;
			order5_total_hold_size_.fetch_sub(n, std::memory_order_release);
			shard.hold_buffer.erase(hold_it);
		}
		shard.clients_with_held_batches.erase(cid);
		shard.client_state.erase(cid);
		shard.client_emitted_tracker.erase(cid);
	}
}

void Topic::CollectOrder5HeldSlotIdentities(
		absl::flat_hash_set<std::tuple<int, size_t, uint64_t>>& out) {
	// Full sweep of all hold-side structures. This replaced the per-batch IsOrder5HeldSlot
	// scan (O(batches × total_held) per CommitEpoch), but at 2-proc saturation the hold
	// buffer legitimately reaches ~27K entries and even one sweep per epoch dominated the
	// sequencer (heldsweep ≈ 900 ms/s, run 14, 2026-07-12). The hot path now uses
	// IsOrder5SlotHeldInHoldBuffer (keyed O(1) probe) + CollectOrder5DeferredSlotIdentities
	// (small lists only); this full sweep remains as the reference for the env-gated
	// validation mode (EMBARCADERO_ORDER5_HELDCHECK_VALIDATE=1).
	for (const auto& shard_ptr : level5_shards_) {
		if (!shard_ptr) continue;
		std::lock_guard<std::mutex> lock(shard_ptr->mu);
		for (const PendingBatch5& p : shard_ptr->deferred_level5) {
			out.emplace(p.broker_id, p.slot_offset, p.cached_pbr_absolute_index);
		}
		for (const Order5SlotIdentity& slot : shard_ptr->backpressured_level5_slots) {
			out.emplace(slot.broker_id, slot.slot_offset, slot.pbr_index);
		}
		for (const auto& [session_key, held] : shard_ptr->hold_buffer) {
			(void)session_key;
			for (const auto& [seq, entry] : held) {
				(void)seq;
				const HoldBatchMetadata& meta = entry.meta;
				out.emplace(meta.broker_id, meta.slot_offset, meta.pbr_absolute_index);
			}
		}
	}
}

void Topic::CollectOrder5DeferredSlotIdentities(
		absl::flat_hash_set<std::tuple<int, size_t, uint64_t>>& out) {
	// Deferred + backpressured lists only. Both are near-empty in steady state (deferred
	// drains into the next epoch's input; backpressure only arises when the hold buffer is
	// at capacity), so sweeping them per Advance call is cheap. The large structure —
	// hold_buffer — is covered by the keyed probe instead.
	for (const auto& shard_ptr : level5_shards_) {
		if (!shard_ptr) continue;
		std::lock_guard<std::mutex> lock(shard_ptr->mu);
		for (const PendingBatch5& p : shard_ptr->deferred_level5) {
			out.emplace(p.broker_id, p.slot_offset, p.cached_pbr_absolute_index);
		}
		for (const Order5SlotIdentity& slot : shard_ptr->backpressured_level5_slots) {
			out.emplace(slot.broker_id, slot.slot_offset, slot.pbr_index);
		}
	}
}

bool Topic::IsOrder5SlotHeldInHoldBuffer(const PendingBatch5& p) {
	// Keyed O(1) replacement for the full hold_buffer sweep: a batch-list entry's slot can
	// only be held by ITS OWN batch (a (broker, slot_offset, pbr_index) triple is unique to
	// one batch generation), and hold entries are keyed by exactly the identity fields the
	// scanner stamped on the entry (client_id, session_epoch [, broker_id for legacy
	// streams], batch_seq). So probing the owning session's stream map and comparing the
	// triple is equivalent to scanning every held entry for a triple match.
	// AdvanceConsumedThrough walks the epoch's batch_list AFTER those entries were
	// std::move'd into level5/hold; PendingBatch5 is trivially movable so identity fields
	// remain intact in the moved-from batch_list slots (do not clear them on move).
	// client_id==0 (level0) batches are never held; SIZE_MAX marks anonymous skip markers,
	// which reference CLAIMED-stuck producer slots, never held ones.
	if (level5_shards_.empty() || level5_num_shards_ == 0) return false;
	if (p.client_id == 0 || p.client_id == SIZE_MAX) return false;
	Level5ShardState* shard_ptr = level5_shards_[p.client_id % level5_num_shards_].get();
	if (shard_ptr == nullptr) return false;
	const uint64_t key = UsesTrueClientChainOrdering(order_)
		? MakeSessionKey(p.client_id, p.session_epoch)
		: MakeClientBrokerStreamKey(p.client_id, p.session_epoch, p.broker_id);
	std::lock_guard<std::mutex> lock(shard_ptr->mu);
	auto it = shard_ptr->hold_buffer.find(key);
	if (it == shard_ptr->hold_buffer.end()) return false;
	auto jt = it->second.find(p.batch_seq);
	if (jt == it->second.end()) return false;
	const HoldBatchMetadata& meta = jt->second.meta;
	return meta.broker_id == p.broker_id &&
	       meta.slot_offset == p.slot_offset &&
	       meta.pbr_absolute_index == p.cached_pbr_absolute_index;
}

bool Topic::IsOrder5HeldSlot(int broker_id, size_t slot_offset, uint64_t pbr_index) {
	for (const auto& shard_ptr : level5_shards_) {
		if (!shard_ptr) continue;
		std::lock_guard<std::mutex> lock(shard_ptr->mu);
		for (const PendingBatch5& p : shard_ptr->deferred_level5) {
			Order5SlotIdentity slot{p.broker_id, p.slot_offset, p.cached_pbr_absolute_index};
			if (Order5SlotMatches(slot, broker_id, slot_offset, pbr_index)) {
				return true;
			}
		}
		for (const Order5SlotIdentity& slot : shard_ptr->backpressured_level5_slots) {
			if (Order5SlotMatches(slot, broker_id, slot_offset, pbr_index)) {
				return true;
			}
		}
		for (const auto& [session_key, held] : shard_ptr->hold_buffer) {
			(void)session_key;
			for (const auto& [seq, entry] : held) {
				(void)seq;
				const HoldBatchMetadata& meta = entry.meta;
				Order5SlotIdentity slot{meta.broker_id, meta.slot_offset, meta.pbr_absolute_index};
				if (Order5SlotMatches(slot, broker_id, slot_offset, pbr_index)) {
					return true;
				}
			}
		}
	}
	return false;
}

void Topic::RetireOrder5HeldSlotAndAdvance(BatchHeader* hdr, int broker_id, size_t slot_offset) {
    if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
	InvalidateOrder5HeldSlot(hdr);
	if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS) return;
	if (slot_offset >= BATCHHEADERS_SIZE || (slot_offset % sizeof(BatchHeader)) != 0) return;

	const volatile void* addr = reinterpret_cast<const volatile void*>(
		&tinode_->offsets[broker_id].batch_headers_consumed_through);
	CXL::flush_cacheline(const_cast<const void*>(addr));
	CXL::load_fence();
	size_t consumed = tinode_->offsets[broker_id].batch_headers_consumed_through;
	if (consumed == BATCHHEADERS_SIZE) consumed = 0;
	if (consumed == slot_offset) {
		size_t next = slot_offset + sizeof(BatchHeader);
		if (next >= BATCHHEADERS_SIZE) next = BATCHHEADERS_SIZE;
		tinode_->offsets[broker_id].batch_headers_consumed_through = next;
		CXL::store_fence();
		CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[broker_id].batch_headers_consumed_through));
		CXL::store_fence();
	}
}

void Topic::ProcessLevel5Batches(std::vector<PendingBatch5>& level5, std::vector<PendingBatch5>& ready) {
    if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
	if (level5_shards_.empty()) {
		LOG(FATAL) << "ProcessLevel5Batches: level5_shards_ empty (InitLevel5Shards was not run)";
	}
	CHECK(order5_recovery_complete_.load(std::memory_order_acquire))
		<< "ORDER5 inbound classification reached before RecoverSequencer5State completed";
	if (level5_num_shards_ <= 1) {
		ProcessLevel5BatchesShard(*level5_shards_[0], level5, ready);
		return;
	}

	if (level5_per_shard_cache_.size() != level5_num_shards_) {
		level5_per_shard_cache_.resize(level5_num_shards_);
	}
	for (auto& v : level5_per_shard_cache_) v.clear();

	for (PendingBatch5& p : level5) {
		size_t shard_id = p.client_id % level5_num_shards_;
		level5_per_shard_cache_[shard_id].push_back(std::move(p));
	}

	for (size_t i = 0; i < level5_num_shards_; ++i) {
		Level5ShardState& shard = *level5_shards_[i];
		{
			std::lock_guard<std::mutex> lock(shard.mu);
            if (shard.stop) return;
			shard.input.swap(level5_per_shard_cache_[i]);
			shard.ready.clear();
			shard.done = false;
			shard.has_work = true;
		}
		shard.cv.notify_one();
	}

	for (size_t i = 0; i < level5_num_shards_; ++i) {
		Level5ShardState& shard = *level5_shards_[i];
		std::unique_lock<std::mutex> lock(shard.mu);
		shard.cv.wait(lock, [&shard]() { return shard.done || shard.stop; });
        if (shard.stop) return;
		ready.insert(ready.end(),
			std::make_move_iterator(shard.ready.begin()),
			std::make_move_iterator(shard.ready.end()));
		shard.ready.clear();
	}
}

void Topic::ProcessLevel5BatchesShard(Level5ShardState& shard,
                                     std::vector<PendingBatch5>& level5,
                                     std::vector<PendingBatch5>& ready) {
	const bool true_client_chain = UsesTrueClientChainOrdering(order_);
	auto record_hold_insert = [&](size_t client_id, const std::map<size_t, HoldEntry5>& hold_map) {
		if (!true_client_chain) return;
		ClientPressureStats& stats = shard.client_pressure_stats[client_id];
		stats.hold_inserts++;
		stats.max_held_depth = std::max<uint64_t>(stats.max_held_depth, hold_map.size());
	};
	auto record_expiry = [&](size_t client_id) {
		if (!true_client_chain) return;
		shard.client_pressure_stats[client_id].expiries++;
	};
	auto record_forced_skip = [&](size_t client_id) {
		if (!true_client_chain) return;
		shard.client_pressure_stats[client_id].forced_skips++;
	};
	auto record_late_drop = [&](size_t client_id) {
		if (!true_client_chain) return;
		shard.client_pressure_stats[client_id].late_drops++;
	};
	auto purge_fenced_session = [&](size_t shard_session_key) {
		auto hold_it = shard.hold_buffer.find(shard_session_key);
		if (hold_it != shard.hold_buffer.end()) {
			std::vector<std::tuple<int, size_t, BatchHeader*>> retire;
			retire.reserve(hold_it->second.size());
			for (auto& [seq, held] : hold_it->second) {
				(void)seq;
				retire.emplace_back(
					held.meta.broker_id, held.meta.slot_offset, held.batch.hdr);
			}
			std::array<size_t, NUM_MAX_BROKERS> consume_start{};
			for (int broker_id = 0; broker_id < NUM_MAX_BROKERS; ++broker_id) {
				const volatile void* addr = reinterpret_cast<const volatile void*>(
					&tinode_->offsets[broker_id].batch_headers_consumed_through);
				CXL::flush_cacheline(const_cast<const void*>(addr));
				CXL::load_fence();
				consume_start[broker_id] =
					tinode_->offsets[broker_id].batch_headers_consumed_through;
				if (consume_start[broker_id] == BATCHHEADERS_SIZE) consume_start[broker_id] = 0;
			}
			std::sort(retire.begin(), retire.end(),
				[&](const auto& a, const auto& b) {
					const int broker_a = std::get<0>(a);
					const int broker_b = std::get<0>(b);
					if (broker_a != broker_b) return broker_a < broker_b;
					if (broker_a < 0 || broker_a >= NUM_MAX_BROKERS) {
						return std::get<1>(a) < std::get<1>(b);
					}
					const size_t start = consume_start[broker_a];
					const size_t distance_a =
						(std::get<1>(a) + BATCHHEADERS_SIZE - start) % BATCHHEADERS_SIZE;
					const size_t distance_b =
						(std::get<1>(b) + BATCHHEADERS_SIZE - start) % BATCHHEADERS_SIZE;
					return distance_a < distance_b;
				});
			for (const auto& [broker_id, slot_offset, hdr] : retire) {
				RetireOrder5HeldSlotAndAdvance(hdr, broker_id, slot_offset);
			}
			const size_t n = hold_it->second.size();
			shard.hold_buffer_size -= n;
			order5_total_hold_size_.fetch_sub(n, std::memory_order_release);
			shard.hold_buffer.erase(hold_it);
		}
		shard.clients_with_held_batches.erase(shard_session_key);
		shard.client_emitted_tracker.erase(shard_session_key);
		shard.deferred_level5.erase(
			std::remove_if(shard.deferred_level5.begin(), shard.deferred_level5.end(),
				[&](const PendingBatch5& p) {
					return MakeSessionKey(p.client_id, p.session_epoch) == shard_session_key;
				}),
			shard.deferred_level5.end());
	};
		auto record_fence = [&](size_t shard_session_key, ClientState5& state) {
			if (!true_client_chain) return;
            if (!CanFenceSessionEpoch(state.session_epoch)) return;
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
            (void)fault::Pause("fence.before_publication_gate",
                {shard_session_key >> 32, state.session_epoch, state.next_expected}, &stop_threads_);
#endif
            auto publication_lock = session_publication_gate_.Lock();
            SessionEntry* committed_entry = FindOrCreateSessionEntry(shard_session_key);
            if (!committed_entry) {
              publication_lock.unlock();
              StopForCapacityExhaustion();
              return;
            }
            // next_expected in the classifier can include ready-but-uncommitted
            // batches. A fence reports only the prefix published by CommitEpoch.
            const uint64_t committed_expected = committed_entry->expected_seq.load(std::memory_order_acquire);
            const uint64_t committed_hwm = committed_entry->committed_hwm.load(std::memory_order_acquire);
			const bool first_fence = !state.fenced;
			state.fence();
            state.committed_hwm = committed_hwm;
			if (first_fence || ShouldEnableOrder5SessionTestTrace()) {
				LOG(WARNING) << "[ORDER5_SESSION_FENCE]"
				             << " client=" << static_cast<uint32_t>(shard_session_key >> 32)
				             << " session_epoch=" << state.session_epoch
				             << " next_expected=" << state.next_expected
				             << " committed_hwm=" << state.committed_hwm
				             << " highest_sequenced=" << state.highest_sequenced
				             << " first=" << (first_fence ? 1 : 0);
			}
			SessionPublishSnapshot snapshot;
			snapshot.session_epoch = state.session_epoch;
			snapshot.expected_seq = committed_expected;
			snapshot.committed_hwm = state.committed_hwm;
			snapshot.highest_sequenced = state.highest_sequenced;
			snapshot.fenced = true;
            if (!PublishSessionEntry(shard_session_key, snapshot)) {
              publication_lock.unlock();
              StopForCapacityExhaustion();
              return;
            }
			if (first_fence) {
				SessionFenceNotification note;
				note.client_id = static_cast<uint32_t>(shard_session_key >> 32);
				note.session_epoch = state.session_epoch;
				note.committed_batch_seq = state.committed_hwm;
				note.has_committed_prefix = committed_expected > 0;
				note.committed_msg_hwm = GetClientOrdered(note.client_id);
				note.control_epoch = CurrentControlEpoch();
				note.reason = 0;  // SessionFenced::HOLD_EXPIRY
				PublishSessionFenceNotification(note);
				// Disarm idle force-expire and suppress re-arm for a full session
				// lease. Otherwise run_idle_hold_tick re-arms within ~1ms while hold
				// work remains, and the 250ms short lease instantly fences the
				// reopen/resubmit epoch (fence storm → pool-pin hang).
				force_expire_hold_until_ns_.store(0, std::memory_order_release);
				force_expire_rearm_cooldown_until_ns_.store(
					SteadyNowNs() + GetSessionLeaseNs(replication_factor_ > 0),
					std::memory_order_release);
			}
			purge_fenced_session(shard_session_key);
		};
	auto accumulate_logical_only = [&](int broker_id, uint64_t start_logical_offset,
	                                  uint32_t num_msg, uint64_t pbr_index) {
		if (!true_client_chain) return;
		if (broker_id < 0 || broker_id >= NUM_MAX_BROKERS) return;
		const uint64_t cumulative = start_logical_offset + static_cast<uint64_t>(num_msg);
		auto& slot = shard.cv_logical_only_cumulative[broker_id];
		if (cumulative > slot) slot = cumulative;
		const uint64_t encoded_pbr_index = pbr_index + 1;
		auto& pbr_slot = shard.cv_logical_only_pbr_index[broker_id];
		if (encoded_pbr_index > pbr_slot) pbr_slot = encoded_pbr_index;
	};
	auto terminalize_already_emitted = [&](size_t client_id, int broker_id, uint64_t start_logical_offset,
	                                      uint32_t num_msg, uint64_t batch_id, uint64_t seq,
	                                      uint64_t pbr_index) {
		if (!true_client_chain) return;
		accumulate_logical_only(broker_id, start_logical_offset, num_msg, pbr_index);
		record_late_drop(client_id);
		CheckAndInsertBatchId(shard, batch_id);
		if (ShouldEnableOrder5Trace()) {
			LOG(INFO) << "[ORDER5_TRACE_TERMINAL_ALREADY_EMITTED B" << broker_id << "]"
			          << " client=" << client_id
			          << " seq=" << seq
			          << " pbr=" << pbr_index
			          << " cumulative=" << (start_logical_offset + static_cast<uint64_t>(num_msg));
		}
	};
	if (!shard.deferred_level5.empty()) {
		level5.insert(level5.end(),
			std::make_move_iterator(shard.deferred_level5.begin()),
			std::make_move_iterator(shard.deferred_level5.end()));
		shard.deferred_level5.clear();
	}
	for (const PendingBatch5& p : level5) {
		shard.backpressured_level5_slots.erase(
			std::remove_if(shard.backpressured_level5_slots.begin(),
				shard.backpressured_level5_slots.end(),
				[&](const Order5SlotIdentity& slot) {
					return Order5SlotMatches(
						slot, p.broker_id, p.slot_offset, p.cached_pbr_absolute_index);
				}),
			shard.backpressured_level5_slots.end());
	}
	// [PHASE-3] Clear shard CV accumulation for this invocation
	shard.cv_max_cumulative.clear();
	shard.cv_max_pbr_index.clear();
	shard.cv_logical_only_cumulative.clear();
	shard.cv_logical_only_pbr_index.clear();
	std::array<uint64_t, NUM_MAX_BROKERS> shard_cv_max_cumulative{};
	std::array<uint64_t, NUM_MAX_BROKERS> shard_cv_max_pbr_index{};
	// ORDER=5 ACK1 on the head broker is per-client. Some true-client-chain recovery paths
	// terminalize a late batch without re-emitting it; those batches still need to count
	// toward the per-client ACK frontier or broker 0 can remain permanently short.
	absl::flat_hash_map<uint32_t, uint64_t> per_client_terminalized_delta_epoch;

	// [[S2_CLASSIFIER]] Single seq classifier shared by all 4 per-record loops (fast/general ×
	// true-chain/legacy). One decision tree: DUP-already-emitted / IsEmitted-only / EQ / LT / GT.
	// Divergences between the true-chain (tcc) and legacy stream flavors are expressed inline:
	//  - key: client_id (tcc) vs MakeClientBrokerStreamKey (legacy) — chosen by the caller.
	//  - LT: tcc drops-late (terminalize, per-client ACK credit); legacy emits-late
	//    (fifo_violation + ready).
	//  - IsEmitted-only: tcc terminalizes; legacy re-emits with fifo_violation.
	//  - GT hold-full: legacy counts fifo pressure; tcc records backpressure pressure.
	//  - GT hold-insert: record_hold_insert is tcc-only.
	// terminalize_already_emitted / record_* / accumulate_logical_only self-gate on
	// true_client_chain, so tcc-gated calls are no-ops for legacy streams and vice versa.
	// D1 session fencing lives here so late/fenced/drop decisions stay centralized.
	auto classify_one = [&](ClientState5& state, ClientEmitTracker& emitted, size_t key,
	                        PendingBatch5& p, bool tcc) {
		size_t seq = p.batch_seq;
			if (tcc && state.fenced) {
				accumulate_logical_only(
					p.broker_id, p.cached_start_logical_offset, p.num_msg,
					p.cached_pbr_absolute_index);
				if (ShouldEnableOrder5SessionTestTrace()) {
					LOG(WARNING) << "[ORDER5_FENCED_SUFFIX_DROP]"
					             << " client=" << p.client_id
					             << " session_epoch=" << p.session_epoch
					             << " batch_seq=" << p.batch_seq
					             << " pbr=" << p.cached_pbr_absolute_index
					             << " num_msg=" << p.num_msg;
				}
				record_late_drop(p.client_id);
				return;
			}
		if (state.is_duplicate(seq) && emitted.IsEmitted(static_cast<uint64_t>(seq))) {
			terminalize_already_emitted(
				p.client_id, p.broker_id, p.cached_start_logical_offset,
				p.num_msg, p.cached_batch_id, seq, p.cached_pbr_absolute_index);
			return;
		}
		if (emitted.IsEmitted(static_cast<uint64_t>(seq))) {
			state.mark_sequenced(seq);
			if (seq >= state.next_expected) state.set_next_expected(seq + 1);
			if (tcc) {
				terminalize_already_emitted(
					p.client_id, p.broker_id, p.cached_start_logical_offset,
					p.num_msg, p.cached_batch_id, seq, p.cached_pbr_absolute_index);
			} else if (!CheckAndInsertBatchId(shard, p.cached_batch_id)) {
				ready.push_back(std::move(p));
				emitted.MarkEmitted(static_cast<uint64_t>(seq));
				order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
			}
			return;
		}
		if (ShouldBypassOrder5SessionFIFOForAblation()) {
			// Fencing and both duplicate-suppression branches above remain identical
			// to normal ORDER=5.  The ablation removes only predecessor comparison
			// and hold-map enforcement: every unique, active-session batch proceeds
			// to the unchanged epoch/GOI/CV/ACK path in scanner-observation order.
			state.mark_sequenced(seq);
			if (seq >= state.next_expected) state.set_next_expected(seq + 1);
			if (!CheckAndInsertBatchId(shard, p.cached_batch_id)) {
				emitted.MarkEmitted(static_cast<uint64_t>(seq));
				ready.push_back(std::move(p));
			}
			return;
		}
		if (seq == state.next_expected) {
			// [[TR_TRACE]] Capture gap-detect time + num_msg before advance/move.
			const uint64_t tr_gap0 = state.gap_since_ns;
			const uint32_t tr_broker = static_cast<uint32_t>(p.broker_id);
			const uint32_t tr_nmsg = p.num_msg;
			state.mark_sequenced(seq);
			state.advance_next_expected();
			if (!CheckAndInsertBatchId(shard, p.cached_batch_id)) {
				emitted.MarkEmitted(static_cast<uint64_t>(seq));
				ready.push_back(std::move(p));
				Order5TrTrace& tr = Order5TrTrace::Instance();
				if (tr.enabled()) {
					const uint64_t tr_now = SteadyNowNs();
					tr.RecordCommit(tr_broker, key, tr_now, state.next_expected - 1, tr_nmsg);
					if (tr_gap0 != 0) {
						// This in-order batch resolved an open gap -> release the held suffix.
						tr.RecordGapRelease(tr_broker, key, tr_now, state.next_expected,
							epoch_index_.load(std::memory_order_relaxed),
							tr.ScanPassTotal(),
							order5_total_hold_size_.load(std::memory_order_relaxed));
					}
				}
			}
		} else if (seq < state.next_expected) {
			state.mark_sequenced(seq);
			if (!CheckAndInsertBatchId(shard, p.cached_batch_id)) {
				if (tcc) {
					accumulate_logical_only(
						p.broker_id, p.cached_start_logical_offset, p.num_msg,
						p.cached_pbr_absolute_index);
					record_late_drop(p.client_id);
					per_client_terminalized_delta_epoch[static_cast<uint32_t>(p.client_id)] += p.num_msg;
				} else {
					order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
					ready.push_back(std::move(p));
					emitted.MarkEmitted(static_cast<uint64_t>(seq));
				}
			}
		} else {
			if (tcc) {
				// [[TR_TRACE]] Record only the FIRST detection of this gap (note_gap is idempotent).
				const bool tr_was_open = (state.gap_since_ns != 0);
				state.note_gap(SteadyNowNs());
				Order5TrTrace& tr = Order5TrTrace::Instance();
				if (tr.enabled() && !tr_was_open && state.gap_since_ns != 0) {
					tr.RecordGapDetect(static_cast<uint32_t>(p.broker_id), key,
						state.gap_since_ns, state.next_expected,
						epoch_index_.load(std::memory_order_relaxed),
						tr.ScanPassTotal(),
						order5_total_hold_size_.load(std::memory_order_relaxed));
				}
			}
			if (shard.hold_buffer_size >= kHoldBufferMaxEntries) {
				std::this_thread::sleep_for(std::chrono::microseconds(10));
				if (shard.deferred_level5.size() < kDeferredL5MaxEntries) {
					shard.deferred_level5.push_back(std::move(p));
					return;
				}
				Order5SlotIdentity blocked_slot{
					p.broker_id, p.slot_offset, p.cached_pbr_absolute_index};
				const bool already_blocked = std::any_of(
					shard.backpressured_level5_slots.begin(),
					shard.backpressured_level5_slots.end(),
					[&](const Order5SlotIdentity& slot) {
						return Order5SlotMatches(
							slot, blocked_slot.broker_id, blocked_slot.slot_offset,
							blocked_slot.pbr_index);
					});
				if (!already_blocked) {
					shard.backpressured_level5_slots.push_back(blocked_slot);
				}
				LOG_EVERY_N(WARNING, 100)
					<< "[ORDER5_HOLD_BACKPRESSURE] deferred full; leaving session frontier unchanged"
					<< " client=" << p.client_id
					<< " session_epoch=" << p.session_epoch
					<< " seq=" << seq
					<< " next_expected=" << state.next_expected;
				return;
			}

			auto& stream_map = shard.hold_buffer[key];
			if (stream_map.find(seq) != stream_map.end()) return;

			HoldEntry5 he;
			he.meta.log_idx = p.cached_log_idx;
			he.meta.total_size = p.cached_total_size;
			he.meta.batch_id = p.cached_batch_id;
			he.meta.batch_seq = p.batch_seq;
			he.meta.session_epoch = p.session_epoch;
			he.meta.pbr_absolute_index = p.cached_pbr_absolute_index;
			he.meta.client_id = p.client_id;
			he.meta.epoch_created = p.epoch_created;
			he.meta.broker_id = p.broker_id;
			he.meta.num_msg = p.num_msg;
			he.meta.start_logical_offset = p.cached_start_logical_offset;
			he.meta.slot_offset = p.slot_offset;
			he.hold_start_ns = SteadyNowNs();
			he.batch = std::move(p);
			stream_map.emplace(seq, he);
			if (tcc) {
				record_hold_insert(p.client_id, stream_map);
			}
			shard.hold_buffer_size++;
			// [[FAST-SEAL]] Increment the lock-free aggregate; sequencer reads this
			// without holding any shard mutex to determine steady-state.
			order5_total_hold_size_.fetch_add(1, std::memory_order_release);
			shard.clients_with_held_batches.insert(key);

			PendingBatch5 marker;
			marker.broker_id = he.batch.broker_id;
			marker.slot_offset = he.batch.slot_offset;
			marker.is_held_marker = true;
			marker.hdr = nullptr;
			marker.num_msg = 0;
			marker.cached_pbr_absolute_index = he.meta.pbr_absolute_index;
			ready.push_back(marker);
		}
	};

	// [[SKIP_MARKER_FILTER]] Extract scanner timeout markers before sorting/grouping.
	// Anonymous skips advance only consumed_through. Targeted skips (client_id/batch_seq known
	// from the claim descriptor) must also advance the affected client frontier so later held
	// batches can drain.
	uint64_t hold_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
	auto skip_it = level5.begin();
	while (skip_it != level5.end()) {
		if (!skip_it->skipped) {
			++skip_it;
			continue;
		}
		if (skip_it->client_id != SIZE_MAX) {
			const uint64_t session_key = MakeSessionKey(skip_it->client_id, skip_it->session_epoch);
			ClientState5& state = shard.client_state[session_key];
			state.session_epoch = skip_it->session_epoch;
			state.last_epoch = hold_epoch;
			ReconstructClientStateFromSessionEntry(session_key, state);
			LOG(INFO) << "[ORDER5_TARGETED_SKIP]"
			          << " client=" << skip_it->client_id
			          << " session_epoch=" << skip_it->session_epoch
			          << " seq=" << skip_it->batch_seq
			          << " broker=" << skip_it->broker_id
			          << " next_expected_before=" << state.next_expected
			          << " hold_size=" << shard.hold_buffer_size;
			if (true_client_chain) {
				if (CanFenceSessionEpoch(state.session_epoch)) {
					record_fence(session_key, state);
					record_forced_skip(skip_it->client_id);
				} else {
					state.mark_sequenced(skip_it->batch_seq);
					if (skip_it->batch_seq >= state.next_expected) {
						state.set_next_expected(skip_it->batch_seq + 1);
						record_forced_skip(skip_it->client_id);
					}
				}
			} else {
				state.mark_sequenced(skip_it->batch_seq);
				if (skip_it->batch_seq >= state.next_expected) {
					state.set_next_expected(skip_it->batch_seq + 1);
					record_forced_skip(skip_it->client_id);
				}
			}
		}
		ready.push_back(std::move(*skip_it));
		skip_it = level5.erase(skip_it);
	}

	// [PHASE-6] Fast path: single client, no held batches, no deferred.
	// Skip radix sort, dedup check, and hold buffer management when possible.
	if (shard.hold_buffer_size == 0 &&
	    shard.deferred_level5.empty() &&
	    !level5.empty()) {
		bool single_client = true;
		size_t first_client = level5[0].client_id;
		uint32_t first_session_epoch = level5[0].session_epoch;
		for (size_t i = 1; i < level5.size(); ++i) {
			if (level5[i].client_id != first_client ||
			    level5[i].session_epoch != first_session_epoch) {
				single_client = false;
				break;
			}
		}
		if (single_client) {
			if (true_client_chain) {
				std::vector<PendingBatch5*> ordered;
				ordered.reserve(level5.size());
				for (auto& p : level5) {
					ordered.push_back(&p);
				}
				if (ordered.size() > 1) {
					std::sort(ordered.begin(), ordered.end(),
						[](const PendingBatch5* a, const PendingBatch5* b) {
							return a->batch_seq < b->batch_seq;
						});
				}
				const uint64_t session_key = MakeSessionKey(first_client, first_session_epoch);
				ClientState5& state = shard.client_state[session_key];
				ClientEmitTracker& emitted = shard.client_emitted_tracker[session_key];
				state.session_epoch = first_session_epoch;
				state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
				ReconstructClientStateFromSessionEntry(session_key, state);
				if (state.next_expected == 0 && emitted.contiguous_max != UINT64_MAX) {
					state.set_next_expected(emitted.contiguous_max + 1);
					state.highest_sequenced = emitted.contiguous_max;
				}
				if (state.next_expected == 0 &&
				    state.highest_sequenced == 0 &&
				    emitted.contiguous_max == UINT64_MAX) {
					// True client-chain ordering is defined over the full client sequence, not the
					// first fragment we happened to observe in this epoch. Starting at the first
					// observed batch_seq can silently treat earlier striped batches as "late" and
					// drop them behind the frontier. New clients always start from seq 0.
					state.set_next_expected(0);
				}
				for (PendingBatch5* p : ordered) {
					classify_one(state, emitted, session_key, *p, true_client_chain);
				}
			} else {
				absl::flat_hash_map<uint64_t, std::vector<PendingBatch5*>> by_stream;
				by_stream.reserve(8);
				for (auto& p : level5) {
					const uint64_t stream_key =
						MakeClientBrokerStreamKey(first_client, p.session_epoch, p.broker_id);
					by_stream[stream_key].push_back(&p);
				}
				for (auto& [stream_key, vec] : by_stream) {
					if (vec.size() > 1) {
						std::sort(vec.begin(), vec.end(),
							[](const PendingBatch5* a, const PendingBatch5* b) {
								return a->batch_seq < b->batch_seq;
							});
					}
					ClientState5& state = shard.client_state[stream_key];
					ClientEmitTracker& emitted = shard.client_emitted_tracker[stream_key];
					state.session_epoch = vec.empty() ? 0 : vec.front()->session_epoch;
					state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
					if (!vec.empty()) {
						ReconstructClientStateFromSessionEntry(
							MakeSessionKey(first_client, vec.front()->session_epoch), state);
					}
					if (state.next_expected == 0 && emitted.contiguous_max != UINT64_MAX) {
						state.set_next_expected(emitted.contiguous_max + 1);
						state.highest_sequenced = emitted.contiguous_max;
					}
					if (state.next_expected == 0 && !vec.empty()) {
						// [[C5_SEED_UNIFY]] Same rule as the true-client-chain sites: a new
						// session starts at seq 0. Seeding from the first *observed* batch_seq
						// silently treated earlier striped batches as "late" and dropped them.
						state.set_next_expected(0);
					}
					for (PendingBatch5* p : vec) {
						classify_one(state, emitted, stream_key, *p, true_client_chain);
					}
				}
			}
			ClientGc(shard);
			return;
		}
	}

	if (true_client_chain) {
		std::sort(level5.begin(), level5.end(),
			[](const PendingBatch5& a, const PendingBatch5& b) {
				return std::tie(a.client_id, a.session_epoch, a.batch_seq) <
				       std::tie(b.client_id, b.session_epoch, b.batch_seq);
			});
	} else {
		shard.radix_sorter.sort_by_client_id(level5);
	}

	for (auto it = level5.begin(); it != level5.end(); ) {
		size_t client_id = it->client_id;
		uint32_t session_epoch = it->session_epoch;
		auto group_end = it;
		while (group_end != level5.end() &&
		       group_end->client_id == client_id &&
		       group_end->session_epoch == session_epoch) {
			++group_end;
		}

		if (true_client_chain) {
			const uint64_t session_key = MakeSessionKey(client_id, session_epoch);
			ClientState5& state = shard.client_state[session_key];
			state.session_epoch = session_epoch;
			state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
			ReconstructClientStateFromSessionEntry(session_key, state);
			ClientEmitTracker& emitted = shard.client_emitted_tracker[session_key];
			if (state.next_expected == 0 && emitted.contiguous_max != UINT64_MAX) {
				state.set_next_expected(emitted.contiguous_max + 1);
				state.highest_sequenced = emitted.contiguous_max;
			}
				if (state.next_expected == 0 &&
				    state.highest_sequenced == 0 &&
				    emitted.contiguous_max == UINT64_MAX) {
					state.set_next_expected(0);
				}

			for (auto jt = it; jt != group_end; ++jt) {
				classify_one(state, emitted, session_key, *jt, true_client_chain);
			}
		} else {
			std::sort(it, group_end,
				[](const PendingBatch5& a, const PendingBatch5& b) {
					if (a.broker_id != b.broker_id) return a.broker_id < b.broker_id;
					if (a.session_epoch != b.session_epoch) return a.session_epoch < b.session_epoch;
					return a.batch_seq < b.batch_seq;
				});

			for (auto bit = it; bit != group_end; ) {
				int stream_broker = bit->broker_id;
				uint32_t stream_epoch = bit->session_epoch;
				auto broker_end = bit;
				while (broker_end != group_end &&
				       broker_end->broker_id == stream_broker &&
				       broker_end->session_epoch == stream_epoch) {
					++broker_end;
				}

				const uint64_t stream_key = MakeClientBrokerStreamKey(client_id, stream_epoch, stream_broker);
				ClientState5& state = shard.client_state[stream_key];
				state.session_epoch = stream_epoch;
				state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
				ReconstructClientStateFromSessionEntry(
					MakeSessionKey(client_id, stream_epoch), state);
				ClientEmitTracker& emitted = shard.client_emitted_tracker[stream_key];
				if (state.next_expected == 0 && emitted.contiguous_max != UINT64_MAX) {
					state.set_next_expected(emitted.contiguous_max + 1);
					state.highest_sequenced = emitted.contiguous_max;
				}
				if (state.next_expected == 0 && state.highest_sequenced == 0 && emitted.contiguous_max == UINT64_MAX) {
					// [[C5_SEED_UNIFY]] New sessions start at seq 0 (see the fast-path note).
					state.set_next_expected(0);
				}

				for (auto jt = bit; jt != broker_end; ++jt) {
					classify_one(state, emitted, stream_key, *jt, true_client_chain);
				}

				bit = broker_end;
			}
		}
		it = group_end;
	}

	// Drain hold buffer: in-order entries for active clients
	for (auto sit = shard.clients_with_held_batches.begin(); sit != shard.clients_with_held_batches.end(); ) {
		size_t session_key = *sit;
		auto map_it = shard.hold_buffer.find(session_key);
		if (map_it == shard.hold_buffer.end()) {
			auto to_erase = sit++;
			shard.clients_with_held_batches.erase(*to_erase);
			continue;
		}
		ClientState5& state = shard.client_state[session_key];
		state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
		ClientEmitTracker& emitted = shard.client_emitted_tracker[session_key];
		auto& cmap = map_it->second;
			while (true) {
				auto seq_it = cmap.find(state.next_expected);
				if (seq_it == cmap.end()) break;
				HoldEntry5& he = seq_it->second;
				if (emitted.IsEmitted(static_cast<uint64_t>(he.batch.batch_seq))) {
					state.mark_sequenced(he.batch.batch_seq);
					state.advance_next_expected();
					bool handed_to_commit = false;
					if (true_client_chain) {
						terminalize_already_emitted(
							he.meta.client_id, he.meta.broker_id, he.meta.start_logical_offset,
							he.meta.num_msg, he.meta.batch_id, he.batch.batch_seq,
							he.meta.pbr_absolute_index);
					} else {
						PendingBatch5 late = std::move(he.batch);
						late.from_hold = true;
						late.hold_meta = he.meta;
						if (!CheckAndInsertBatchId(shard, he.meta.batch_id)) {
							ready.push_back(std::move(late));
							emitted.MarkEmitted(static_cast<uint64_t>(ready.back().batch_seq));
							order5_fifo_violations_.fetch_add(1, std::memory_order_relaxed);
							handed_to_commit = true;
						}
					}
					if (!handed_to_commit) {
						RetireOrder5HeldSlotAndAdvance(
							he.batch.hdr, he.meta.broker_id, he.meta.slot_offset);
					}
					cmap.erase(seq_it);
					shard.hold_buffer_size--;
				order5_total_hold_size_.fetch_sub(1, std::memory_order_release);
				continue;
			}
			PendingBatch5 b = std::move(he.batch);
			b.from_hold = true;
			b.hold_meta = he.meta;
			state.mark_sequenced(b.batch_seq);
			state.advance_next_expected();
			if (!CheckAndInsertBatchId(shard, he.meta.batch_id)) {
				emitted.MarkEmitted(static_cast<uint64_t>(b.batch_seq));
				ready.push_back(std::move(b));
			}
			cmap.erase(seq_it);
			shard.hold_buffer_size--;
            order5_total_hold_size_.fetch_sub(1, std::memory_order_release);
		}
		if (cmap.empty()) {
			shard.hold_buffer.erase(map_it);
			auto to_erase = sit++;
			shard.clients_with_held_batches.erase(*to_erase);
		} else {
			++sit;
		}
	}

	// E/R-A: fence when a per-session head gap outlives the session lease.
	// [[FORCE_EXPIRE_FENCE_CLOCK]] Detector-armed force-expire windows shorten the
	// effective lease to 0 so stalled B2/B3 holds resolve via SESSION_FENCED rather
	// than waiting the full lease after the idle stall detector already fired.
	const uint64_t now_ns = SteadyNowNs();
	shard.expired_hold_keys_buffer.clear();
	const uint64_t session_lease_ns = GetSessionLeaseNs(replication_factor_ > 0);
	const bool force_expire_active =
		now_ns < force_expire_hold_until_ns_.load(std::memory_order_acquire);
	const uint64_t effective_lease_ns =
		EffectiveOrder5SessionFenceLeaseNs(session_lease_ns, force_expire_active);
	for (size_t session_key : shard.clients_with_held_batches) {
		auto map_it = shard.hold_buffer.find(session_key);
		if (map_it == shard.hold_buffer.end() || map_it->second.empty()) continue;
		ClientState5& state = shard.client_state[session_key];
		if (!CanFenceSessionEpoch(state.session_epoch)) continue;
		if (state.fenced) {
			shard.expired_hold_keys_buffer.emplace_back(session_key, state.next_expected);
			continue;
		}
		auto& cmap = map_it->second;
		if (cmap.find(state.next_expected) != cmap.end()) {
			state.gap_since_ns = 0;
			continue;
		}
		if (ShouldClearSessionGapFromHeldMax(state.next_expected, cmap.rbegin()->first)) {
			state.gap_since_ns = 0;
			continue;
		}
		// [[TR_TRACE]] Symmetric gap-detect for gaps first observed by the sweep
		// (not classify_one) so detect/release pair 1:1 for clean hold-time stats.
		const bool tr_was_open = (state.gap_since_ns != 0);
		state.note_gap(now_ns);
		if (!tr_was_open && state.gap_since_ns != 0) {
			Order5TrTrace& tr = Order5TrTrace::Instance();
			if (tr.enabled()) {
				tr.RecordGapDetect(0, session_key, state.gap_since_ns, state.next_expected,
					epoch_index_.load(std::memory_order_relaxed), tr.ScanPassTotal(),
					order5_total_hold_size_.load(std::memory_order_relaxed));
			}
		}
        uint64_t expiry_now_ns = now_ns;
#if EMBARCADERO_ENABLE_FAULT_INJECTION == 1
        uint64_t advance_ns = 0;
        const auto ready_count = std::count_if(ready.begin(), ready.end(), [&](const PendingBatch5& batch) {
            return MakeSessionKey(batch.client_id, batch.session_epoch) == session_key;
        });
        if (!fault::Pause("classification.before_expiry_sweep",
                {static_cast<uint64_t>(session_key >> 32), state.session_epoch, state.next_expected,
                 static_cast<uint64_t>(ready_count), state.gap_since_ns}, &stop_threads_, &advance_ns)) return;
        expiry_now_ns = advance_ns > UINT64_MAX - now_ns ? UINT64_MAX : now_ns + advance_ns;
#endif
        if (ShouldFenceSessionGap(state, expiry_now_ns, effective_lease_ns)) {
			shard.expired_hold_keys_buffer.emplace_back(session_key, state.next_expected);
		}
	}
	for (const auto& [session_key, missing_seq] : shard.expired_hold_keys_buffer) {
		ClientState5& state = shard.client_state[session_key];
		state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
		order5_hold_timeout_skips_.fetch_add(1, std::memory_order_relaxed);
		record_expiry(static_cast<size_t>(session_key >> 32));
		RecordOrder5FlightEvent(
			kOrder5FlightExpiry,
			static_cast<uint32_t>(broker_id_),
			static_cast<uint64_t>(session_key),
			missing_seq,
			shard.hold_buffer_size,
			effective_lease_ns);
		record_fence(session_key, state);
	}

	// Second drain: emit batches that became ready after the first drain.
	for (auto sit = shard.clients_with_held_batches.begin(); sit != shard.clients_with_held_batches.end(); ) {
		size_t session_key = *sit;
		auto map_it = shard.hold_buffer.find(session_key);
		if (map_it == shard.hold_buffer.end()) {
			auto to_erase = sit++;
			shard.clients_with_held_batches.erase(*to_erase);
			continue;
		}
		ClientState5& state = shard.client_state[session_key];
		state.last_epoch = current_epoch_for_hold_.load(std::memory_order_acquire);
		ClientEmitTracker& emitted = shard.client_emitted_tracker[session_key];
		auto& cmap = map_it->second;
			while (true) {
				auto seq_it = cmap.find(state.next_expected);
				if (seq_it == cmap.end()) break;
				HoldEntry5& he = seq_it->second;
				if (emitted.IsEmitted(static_cast<uint64_t>(he.batch.batch_seq))) {
					state.mark_sequenced(he.batch.batch_seq);
					state.advance_next_expected();
					terminalize_already_emitted(
						he.meta.client_id, he.meta.broker_id, he.meta.start_logical_offset,
						he.meta.num_msg, he.meta.batch_id, he.batch.batch_seq,
						he.meta.pbr_absolute_index);
					RetireOrder5HeldSlotAndAdvance(
						he.batch.hdr, he.meta.broker_id, he.meta.slot_offset);
					cmap.erase(seq_it);
					shard.hold_buffer_size--;
				order5_total_hold_size_.fetch_sub(1, std::memory_order_release);
				continue;
			}
			PendingBatch5 b = std::move(he.batch);
			b.from_hold = true;
			b.hold_meta = he.meta;
			state.mark_sequenced(b.batch_seq);
			state.advance_next_expected();
			if (!CheckAndInsertBatchId(shard, he.meta.batch_id)) {
				emitted.MarkEmitted(static_cast<uint64_t>(b.batch_seq));
				ready.push_back(std::move(b));
			}
			cmap.erase(seq_it);
			shard.hold_buffer_size--;
            order5_total_hold_size_.fetch_sub(1, std::memory_order_release);
		}
		if (cmap.empty()) {
			shard.hold_buffer.erase(map_it);
			auto to_erase = sit++;
			shard.clients_with_held_batches.erase(*to_erase);
		} else {
			++sit;
		}
	}

    if (order5_capacity_exhausted_.load(std::memory_order_acquire)) return;
	if (!per_client_terminalized_delta_epoch.empty()) {
		absl::MutexLock lock(&per_client_mu_);
		const uint64_t producing_epoch = cached_epoch_.load(std::memory_order_acquire);
		for (auto& [cid, cnt] : per_client_terminalized_delta_epoch) {
			per_client_ordered_[cid] += cnt;
			per_client_ordered_epoch_[cid] = producing_epoch;
		}
	}

	for (int b = 0; b < NUM_MAX_BROKERS; ++b) {
		if (shard_cv_max_cumulative[b] > 0) {
			shard.cv_max_cumulative[b] = shard_cv_max_cumulative[b];
		}
		if (shard_cv_max_pbr_index[b] > 0) {
			shard.cv_max_pbr_index[b] = shard_cv_max_pbr_index[b];
		}
	}
	ClientGc(shard);
}

size_t Topic::GetTotalHoldBufferSize() {
	size_t total = 0;
	for (auto& sp : level5_shards_) {
		if (!sp) continue;
		std::lock_guard<std::mutex> lock(sp->mu);
		total += sp->hold_buffer_size;
	}
	return total;
}

void Topic::WriteOrder5AnomalyCountersCsv() const {
	if (order_ != 5 || seq_type_ != heartbeat_system::EMBARCADERO) return;
	LOG(INFO) << "[W1.2_SUMMARY] topic=" << topic_name_ << " broker=" << broker_id_
	          << " order5_commit_order_violations="
	          << order5_commit_order_violations_.load(std::memory_order_relaxed);

	const std::string path = "order5_anomaly_counters_broker" + std::to_string(broker_id_) + ".csv";
	const bool write_header = !std::ifstream(path).good();
	std::ofstream out(path, std::ios::app);
	if (!out.is_open()) {
		LOG(ERROR) << "Failed to open anomaly counter CSV: " << path;
		return;
	}
	if (write_header) {
		out << "TimestampNs,Topic,BrokerId,Order,FifoViolations,AckOrderViolations,"
		    << "SkippedBatches,ScannerTimeoutSkips,HoldTimeoutSkips,"
		    << "HoldBufferForcedSkips,StaleEpochSkips,"
		    << "ExportOverruns,ExportSkippedBatches\n";
	}
	out << SteadyNowNs() << ","
	    << topic_name_ << ","
	    << broker_id_ << ","
	    << order_ << ","
	    << order5_fifo_violations_.load(std::memory_order_relaxed) << ","
	    << order5_ack_order_violations_.load(std::memory_order_relaxed) << ","
	    << order5_skipped_batches_.load(std::memory_order_relaxed) << ","
	    << order5_scanner_timeout_skips_.load(std::memory_order_relaxed) << ","
	    << order5_hold_timeout_skips_.load(std::memory_order_relaxed) << ","
	    << order5_hold_buffer_forced_skips_.load(std::memory_order_relaxed) << ","
	    << order5_stale_epoch_skips_.load(std::memory_order_relaxed) << ","
	    << order5_export_overruns_.load(std::memory_order_relaxed) << ","
	    << order5_export_skipped_batches_.load(std::memory_order_relaxed) << "\n";
}

void Topic::WriteOrder5ClientPressureSummary() const {
	if (order_ != kOrderStrong || seq_type_ != heartbeat_system::EMBARCADERO) return;

	absl::btree_map<size_t, ClientPressureStats> aggregate;
	for (const auto& shard_ptr : level5_shards_) {
		if (!shard_ptr) continue;
		for (const auto& [client_id, stats] : shard_ptr->client_pressure_stats) {
			ClientPressureStats& dst = aggregate[client_id];
			dst.hold_inserts += stats.hold_inserts;
			dst.expiries += stats.expiries;
			dst.forced_skips += stats.forced_skips;
			dst.late_drops += stats.late_drops;
			dst.max_held_depth = std::max(dst.max_held_depth, stats.max_held_depth);
		}
	}
	if (aggregate.empty()) return;

	struct ClientPressureRow {
		size_t client_id{0};
		ClientPressureStats stats;
	};
	std::vector<ClientPressureRow> rows;
	rows.reserve(aggregate.size());
	for (const auto& [client_id, stats] : aggregate) {
		rows.push_back(ClientPressureRow{client_id, stats});
	}
	std::sort(rows.begin(), rows.end(),
		[](const ClientPressureRow& a, const ClientPressureRow& b) {
			return std::tie(a.stats.expiries, a.stats.hold_inserts, a.stats.forced_skips,
			                a.stats.late_drops, a.stats.max_held_depth, a.client_id) >
			       std::tie(b.stats.expiries, b.stats.hold_inserts, b.stats.forced_skips,
			                b.stats.late_drops, b.stats.max_held_depth, b.client_id);
		});

	const size_t top_n = std::min<size_t>(rows.size(), 8);
	LOG(INFO) << "[ORDER5_CLIENT_PRESSURE_SUMMARY broker=" << broker_id_
	          << " topic=" << topic_name_
	          << "] tracked_clients=" << rows.size()
	          << " top_n=" << top_n;
	for (size_t i = 0; i < top_n; ++i) {
		const auto& row = rows[i];
		LOG(INFO) << "  client_id=" << row.client_id
		          << " holds=" << row.stats.hold_inserts
		          << " expiries=" << row.stats.expiries
		          << " forced_skips=" << row.stats.forced_skips
		          << " late_drops=" << row.stats.late_drops
		          << " max_held_depth=" << row.stats.max_held_depth;
	}

	const std::string path = "order5_client_pressure_broker" + std::to_string(broker_id_) + ".csv";
	std::ofstream out(path, std::ios::trunc);
	if (!out.is_open()) {
		LOG(ERROR) << "Failed to open client pressure CSV: " << path;
		return;
	}
	out << "ClientId,HoldInserts,Expiries,ForcedSkips,LateDrops,MaxHeldDepth\n";
	for (const auto& row : rows) {
		out << row.client_id << ","
		    << row.stats.hold_inserts << ","
		    << row.stats.expiries << ","
		    << row.stats.forced_skips << ","
		    << row.stats.late_drops << ","
		    << row.stats.max_held_depth << "\n";
	}
}

}  // namespace Embarcadero
