// Embarcadero Sequencer - Ablation Study for OSDI/SOSP/SIGCOMM
// Correctness-first: proper dedup, MPSC ring, Level 5 sliding window,
// hold-buffer emit, ordering validation, per-batch vs per-epoch comparison.
//
// Build: see CMakeLists.txt
// Usage: ./sequencer5_benchmark [brokers] [producers] [duration_sec] [level5_ratio] [num_runs] [use_radix_sort]

#include <atomic>
#include <vector>
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <map>
#include <deque>
#include <random>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <iomanip>
#include <cassert>
#include <memory>
#include <optional>

#ifdef __linux__
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define CPU_PAUSE() _mm_pause()
#define PREFETCH_L1(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T0)
#define PREFETCH_L2(addr) _mm_prefetch(reinterpret_cast<const char*>(addr), _MM_HINT_T1)
#elif defined(__aarch64__)
#define CPU_PAUSE() asm volatile("yield" ::: "memory")
#define PREFETCH_L1(addr) __builtin_prefetch(addr, 0, 3)
#define PREFETCH_L2(addr) __builtin_prefetch(addr, 0, 2)
#else
#define CPU_PAUSE() ((void)0)
#define PREFETCH_L1(addr) ((void)0)
#define PREFETCH_L2(addr) ((void)0)
#endif

namespace embarcadero {

// ============================================================================
// Configuration
// ============================================================================

namespace config {
    inline constexpr size_t CACHE_LINE = 64;
    inline constexpr uint32_t MAX_BROKERS = 32;
    inline constexpr uint32_t MAX_COLLECTORS = 8;
    inline constexpr size_t DEFAULT_PBR_ENTRIES = 64 * 1024;
    inline constexpr size_t DEFAULT_GOI_ENTRIES = 1024 * 1024;
    inline constexpr uint64_t DEFAULT_EPOCH_US = 500;
    inline constexpr uint32_t PIPELINE_DEPTH = 3;
    inline constexpr size_t HOLD_BUFFER_MAX = 100000;  // 10x for high-reorder L5
    inline constexpr int HOLD_MAX_WAIT_EPOCHS = 3;
    inline constexpr size_t CLIENT_SHARDS = 16;
    inline constexpr size_t MAX_CLIENTS_PER_SHARD = 8192;
    inline constexpr uint64_t CLIENT_TTL_EPOCHS = 20000;
    inline constexpr size_t BLOOM_BITS = 1 << 20;       // 1M bits = 128KB
    inline constexpr size_t BLOOM_WORDS = BLOOM_BITS / 64;
    inline constexpr uint32_t BLOOM_HASHES = 4;
    inline constexpr uint64_t BLOOM_RESET_INTERVAL = 1000000;
    inline constexpr uint32_t PREFETCH_DISTANCE = 4;
    inline constexpr size_t COLLECTOR_STATE_CHECK_INTERVAL = 64;  // Was magic 63
    inline constexpr size_t CLIENT_WINDOW_SIZE = 1024;
    inline constexpr size_t RADIX_THRESHOLD = 4096;  // Below this, use std::sort
    inline constexpr size_t LATENCY_SAMPLE_LIMIT = 1000000;
    inline constexpr size_t DEDUP_WINDOW = 1024;  // Per-client recent seqs
    inline constexpr size_t DEDUP_NUM_SHARDS = 16;
    inline constexpr size_t DEDUP_MAX_PER_SHARD = 65536;
    inline constexpr size_t DEDUP_EVICT_BATCH = 256;  // Amortized eviction to avoid latency spikes
    inline constexpr size_t LATENCY_SAMPLE_RESERVE = 1000000;
    inline constexpr size_t LATENCY_RING_SIZE = 1 << 20;  // 1M, lock-free SPSC
    inline constexpr size_t MAX_VALIDATION_ENTRIES = 10000000;
    inline constexpr int RING_MAX_RETRIES = 64;
    inline constexpr size_t RADIX_BITS = 8;
    inline constexpr size_t RADIX_BUCKETS = 1 << RADIX_BITS;
    /// Document §4.2.1: reject batches with stale epoch_created (zombie broker / recovery)
    inline constexpr uint16_t MAX_EPOCH_AGE = 4;
    /// client_gc runs every 2^CLIENT_GC_EPOCH_INTERVAL_LOG2 epochs (was magic 0x3FF)
    inline constexpr uint64_t CLIENT_GC_EPOCH_INTERVAL = 1024;
}

namespace flags {
    inline constexpr uint32_t VALID = 1u << 31;
    inline constexpr uint32_t STRONG_ORDER = 1u << 0;
}

// ============================================================================
// CXL Memory Allocation
// ============================================================================

extern "C" void* allocate_shm(size_t size);

inline uint64_t now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ============================================================================
// Lock-Free Latency Ring (SPSC: sequencer writes, main thread drains after stop)
// ============================================================================
class alignas(128) LatencyRing {
public:
    LatencyRing() : samples_(config::LATENCY_RING_SIZE) {}

    void record(uint64_t ns) noexcept {
        uint64_t idx = write_pos_.fetch_add(1, std::memory_order_relaxed);
        samples_[idx & (config::LATENCY_RING_SIZE - 1)] = ns;
    }

    std::vector<uint64_t> drain() {
        uint64_t wp = write_pos_.load(std::memory_order_acquire);
        std::vector<uint64_t> result;

        if (wp <= config::LATENCY_RING_SIZE) {
            result.assign(samples_.begin(), samples_.begin() + static_cast<size_t>(wp));
        } else {
            // Wraparound: physical order is [start..end)[0..start); logical order is older first
            size_t start = static_cast<size_t>(wp & (config::LATENCY_RING_SIZE - 1));
            result.reserve(config::LATENCY_RING_SIZE);
            result.insert(result.end(), samples_.begin() + start, samples_.end());
            result.insert(result.end(), samples_.begin(), samples_.begin() + start);
        }
        write_pos_.store(0, std::memory_order_release);
        return result;
    }

