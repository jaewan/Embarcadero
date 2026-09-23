
#include "common/env_flags.h"
#include "common/stage_trace.h"
#include <array>
#include <iomanip>
#include "publisher.h"
#include "publisher_profile.h"
#include "latency_stats.h"
#include "common/config.h"
#include "common/order_level.h"
#include "common/scoped_fd.h"
#include "common/fault_injection.h"
#include "network_manager/protocol.h"
#include "session.pb.h"
#include "absl/container/flat_hash_map.h"
#include <cstring>
#include <random>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <limits>
#include <mutex>
#include <condition_variable>
#include <netdb.h>
#include <numeric>
#include <set>
#include <stdexcept>



#include "publisher_internal.h"

using namespace Embarcadero::client::detail;

// Retained batch ownership, capacity, and monotonic ACK retirement.
bool Publisher::IsMemoryEmulatedAck2() const {
	if (ack_level_ < 2) return false;
	if (const char* sink = std::getenv("EMBARCADERO_CHAIN_REPLICATION_SINK")) {
		if (std::strcmp(sink, "memory-copy") == 0 ||
		    std::strcmp(sink, "memory_copy") == 0 ||
		    std::strcmp(sink, "memory-accounting") == 0 ||
		    std::strcmp(sink, "memory_accounting") == 0 ||
		    std::strcmp(sink, "accounting") == 0 ||
		    std::strcmp(sink, "copy") == 0) {
			return true;
		}
		if (std::strcmp(sink, "disk-durable") == 0 || std::strcmp(sink, "disk") == 0) {
			return false;
		}
	}
	if (const char* inmem = std::getenv("EMBARCADERO_CHAIN_REPLICATION_INMEM")) {
		if (inmem[0] == '1' || inmem[0] == 'y' || inmem[0] == 'Y') {
			return true;
		}
	}
	return false;
}

bool Publisher::Ack2UsesOwnedRtoCopy() const {
	if (ack_level_ < 2) return false;
	if (const char* retention = std::getenv("EMBARCADERO_ACK2_RETENTION")) {
		if (std::strcmp(retention, "owned_rto_copy") == 0 ||
		    std::strcmp(retention, "owned") == 0) {
			return true;
		}
		if (std::strcmp(retention, "pool_pin") == 0 ||
		    std::strcmp(retention, "pin") == 0) {
			return false;
		}
	}
	// Disk-durable ACK2 keeps an owned RTO copy so slow fdatasync cannot pin the
	// hugepage send pool. Memory-emulated ACK2 pins like ACK1 (credit capped).
	return !IsMemoryEmulatedAck2();
}

size_t Publisher::ComputeUnackedByteCap() const {
	const uint64_t lease_ns = SessionLeaseNs();
	// ACK-class offered rates: ACK1 / memory-emulated ACK2 track CXL/GOI BDP;
	// disk ACK2 tracks media-durable BDP so credit cannot outrun fdatasync.
	const bool memory_ack2 = IsMemoryEmulatedAck2();
	uint64_t offered_rate = (ack_level_ >= 2 && !memory_ack2)
		? (1ULL * 1024ULL * 1024ULL * 1024ULL)   // 1 GiB/s durable default
		: (12ULL * 1024ULL * 1024ULL * 1024ULL); // 12 GiB/s CXL / mem-ACK2 default
	if (ack_level_ >= 2) {
		if (const char* env = std::getenv("EMBARCADERO_ACK2_OFFERED_RATE_BYTES_PER_SEC")) {
			char* end = nullptr;
			unsigned long long parsed = std::strtoull(env, &end, 10);
			if (end != env && *end == '\0' && parsed > 0) {
				offered_rate = parsed;
			}
		}
	}
	if (const char* env = std::getenv("EMBARCADERO_OFFERED_RATE_BYTES_PER_SEC")) {
		char* end = nullptr;
		unsigned long long parsed = std::strtoull(env, &end, 10);
		if (end != env && *end == '\0' && parsed > 0) {
			offered_rate = parsed;
		}
	}
	const long double cap = static_cast<long double>(offered_rate) *
	                        static_cast<long double>(lease_ns) / 1.0e9L;
	const long double max_size = static_cast<long double>(std::numeric_limits<size_t>::max());
	return static_cast<size_t>(std::max<long double>(BATCH_SIZE, std::min(cap, max_size)));
}

