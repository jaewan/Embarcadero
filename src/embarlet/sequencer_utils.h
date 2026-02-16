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

	static constexpr size_t kWindowSize = Sequencer5Config::kClientWindowSize;
	uint64_t window_base{0};
	std::array<bool, kWindowSize> window_seen{};

	bool is_duplicate(uint64_t seq) const {
		if (seq < next_expected) return true;
		if (seq < window_base) return true;
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

	/// Advance next_expected by one (after emitting or discarding the batch at next_expected).
	/// For gap-skip use direct assignment next_expected = seq + 1.
	void advance_next_expected() { next_expected++; }
};

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