    void reset() noexcept { write_pos_.store(0, std::memory_order_release); }

private:
    std::vector<uint64_t> samples_;
    alignas(64) std::atomic<uint64_t> write_pos_{0};
};

#ifndef HAS_CXL_ALLOCATOR
void* allocate_shm(size_t size) {
    void* ptr = nullptr;
#ifdef __linux__
    ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (ptr == MAP_FAILED) return nullptr;
    madvise(ptr, size, MADV_HUGEPAGE);
#else
    ptr = aligned_alloc(config::CACHE_LINE, size);
#endif
    return ptr;
}
#endif

// ============================================================================
// Utility
// ============================================================================

constexpr bool is_power_of_2(size_t n) { return n && !(n & (n - 1)); }

inline size_t next_pow2(size_t n) {
    if (n == 0) return 1;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4;
    n |= n >> 8; n |= n >> 16; n |= n >> 32;
    return n + 1;
}

inline uint64_t hash64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

inline uint64_t hash64_2(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

// ============================================================================
// Data Structures (CXL-resident)
// ============================================================================

struct alignas(128) ControlBlock {
    std::atomic<uint64_t> epoch{1};
    std::atomic<uint64_t> sequencer_id{0};
    std::atomic<uint64_t> sequencer_lease{0};
    std::atomic<uint32_t> broker_mask{0};
    std::atomic<uint32_t> num_brokers{0};
    std::atomic<uint64_t> committed_seq{0};
    char _pad[80];
};
static_assert(sizeof(ControlBlock) == 128);

// Producer writes all fields then flags with release; consumer reads flags(acquire) then fields.
// If producer crashes before the release store, slot reuse may expose stale data (acceptable for ablation).
struct alignas(64) PBREntry {
    uint64_t blog_offset;
    uint32_t payload_size;
    uint32_t message_count;
    uint64_t batch_id;
    uint16_t broker_id;
    uint16_t epoch_created;
    std::atomic<uint32_t> flags{0};
    uint64_t client_id;
    uint64_t client_seq;
    uint64_t timestamp_ns;  // For end-to-end latency (inject -> committed)
};
static_assert(sizeof(PBREntry) == 64);

struct alignas(64) GOIEntry {
    std::atomic<uint64_t> global_seq{0};  // Written last with release for safe publish
    uint64_t batch_id;
    uint16_t broker_id;
    uint16_t epoch_sequenced;
    uint32_t pbr_index;
    uint64_t blog_offset;
    uint32_t payload_size;
    uint32_t message_count;
    std::atomic<uint32_t> num_replicated{0};
    uint32_t _pad;
    uint64_t client_id;
    uint64_t client_seq;
};
static_assert(sizeof(GOIEntry) == 64);

struct alignas(128) CompletionVectorEntry {
    std::atomic<uint64_t> completed_pbr_head{0};
    char _pad[120];
};
static_assert(sizeof(CompletionVectorEntry) == 128);

// ============================================================================
// Internal Structures
// ============================================================================

struct BatchInfo {
    uint64_t batch_id;
    uint16_t broker_id;
    uint64_t blog_offset;
    uint32_t payload_size;
    uint32_t message_count;
    uint32_t flags;
    uint64_t client_id;
    uint64_t client_seq;
    uint32_t pbr_index;
    uint64_t timestamp_ns = 0;
    uint64_t global_seq = 0;

    bool is_level5() const noexcept { return (flags & flags::STRONG_ORDER) != 0; }
};

/// Per-client state for Level 5 ordering. Sliding window via bitmask + modular array.
struct ClientState {
    uint64_t next_expected = 1;
    uint64_t highest_sequenced = 0;
    uint64_t last_epoch = 0;

    static constexpr size_t WINDOW_SIZE = config::CLIENT_WINDOW_SIZE;
    uint64_t window_base = 1;
    std::array<bool, WINDOW_SIZE> window_seen{};

    bool is_duplicate(uint64_t seq) const {
        if (seq < next_expected) return true;
        if (seq < window_base) return true;
        if (seq >= window_base + WINDOW_SIZE) return false;
        return window_seen[seq % WINDOW_SIZE];
    }

    void mark_sequenced(uint64_t seq) {
        highest_sequenced = std::max(highest_sequenced, seq);
        if (seq >= window_base && seq < window_base + WINDOW_SIZE) {
            window_seen[seq % WINDOW_SIZE] = true;
        }
        while (window_base < next_expected &&
               window_base + WINDOW_SIZE <= seq + WINDOW_SIZE / 2) {
            window_seen[window_base % WINDOW_SIZE] = false;
            window_base++;
        }
    }

    void advance_next_expected() { next_expected++; }
};

struct HoldEntry {
    BatchInfo batch;
    int wait_epochs = 0;
};

// ============================================================================
// Radix sort by client_id: 4 passes x 16-bit = even swaps, result in original array.
// ============================================================================

class RadixSorter {
public:
    void sort_by_client_id(std::vector<BatchInfo>& arr) {
        const size_t n = arr.size();
        if (n <= 1) return;
        if (n < config::RADIX_THRESHOLD) {
            std::sort(arr.begin(), arr.end(), [](const BatchInfo& a, const BatchInfo& b) {
                return a.client_id < b.client_id;
            });
            return;
        }
        temp_.resize(n);
        constexpr size_t BITS = 16;
        constexpr size_t BUCKETS = 1 << BITS;
        constexpr size_t MASK = BUCKETS - 1;

        BatchInfo* src = arr.data();
        BatchInfo* dst = temp_.data();

        for (size_t pass = 0; pass < 4; ++pass) {
            size_t shift = pass * BITS;
            std::array<size_t, BUCKETS> counts{};
            for (size_t i = 0; i < n; ++i) {
                size_t bucket = (src[i].client_id >> shift) & MASK;
                ++counts[bucket];
            }
            size_t total = 0;
            for (size_t i = 0; i < BUCKETS; ++i) {
                size_t c = counts[i];
                counts[i] = total;
                total += c;
            }
            for (size_t i = 0; i < n; ++i) {
                size_t bucket = (src[i].client_id >> shift) & MASK;
                dst[counts[bucket]++] = std::move(src[i]);
            }
            std::swap(src, dst);
        }
        // After 4 swaps, src == arr.data(), no copy back needed
    }

    void sort_by_client_seq(BatchInfo* begin, BatchInfo* end) {
        size_t n = end - begin;
        if (n <= 1) return;
        for (BatchInfo* it = begin + 1; it < end; ++it) {
            BatchInfo key = std::move(*it);
            uint64_t kseq = key.client_seq;
            BatchInfo* j = it;
            while (j > begin && (j - 1)->client_seq > kseq) {
                *j = std::move(*(j - 1));
                --j;
            }
            *j = std::move(key);
        }
    }

private:
    std::vector<BatchInfo> temp_;
};

// ============================================================================
// MPSC Ring Buffer (Multiple Producers, Single Consumer per broker)
// ============================================================================
// Benchmark has multiple producer threads per broker (e.g. 8 producers, 4 brokers).
// CAS required for reserve().

class alignas(128) MPSCRing {
public:
    explicit MPSCRing(size_t capacity) : capacity_(capacity), mask_(capacity - 1) {
        assert(is_power_of_2(capacity));
    }

    [[nodiscard]] std::optional<uint64_t> reserve() {
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        uint32_t backoff = 1;
        for (int retry = 0; retry < config::RING_MAX_RETRIES; ++retry) {
            uint64_t head = head_.load(std::memory_order_acquire);
            if (tail - head >= capacity_) return std::nullopt;
            if (tail_.compare_exchange_weak(tail, tail + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                return tail;
            }
            for (uint32_t i = 0; i < backoff; ++i) CPU_PAUSE();
            backoff = std::min(backoff * 2, 64u);
        }
        return std::nullopt;
    }

    void advance_head(uint64_t new_head) {
        head_.store(new_head, std::memory_order_release);
    }

    uint64_t head() const noexcept { return head_.load(std::memory_order_acquire); }
    uint64_t tail() const noexcept { return tail_.load(std::memory_order_acquire); }
    size_t mask() const noexcept { return mask_; }

private:
    const size_t capacity_;
    const size_t mask_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

// ============================================================================
// Deduplication with FIFO Eviction (correctness: no false positives)
// ============================================================================

// Single-threaded: only sequencer thread calls check_and_insert; no locks.
class Deduplicator {
public:
    bool check_and_insert(uint64_t batch_id) {
        size_t shard = hash64(batch_id) % config::DEDUP_NUM_SHARDS;
        auto& s = shards_[shard];
        auto [it, inserted] = s.seen.insert(batch_id);
        if (!inserted) return true;

        if (s.seen.size() > config::DEDUP_MAX_PER_SHARD) {
            for (size_t i = 0; i < config::DEDUP_EVICT_BATCH && !s.insertion_order.empty(); ++i) {
                s.seen.erase(s.insertion_order.front());
                s.insertion_order.pop_front();
            }
        }
        s.insertion_order.push_back(batch_id);
        return false;
    }

private:
    struct alignas(64) Shard {
        std::unordered_set<uint64_t> seen;
        std::deque<uint64_t> insertion_order;
    };
    std::array<Shard, config::DEDUP_NUM_SHARDS> shards_;
};

// ============================================================================
// Epoch Buffer State Machine
// ============================================================================
//
// State transitions (single writer per transition):
//   IDLE ──[timer]──► COLLECTING ──[timer]──► SEALED ──[sequencer]──► SEQUENCED
//     ▲                                                                   │
//     │                                                                   ▼
//     └────────────────────────[writer]────────────────────────────── COMMITTED
//
// Thread ownership:
//   IDLE → COLLECTING:      timer thread
//   COLLECTING → SEALED:    timer thread (after waiting for collectors)
//   SEALED → SEQUENCED:     sequencer thread
//   SEQUENCED → COMMITTED:  writer thread
//   COMMITTED → IDLE:       timer thread (implicit in reset_and_start)

class EpochBuffer {
public:
    enum class State : uint32_t {
        IDLE,
        COLLECTING,
        SEALED,      // Collection done, waiting for sequencing
        SEQUENCED,   // Sequencing done, waiting for commit
        COMMITTED
    };

    static const char* state_name(State s) {
        switch (s) {
            case State::IDLE: return "IDLE";
            case State::COLLECTING: return "COLLECTING";
            case State::SEALED: return "SEALED";
            case State::SEQUENCED: return "SEQUENCED";
            case State::COMMITTED: return "COMMITTED";
        }
        return "UNKNOWN";
    }

    struct alignas(64) CollectorBuf {
        std::vector<BatchInfo> batches;
        CollectorBuf() { batches.reserve(16384); }
        void clear() { batches.clear(); }
    };

    std::array<CollectorBuf, config::MAX_COLLECTORS> collectors;
    std::vector<BatchInfo> sequenced;
    uint64_t epoch_number = 0;
    /// Only updated for successfully read (VALID) PBR entries
    std::array<std::atomic<uint64_t>, config::MAX_BROKERS> broker_committed_pbr;

    EpochBuffer() {
        sequenced.reserve(65536);
        for (auto& m : broker_committed_pbr) m.store(0, std::memory_order_relaxed);
    }

    /** Lock-free enter: only mutex used when waiting in seal(). */
    [[nodiscard]] bool enter_collection() {
        State current = state_.load(std::memory_order_acquire);
        if (current != State::COLLECTING) return false;
        active_collectors_.fetch_add(1, std::memory_order_acq_rel);
        if (state_.load(std::memory_order_acquire) != State::COLLECTING) {
            active_collectors_.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        return true;
    }

    void exit_collection() {
        uint32_t prev = active_collectors_.fetch_sub(1, std::memory_order_acq_rel);
        if (prev == 1) {
            std::lock_guard<std::mutex> lk(seal_mutex_);
            seal_cv_.notify_all();
        }
    }

    /** Timer: seal buffer (COLLECTING → SEALED). Waits for active collectors to exit. */
    [[nodiscard]] bool seal() {
        State expected = State::COLLECTING;
        if (!state_.compare_exchange_strong(expected, State::SEALED,
                std::memory_order_acq_rel)) {
            return false;
        }
        std::unique_lock<std::mutex> lk(seal_mutex_);
        seal_cv_.wait(lk, [this] {
            return active_collectors_.load(std::memory_order_acquire) == 0;
        });
        return true;
    }

    void mark_sequenced() {
        state_.store(State::SEQUENCED, std::memory_order_release);
        cv_.notify_all();
    }

    void mark_committed() {
        state_.store(State::COMMITTED, std::memory_order_release);
        cv_.notify_all();
    }

    /** Timer: reset for reuse (COMMITTED → IDLE, then IDLE → COLLECTING). */
    void reset_and_start(uint64_t new_epoch) {
        for (auto& c : collectors) c.clear();
        sequenced.clear();
        for (auto& m : broker_committed_pbr) m.store(0, std::memory_order_relaxed);
        epoch_number = new_epoch;
        active_collectors_.store(0, std::memory_order_relaxed);
        state_.store(State::COLLECTING, std::memory_order_release);
    }

    State state() const { return state_.load(std::memory_order_acquire); }

    bool wait_for(State target, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mutex_);
        return cv_.wait_for(lk, timeout, [&] {
            return state_.load(std::memory_order_relaxed) == target;
        });
    }

    void notify() {
        cv_.notify_all();
        seal_cv_.notify_all();
    }

    bool is_available() const noexcept {
        State s = state_.load(std::memory_order_acquire);
        return s == State::IDLE || s == State::COMMITTED;
    }

private:
    mutable std::mutex mutex_;
    mutable std::mutex seal_mutex_;
    std::condition_variable cv_;
    std::condition_variable seal_cv_;
    std::atomic<State> state_{State::IDLE};
    std::atomic<uint32_t> active_collectors_{0};
};

enum class SequencerMode {
    PER_BATCH_ATOMIC,
    PER_EPOCH_ATOMIC
};

// ============================================================================
// Statistics
// ============================================================================

struct alignas(128) Stats {
    // Sequencer thread (one cache line)
    std::atomic<uint64_t> batches_sequenced{0};
    std::atomic<uint64_t> bytes_sequenced{0};
    std::atomic<uint64_t> epochs_completed{0};
    std::atomic<uint64_t> level5_batches{0};
    std::atomic<uint64_t> gaps_skipped{0};
    std::atomic<uint64_t> duplicates{0};
    std::atomic<uint64_t> batches_dropped{0};
    std::atomic<uint64_t> hold_buffer_size{0};

    alignas(64) std::atomic<uint64_t> pbr_full_count{0};  // Producer threads

    alignas(64) std::atomic<uint64_t> seq_total_ns{0};
    std::atomic<uint64_t> seq_count{0};
    std::atomic<uint64_t> seq_min_ns{UINT64_MAX};
    std::atomic<uint64_t> seq_max_ns{0};
    LatencyRing latency_ring;
    std::atomic<uint64_t> atomics_executed{0};
    std::atomic<uint64_t> sort_time_ns{0};   // L5 sort_by_client_id (diagnostic)
    std::atomic<uint64_t> hold_phase_ns{0};  // drain_hold + age_hold (diagnostic)
    std::atomic<uint64_t> merge_time_ns{0};  // Phase: merge collector buffers
    std::atomic<uint64_t> l5_time_ns{0};    // Phase: process_level5 + drain_hold + age_hold

    void record_seq_latency(uint64_t ns) {
        seq_total_ns.fetch_add(ns, std::memory_order_relaxed);
        seq_count.fetch_add(1, std::memory_order_relaxed);

        uint64_t old = seq_min_ns.load(std::memory_order_relaxed);
        while (ns < old && !seq_min_ns.compare_exchange_weak(old, ns));

        old = seq_max_ns.load(std::memory_order_relaxed);
        while (ns > old && !seq_max_ns.compare_exchange_weak(old, ns));
    }

    void reset_latency() {
        seq_total_ns = 0;
        seq_count = 0;
        seq_min_ns = UINT64_MAX;
        seq_max_ns = 0;
        merge_time_ns = 0;
        l5_time_ns = 0;
        sort_time_ns = 0;
        hold_phase_ns = 0;
        latency_ring.reset();
    }

    double avg_seq_ns() const {
        uint64_t c = seq_count.load();
        return c ? double(seq_total_ns.load()) / c : 0;
    }
};

// ============================================================================
// Configuration
// ============================================================================

struct SequencerConfig {
    uint32_t num_brokers = 4;
    uint32_t num_collectors = 4;
    uint64_t epoch_us = config::DEFAULT_EPOCH_US;
    bool level5_enabled = true;
    SequencerMode mode = SequencerMode::PER_EPOCH_ATOMIC;
    size_t pbr_entries = config::DEFAULT_PBR_ENTRIES;
    size_t goi_entries = config::DEFAULT_GOI_ENTRIES;
    bool use_radix_sort = false;  // A/B: false = std::sort (safe default)

    void validate() {
        num_brokers = std::clamp(num_brokers, 1u, config::MAX_BROKERS);
        num_collectors = std::clamp(num_collectors, 1u, config::MAX_COLLECTORS);
        num_collectors = std::min(num_collectors, num_brokers);
        if (!is_power_of_2(pbr_entries)) pbr_entries = next_pow2(pbr_entries);
        if (!is_power_of_2(goi_entries)) goi_entries = next_pow2(goi_entries);
    }
};

// ============================================================================
// Sequencer
// ============================================================================

class Sequencer {
public:
    explicit Sequencer(SequencerConfig cfg);
    ~Sequencer();

    [[nodiscard]] bool initialize();
    void start();
    void stop();
    [[nodiscard]] bool running() const { return running_.load(std::memory_order_acquire); }

    const Stats& stats() const { return stats_; }
    Stats& stats() { return stats_; }
    uint64_t global_seq() const { return global_seq_.load(std::memory_order_acquire); }
    uint64_t committed_seq() const;
    const SequencerConfig& config() const { return config_; }

    [[nodiscard]] bool inject_batch(uint16_t broker_id, uint64_t batch_id,
                                     uint32_t payload_size, uint32_t msg_count,
                                     uint32_t extra_flags = 0,
                                     uint64_t client_id = 0, uint64_t client_seq = 0);

    bool validate_ordering() const;

    Sequencer(const Sequencer&) = delete;
    Sequencer& operator=(const Sequencer&) = delete;

private:
    void alloc_memory();
    void init_structures();
    void free_memory();

    void timer_loop();
    void collector_loop(int id, std::vector<int> pbr_ids);
    void sequencer_loop();
    void writer_loop();

    void advance_epoch();
    void process_epoch(EpochBuffer& buf);
    void sequence_batches_per_batch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);
    void sequence_batches_per_epoch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);
    void process_level0(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out);
    void process_level5(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);

    void add_to_hold(uint64_t client_id, BatchInfo&& batch, std::vector<BatchInfo>& overflow, uint64_t current_epoch);
    void drain_hold(std::vector<BatchInfo>& out, uint64_t epoch);
    void age_hold(std::vector<BatchInfo>& out, uint64_t current_epoch);
    void client_gc(uint64_t cur_epoch);

    int client_shard(uint64_t id) const {
        return int(hash64(id) & (config::CLIENT_SHARDS - 1));
    }

    SequencerConfig config_;
    Stats stats_;

    // CXL memory
    void* cxl_mem_ = nullptr;
    size_t cxl_size_ = 0;
    ControlBlock* control_ = nullptr;
    CompletionVectorEntry* cv_ = nullptr;
    GOIEntry* goi_ = nullptr;
    PBREntry* pbr_ = nullptr;

    std::vector<std::unique_ptr<MPSCRing>> pbr_rings_;

    /// Separate scan position (where to look next) from committed (highest valid read).
    struct alignas(64) PBRReadState {
        uint64_t scan_pos = 0;
        uint64_t committed_pos = 0;
    };
    std::array<PBRReadState, config::MAX_BROKERS> pbr_state_;

    // Epoch pipeline
    std::array<EpochBuffer, config::PIPELINE_DEPTH> epoch_bufs_;
    alignas(64) std::atomic<uint64_t> global_seq_{0};
    alignas(64) std::atomic<uint64_t> current_epoch_{0};

    // Threads
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};
    std::thread timer_thread_;
    std::vector<std::thread> collector_threads_;
    std::thread sequencer_thread_;
    std::thread writer_thread_;

    // Inter-thread sync
    std::mutex epoch_mtx_;
    std::condition_variable epoch_cv_;

    Deduplicator dedup_;
    RadixSorter sorter_;
    std::vector<BatchInfo> merge_buffer_;
    std::vector<BatchInfo> ready_buffer_;
    std::vector<BatchInfo> l0_buffer_;
    std::vector<BatchInfo> l5_buffer_;
    mutable std::mutex validation_mtx_;
    std::vector<std::pair<uint64_t, uint64_t>> committed_batches_;
    static constexpr size_t MAX_VALIDATION_ENTRIES = config::MAX_VALIDATION_ENTRIES;

    // Per-client state (Level 5) - single-threaded sequencer only; no locks
    struct alignas(64) ClientShard {
        std::unordered_map<uint64_t, ClientState> states;
    };
    std::array<ClientShard, config::CLIENT_SHARDS> client_shards_;

    // Hold buffer - ONLY accessed by sequencer thread (no lock needed for access)
    std::unordered_map<uint64_t, std::map<uint64_t, HoldEntry>> hold_buf_;
    size_t hold_size_ = 0;
    std::multimap<uint64_t, std::pair<uint64_t, uint64_t>> expiry_queue_;  // expiry_epoch -> (client_id, client_seq)
    int timer_fd_ = -1;
};

// ============================================================================
// Implementation
// ============================================================================

Sequencer::Sequencer(SequencerConfig cfg) : config_(cfg) {
    config_.validate();
}

Sequencer::~Sequencer() {
    stop();
#ifdef __linux__
    if (timer_fd_ >= 0) { ::close(timer_fd_); timer_fd_ = -1; }
#endif
    free_memory();
}

bool Sequencer::initialize() {
    try {
        alloc_memory();
        init_structures();
#ifdef __linux__
        timer_fd_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
#endif
        return true;
    } catch (...) {
        return false;
    }
}

void Sequencer::alloc_memory() {
    const size_t ctrl_sz = 4096;
    const size_t cv_sz = config::MAX_BROKERS * sizeof(CompletionVectorEntry);
    const size_t goi_sz = config_.goi_entries * sizeof(GOIEntry);
    const size_t pbr_sz = config_.num_brokers * config_.pbr_entries * sizeof(PBREntry);

    cxl_size_ = ctrl_sz + cv_sz + goi_sz + pbr_sz;
    cxl_mem_ = allocate_shm(cxl_size_);
    if (!cxl_mem_) throw std::bad_alloc();

    std::memset(cxl_mem_, 0, cxl_size_);

    auto* p = static_cast<uint8_t*>(cxl_mem_);
    control_ = reinterpret_cast<ControlBlock*>(p); p += ctrl_sz;
    cv_ = reinterpret_cast<CompletionVectorEntry*>(p); p += cv_sz;
    goi_ = reinterpret_cast<GOIEntry*>(p); p += goi_sz;
    pbr_ = reinterpret_cast<PBREntry*>(p);
}

void Sequencer::init_structures() {
    control_->epoch.store(1, std::memory_order_relaxed);
    control_->num_brokers.store(config_.num_brokers, std::memory_order_relaxed);
    control_->broker_mask.store((1u << config_.num_brokers) - 1, std::memory_order_relaxed);

    pbr_rings_.clear();
    for (uint32_t i = 0; i < config_.num_brokers; ++i) {
        pbr_rings_.push_back(std::make_unique<MPSCRing>(config_.pbr_entries));
    }

    for (auto& st : pbr_state_) {
        st.scan_pos = 0;
        st.committed_pos = 0;
    }

    epoch_bufs_[0].reset_and_start(0);
    global_seq_.store(0, std::memory_order_relaxed);
    current_epoch_.store(0, std::memory_order_relaxed);
}

void Sequencer::free_memory() {
    if (cxl_mem_) {
#ifdef __linux__
        munmap(cxl_mem_, cxl_size_);
#else
        free(cxl_mem_);
#endif
        cxl_mem_ = nullptr;
    }
}

uint64_t Sequencer::committed_seq() const {
    return control_ ? control_->committed_seq.load(std::memory_order_acquire) : 0;
}

void Sequencer::start() {
    if (running_.exchange(true)) return;
    shutdown_.store(false);

    // Assign PBRs to collectors
    std::vector<std::vector<int>> assignments(config_.num_collectors);
    for (uint32_t i = 0; i < config_.num_brokers; ++i) {
        assignments[i % config_.num_collectors].push_back(int(i));
    }

    writer_thread_ = std::thread(&Sequencer::writer_loop, this);
    sequencer_thread_ = std::thread(&Sequencer::sequencer_loop, this);

    for (uint32_t i = 0; i < config_.num_collectors; ++i) {
        collector_threads_.emplace_back(&Sequencer::collector_loop, this,
                                        int(i), std::move(assignments[i]));
    }

    timer_thread_ = std::thread(&Sequencer::timer_loop, this);
}

void Sequencer::stop() {
    if (!running_.exchange(false)) return;
    shutdown_.store(true);

    for (auto& buf : epoch_bufs_) buf.notify();
    {
        std::lock_guard<std::mutex> lk(epoch_mtx_);
        epoch_cv_.notify_all();
    }

    if (timer_thread_.joinable()) timer_thread_.join();
    for (auto& t : collector_threads_) if (t.joinable()) t.join();
    collector_threads_.clear();
    if (sequencer_thread_.joinable()) sequencer_thread_.join();
    if (writer_thread_.joinable()) writer_thread_.join();
}

void Sequencer::timer_loop() {
    using Clock = std::chrono::steady_clock;
    const auto epoch_duration = std::chrono::microseconds(config_.epoch_us);
    auto next_tick = Clock::now() + epoch_duration;

#ifdef __linux__
    if (timer_fd_ >= 0) {
        struct itimerspec its {};
        its.it_value.tv_nsec = config_.epoch_us * 1000ULL;
        its.it_interval.tv_nsec = config_.epoch_us * 1000ULL;
        timerfd_settime(timer_fd_, 0, &its, nullptr);

        while (!shutdown_.load(std::memory_order_acquire)) {
            uint64_t expirations = 0;
            bool fired = false;
            for (int spin = 0; spin < 200; ++spin) {
                if (read(timer_fd_, &expirations, sizeof(expirations)) > 0) { fired = true; break; }
                if (shutdown_.load(std::memory_order_acquire)) return;
                CPU_PAUSE();
            }
            if (!fired) {
                struct pollfd pfd { timer_fd_, POLLIN, 0 };
                poll(&pfd, 1, static_cast<int>(config_.epoch_us / 500));
            } else {
                advance_epoch();
            }
        }
        return;
    }
#endif

    while (!shutdown_.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(next_tick - now);
        if (remaining.count() > 100) {
            std::this_thread::sleep_for(remaining - std::chrono::microseconds(50));
        }
        while (Clock::now() < next_tick) {
            if (shutdown_.load(std::memory_order_acquire)) return;
            CPU_PAUSE();
        }
        next_tick += epoch_duration;
        advance_epoch();
    }
}

void Sequencer::advance_epoch() {
    uint64_t cur = current_epoch_.load(std::memory_order_acquire);
    auto& cur_buf = epoch_bufs_[cur % config::PIPELINE_DEPTH];

    // Pre-check: is next buffer available? (avoids livelock: never seal without a free slot)
    uint64_t next = cur + 1;
    auto& next_buf = epoch_bufs_[next % config::PIPELINE_DEPTH];
    EpochBuffer::State next_state = next_buf.state();
    if (next_state != EpochBuffer::State::IDLE &&
        next_state != EpochBuffer::State::COMMITTED) {
        return;  // Pipeline backed up - don't seal, wait for drain
    }

    if (!cur_buf.seal()) {
        return;  // Not in COLLECTING state
    }

    next_buf.reset_and_start(next);
    current_epoch_.store(next, std::memory_order_release);
    epoch_cv_.notify_all();
}

void Sequencer::collector_loop(int id, std::vector<int> pbr_ids) {
    const size_t pbr_mask = config_.pbr_entries - 1;

    while (!shutdown_.load(std::memory_order_acquire)) {
        uint64_t epoch = current_epoch_.load(std::memory_order_acquire);
        auto& buf = epoch_bufs_[epoch % config::PIPELINE_DEPTH];

        if (!buf.enter_collection()) {
            std::unique_lock<std::mutex> lk(epoch_mtx_);
            epoch_cv_.wait_for(lk, std::chrono::milliseconds(1));
            continue;
        }

        auto& my_batches = buf.collectors[id].batches;

        for (int pbr_id : pbr_ids) {
            auto& ring = *pbr_rings_[pbr_id];
            auto& state = pbr_state_[pbr_id];
            uint64_t tail = ring.tail();
            PBREntry* base = pbr_ + (pbr_id * config_.pbr_entries);

            for (uint32_t i = 0; i < config::PREFETCH_DISTANCE && state.scan_pos + i < tail; ++i) {
                PREFETCH_L1(&base[(state.scan_pos + i) & pbr_mask]);
            }

            uint64_t highest_committed = state.committed_pos;
            uint64_t scan = state.scan_pos;

            while (scan < tail) {
                if ((scan & (config::COLLECTOR_STATE_CHECK_INTERVAL - 1)) == 0 &&
                    buf.state() != EpochBuffer::State::COLLECTING) {
                    break;
                }

                if (scan + config::PREFETCH_DISTANCE < tail) {
                    PREFETCH_L1(&base[(scan + config::PREFETCH_DISTANCE) & pbr_mask]);
                }

                PBREntry& e = base[scan & pbr_mask];
                uint32_t f = e.flags.load(std::memory_order_acquire);

                if (!(f & flags::VALID)) {
                    ++scan;
                    continue;
                }

                // Document §4.2.1: skip stale epoch (zombie broker or recovery artifact)
                uint16_t cur_epoch_16 = static_cast<uint16_t>(
                    control_->epoch.load(std::memory_order_relaxed));
                uint16_t created = e.epoch_created;
                if ((static_cast<uint16_t>(cur_epoch_16 - created) & 0xFFFFu) >
                    config::MAX_EPOCH_AGE) {
                    ++scan;
                    continue;
                }

                BatchInfo info;
                info.batch_id = e.batch_id;
                info.broker_id = e.broker_id;
                info.blog_offset = e.blog_offset;
                info.payload_size = e.payload_size;
                info.message_count = e.message_count;
                info.flags = f;
                info.client_id = e.client_id;
                info.client_seq = e.client_seq;
                info.pbr_index = static_cast<uint32_t>(scan);
                info.timestamp_ns = e.timestamp_ns;
                info.global_seq = 0;

                my_batches.push_back(info);
                highest_committed = scan + 1;
                ++scan;
            }

            state.scan_pos = scan;
            if (highest_committed > state.committed_pos) {
                state.committed_pos = highest_committed;
                buf.broker_committed_pbr[pbr_id].store(highest_committed, std::memory_order_release);
            }
        }

        buf.exit_collection();
    }
}

void Sequencer::sequencer_loop() {
    uint64_t next = 0;

    while (!shutdown_.load(std::memory_order_acquire)) {
        auto& buf = epoch_bufs_[next % config::PIPELINE_DEPTH];

        if (!buf.wait_for(EpochBuffer::State::SEALED, std::chrono::milliseconds(10))) {
            if (shutdown_.load(std::memory_order_acquire)) break;
            continue;
        }

        if (buf.epoch_number != next) {
            assert(false && "Epoch number mismatch");
            continue;
        }

        auto t0 = std::chrono::steady_clock::now();
        process_epoch(buf);
        auto t1 = std::chrono::steady_clock::now();

        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        stats_.record_seq_latency(ns);
        stats_.epochs_completed.fetch_add(1, std::memory_order_relaxed);

        // Phase timing (after record so total/merge/l5/count refer to same epoch count)
        uint64_t count = stats_.seq_count.load(std::memory_order_relaxed);
        if (count > 0 && (count % 100) == 0) {
            uint64_t merge = stats_.merge_time_ns.load(std::memory_order_relaxed);
            uint64_t l5 = stats_.l5_time_ns.load(std::memory_order_relaxed);
            uint64_t total = stats_.seq_total_ns.load(std::memory_order_relaxed);
            uint64_t other_ns = (total > merge + l5) ? (total - merge - l5) : 0;
            std::cerr << "[PHASE] merge=" << (merge / count / 1000) << "μs "
                      << "l5=" << (l5 / count / 1000) << "μs "
                      << "other=" << (other_ns / count / 1000) << "μs\n";
            // L5 breakdown: sort% vs hold_phase% vs other (within L5 phase)
            uint64_t sort_ns = stats_.sort_time_ns.load(std::memory_order_relaxed);
            uint64_t hold_ns = stats_.hold_phase_ns.load(std::memory_order_relaxed);
            if (l5 > 0) {
                double sort_pct = 100.0 * static_cast<double>(sort_ns) / static_cast<double>(l5);
                double hold_pct = 100.0 * static_cast<double>(hold_ns) / static_cast<double>(l5);
                uint64_t rest = (sort_ns + hold_ns <= l5) ? (l5 - sort_ns - hold_ns) : 0;
                double rest_pct = 100.0 * static_cast<double>(rest) / static_cast<double>(l5);
                std::cerr << "[L5 BREAKDOWN] sort=" << std::fixed << std::setprecision(1)
                          << sort_pct << "% hold=" << hold_pct << "% other=" << rest_pct << "%\n";
            }
        }

        for (uint32_t i = 0; i < config_.num_brokers; ++i) {
            uint64_t committed = buf.broker_committed_pbr[i].load(std::memory_order_acquire);
            if (committed > 0) {
                pbr_rings_[i]->advance_head(committed);
            }
        }

        buf.mark_sequenced();
        ++next;
    }
}

void Sequencer::process_epoch(EpochBuffer& buf) {
    merge_buffer_.clear();
    ready_buffer_.clear();

    uint64_t t_merge0 = now_ns();
    size_t total = 0;
    for (uint32_t i = 0; i < config_.num_collectors; ++i) {
        total += buf.collectors[i].batches.size();
    }
    merge_buffer_.reserve(total);
    for (uint32_t i = 0; i < config_.num_collectors; ++i) {
        auto& v = buf.collectors[i].batches;
        merge_buffer_.insert(merge_buffer_.end(),
                             std::make_move_iterator(v.begin()),
                             std::make_move_iterator(v.end()));
        v.clear();
    }
    stats_.merge_time_ns.fetch_add(now_ns() - t_merge0, std::memory_order_relaxed);

    uint64_t t_seq0 = now_ns();
    ready_buffer_.reserve(merge_buffer_.size() + 1024);
    if (config_.mode == SequencerMode::PER_BATCH_ATOMIC) {
        sequence_batches_per_batch(merge_buffer_, ready_buffer_, buf.epoch_number);
    } else {
        sequence_batches_per_epoch(merge_buffer_, ready_buffer_, buf.epoch_number);
    }
    stats_.l5_time_ns.fetch_add(now_ns() - t_seq0, std::memory_order_relaxed);

    uint64_t now = now_ns();
    for (const auto& b : ready_buffer_) {
        if (b.timestamp_ns > 0) {
            stats_.latency_ring.record(now - b.timestamp_ns);
        }
    }

    uint64_t bytes = 0;
    for (const auto& b : ready_buffer_) bytes += b.payload_size;
    stats_.batches_sequenced.fetch_add(ready_buffer_.size(), std::memory_order_relaxed);
    stats_.bytes_sequenced.fetch_add(bytes, std::memory_order_relaxed);

    buf.sequenced = std::move(ready_buffer_);
    client_gc(buf.epoch_number);
}

void Sequencer::sequence_batches_per_batch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch) {
    l0_buffer_.clear();
    l5_buffer_.clear();
    for (auto& b : in) {
        if (b.is_level5() && config_.level5_enabled) {
            l5_buffer_.push_back(std::move(b));
        } else {
            l0_buffer_.push_back(std::move(b));
        }
    }
    for (auto& b : l0_buffer_) {
        if (!dedup_.check_and_insert(b.batch_id)) {
            b.global_seq = global_seq_.fetch_add(1, std::memory_order_relaxed);
            stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
            out.push_back(std::move(b));
        } else {
            stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!l5_buffer_.empty()) {
        std::vector<BatchInfo> l5_out;
        process_level5(l5_buffer_, l5_out, epoch);
        for (auto& b : l5_out) {
            b.global_seq = global_seq_.fetch_add(1, std::memory_order_relaxed);
            stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
            out.push_back(std::move(b));
        }
    }
    uint64_t t_hold = now_ns();
    drain_hold(out, epoch);
    age_hold(out, epoch);
    stats_.hold_phase_ns.fetch_add(now_ns() - t_hold, std::memory_order_relaxed);
}

void Sequencer::sequence_batches_per_epoch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch) {
    l0_buffer_.clear();
    l5_buffer_.clear();
    for (auto& b : in) {
        if (b.is_level5() && config_.level5_enabled) {
            l5_buffer_.push_back(std::move(b));
        } else {
            l0_buffer_.push_back(std::move(b));
        }
    }
    process_level0(l0_buffer_, out);
    if (!l5_buffer_.empty()) process_level5(l5_buffer_, out, epoch);
    uint64_t t_hold = now_ns();
    drain_hold(out, epoch);
    age_hold(out, epoch);
    stats_.hold_phase_ns.fetch_add(now_ns() - t_hold, std::memory_order_relaxed);
    if (!out.empty()) {
        uint64_t base = global_seq_.fetch_add(out.size(), std::memory_order_relaxed);
        stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < out.size(); ++i) out[i].global_seq = base + i;
    }
}