void Publisher::ApplyUnackedByteCapBounds() {
	unacked_byte_cap_ = ComputeUnackedByteCap();
	// Only memory-emulated ACK2 pool-pin needs credit ≤ pool (minus seal reserve).
	// ACK1 also pool-pins, but must keep BDP-sized credit: blocking pool acquire
	// already backpressures. Capping ACK1 to ~pool bytes caused N=3 session-fence
	// stalls in AcquireNextBatchFromPool (seen 20260713T175917Z n3 trial3).
	if (IsMemoryEmulatedAck2() && !Ack2UsesOwnedRtoCopy()) {
		const size_t pool_bytes = pubQue_.PoolBytes();
		if (pool_bytes > 0) {
			const size_t reserve = std::min(pool_bytes / 8, 64UL * 1024UL * 1024UL);
			const size_t pin_cap = std::max<size_t>(BATCH_SIZE, pool_bytes - reserve);
			unacked_byte_cap_ = std::min(unacked_byte_cap_, pin_cap);
		}
	}
}

size_t Publisher::TrimUnackedThroughBatchSeqLocked(
		uint64_t committed_batch_hwm,
		std::vector<Embarcadero::BatchHeader*>* release_after_unlock) {
	size_t newly_credited_msgs = 0;
	size_t erased = 0;
	for (auto it = unacked_batches_.begin(); it != unacked_batches_.end();) {
		if (it->original_batch_seq <= committed_batch_hwm) {
			// Only batches still awaiting contiguous retirement contribute to
			// retire_prefix_hwm_; earlier seqs were already folded in.
			if (it->current_batch_seq >= session_next_retire_batch_seq_) {
				newly_credited_msgs += it->num_msg;
			}
			if (it->pool_batch != nullptr && release_after_unlock != nullptr) {
				release_after_unlock->push_back(it->pool_batch);
			}
			unacked_bytes_ -= it->wire_bytes;
			it = unacked_batches_.erase(it);
			++erased;
		} else {
			++it;
		}
	}
	if (!session_trim_committed_valid_ ||
	    committed_batch_hwm > session_trim_committed_hwm_) {
		session_trim_committed_hwm_ = committed_batch_hwm;
		session_trim_committed_valid_ = true;
	}
	session_retire_prefix_hwm_ += newly_credited_msgs;
	AdvanceRetireThroughTrimmedHolesLocked(release_after_unlock);
	if (erased > 0) {
		unacked_cv_.notify_all();
	}
	return newly_credited_msgs;
}

void Publisher::AdvanceRetireThroughTrimmedHolesLocked(
		std::vector<Embarcadero::BatchHeader*>* release_after_unlock) {
	if (!session_trim_committed_valid_) return;
	// Drop late-recorded orphans that landed behind the retire cursor after a
	// reconnect trim (send completed before RecordUnackedBatch).
	while (!unacked_batches_.empty() &&
	       unacked_batches_.front().current_batch_seq < session_next_retire_batch_seq_) {
		auto& head = unacked_batches_.front();
		if (head.pool_batch != nullptr && release_after_unlock != nullptr) {
			release_after_unlock->push_back(head.pool_batch);
		}
		unacked_bytes_ -= head.wire_bytes;
		unacked_batches_.pop_front();
	}
	// Skip seq holes inside the trimmed committed prefix. Batches still present
	// at next_retire wait for normal ACK-HWM retirement.
	while (session_next_retire_batch_seq_ <= session_trim_committed_hwm_) {
		if (!unacked_batches_.empty() &&
		    unacked_batches_.front().current_batch_seq == session_next_retire_batch_seq_) {
			break;
		}
		++session_next_retire_batch_seq_;
	}
}

void Publisher::WaitForUnackedCapacity(size_t bytes) {
	if (!IsOrder5SessionMode() || ack_level_ < 1 || unacked_byte_cap_ == 0) return;
	std::unique_lock<std::mutex> lock(unacked_mu_);
	unacked_cv_.wait(lock, [&]() {
		return shutdown_.load(std::memory_order_relaxed) ||
		       unacked_bytes_ + bytes <= unacked_byte_cap_;
	});
}

