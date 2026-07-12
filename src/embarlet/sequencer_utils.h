#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace Embarcadero {

namespace Sequencer5Config {
static constexpr size_t kClientWindowSize = 1024;
static constexpr size_t kRadixThreshold = 4096;  // Below this, use std::sort
static constexpr size_t kMaxBatchesPerEpoch = 256 * 1024;
}  // namespace Sequencer5Config

struct OptimizedClientState {
	/// First client_seq we expect (0 = first batch). Main code uses 0-based sequences.
	uint64_t next_expected{0};
	uint64_t highest_sequenced{0};
	uint64_t last_epoch{0};
	bool fenced{false};
	uint64_t committed_hwm{0};
	uint32_t fence_epoch{0};
	uint32_t session_epoch{0};
	uint64_t gap_since_ns{0};

	static constexpr size_t kWindowSize = Sequencer5Config::kClientWindowSize;
	uint64_t window_base{0};
	std::array<bool, kWindowSize> window_seen{};

	bool is_duplicate(uint64_t seq) const {
		// Late ORDER=5 batches can legitimately arrive after next_expected/window_base have
		// advanced due to prior committed progress. Those batches still need to flow through
		// emitted-tracker logic so the sequencer can handle them once for ACK/export progress.
		// Treat only the active window bitmap as authoritative duplicate state here.
		if (seq < window_base) return false;
		if (seq >= window_base + kWindowSize) return false;
		return window_seen[seq % kWindowSize];
	}

	void mark_sequenced(uint64_t seq) {
		highest_sequenced = std::max(highest_sequenced, seq);
		if (seq >= window_base && seq < window_base + kWindowSize) {
			window_seen[seq % kWindowSize] = true;
		}
		while (window_base < next_expected &&
				window_base + kWindowSize <= seq + kWindowSize / 2) {
			window_seen[window_base % kWindowSize] = false;
			window_base++;
		}
	}

	/// Advance next_expected by one after emitting the batch at next_expected.
	void advance_next_expected() {
		next_expected++;
		gap_since_ns = 0;
	}

	void set_next_expected(uint64_t next) {
		if (next != next_expected) {
			gap_since_ns = 0;
		}
		next_expected = next;
	}

	void note_gap(uint64_t now_ns) {
		if (!fenced && gap_since_ns == 0) {
			gap_since_ns = now_ns;
		}
	}

	/// Idempotent, monotone fence cache.
	/// committed_hwm is an inclusive last-committed batch_seq.
	/// Empty prefix (next_expected == 0) keeps has_committed_prefix=false via
	/// SessionOpenAck.has_committed_prefix on the wire; local state uses 0.
	void fence() {
		if (!fenced) {
			fenced = true;
			committed_hwm = (next_expected == 0) ? 0 : (next_expected - 1);
		}
	}

	bool HasCommittedPrefix() const {
		return next_expected > 0;
	}
};

struct Order5SlotIdentity {
	int broker_id{0};
	size_t slot_offset{0};
	uint64_t pbr_index{0};
};

inline bool CanFenceSessionEpoch(uint32_t session_epoch) {
	return session_epoch != 0;
}

inline bool ShouldFenceSessionGap(
		const OptimizedClientState& state,
		uint64_t now_ns,
		uint64_t session_lease_ns) {
	return CanFenceSessionEpoch(state.session_epoch) &&
	       !state.fenced &&
	       state.gap_since_ns != 0 &&
	       now_ns - state.gap_since_ns >= session_lease_ns;
}

inline bool ShouldClearSessionGapFromHeldMax(uint64_t next_expected, uint64_t max_held_seq) {
	return max_held_seq <= next_expected;
}

inline bool Order5SlotMatches(
		const Order5SlotIdentity& slot,
		int broker_id,
		size_t slot_offset,
		uint64_t pbr_index) {
	return slot.broker_id == broker_id &&
	       slot.slot_offset == slot_offset &&
	       slot.pbr_index == pbr_index;
}

inline uint64_t RecoveredNextExpected(uint64_t goi_scan_next, uint64_t session_entry_expected) {
	return std::max(goi_scan_next, session_entry_expected);
}

/// Reconnect HWM is the min of durable SessionEntry and GOI inclusive batch HWMs.
/// When GOI is unreadable, fall back to the SessionEntry HWM rather than collapsing to 0.
inline uint64_t ReconnectAnswerHwm(uint64_t session_entry_hwm, uint64_t goi_committed_hwm) {
	return std::min(session_entry_hwm, goi_committed_hwm);
}

inline uint64_t ReconnectAnswerHwm(
		uint64_t session_entry_hwm,
		bool goi_hwm_found,
		uint64_t goi_committed_hwm) {
	if (!goi_hwm_found) {
		return session_entry_hwm;
	}
	return std::min(session_entry_hwm, goi_committed_hwm);
}

/// Both arguments must be inclusive batch-seq HWMs (same typed space).
/// Terminal-fence when GOI is missing or the client-visible inclusive HWM exceeds GOI.
inline bool ShouldTerminalFenceAckRelay(
		uint64_t client_inclusive_batch_hwm,
		bool goi_hwm_found,
		uint64_t goi_committed_hwm) {
	return !goi_hwm_found || client_inclusive_batch_hwm > goi_committed_hwm;
}

inline uint64_t SessionKeyFor(uint32_t client_id, uint32_t session_epoch) {
	return (static_cast<uint64_t>(client_id) << 32) | static_cast<uint64_t>(session_epoch);
}

inline bool ShouldWithholdAckRelay(uint64_t producing_epoch, uint64_t control_epoch) {
	return control_epoch != 0 && producing_epoch < control_epoch;
}