void Sequencer::process_level0(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out) {
    for (auto& b : in) {
        if (!dedup_.check_and_insert(b.batch_id)) {
            out.push_back(std::move(b));
        } else {
            stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Sequencer::process_level5(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch) {
    stats_.level5_batches.fetch_add(in.size(), std::memory_order_relaxed);

    std::vector<BatchInfo> l5_overflow;
    l5_overflow.reserve(256);

    uint64_t t0 = now_ns();
    if (config_.use_radix_sort) {
        sorter_.sort_by_client_id(in);
    } else {
        std::sort(in.begin(), in.end(), [](const BatchInfo& a, const BatchInfo& b) {
            return a.client_id < b.client_id;
        });
    }
    stats_.sort_time_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);

    auto it = in.begin();
    while (it != in.end()) {
        uint64_t cid = it->client_id;
        auto start = it;
        while (it != in.end() && it->client_id == cid) ++it;

        sorter_.sort_by_client_seq(&*start, &*it);

        int shard = client_shard(cid);
        auto& st = client_shards_[shard].states[cid];
        st.last_epoch = epoch;

        for (auto curr = start; curr != it; ++curr) {
            auto& b = *curr;
            if (st.is_duplicate(b.client_seq)) {
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (b.client_seq == st.next_expected) {
                if (!dedup_.check_and_insert(b.batch_id)) {
                    out.push_back(std::move(b));
                    st.mark_sequenced(b.client_seq);
                    st.advance_next_expected();
                } else {
                    st.mark_sequenced(b.client_seq);
                    st.advance_next_expected();
                    stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                }
            } else if (b.client_seq > st.next_expected) {
                add_to_hold(cid, std::move(b), l5_overflow, epoch);
            }
        }
    }
    for (auto& b : l5_overflow) out.push_back(std::move(b));
}

void Sequencer::add_to_hold(uint64_t client_id, BatchInfo&& batch, std::vector<BatchInfo>& overflow,
                            uint64_t current_epoch) {
    if (hold_size_ >= config::HOLD_BUFFER_MAX) {
        stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
        overflow.push_back(std::move(batch));
        return;
    }

    uint64_t seq = batch.client_seq;
    uint64_t expiry_epoch = current_epoch + config::HOLD_MAX_WAIT_EPOCHS;

    auto& client_map = hold_buf_[client_id];
    auto [iter, inserted] = client_map.try_emplace(seq, HoldEntry{std::move(batch), 0});
    if (inserted) {
        ++hold_size_;
        expiry_queue_.emplace(expiry_epoch, std::make_pair(client_id, seq));
        stats_.hold_buffer_size.store(hold_size_, std::memory_order_relaxed);
    }
}

void Sequencer::drain_hold(std::vector<BatchInfo>& out, uint64_t epoch) {
    for (auto cit = hold_buf_.begin(); cit != hold_buf_.end(); ) {
        uint64_t cid = cit->first;
        auto& held = cit->second;

        int shard = client_shard(cid);
        auto& st = client_shards_[shard].states[cid];
        st.last_epoch = epoch;

        auto it = held.begin();
        while (it != held.end() && it->first == st.next_expected) {
            bool is_dup = dedup_.check_and_insert(it->second.batch.batch_id);
            if (!is_dup) {
                out.push_back(std::move(it->second.batch));
            } else {
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
            }
            st.mark_sequenced(it->first);
            st.advance_next_expected();
            it = held.erase(it);
            --hold_size_;
        }

        if (held.empty()) {
            cit = hold_buf_.erase(cit);
        } else {
            ++cit;
        }
    }
    stats_.hold_buffer_size.store(hold_size_, std::memory_order_relaxed);
}

void Sequencer::age_hold(std::vector<BatchInfo>& out, uint64_t current_epoch) {
    // Only process entries that have actually expired - O(expired) not O(hold_size)
    auto end_it = expiry_queue_.upper_bound(current_epoch);

    for (auto it = expiry_queue_.begin(); it != end_it; ) {
        uint64_t cid = it->second.first;
        uint64_t seq = it->second.second;
        it = expiry_queue_.erase(it);

        auto cit = hold_buf_.find(cid);
        if (cit == hold_buf_.end()) continue;
        auto entry_it = cit->second.find(seq);
        if (entry_it == cit->second.end()) continue;  // Already drained in-order

        BatchInfo batch = std::move(entry_it->second.batch);
        cit->second.erase(entry_it);
        if (cit->second.empty()) hold_buf_.erase(cit);
        --hold_size_;

        int shard = client_shard(cid);
        auto& st = client_shards_[shard].states[cid];
        if (seq >= st.next_expected) {
            stats_.gaps_skipped.fetch_add(seq - st.next_expected + 1, std::memory_order_relaxed);
            st.next_expected = seq + 1;
        }
        st.mark_sequenced(seq);
        if (!dedup_.check_and_insert(batch.batch_id)) {
            out.push_back(std::move(batch));
        }
    }

    for (auto it = hold_buf_.begin(); it != hold_buf_.end(); ) {
        if (it->second.empty()) it = hold_buf_.erase(it);
        else ++it;
    }
    stats_.hold_buffer_size.store(hold_size_, std::memory_order_relaxed);
}

void Sequencer::client_gc(uint64_t cur_epoch) {
    if ((cur_epoch & (config::CLIENT_GC_EPOCH_INTERVAL - 1)) != 0) return;

    for (auto& shard : client_shards_) {
        if (shard.states.size() <= config::MAX_CLIENTS_PER_SHARD) continue;

        std::vector<uint64_t> evict;
        for (const auto& [cid, st] : shard.states) {
            if (cur_epoch - st.last_epoch > config::CLIENT_TTL_EPOCHS) {
                evict.push_back(cid);
            }
        }
        for (uint64_t c : evict) shard.states.erase(c);
    }
}

void Sequencer::writer_loop() {
    uint64_t next = 0;
    const size_t goi_mask = config_.goi_entries - 1;

    while (!shutdown_.load(std::memory_order_acquire)) {
        auto& buf = epoch_bufs_[next % config::PIPELINE_DEPTH];

        if (!buf.wait_for(EpochBuffer::State::SEQUENCED, std::chrono::milliseconds(10))) {
            if (shutdown_.load(std::memory_order_acquire)) break;
            continue;
        }

        uint64_t max_seq = 0;
        for (const auto& b : buf.sequenced) {
            GOIEntry& e = goi_[b.global_seq & goi_mask];
            e.batch_id = b.batch_id;
            e.broker_id = b.broker_id;
            e.epoch_sequenced = static_cast<uint16_t>(control_->epoch.load(std::memory_order_relaxed));
            e.pbr_index = b.pbr_index;
            e.blog_offset = b.blog_offset;
            e.payload_size = b.payload_size;
            e.message_count = b.message_count;
            e.num_replicated.store(0, std::memory_order_relaxed);
            e.client_id = b.client_id;
            e.client_seq = b.client_seq;
            e.global_seq.store(b.global_seq, std::memory_order_release);
            max_seq = std::max(max_seq, b.global_seq);
            {
                std::lock_guard<std::mutex> lk(validation_mtx_);
                if (committed_batches_.size() < MAX_VALIDATION_ENTRIES) {
                    committed_batches_.emplace_back(b.global_seq, b.batch_id);
                }
            }
        }

        if (!buf.sequenced.empty()) {
            control_->committed_seq.store(max_seq, std::memory_order_release);
        }

        buf.sequenced.clear();
        buf.mark_committed();
        ++next;
    }
}

bool Sequencer::inject_batch(uint16_t broker_id, uint64_t batch_id,
                              uint32_t payload_size, uint32_t msg_count,
                              uint32_t extra_flags,
                              uint64_t client_id, uint64_t client_seq) {
    if (broker_id >= config_.num_brokers) return false;
    auto& ring = *pbr_rings_[broker_id];
    auto slot = ring.reserve();
    if (!slot) {
        stats_.pbr_full_count.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    size_t idx = *slot & ring.mask();
    PBREntry& e = pbr_[broker_id * config_.pbr_entries + idx];
    e.blog_offset = 0;
    e.payload_size = payload_size;
    e.message_count = msg_count;
    e.batch_id = batch_id;
    e.broker_id = broker_id;
    e.epoch_created = static_cast<uint16_t>(control_->epoch.load(std::memory_order_relaxed));
    e.client_id = client_id;
    e.client_seq = client_seq;
    e.timestamp_ns = now_ns();
    e.flags.store(extra_flags | flags::VALID, std::memory_order_release);
    return true;
}

bool Sequencer::validate_ordering() const {
    std::lock_guard<std::mutex> lk(validation_mtx_);
    if (committed_batches_.empty()) return true;
    uint64_t prev_seq = 0;
    for (const auto& p : committed_batches_) {
        if (p.first <= prev_seq && prev_seq > 0) return false;
        prev_seq = p.first;
    }
    std::vector<uint64_t> seqs;
    for (const auto& p : committed_batches_) seqs.push_back(p.first);
    std::sort(seqs.begin(), seqs.end());
    for (size_t i = 1; i < seqs.size(); ++i) {
        if (seqs[i] != seqs[i - 1] + 1 && seqs[i] != seqs[i - 1]) return false;
    }
    return true;
}

}  // namespace embarcadero

// ============================================================================
// Benchmark
// ============================================================================

namespace bench {

using namespace embarcadero;
using Clock = std::chrono::steady_clock;

struct Config {
    uint32_t brokers = 4;
    uint32_t collectors = 4;
    uint64_t epoch_us = 500;
    uint32_t producers = 8;
    uint32_t batch_size = 1024;
    uint32_t msgs_per_batch = 10;
    bool level5 = true;
    double level5_ratio = 0.1;
    uint32_t clients = 100;
    uint64_t warmup_ms = 1000;
    uint64_t duration_sec = 10;
    SequencerMode mode = SequencerMode::PER_EPOCH_ATOMIC;
    bool validate = true;
    bool use_radix_sort = false;  // A/B: false = std::sort (safe default)
    int num_runs = 5;             // Runs per test; if >= 2, report median [min, max]. Use 5+ for publication.
    bool stress_test = true;      // Include 100% L5 stress test
    size_t pbr_entries = 0;      // 0 = use sequencer default; set >0 for high producer count to avoid PBR saturation
};

struct Result {
    double batches_sec = 0;
    double mb_sec = 0;
    double msgs_sec = 0;
    double p50_us = 0, p95_us = 0, p99_us = 0, p999_us = 0;
    double avg_seq_ns = 0;
    uint64_t min_seq_ns = 0, max_seq_ns = 0;
    uint64_t batches = 0, epochs = 0, level5 = 0, gaps = 0, dropped = 0, atomics = 0;
    uint64_t pbr_full = 0;  // PBR reserve failures (saturation)
    double duration = 0;
    double efficiency_pct = 0;  // 100 * achieved / (21 GB/s theoretical max)
};

struct StatResult {
    double median = 0;
    double min_val = 0;
    double max_val = 0;
    double stddev = 0;

    static StatResult compute(std::vector<double>& values) {
        StatResult out;
        if (values.empty()) return out;
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        out.median = (n % 2) ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
        out.min_val = values.front();
        out.max_val = values.back();
        const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(n);
        double sq_sum = 0;
        for (double v : values) sq_sum += (v - mean) * (v - mean);
        out.stddev = (n > 1) ? std::sqrt(sq_sum / static_cast<double>(n)) : 0;
        return out;
    }
};

struct MultiRunResult {
    StatResult throughput;
    StatResult latency_p99;
    double median_mb_sec = 0;
    double median_efficiency_pct = 0;  // 100 * throughput / (21 GB/s theoretical max)
    uint64_t total_atomics = 0;
    uint64_t total_epochs = 0;   // Epochs completed (per-epoch: atomics == epochs with non-empty output)
    uint64_t total_gaps = 0;
    uint64_t total_dropped = 0;
    uint64_t total_pbr_full = 0; // PBR reserve failures across runs
};

class Runner {
public:
    explicit Runner(Config c) : cfg_(c) {}

    Result run() {
        SequencerConfig sc;
        sc.num_brokers = cfg_.brokers;
        sc.num_collectors = cfg_.collectors;
        sc.epoch_us = cfg_.epoch_us;
        sc.level5_enabled = cfg_.level5;
        sc.mode = cfg_.mode;
        sc.use_radix_sort = cfg_.use_radix_sort;
        if (cfg_.pbr_entries > 0) sc.pbr_entries = cfg_.pbr_entries;

        Sequencer seq(sc);
        if (!seq.initialize()) throw std::runtime_error("Init failed");
        seq.start();

        std::cout << "  Warmup..." << std::flush;
        produce(seq, cfg_.warmup_ms, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::cout << " done\n";

        auto base_b = seq.stats().batches_sequenced.load();
        auto base_bytes = seq.stats().bytes_sequenced.load();
        auto base_e = seq.stats().epochs_completed.load();
        auto base_level5 = seq.stats().level5_batches.load();
        auto base_gaps = seq.stats().gaps_skipped.load();
        auto base_dropped = seq.stats().batches_dropped.load();
        auto base_atomics = seq.stats().atomics_executed.load();
        auto base_pbr_full = seq.stats().pbr_full_count.load();
        seq.stats().reset_latency();

        std::cout << "  Running..." << std::flush;
        std::vector<double> lats;
        auto t0 = Clock::now();
        produce(seq, cfg_.duration_sec * 1000, &lats);
        auto t1 = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << " done\n";

        if (cfg_.validate) {
            std::cout << "  Validating..." << std::flush;
            if (seq.validate_ordering()) std::cout << " PASSED\n";
            else std::cout << " FAILED\n";
        }

        seq.stop();

        Result r;
        r.duration = std::chrono::duration<double>(t1 - t0).count();
        r.batches = seq.stats().batches_sequenced.load() - base_b;
        r.epochs = seq.stats().epochs_completed.load() - base_e;
        r.level5 = seq.stats().level5_batches.load() - base_level5;
        r.gaps = seq.stats().gaps_skipped.load() - base_gaps;
        r.dropped = seq.stats().batches_dropped.load() - base_dropped;
        r.atomics = seq.stats().atomics_executed.load() - base_atomics;
        r.pbr_full = seq.stats().pbr_full_count.load() - base_pbr_full;

        auto bytes = seq.stats().bytes_sequenced.load() - base_bytes;
        r.batches_sec = r.batches / r.duration;
        r.mb_sec = (bytes / 1e6) / r.duration;
        r.msgs_sec = (r.batches * cfg_.msgs_per_batch) / r.duration;

        r.avg_seq_ns = seq.stats().avg_seq_ns();
        r.min_seq_ns = seq.stats().seq_min_ns.load();
        r.max_seq_ns = seq.stats().seq_max_ns.load();

        if (!lats.empty()) {
            std::sort(lats.begin(), lats.end());
            size_t n = lats.size();
            r.p50_us = lats[n / 2];
            r.p95_us = lats[n * 95 / 100];
            r.p99_us = lats[n * 99 / 100];
            r.p999_us = lats[std::min(n * 999 / 1000, n - 1)];
        }

        if (r.batches_sec == 0 && r.duration > 0) {
            std::cerr << "  WARNING: Zero throughput. PBR_full=" << r.pbr_full
                      << " (if high: PBR saturated - try larger pbr_entries or fewer producers).\n";
        }

        // Efficiency vs theoretical max (21 GB/s CXL bandwidth)
        const double theoretical_max_batches_sec = 21e9 / static_cast<double>(cfg_.batch_size);
        r.efficiency_pct = (theoretical_max_batches_sec > 0)
            ? (100.0 * r.batches_sec / theoretical_max_batches_sec) : 0;

        return r;
    }

private:
    void produce(Sequencer& seq, uint64_t ms, std::vector<double>* lats) {
        std::atomic<bool> go{true};
        std::vector<std::thread> threads;
        std::vector<std::vector<double>> tlats(cfg_.producers);

        // Producer affinity: each producer owns a disjoint subset of clients so (client_id, client_seq)
        // is unique and strictly increasing per client (no duplicate seq from different producers).
        const uint32_t clients_per_producer = std::max(1u, (cfg_.clients + cfg_.producers - 1) / cfg_.producers);

        for (uint32_t tid = 0; tid < cfg_.producers; ++tid) {
            threads.emplace_back([&, tid] {
                thread_local std::mt19937_64 rng(tid + 42);
                std::uniform_real_distribution<> ldist(0, 1);

                const uint32_t client_base = tid * clients_per_producer;
                const uint32_t my_clients = std::min(clients_per_producer, cfg_.clients - client_base);
                std::uniform_int_distribution<uint32_t> cdist(0, my_clients > 0 ? my_clients - 1 : 0);
                std::vector<uint64_t> cseqs(my_clients, 0);

                uint64_t bid = tid;
                auto& mylats = tlats[tid];

                while (go.load(std::memory_order_relaxed)) {
                    auto t0 = Clock::now();
                    uint32_t ef = 0;
                    uint64_t cid = 0, cseq_val = 0;
                    if (cfg_.level5 && my_clients > 0 && ldist(rng) < cfg_.level5_ratio) {
                        ef = flags::STRONG_ORDER;
                        uint32_t local_c = cdist(rng);
                        cid = client_base + local_c + 1;  // 1-based client_id
                        cseq_val = ++cseqs[local_c];
                    }
                    uint64_t batch_id = (uint64_t(tid) << 48) | bid;
                    if (seq.inject_batch(bid % cfg_.brokers, batch_id,
                            cfg_.batch_size, cfg_.msgs_per_batch, ef, cid, cseq_val)) {
                        auto t1 = Clock::now();
                        if (mylats.size() < config::LATENCY_SAMPLE_LIMIT) {
                            mylats.push_back(
                                std::chrono::duration<double, std::micro>(t1 - t0).count());
                        }
                        bid += cfg_.producers;
                    } else {
                        std::this_thread::sleep_for(std::chrono::microseconds(10));
                    }

                    if ((bid & 0x3F) == 0) std::this_thread::yield();
                }
            });
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        go.store(false);
        for (auto& t : threads) t.join();

        if (lats) {
            for (auto& tl : tlats) {
                lats->insert(lats->end(), tl.begin(), tl.end());
            }
        }
    }

    Config cfg_;
};

void sep(char c = '=', int w = 65) { std::cout << std::string(w, c) << '\n'; }

MultiRunResult run_multiple(const Config& cfg) {
    // Warm-up run (discarded) to stabilize cache/allocator
    Runner(cfg).run();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<double> throughputs, latencies, mb_secs, efficiencies;
    uint64_t atomics = 0, epochs = 0, gaps = 0, dropped = 0, pbr_full = 0;

    const double theoretical_max = (cfg.batch_size > 0) ? (21e9 / static_cast<double>(cfg.batch_size)) : 0;

    for (int i = 0; i < cfg.num_runs; ++i) {
        Result r = Runner(cfg).run();
        throughputs.push_back(r.batches_sec);
        latencies.push_back(r.p99_us);
        mb_secs.push_back(r.mb_sec);
        if (theoretical_max > 0 && r.batches_sec > 0)
            efficiencies.push_back(100.0 * r.batches_sec / theoretical_max);
        atomics += r.atomics;
        epochs += r.epochs;
        gaps += r.gaps;
        dropped += r.dropped;
        pbr_full += r.pbr_full;
    }

    MultiRunResult out;
    out.throughput = StatResult::compute(throughputs);
    out.latency_p99 = StatResult::compute(latencies);
    if (!mb_secs.empty()) {
        std::sort(mb_secs.begin(), mb_secs.end());
        size_t n = mb_secs.size();
        out.median_mb_sec = (n % 2) ? mb_secs[n / 2] : (mb_secs[n / 2 - 1] + mb_secs[n / 2]) / 2.0;
    }
    if (!efficiencies.empty()) {
        std::sort(efficiencies.begin(), efficiencies.end());
        size_t n = efficiencies.size();
        out.median_efficiency_pct = (n % 2) ? efficiencies[n / 2] : (efficiencies[n / 2 - 1] + efficiencies[n / 2]) / 2.0;
    }
    const uint64_t nr = static_cast<uint64_t>(cfg.num_runs);
    out.total_atomics = atomics / nr;
    out.total_epochs = epochs / nr;
    out.total_gaps = gaps / nr;
    out.total_dropped = dropped / nr;
    out.total_pbr_full = pbr_full / nr;
    return out;
}

// Paired ablation 1: run baseline then optimized per iteration for speedup distribution.
struct Ablation1Result {
    MultiRunResult baseline;
    MultiRunResult optimized;
    StatResult speedup;   // per-run speedup (optimized_i / baseline_i)
    double atomic_reduction = 0;
};

Ablation1Result run_ablation1_paired(const Config& cfg) {
    Config c_baseline = cfg;
    c_baseline.level5 = false;
    c_baseline.mode = embarcadero::SequencerMode::PER_BATCH_ATOMIC;
    Config c_optimized = cfg;
    c_optimized.level5 = false;
    c_optimized.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;

    // Warm-up (one baseline + one optimized, discarded)
    Runner(c_baseline).run();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Runner(c_optimized).run();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<double> base_throughputs, base_latencies, base_mb, base_eff, opt_eff;
    std::vector<double> opt_throughputs, opt_latencies, opt_mb;
    std::vector<double> speedups;
    uint64_t base_atomics = 0, base_epochs = 0, base_gaps = 0, base_dropped = 0, base_pbr = 0;
    uint64_t opt_atomics = 0, opt_epochs = 0, opt_gaps = 0, opt_dropped = 0, opt_pbr = 0;
    const double theoretical_max = (cfg.batch_size > 0) ? (21e9 / static_cast<double>(cfg.batch_size)) : 0;

    for (int i = 0; i < cfg.num_runs; ++i) {
        Result r_base = Runner(c_baseline).run();
        Result r_opt = Runner(c_optimized).run();
        base_throughputs.push_back(r_base.batches_sec);
        base_latencies.push_back(r_base.p99_us);
        base_mb.push_back(r_base.mb_sec);
        if (theoretical_max > 0 && r_base.batches_sec > 0)
            base_eff.push_back(100.0 * r_base.batches_sec / theoretical_max);
        base_atomics += r_base.atomics;
        base_epochs += r_base.epochs;
        base_gaps += r_base.gaps;
        base_dropped += r_base.dropped;
        base_pbr += r_base.pbr_full;
        opt_throughputs.push_back(r_opt.batches_sec);
        opt_latencies.push_back(r_opt.p99_us);
        opt_mb.push_back(r_opt.mb_sec);
        if (theoretical_max > 0 && r_opt.batches_sec > 0)
            opt_eff.push_back(100.0 * r_opt.batches_sec / theoretical_max);
        opt_atomics += r_opt.atomics;
        opt_epochs += r_opt.epochs;
        opt_gaps += r_opt.gaps;
        opt_dropped += r_opt.dropped;
        opt_pbr += r_opt.pbr_full;
        if (r_base.batches_sec > 0)
            speedups.push_back(r_opt.batches_sec / r_base.batches_sec);
    }

    const uint64_t nr = static_cast<uint64_t>(cfg.num_runs);
    Ablation1Result out;
    out.baseline.throughput = StatResult::compute(base_throughputs);
    out.baseline.latency_p99 = StatResult::compute(base_latencies);
    out.baseline.total_atomics = base_atomics / nr;
    out.baseline.total_epochs = base_epochs / nr;
    out.baseline.total_gaps = base_gaps / nr;
    out.baseline.total_dropped = base_dropped / nr;
    out.baseline.total_pbr_full = base_pbr / nr;
    if (!base_mb.empty()) {
        std::sort(base_mb.begin(), base_mb.end());
        size_t n = base_mb.size();
        out.baseline.median_mb_sec = (n % 2) ? base_mb[n / 2] : (base_mb[n / 2 - 1] + base_mb[n / 2]) / 2.0;
    }
    if (!base_eff.empty()) {
        std::sort(base_eff.begin(), base_eff.end());
        size_t n = base_eff.size();
        out.baseline.median_efficiency_pct = (n % 2) ? base_eff[n / 2] : (base_eff[n / 2 - 1] + base_eff[n / 2]) / 2.0;
    }
    out.optimized.throughput = StatResult::compute(opt_throughputs);
    out.optimized.latency_p99 = StatResult::compute(opt_latencies);
    out.optimized.total_atomics = opt_atomics / nr;
    out.optimized.total_epochs = opt_epochs / nr;
    out.optimized.total_gaps = opt_gaps / nr;
    out.optimized.total_dropped = opt_dropped / nr;
    out.optimized.total_pbr_full = opt_pbr / nr;
    if (!opt_mb.empty()) {
        std::sort(opt_mb.begin(), opt_mb.end());
        size_t n = opt_mb.size();
        out.optimized.median_mb_sec = (n % 2) ? opt_mb[n / 2] : (opt_mb[n / 2 - 1] + opt_mb[n / 2]) / 2.0;
    }
    if (!opt_eff.empty()) {
        std::sort(opt_eff.begin(), opt_eff.end());
        size_t n = opt_eff.size();
        out.optimized.median_efficiency_pct = (n % 2) ? opt_eff[n / 2] : (opt_eff[n / 2 - 1] + opt_eff[n / 2]) / 2.0;
    }
    if (!speedups.empty())
        out.speedup = StatResult::compute(speedups);
    if (out.optimized.total_atomics > 0)
        out.atomic_reduction = static_cast<double>(out.baseline.total_atomics) / out.optimized.total_atomics;
    return out;
}

void print_stat_result(const std::string& name, const MultiRunResult& r, int num_runs) {
    std::cout << std::fixed;
    sep('-');
    std::cout << "  " << name << " (" << num_runs << " runs)\n";
    sep('-');
    double cv_pct = (r.throughput.median > 0) ? (100.0 * r.throughput.stddev / r.throughput.median) : 0;
    std::cout << std::setprecision(2)
              << "  Throughput: " << (r.throughput.median / 1e6) << " M/s "
              << "[" << (r.throughput.min_val / 1e6) << ", " << (r.throughput.max_val / 1e6) << "] "
              << "±" << (r.throughput.stddev / 1e6) << " M/s (" << std::setprecision(1) << cv_pct << "% of median)\n"
              << std::setprecision(2)
              << "  P99 Latency: " << r.latency_p99.median << " μs "
              << "[" << r.latency_p99.min_val << ", " << r.latency_p99.max_val << "]\n"
              << std::setprecision(0)
              << "  Atomics: " << r.total_atomics << "  Epochs: " << r.total_epochs << "  Gaps: " << r.total_gaps;
    if (r.total_dropped > 0) std::cout << "  Dropped: " << r.total_dropped;
    if (r.total_pbr_full > 0) std::cout << "  PBR_full: " << r.total_pbr_full;
    std::cout << "\n";
    if (r.median_efficiency_pct > 0) {
        std::cout << std::setprecision(1)
                  << "  Efficiency: " << r.median_efficiency_pct << "% of theoretical max (21 GB/s)\n";
    }
}

void print(const std::string& name, const Result& r) {
    std::cout << std::fixed;
    sep('-');
    std::cout << "  " << name << '\n';
    sep('-');
    std::cout << std::setprecision(0)
              << "  Throughput: " << std::setw(12) << r.batches_sec << " batches/s, "
              << std::setw(12) << r.msgs_sec << " msgs/s, "
              << std::setprecision(2) << std::setw(8) << r.mb_sec << " MB/s\n"
              << std::setprecision(1)
              << "  Latency:    P50=" << r.p50_us << " P99=" << r.p99_us
              << " P99.9=" << r.p999_us << " μs\n"
              << std::setprecision(1)
              << "  Epoch CPU:  avg=" << (r.avg_seq_ns / 1000.0) << " min=" << (r.min_seq_ns / 1000.0)
              << " max=" << (r.max_seq_ns / 1000.0) << " μs (time to process one epoch)\n"
              << "  Stats:      " << r.batches << " batches, " << r.epochs << " epochs, "
              << r.level5 << " L5, " << r.gaps << " gaps";
    if (r.atomics > 0) std::cout << ", " << r.atomics << " atomics";
    if (r.dropped > 0) std::cout << ", " << r.dropped << " dropped";
    std::cout << "\n";
    if (r.efficiency_pct > 0) {
        std::cout << std::setprecision(1)
                  << "  Efficiency: " << r.efficiency_pct << "% of theoretical max (21 GB/s)\n";
    }
}

void scalability_test(const Config& base) {
    std::cout << "\n"; sep();
    std::cout << "  SCALABILITY: Threads\n"; sep();
    std::cout << std::setw(8) << "Threads" << std::setw(14) << "Batches/s"
              << std::setw(10) << "MB/s" << std::setw(10) << "P99 μs" << '\n';
    sep('-');

    for (uint32_t t : {1, 2, 4, 8, 16}) {
        auto c = base; c.producers = t; c.duration_sec = 5;
        if (t >= 16) c.pbr_entries = 256 * 1024;  // Avoid PBR saturation with 16 producers
        auto r = Runner(c).run();
        std::cout << std::fixed << std::setw(8) << t
                  << std::setw(14) << std::setprecision(0) << r.batches_sec
                  << std::setw(10) << std::setprecision(2) << r.mb_sec
                  << std::setw(10) << std::setprecision(1) << r.p99_us << '\n';
    }
}

void epoch_test(const Config& base) {
    std::cout << "\n"; sep();
    std::cout << "  EPOCH IMPACT (Seq = CPU time to process one epoch)\n"; sep();
    std::cout << std::setw(10) << "Epoch(μs)" << std::setw(14) << "Batches/s"
              << std::setw(16) << "Epoch CPU (μs)" << '\n';
    sep('-');

    for (uint64_t e : {100, 250, 500, 1000, 2000}) {
        auto c = base; c.epoch_us = e; c.duration_sec = 5;
        auto r = Runner(c).run();
        double epoch_cpu_us = r.avg_seq_ns / 1000.0;
        std::cout << std::fixed << std::setw(10) << e
                  << std::setw(14) << std::setprecision(0) << r.batches_sec
                  << std::setw(16) << std::setprecision(1) << epoch_cpu_us << '\n';
    }
}

void broker_test(const Config& base) {
    std::cout << "\n"; sep();
    std::cout << "  BROKER SCALABILITY\n"; sep();
    std::cout << std::setw(10) << "Brokers" << std::setw(14) << "Batches/s"
              << std::setw(10) << "MB/s" << '\n';
    sep('-');

    for (uint32_t b : {1, 2, 4, 8, 16}) {
        auto c = base; c.brokers = b; c.collectors = std::min(b, 8u); c.duration_sec = 5;
        auto r = Runner(c).run();
        std::cout << std::fixed << std::setw(10) << b
                  << std::setw(14) << std::setprecision(0) << r.batches_sec
                  << std::setw(10) << std::setprecision(2) << r.mb_sec << '\n';
    }
}

}  // namespace bench

// ============================================================================
// Main
// ============================================================================

int main(int argc, char* argv[]) {
    std::cout << "\n";
    bench::sep();
    std::cout << "  EMBARCADERO SEQUENCER - ABLATION STUDY\n";
    bench::sep();

    bench::Config cfg;
    if (argc > 1) cfg.brokers = static_cast<uint32_t>(std::max(1, std::atoi(argv[1])));
    if (argc > 2) cfg.producers = static_cast<uint32_t>(std::max(1, std::atoi(argv[2])));
    if (argc > 3) cfg.duration_sec = static_cast<uint64_t>(std::max(1, std::atoi(argv[3])));
    if (argc > 4) cfg.level5_ratio = std::clamp(std::atof(argv[4]), 0.0, 1.0);
    if (argc > 5) cfg.num_runs = std::max(1, std::atoi(argv[5]));
    if (argc > 6) cfg.use_radix_sort = (std::atoi(argv[6]) != 0);

    std::cout << "\nConfig: " << cfg.brokers << " brokers, " << cfg.producers
              << " producers, " << cfg.duration_sec << "s, "
              << int(cfg.level5_ratio * 100) << "% L5, "
              << cfg.num_runs << " runs, "
              << (cfg.use_radix_sort ? "radix" : "std::sort")
              << ", validate=" << (cfg.validate ? "yes" : "no") << "\n";

    try {
        const bool multi_run = (cfg.num_runs >= 2);

        if (multi_run) {
            // ================================================================
            // ABLATION 1: Per-Batch vs Per-Epoch (paired runs, speedup distribution)
            // ================================================================
            std::cout << "\n"; bench::sep();
            std::cout << "  ABLATION 1: Per-Batch vs Per-Epoch (paired, " << cfg.num_runs << " runs)\n"; bench::sep();
            std::cout << "  (Per-epoch: atomics = epochs with non-empty output; when sequencer is bottleneck,\n"
                      << "   epochs << duration/τ, so atomics << 10000 for 5s @ 500μs.)\n";
            bench::Ablation1Result a1 = bench::run_ablation1_paired(cfg);
            bench::print_stat_result("Per-Batch Atomic (Baseline)", a1.baseline, cfg.num_runs);
            bench::print_stat_result("Per-Epoch Atomic (Optimized)", a1.optimized, cfg.num_runs);
            std::cout << std::fixed << std::setprecision(2)
                      << "\n  >>> SPEEDUP: " << a1.speedup.median << "× "
                      << "[" << a1.speedup.min_val << ", " << a1.speedup.max_val << "] "
                      << "(±" << a1.speedup.stddev << ")\n"
                      << "  >>> ATOMIC REDUCTION: " << std::setprecision(0) << a1.atomic_reduction << "×\n";

            // ================================================================
            // TEST 2-4: Ordering levels (multi-run)
            // ================================================================
            struct LevelTest { const char* name; double ratio; };
            std::vector<LevelTest> level_tests = {
                {"Level 0 (Total Order)", 0.0},
                {"Mixed 10% L5", 0.1},
            };
            if (cfg.stress_test) level_tests.push_back({"Stress Test (100% L5)", 1.0});

            for (const auto& t : level_tests) {
                std::cout << "\n"; bench::sep();
                std::cout << "  " << t.name << "\n"; bench::sep();
                auto c = cfg;
                c.level5 = (t.ratio > 0);
                c.level5_ratio = t.ratio;
                c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;
                bench::MultiRunResult r = bench::run_multiple(c);
                bench::print_stat_result(t.name, r, cfg.num_runs);
            }

            // Scalability: median of num_runs when num_runs >= 3
            if (cfg.num_runs >= 3) {
                std::cout << "\n"; bench::sep();
                std::cout << "  SCALABILITY: Threads (median of " << cfg.num_runs << " runs)\n"; bench::sep();
                std::cout << std::setw(8) << "Threads" << std::setw(14) << "Batches/s"
                          << std::setw(12) << "[min, max]" << std::setw(10) << "MB/s"
                          << std::setw(10) << "P99 μs" << '\n';
                bench::sep('-');
                for (uint32_t t : {1u, 2u, 4u, 8u, 16u}) {
                    auto c = cfg; c.producers = t; c.duration_sec = 5;
                    if (t >= 16) c.pbr_entries = 256 * 1024;  // Avoid PBR saturation with 16 producers
                    bench::MultiRunResult r = bench::run_multiple(c);
                    std::cout << std::fixed << std::setw(8) << t
                              << std::setw(14) << std::setprecision(0) << r.throughput.median
                              << " [" << std::setprecision(0) << r.throughput.min_val
                              << ", " << r.throughput.max_val << "]"
                              << std::setw(10) << std::setprecision(2) << r.median_mb_sec << " MB/s "
                              << std::setprecision(1) << r.latency_p99.median << " μs\n";
                }
            } else {
                bench::scalability_test(cfg);
            }
            bench::epoch_test(cfg);
            bench::broker_test(cfg);
        } else {
            // Single run: original format
            bench::Result baseline, optimized;
            {
                std::cout << "\n"; bench::sep();
                std::cout << "  ABLATION 1a: Per-Batch Atomic (Baseline)\n"; bench::sep();
                auto c = cfg; c.level5 = false; c.mode = embarcadero::SequencerMode::PER_BATCH_ATOMIC;
                baseline = bench::Runner(c).run();
                bench::print("Per-Batch Atomic", baseline);
            }
            {
                std::cout << "\n"; bench::sep();
                std::cout << "  ABLATION 1b: Per-Epoch Atomic (Optimized)\n"; bench::sep();
                auto c = cfg; c.level5 = false; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;
                optimized = bench::Runner(c).run();
                bench::print("Per-Epoch Atomic", optimized);
            }
            if (baseline.batches_sec > 0 && optimized.batches_sec > 0) {
                double speedup = optimized.batches_sec / baseline.batches_sec;
                double atomic_red = (baseline.atomics > 0 && optimized.atomics > 0)
                    ? static_cast<double>(baseline.atomics) / optimized.atomics : 0;
                std::cout << "\n  >>> SPEEDUP: " << std::fixed << std::setprecision(2) << speedup << "×  "
                          << "ATOMIC REDUCTION: " << std::setprecision(0) << atomic_red << "×\n";
            }

            std::cout << "\n"; bench::sep();
            std::cout << "  TEST 2: Level 0\n"; bench::sep();
            { auto c = cfg; c.level5 = false; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Level 0", bench::Runner(c).run()); }

            std::cout << "\n"; bench::sep();
            std::cout << "  TEST 3: Mixed 10% L5\n"; bench::sep();
            { auto c = cfg; c.level5_ratio = 0.1; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Mixed", bench::Runner(c).run()); }

            if (cfg.stress_test) {
                std::cout << "\n"; bench::sep();
                std::cout << "  Stress Test (100% L5)\n"; bench::sep();
                { auto c = cfg; c.level5_ratio = 1.0; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Stress Test (100% L5)", bench::Runner(c).run()); }
            }

            bench::scalability_test(cfg);
            bench::epoch_test(cfg);
            bench::broker_test(cfg);
        }

        std::cout << "\n"; bench::sep();
        std::cout << "  Complete\n"; bench::sep(); std::cout << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