bool Publisher::RecordUnackedBatch(const Embarcadero::BatchHeader& header,
                                   const void* batch_bytes,
                                   size_t wire_bytes,
                                   int broker_id,
                                   size_t) {
	if (!IsOrder5SessionMode() || ack_level_ < 1 || header.session_epoch32 == 0) return false;
	if (batch_bytes == nullptr || wire_bytes < sizeof(Embarcadero::BatchHeader)) return false;
	Embarcadero::StageTrace::Record(
		Embarcadero::StageTrace::Stage::ClientSendDone,
		client_id_,
		header.batch_seq,
		static_cast<uint64_t>(SteadyNowNs()),
		unacked_bytes_);
	const bool owned_rto_copy = Ack2UsesOwnedRtoCopy();
	{
		std::lock_guard<std::mutex> lock(unacked_mu_);
		// Send-before-Record race with SessionOpen trim: broker already reported
		// this seq in committed_hwm, so pinning it would recreate a retire gap.
		if (session_trim_committed_valid_ &&
		    header.batch_seq <= session_trim_committed_hwm_) {
			AdvanceRetireThroughTrimmedHolesLocked(nullptr);
			unacked_cv_.notify_all();
			return false;
		}
	}
	UnackedBatch rec;
	rec.original_batch_seq = header.batch_seq;
	rec.current_batch_seq = header.batch_seq;
	rec.num_msg = header.num_msg;
	rec.session_epoch = header.session_epoch32;
	rec.broker_id = broker_id;
	rec.last_send_ns = SteadyNowNs();
	last_unacked_send_ns_.store(rec.last_send_ns, std::memory_order_release);
	rec.wire_bytes = wire_bytes;
	// Disk ACK2: own an RTO copy and release the hugepage send slot immediately
	// so durable latency cannot exhaust the send pool.
	// ACK1 / memory-emulated ACK2: pin the pool slot until ACK (credit capped to
	// pool/2) to avoid a line-rate DRAM memcpy on the fast path.
	if (owned_rto_copy) {
		const auto* bytes = static_cast<const uint8_t*>(batch_bytes);
		rec.wire.assign(bytes, bytes + wire_bytes);
	} else {
		rec.pool_batch = const_cast<Embarcadero::BatchHeader*>(
			static_cast<const Embarcadero::BatchHeader*>(batch_bytes));
	}
	std::lock_guard<std::mutex> lock(unacked_mu_);
	// Re-check after building the record — trim may have landed meanwhile.
	if (session_trim_committed_valid_ &&
	    header.batch_seq <= session_trim_committed_hwm_) {
		AdvanceRetireThroughTrimmedHolesLocked(nullptr);
		unacked_cv_.notify_all();
		return false;
	}
	session_sent_hwm_ += rec.num_msg;
	rec.broker_ack_end = std::numeric_limits<size_t>::max();
	unacked_bytes_ += rec.wire_bytes;
	if (unacked_batches_.empty() ||
	    rec.current_batch_seq >= unacked_batches_.back().current_batch_seq) {
		unacked_batches_.push_back(std::move(rec));
	} else {
		auto it = std::upper_bound(unacked_batches_.begin(), unacked_batches_.end(),
			rec.current_batch_seq,
			[](uint64_t target_batch_seq, const UnackedBatch& candidate) {
				return target_batch_seq < candidate.current_batch_seq;
			});
		unacked_batches_.insert(it, std::move(rec));
	}
	return !owned_rto_copy;
}

void Publisher::CompleteUnackedThrough(int, size_t broker_ack_hwm) {
	if (!IsOrder5SessionMode() || ack_level_ < 1) return;
	const int64_t now_ns = SteadyNowNs();
	std::vector<Embarcadero::BatchHeader*> release_after_unlock;
	{
		std::lock_guard<std::mutex> lock(unacked_mu_);
		for (auto it = unacked_batches_.begin(); it != unacked_batches_.end();) {
			// Heal trim/record races before testing contiguous retirement.
			if (session_trim_committed_valid_ &&
			    (it->current_batch_seq < session_next_retire_batch_seq_ ||
			     (it->current_batch_seq > session_next_retire_batch_seq_ &&
			      session_next_retire_batch_seq_ <= session_trim_committed_hwm_))) {
				AdvanceRetireThroughTrimmedHolesLocked(&release_after_unlock);
				it = unacked_batches_.begin();
				if (it == unacked_batches_.end()) break;
			}
			size_t candidate_hwm = 0;
			if (!SessionPrefixAckEnd(it->current_batch_seq,
			                         session_next_retire_batch_seq_,
			                         session_retire_prefix_hwm_,
			                         it->num_msg,
			                         &candidate_hwm)) {
				break;
			}
			it->broker_ack_end = candidate_hwm;
			if (!SessionGlobalUnackedRetired(candidate_hwm, broker_ack_hwm)) {
				break;
			}
			{
				std::lock_guard<std::mutex> delta_lock(delta_mu_);
				delta_estimator_.sample(static_cast<uint64_t>(std::max<int64_t>(0, now_ns - it->last_send_ns)),
				                        it->attempt);
			}
			session_retire_prefix_hwm_ = candidate_hwm;
			session_next_retire_batch_seq_++;
			unacked_bytes_ -= it->wire_bytes;
			if (it->pool_batch != nullptr) {
				release_after_unlock.push_back(it->pool_batch);
			}
			it = unacked_batches_.erase(it);
			unacked_cv_.notify_all();
		}
	}
	for (auto* batch : release_after_unlock) {
		pubQue_.ReleaseBatch(batch);
	}
}