inline bool ShouldRejectOrder5SpatialGuard(
		bool session_active,
		bool session_fenced,
		uint64_t batch_seq,
		uint64_t durable_expected) {
	return session_active && (session_fenced || batch_seq < durable_expected);
}

template <typename T>
class RadixSorter {
public:
	RadixSorter() {
		temp_.reserve(Sequencer5Config::kMaxBatchesPerEpoch);
	}

	void sort_by_client_id(std::vector<T>& arr) {
		const size_t n = arr.size();
		if (n <= 1) return;
		if (n < Sequencer5Config::kRadixThreshold) {
			std::sort(arr.begin(), arr.end(), [](const T& a, const T& b) {
				return a.client_id < b.client_id;
			});
			return;
		}

		temp_.resize(n);
		constexpr size_t kBits = 16;
		constexpr size_t kBuckets = 1 << kBits;
		constexpr size_t kMask = kBuckets - 1;

		T* src = arr.data();
		T* dst = temp_.data();

		for (size_t pass = 0; pass < 4; ++pass) {
			size_t shift = pass * kBits;
			std::array<size_t, kBuckets> counts{};
			for (size_t i = 0; i < n; ++i) {
				size_t bucket = (src[i].client_id >> shift) & kMask;
				++counts[bucket];
			}
			size_t total = 0;
			for (size_t i = 0; i < kBuckets; ++i) {
				size_t c = counts[i];
				counts[i] = total;
				total += c;
			}
			for (size_t i = 0; i < n; ++i) {
				size_t bucket = (src[i].client_id >> shift) & kMask;
				dst[counts[bucket]++] = std::move(src[i]);
			}
			std::swap(src, dst);
		}
	}

	void sort_by_client_seq(typename std::vector<T>::iterator begin,
			typename std::vector<T>::iterator end) {
		size_t n = static_cast<size_t>(std::distance(begin, end));
		if (n <= 1) return;
		for (auto it = begin + 1; it != end; ++it) {
			T key = std::move(*it);
			uint64_t kseq = key.batch_seq;
			auto j = it;
			while (j > begin && (j - 1)->batch_seq > kseq) {
				*j = std::move(*(j - 1));
				--j;
			}
			*j = std::move(key);
		}
	}

private:
	std::vector<T> temp_;
};

/// Fast deduplication using open-addressed hash table with bloom filter eviction.
/// Based on ablation study implementation for high-throughput batch deduplication.
class FastDeduplicator {
public:
    static constexpr size_t TABLE_SIZE = 1ULL << 21;  // 2M entries
    static constexpr size_t TABLE_MASK = TABLE_SIZE - 1;
    static constexpr size_t RING_SIZE = 1ULL << 20;   // 1M entries
    static constexpr size_t RING_MASK = RING_SIZE - 1;
    static constexpr uint32_t MAX_PROBES = 128;
    static constexpr uint64_t EMPTY = 0xFFFFFFFFFFFFFFFFULL;
    static constexpr uint64_t TOMBSTONE = 0xFFFFFFFFFFFFFFFEULL;

private:
    alignas(64) std::vector<uint64_t> table_;
    alignas(64) std::vector<uint64_t> ring_;
    size_t ring_head_ = 0;
    size_t count_ = 0;
    bool skip_ = false;

public:
    FastDeduplicator() : table_(TABLE_SIZE, EMPTY), ring_(RING_SIZE, EMPTY) {}

    void set_skip(bool skip) noexcept { skip_ = skip; }

    /// Returns true if duplicate (already seen), false if new
    [[nodiscard]] bool check_and_insert(uint64_t batch_id) noexcept {
        if (skip_) return false;
        if (batch_id >= TOMBSTONE) [[unlikely]] return false;

        const uint64_t h = hash64(batch_id);
        size_t idx = h & TABLE_MASK;
        size_t insert_at = static_cast<size_t>(-1);

        for (uint32_t probe = 0; probe < MAX_PROBES; ++probe) {
            uint64_t entry = table_[idx];

            if (entry == batch_id) return true;  // Duplicate

            if (entry == EMPTY) {
                if (insert_at == static_cast<size_t>(-1)) insert_at = idx;
                break;
            }

            if (entry == TOMBSTONE && insert_at == static_cast<size_t>(-1)) {
                insert_at = idx;
            }

            idx = (idx + 1) & TABLE_MASK;
        }

        if (insert_at == static_cast<size_t>(-1)) [[unlikely]] return false;

        // Evict if at capacity
        if (count_ >= RING_SIZE) {
            size_t oldest = (ring_head_ + RING_SIZE - count_) & RING_MASK;
            uint64_t old_id = ring_[oldest];
            if (old_id != EMPTY && old_id < TOMBSTONE) {
                size_t old_idx = hash64(old_id) & TABLE_MASK;
                for (uint32_t p = 0; p < MAX_PROBES; ++p) {
                    if (table_[old_idx] == old_id) {
                        table_[old_idx] = TOMBSTONE;
                        break;
                    }
                    if (table_[old_idx] == EMPTY) break;
                    old_idx = (old_idx + 1) & TABLE_MASK;
                }
            }
            ring_[oldest] = EMPTY;
            --count_;
        }

        table_[insert_at] = batch_id;
        ring_[ring_head_] = batch_id;
        ring_head_ = (ring_head_ + 1) & RING_MASK;
        ++count_;

        return false;  // New entry
    }

private:
    static uint64_t hash64(uint64_t x) noexcept {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }
};

}  // namespace Embarcadero
