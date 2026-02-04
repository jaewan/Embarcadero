// Embarcadero Sequencer - Ablation Study for OSDI/SOSP/SIGCOMM
// Correctness-first: proper dedup, MPSC ring, Level 5 sliding window,
// hold-buffer emit, ordering validation, per-batch vs per-epoch comparison.
//
// Build: see CMakeLists.txt
// Usage:
//   ./sequencer5_benchmark [brokers] [producers] [duration_sec] [level5_ratio] [num_runs] [use_radix_sort] [scatter_gather] [num_shards] [scatter_gather_only] [pbr_entries]
//   --clean   paper-ready ablation: producers=2, pbr_entries=4M (sequencer bottleneck, not PBR)
//   --help or -h  print this usage
// Optional args: ... pbr_entries(0=auto, >0 entries per broker e.g. 4194304 for 4M)

#include <atomic>
#include <vector>
#include <array>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>
#include <unordered_set>
#include <iterator>
#include <map>
#include <deque>
#include <queue>
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
#include <string>
#include <fstream>
#include <sstream>

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

// #region agent log
// Debug logging - disabled by default for production benchmarks.
// Enable with -DDEBUG_SEQUENCER_LOG to enable debug logging.
// If enabled, set SEQUENCER_DEBUG_LOG env var to override the log path.
#ifdef DEBUG_SEQUENCER_LOG
static void debug_log(const char* hyp, const char* loc, const char* msg, const char* data) {
    static const char* log_path = []() {
        const char* env = std::getenv("SEQUENCER_DEBUG_LOG");
        return env ? env : "/tmp/sequencer_debug.log";
    }();
    std::ofstream f(log_path, std::ios::app);
    if (!f) return;
    f << "{\"sessionId\":\"debug\",\"hypothesisId\":\"" << hyp << "\",\"location\":\"" << loc << "\",\"message\":\"" << msg << "\"";
    if (data && data[0]) f << ",\"data\":" << data;
    f << "}\n";
}
#else
static inline void debug_log(const char*, const char*, const char*, const char*) {}
#endif
// #endregion

// ============================================================================
// Configuration
// ============================================================================

namespace config {
    inline constexpr size_t CACHE_LINE = 64;
    /// Supported range: brokers 1..MAX_BROKERS (document for ablation; do not exceed without code change).
    inline constexpr uint32_t MAX_BROKERS = 32;
    inline constexpr uint32_t MAX_COLLECTORS = 8;
    /// Panel: <0.1% PBR saturation for clean measurement. 8 producers @ ~2M/s need headroom.
    inline constexpr size_t DEFAULT_PBR_ENTRIES = 256 * 1024;
    // For algorithm comparison (Ablation 1): PBR large enough that sequencer is bottleneck, not ring.
    inline constexpr size_t CLEAN_ABLATION_PBR_ENTRIES = 4 * 1024 * 1024;  // 4M per broker
    inline constexpr size_t DEFAULT_GOI_ENTRIES = 1024 * 1024;
    inline constexpr uint64_t DEFAULT_EPOCH_US = 500;
    inline constexpr uint32_t PIPELINE_DEPTH = 3;
    inline constexpr size_t HOLD_BUFFER_MAX = 100000;  // 10x for high-reorder L5
    inline constexpr size_t HOLD_BUFFER_SOFT_LIMIT = (HOLD_BUFFER_MAX * 8) / 10;  // Backpressure threshold
    inline constexpr size_t DEFERRED_L5_MAX = HOLD_BUFFER_MAX * 2;  // Cap deferred batches (backpressure only)
    /// Hold buffer timeout (epochs): skip gap after this many epochs. ~1.5ms at 500μs/epoch; balances latency vs gap tolerance.
    inline constexpr int HOLD_MAX_WAIT_EPOCHS = 3;
    inline constexpr size_t CLIENT_SHARDS = 16;
    inline constexpr size_t MAX_CLIENTS_PER_SHARD = 8192;
    /// Client TTL (epochs): evict client state after this many epochs idle (~10s at 500μs/epoch). Must exceed any in-flight batch delay.
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
    /// Lock-free validation ring size (1M entries). When full, oldest are overwritten; drain() called after stop().
    /// At high throughput (e.g. 10M batches/s) the ring fills in ~100ms; only the last VALIDATION_RING_SIZE
    /// entries are validated. For full-trace correctness runs use shorter duration or increase this constant.
    inline constexpr size_t VALIDATION_RING_SIZE = 1 << 20;
    /// Sample 1 in (VALIDATION_SAMPLE_MASK+1) batches for ordering validation to reduce mutex contention.
    inline constexpr unsigned VALIDATION_SAMPLE_MASK = 255;
    inline constexpr int RING_MAX_RETRIES = 64;
    inline constexpr size_t RADIX_BITS = 8;
    inline constexpr size_t RADIX_BUCKETS = 1 << RADIX_BITS;
    /// Document §4.2.1: reject batches with stale epoch_created (zombie broker / recovery)
    inline constexpr uint16_t MAX_EPOCH_AGE = 4;
    /// client_gc runs every CLIENT_GC_EPOCH_INTERVAL epochs (~512ms at 500μs/epoch) to amortize overhead.
    inline constexpr uint64_t CLIENT_GC_EPOCH_INTERVAL = 1024;
    /// Scatter-gather: input buffer reserve per shard epoch (panel: named constant)
    inline constexpr size_t SHARD_INPUT_RESERVE = 16384;
    /// Scatter-gather: extra capacity when reserving shard.ready (panel: named constant)
    inline constexpr size_t SHARD_READY_EXTRA = 1024;
    /// Phase 2: pre-allocate hot-path vectors at startup to avoid malloc in hot loop
    inline constexpr size_t MAX_BATCHES_PER_EPOCH = 256 * 1024;
    /// Phase 3.2: Set true to simulate ~200ns CXL write latency (tune for CPU)
    inline constexpr bool SIMULATE_CXL_WRITE_DELAY = false;
    /// Timer loop: spin count before falling back to poll/sleep (low-latency epoch advance).
    inline constexpr int TIMER_SPIN_COUNT = 200;
    /// committed_seq_updater / wait loops: sleep interval when no work (μs).
    inline constexpr int WAIT_SLEEP_US = 10;
    /// Collector: max μs to wait on a !VALID PBR slot before declaring hole (e.g. dead producer).
    inline constexpr int MAX_HOLE_WAIT_US = 100;
    /// Validation: max wall-clock seconds before returning "validation timeout".
    inline constexpr int VALIDATION_TIMEOUT_SEC = 60;
    /// Validation: cap ring size for non-stress runs (avoids unbounded allocation; 4M entries).
    /// Cap validation ring so drain+sort stays fast; long runs validate the most recent VALIDATION_RING_CAP batches.
    inline constexpr size_t VALIDATION_RING_CAP = 1 << 20;
    /// Proactive backpressure: PBR high/low watermarks (§Step 1)
    inline constexpr double PBR_HIGH_WATERMARK_PCT = 0.90;
    inline constexpr double PBR_LOW_WATERMARK_PCT = 0.50;
    inline constexpr uint64_t BACKPRESSURE_MIN_SLEEP_US = 100;
    inline constexpr uint64_t BACKPRESSURE_MAX_SLEEP_US = 10000;
}

inline void cxl_write_delay() {
    if (config::SIMULATE_CXL_WRITE_DELAY) {
        for (volatile int i = 0; i < 50; ++i) (void)i;
    }
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
// Inject Result (Step 1: Proactive Backpressure)
// ============================================================================
enum class InjectResult {
    SUCCESS,        // Batch successfully injected
    BACKPRESSURE,   // Above high watermark - caller must retry with backoff
    RING_FULL       // CAS failed after retries - transient contention
};

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

// ============================================================================
// Lock-Free Validation Ring (MPSC: writer + shard workers write; drain after stop())
// ============================================================================
struct ValidationEntry {
    uint64_t global_seq = 0;
    uint64_t batch_id = 0;
    uint64_t client_id = 0;
    uint64_t client_seq = 0;
    bool is_level5 = false;
};

/// Ring slot with atomic publication for MPSC (scatter-gather writers). Not copyable.
struct ValidationSlot {
    uint64_t global_seq = 0;
    uint64_t batch_id = 0;
    uint64_t client_id = 0;
    uint64_t client_seq = 0;
    bool is_level5 = false;
    std::atomic<bool> ready{false};
};

class ValidationRing {
public:
    /** @param capacity Power of 2; 0 = use config::VALIDATION_RING_SIZE. Larger = full-trace validation for longer runs. */
    explicit ValidationRing(size_t capacity = 0) {
        size_ = (capacity != 0 && (capacity & (capacity - 1)) == 0)
                    ? capacity
                    : config::VALIDATION_RING_SIZE;
        slots_ = std::make_unique<ValidationSlot[]>(size_);
        for (size_t i = 0; i < size_; ++i)
            slots_[i].ready.store(false, std::memory_order_relaxed);
    }
    size_t mask() const { return size_ - 1; }
    size_t capacity() const { return size_; }

    /** Lock-free: multiple producers. Call only before drain() (i.e. while writers run).
     * Step 2: Removed spin loop - just check for overwrite and proceed.
     * Ensures sequencer never stalls on validation. */
    void record(uint64_t global_seq, uint64_t batch_id, uint64_t client_id, uint64_t client_seq, bool is_level5) noexcept {
        uint64_t idx = write_pos_.fetch_add(1, std::memory_order_relaxed);
        ValidationSlot& e = slots_[idx & mask()];
        
        // Check if slot is ready (being consumed) - if so, we're overwriting
        if (e.ready.load(std::memory_order_acquire)) {
            overwrites_.fetch_add(1, std::memory_order_relaxed);
        }
        
        // Write validation data
        e.global_seq = global_seq;
        e.batch_id = batch_id;
        e.client_id = client_id;
        e.client_seq = client_seq;
        e.is_level5 = is_level5;
        e.ready.store(true, std::memory_order_release);
    }

    /** Call after stop() (no more record()). Returns up to capacity() entries in write order. */
    std::vector<ValidationEntry> drain() {
        uint64_t total = write_pos_.load(std::memory_order_acquire);
        std::vector<ValidationEntry> result;
        if (total == 0) return result;
        size_t n = static_cast<size_t>(std::min(total, static_cast<uint64_t>(size_)));
        result.reserve(n);
        size_t start_idx = (total <= size_) ? 0 : static_cast<size_t>(total - n);
        for (size_t i = 0; i < n; ++i) {
            size_t idx = (start_idx + i) & mask();
            ValidationSlot& e = slots_[idx];
            if (!e.ready.load(std::memory_order_acquire))
                continue;
            ValidationEntry copy;
            copy.global_seq = e.global_seq;
            copy.batch_id = e.batch_id;
            copy.client_id = e.client_id;
            copy.client_seq = e.client_seq;
            copy.is_level5 = e.is_level5;
            result.push_back(copy);
            e.ready.store(false, std::memory_order_release);
        }
        write_pos_.store(0, std::memory_order_release);
        return result;
    }

    uint64_t overwrites() const { return overwrites_.load(std::memory_order_relaxed); }
    void reset_overwrites() { overwrites_.store(0, std::memory_order_relaxed); }

private:
    size_t size_;
    std::unique_ptr<ValidationSlot[]> slots_;
    alignas(64) std::atomic<uint64_t> write_pos_{0};
    alignas(64) std::atomic<uint64_t> overwrites_{0};
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

/// Key for per-shard expiry heap: (expiry_epoch, client_id, client_seq) — plan §2.3
struct HoldExpiryKey {
    uint64_t expiry_epoch = 0;
    uint64_t client_id = 0;
    uint64_t client_seq = 0;
    bool operator>(const HoldExpiryKey& o) const {
        if (expiry_epoch != o.expiry_epoch) return expiry_epoch > o.expiry_epoch;
        if (client_id != o.client_id) return client_id > o.client_id;
        return client_seq > o.client_seq;
    }
};

// ============================================================================
// Radix sort by client_id: 4 passes x 16-bit = even swaps, result in original array.
// ============================================================================

class RadixSorter {
public:
    RadixSorter() {
        temp_.reserve(config::MAX_BATCHES_PER_EPOCH);  // Avoid realloc when input size fluctuates (assessment follow-up)
    }
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

    // Reserve a slot only if ring not full. CAS loop avoids consuming a slot on failure
    // (fetch_add-then-check would waste a slot index when full and shrink effective capacity).
    // Assessment §1.1: 1024 retries + exponential backoff to fix segfault at 8 producers.
    [[nodiscard]] std::optional<uint64_t> reserve() {
        constexpr int max_retries = 1024;
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        for (int retry = 0; retry < max_retries; ++retry) {
            uint64_t head = head_.load(std::memory_order_acquire);
            if (tail - head >= capacity_)
                return std::nullopt;  // Ring full, no state change
            if (tail_.compare_exchange_weak(tail, tail + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed))
                return tail;
            if (retry > 16) {
                for (int i = 0; i < (1 << std::min(retry - 16, 8)); ++i) {
                    CPU_PAUSE();
                }
            } else {
                CPU_PAUSE();
            }
        }
        return std::nullopt;  // Contention after max retries; caller handles as ring full
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
// Shard Queue (MPSC: Multiple Collectors -> Single Shard Worker) — plan §2.2
// ============================================================================
// Lock-free MPSC queue; ready_ uses std::atomic<bool> per slot (not vector<bool>).

template<typename T>
class alignas(128) ShardQueue {
public:
    explicit ShardQueue(size_t capacity)
        : capacity_(capacity), mask_(capacity - 1), buffer_(capacity), ready_(capacity) {
        assert(is_power_of_2(capacity));
        for (auto& r : ready_) r.store(false, std::memory_order_relaxed);
    }

    /** Called by collectors (multiple producers). */
    [[nodiscard]] bool try_push(T&& item) {
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        for (int retry = 0; retry < 64; ++retry) {
            uint64_t head = head_.load(std::memory_order_acquire);
            if (tail - head >= capacity_) return false;
            if (tail_.compare_exchange_weak(tail, tail + 1,
                    std::memory_order_acq_rel, std::memory_order_relaxed)) {
                buffer_[tail & mask_] = std::move(item);
                ready_[tail & mask_].store(true, std::memory_order_release);
                return true;
            }
            CPU_PAUSE();
        }
        return false;
    }

    /** Called by shard worker (single consumer). Returns number of items drained. */
    size_t drain_to(std::vector<T>& out, size_t max_items = SIZE_MAX) {
        uint64_t head = head_.load(std::memory_order_relaxed);
        uint64_t tail = tail_.load(std::memory_order_acquire);
        size_t count = 0;
        while (head < tail && count < max_items) {
            if (!ready_[head & mask_].load(std::memory_order_acquire))
                break;
            out.push_back(std::move(buffer_[head & mask_]));
            ready_[head & mask_].store(false, std::memory_order_relaxed);
            ++head;
            ++count;
        }
        if (count > 0)
            head_.store(head, std::memory_order_release);
        return count;
    }

    size_t size() const noexcept {
        uint64_t head = head_.load(std::memory_order_acquire);
        uint64_t tail = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(tail - head);
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;
    std::vector<std::atomic<bool>> ready_;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

// ============================================================================
// Deduplication with FIFO Eviction (correctness: no false positives)
// ============================================================================

// Single-threaded: only sequencer thread calls check_and_insert; no locks.
class Deduplicator {
public:
    void set_skip(bool skip) { skip_ = skip; }

    Deduplicator() {
        for (auto& s : shards_) {
            s.seen.reserve(config::DEDUP_MAX_PER_SHARD * 2);
            s.seen.max_load_factor(0.7f);
            s.insertion_order.clear();
        }
    }

    bool check_and_insert(uint64_t batch_id) {
        // If dedup is disabled, always return false (not a duplicate)
        if (skip_) return false;

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
    bool skip_ = false;
};

// ============================================================================
// Per-shard state and coordinator (plan §2.3–2.4) — after ShardQueue, Deduplicator
// ============================================================================

/// Per-shard state for scatter-gather. Each shard owns clients with hash(client_id) % num_shards == shard_id.
/// client_highest_committed: persistent max client_seq committed per client (retries / duplicate client_seq).
struct alignas(128) ShardState {
    uint32_t shard_id = 0;
    std::unique_ptr<ShardQueue<BatchInfo>> input_queue;
    std::unordered_map<uint64_t, ClientState> client_states;
    std::unordered_map<uint64_t, uint64_t> client_highest_committed;
    std::unordered_map<uint64_t, std::map<uint64_t, HoldEntry>> hold_buf;
    size_t hold_size = 0;
    std::priority_queue<HoldExpiryKey, std::vector<HoldExpiryKey>, std::greater<HoldExpiryKey>> expiry_heap;
    Deduplicator dedup;
    std::vector<BatchInfo> ready;
    std::vector<BatchInfo> deferred_l5;
    std::vector<BatchInfo> l0_batches;   // Reused across epochs to avoid repeated allocation
    std::vector<BatchInfo> l5_batches;   // Reused across epochs to avoid repeated allocation
    std::atomic<uint64_t> watermark{0};
    std::atomic<uint64_t> batches_processed{0};
    std::atomic<uint64_t> gaps_skipped{0};
    std::atomic<uint64_t> duplicates{0};
    std::atomic<uint64_t> phase_partition_ns{0};
    std::atomic<uint64_t> phase_l0_ns{0};
    std::atomic<uint64_t> phase_l5_ns{0};
    std::atomic<uint64_t> phase_hold_ns{0};
    std::atomic<uint64_t> phase_write_ns{0};
    std::atomic<uint64_t> epochs_processed{0};
    std::thread worker_thread;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> current_epoch{0};
    std::atomic<bool> epoch_complete{false};
    std::condition_variable epoch_cv;
    std::mutex epoch_mutex;
};

/// Global coordination for scatter-gather. committed_seq set by single writer to max_seq (not min watermarks).
/// Supported num_shards: 1..32 (shard_watermarks array size).
/// writer_finished_epoch: timer must wait for writer to finish consuming ready[] before advancing epoch (fixes writer/shard race).
/// One atomic per cache line to avoid false sharing (Phase 2: Performance Fidelity).
struct PaddedAtomicU64 {
    alignas(64) std::atomic<uint64_t> v{0};
};

struct ScatterGatherCoordinator {
    alignas(64) std::atomic<uint64_t> global_seq{0};
    std::array<PaddedAtomicU64, 32> shard_watermarks{};
    std::atomic<uint64_t> current_epoch{0};
    std::atomic<uint64_t> writer_finished_epoch{0};
    std::atomic<uint32_t> shards_ready{0};
    std::mutex barrier_mutex;
    std::condition_variable barrier_cv;
    uint32_t num_shards = 8;
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
            uint32_t prev = active_collectors_.fetch_sub(1, std::memory_order_acq_rel);
            if (prev == 1) {
                std::lock_guard<std::mutex> lk(seal_mutex_);
                seal_cv_.notify_all();
            }
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

    /** Timer: seal buffer (COLLECTING → SEALED). Waits for active collectors to exit *before*
     * setting SEALED so the sequencer never reads while a collector is still writing (prevents
     * use-after-free when collector push_back reallocs and sequencer process_epoch reads).
     * Early state check is optimization only; the CAS below is the real gate (TOCTOU safe). */
    [[nodiscard]] bool seal() {
        State expected = State::COLLECTING;
        if (!state_.compare_exchange_strong(expected, State::SEALED, std::memory_order_acq_rel))
            return false;
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

    uint32_t active_collectors() const noexcept {
        return active_collectors_.load(std::memory_order_acquire);
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
    PER_EPOCH_ATOMIC,
    SCATTER_GATHER  // Parallel: one atomic per shard per epoch
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
    std::atomic<uint64_t> pbr_holes_timed_out{0};  // Collector: !VALID slot skipped after timeout
    /// Assessment §4.2: Backpressure - sequencer_loop sets current backlog (epochs); producer throttles when > 50.
    std::atomic<uint64_t> backlog_epochs{0};

    alignas(64) std::atomic<uint64_t> pbr_full_count{0};  // Producer threads: CAS failures (transient)
    std::atomic<uint64_t> backpressure_events{0};  // Step 1: Above high watermark (controlled backpressure)
    std::atomic<uint64_t> total_injected{0};  // Successful inject_batch (completeness §5.2)

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
    std::atomic<uint64_t> writer_phase_ns{0};  // Scatter-gather: gather+sort+write GOI per epoch
    // Assessment §2.1: Phase timing for bottleneck profiling (per-epoch cumulative; print every 100 epochs).
    std::atomic<uint64_t> phase_partition_ns{0};
    std::atomic<uint64_t> phase_l0_ns{0};
    std::atomic<uint64_t> phase_l5_ns{0};
    std::atomic<uint64_t> phase_l5_reorder_ns{0};  // stable_partition + L5 tail sort in per-epoch path
    std::atomic<uint64_t> phase_l0_dedup_ns{0};    // Deduplicator cost inside process_level0

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
        writer_phase_ns = 0;
        phase_partition_ns = 0;
        phase_l0_ns = 0;
        phase_l5_ns = 0;
        phase_l5_reorder_ns = 0;
        phase_l0_dedup_ns = 0;
        latency_ring.reset();
    }

    double avg_seq_ns() const {
        uint64_t c = seq_count.load();
        return c ? double(seq_total_ns.load()) / c : 0;
    }

    /** Phase 3.1: Drain inject-to-commit latencies (recorded by sequencer/writer). Returns ns. */
    std::vector<uint64_t> drain_latency_ring() { return latency_ring.drain(); }
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

    // Scatter-gather configuration (plan §2.1)
    bool scatter_gather_enabled = false;
    uint32_t num_shards = 8;
    size_t shard_queue_size = 65536;

    /// When true, record every committed batch for validate_ordering() (per-client check requires full trace).
    /// When false, sample 1 in (VALIDATION_SAMPLE_MASK+1) to reduce mutex contention; per-client check may be inaccurate.
    bool validate_ordering = true;

    /// When true, allow lossy behavior under overload (stress tests only).
    bool allow_drops = false;

    /// Validation ring size. 0 = use config::VALIDATION_RING_SIZE. For full-trace validation set to expected_batches (e.g. throughput * duration).
    size_t validation_ring_size = 0;

    /// Phase 2: when >0, use dense vectors for client state (client_id in [0, max_clients)); avoids hash lookups.
    uint32_t max_clients = 0;

    /// Ablation: if true, disable deduplication (benchmark has no retries, dedup just adds overhead)
    bool skip_dedup = false;

    void validate() {
        num_brokers = std::clamp(num_brokers, 1u, config::MAX_BROKERS);
        num_collectors = std::clamp(num_collectors, 1u, config::MAX_COLLECTORS);
        num_collectors = std::min(num_collectors, num_brokers);
        if (!is_power_of_2(pbr_entries)) pbr_entries = next_pow2(pbr_entries);
        if (!is_power_of_2(goi_entries)) goi_entries = next_pow2(goi_entries);
        num_shards = std::clamp(num_shards, 1u, 32u);
        if (!is_power_of_2(num_shards)) num_shards = static_cast<uint32_t>(next_pow2(num_shards));
        if (!is_power_of_2(shard_queue_size)) shard_queue_size = next_pow2(shard_queue_size);
    }
};

/** Compute validation ring capacity (power of 2) for Sequencer ctor. Avoids placement new + double-destruct on throw. */
inline size_t get_validation_ring_size(const SequencerConfig& cfg) {
    size_t r = cfg.validation_ring_size;
    if (r == 0) return config::VALIDATION_RING_SIZE;
    return is_power_of_2(r) ? r : next_pow2(r);
}

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
    uint64_t global_seq() const {
        if (config_.mode == SequencerMode::SCATTER_GATHER)
            return coordinator_.global_seq.load(std::memory_order_acquire);
        return global_seq_.load(std::memory_order_acquire);
    }
    uint64_t committed_seq() const;
    const SequencerConfig& config() const { return config_; }
    uint64_t validation_overwrites() const { return validation_ring_.overwrites(); }
    void reset_validation_overwrites() { validation_ring_.reset_overwrites(); }

    [[nodiscard]] InjectResult inject_batch(uint16_t broker_id, uint64_t batch_id,
                                     uint32_t payload_size, uint32_t msg_count,
                                     uint32_t extra_flags = 0,
                                     uint64_t client_id = 0, uint64_t client_seq = 0);

    /** Query: Is broker_id's PBR below low watermark? Used for adaptive backoff. */
    bool below_low_watermark(uint16_t broker_id) const;

    /** PBR usage snapshot: average (used/capacity)*100 across all brokers. Call after stop() for stable reading. */
    double get_pbr_usage_pct() const;

    bool validate_ordering();
    /** Returns empty if valid; otherwise first failure reason (gap / per-client). Call after stop(). */
    std::string validate_ordering_reason();

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

    void timer_loop_scatter();
    void collector_loop_scatter(int collector_id, std::vector<int> pbr_ids);
    void shard_worker_loop(uint32_t shard_id);
    void committed_seq_updater();  // Phase 2: updates committed_seq and PBR heads when shards write GOI directly

    void process_level5_shard(ShardState& shard, std::vector<BatchInfo>& batches, uint64_t epoch);
    void add_to_hold_shard(ShardState& shard, BatchInfo&& batch, uint64_t current_epoch);
    void drain_hold_shard(ShardState& shard, std::vector<BatchInfo>& out, uint64_t epoch);
    void age_hold_shard(ShardState& shard, std::vector<BatchInfo>& out, uint64_t current_epoch);
    void client_gc_shard(ShardState& shard, uint64_t cur_epoch);

    void advance_epoch();
    void process_epoch(EpochBuffer& buf);
    void sequence_batches_per_batch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);
    void sequence_batches_per_epoch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);
    void process_level0(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out);
    void process_level5(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);

    void add_to_hold(uint64_t client_id, BatchInfo&& batch, uint64_t current_epoch);
    void drain_hold(std::vector<BatchInfo>& out, uint64_t epoch);
    void age_hold(std::vector<BatchInfo>& out, uint64_t current_epoch);
    void client_gc(uint64_t cur_epoch);

    int client_shard(uint64_t id) const {
        return int(hash64(id) & (config::CLIENT_SHARDS - 1));
    }

    /// Phase 2: O(1) access when dense vectors are in use (resized in start() for non-scatter path).
    /// Guard with size() so we never index into empty dense vectors (e.g. scatter mode with max_clients set).
    bool use_dense() const {
        return states_dense_.size() == config_.max_clients && config_.max_clients != 0;
    }
    ClientState& get_state(uint64_t cid) {
        if (use_dense() && cid < config_.max_clients) {
            ClientState& st = states_dense_[cid];
            uint64_t hc = client_highest_committed_dense_[cid];
            if (st.next_expected == 1 && hc > 0) {
                st.next_expected = hc + 1;
                st.highest_sequenced = hc;
                st.window_base = hc + 1;
            }
            return st;
        }
        auto& states = client_shards_[client_shard(cid)].states;
        auto [it, inserted] = states.try_emplace(cid, ClientState{});
        if (inserted) {
            uint64_t hc = get_hc(cid);
            if (hc > 0) {
                it->second.next_expected = hc + 1;
                it->second.highest_sequenced = hc;
                it->second.window_base = hc + 1;
            }
        }
        return it->second;
    }
    uint64_t& get_hc(uint64_t cid) {
        if (use_dense() && cid < config_.max_clients)
            return client_highest_committed_dense_[cid];
        return client_highest_committed_[cid];
    }
    std::map<uint64_t, HoldEntry>& get_hold_buf(uint64_t cid) {
        if (use_dense() && cid < config_.max_clients)
            return hold_buf_dense_[cid];
        return hold_buf_[cid];
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
    /// scan_pos is atomic so writer can advance PBR heads from scanned (not only processed) slots.
    /// hole_wait_start_ns: when we stall on !VALID, first-seen time; used for bounded timeout.
    /// skipped_hole_watermark: highest PBR index skipped due to hole timeout; head must advance past it to reclaim slots.
    /// INVARIANT: Each PBR (broker) is read by exactly one collector (disjoint assignments in collector_loop);
    /// thus skipped_hole_watermark and committed_pos are single-writer. If broker assignment is ever shared, make them atomic.
    struct alignas(64) PBRReadState {
        std::atomic<uint64_t> scan_pos{0};
        uint64_t committed_pos = 0;
        uint64_t hole_wait_start_ns = 0;  // 0 = not waiting
        uint64_t skipped_hole_watermark = 0;  // Advance head past this to avoid permanent slot leak
    };
    std::array<PBRReadState, config::MAX_BROKERS> pbr_state_;

    // Epoch pipeline
    std::array<EpochBuffer, config::PIPELINE_DEPTH> epoch_bufs_;
    alignas(64) std::atomic<uint64_t> global_seq_{0};
    alignas(64) std::atomic<uint64_t> current_epoch_{0};

    // Threads
    std::atomic<bool> running_{false};
    std::atomic<bool> shutdown_{false};
    std::atomic<bool> freeze_collection_{false};  // Pipeline full: pause collectors to prevent super-epochs
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
    ValidationRing validation_ring_;
    std::atomic<uint64_t> validation_batch_count_{0};  // For sampling: record every (VALIDATION_SAMPLE_MASK+1)-th batch

    // Per-client state (Level 5) - single-threaded sequencer only; no locks
    struct alignas(64) ClientShard {
        std::unordered_map<uint64_t, ClientState> states;
    };
    std::array<ClientShard, config::CLIENT_SHARDS> client_shards_;
    /// Persistent max client_seq committed per client (survives client_gc). Never emit if seq <= this.
    /// INVARIANT: After any emit for client C, hc[C] = last_emitted_client_seq and state[C].next_expected = last+1.
    /// Duplicate (batch_id) retries advance next_expected but not hc; late arrivals with seq <= hc are rejected.
    std::unordered_map<uint64_t, uint64_t> client_highest_committed_;

    // Hold buffer - ONLY accessed by sequencer thread (no lock needed for access)
    std::unordered_map<uint64_t, std::map<uint64_t, HoldEntry>> hold_buf_;
    /// Phase 2: dense client state when max_clients > 0 (O(1) index vs hash lookup)
    std::vector<ClientState> states_dense_;
    std::vector<uint64_t> client_highest_committed_dense_;
    std::vector<std::map<uint64_t, HoldEntry>> hold_buf_dense_;
    /// Clients that have at least one batch in hold_buf_; drain_hold iterates only these (O(N_active) not O(N_total)).
    std::unordered_set<uint64_t> clients_with_held_batches_;
    size_t hold_size_ = 0;
    std::multimap<uint64_t, std::pair<uint64_t, uint64_t>> expiry_queue_;  // expiry_epoch -> (client_id, client_seq)
    /// Reused in age_hold() to avoid per-call allocation (assessment §2.1).
    struct ExpiredEntry { uint64_t cid; uint64_t seq; BatchInfo batch; };
    std::vector<ExpiredEntry> age_hold_expired_buffer_;
    std::vector<BatchInfo> deferred_l5_;
    int timer_fd_ = -1;

    // Scatter-gather (plan §2–4). unique_ptr: ShardState not moveable (mutex/cv).
    ScatterGatherCoordinator coordinator_;
    std::vector<std::unique_ptr<ShardState>> shards_;
    std::mutex writer_mutex_;
    std::condition_variable writer_cv_;
    /// Phase 2: max pbr_index+1 written per broker (shards update with CAS); committed_seq_updater advances PBR.
    std::array<std::atomic<uint64_t>, config::MAX_BROKERS> scatter_max_pbr_{};
    std::array<uint64_t, config::MAX_BROKERS> last_advanced_pbr_{};
};

// ============================================================================
// Implementation
// ============================================================================

Sequencer::Sequencer(SequencerConfig cfg) : config_(cfg), validation_ring_(get_validation_ring_size(cfg)) {
    config_.validate();
}

Sequencer::~Sequencer() {
    // #region agent log
    debug_log("H2", "~Sequencer", "entry", "{}");
    // #endregion
    stop();
#ifdef __linux__
    if (timer_fd_ >= 0) { ::close(timer_fd_); timer_fd_ = -1; }
#endif
    // #region agent log
    debug_log("H2", "~Sequencer", "before_free_memory", "{}");
    // #endregion
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
        st.scan_pos.store(0, std::memory_order_relaxed);
        st.committed_pos = 0;
        st.hole_wait_start_ns = 0;
        st.skipped_hole_watermark = 0;
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

    // Phase 2: pre-allocate dense client state when max_clients set (single-threaded path only)
    if (config_.max_clients != 0 && config_.mode != SequencerMode::SCATTER_GATHER) {
        states_dense_.resize(config_.max_clients);
        client_highest_committed_dense_.resize(config_.max_clients, 0);
        hold_buf_dense_.resize(config_.max_clients);
    }
    age_hold_expired_buffer_.reserve(65536);  // Avoid realloc in age_hold hot path

    if (config_.mode == SequencerMode::SCATTER_GATHER) {
        coordinator_.num_shards = config_.num_shards;
        coordinator_.current_epoch.store(0, std::memory_order_relaxed);
        coordinator_.writer_finished_epoch.store(0, std::memory_order_relaxed);
        coordinator_.global_seq.store(0, std::memory_order_relaxed);
        coordinator_.shards_ready.store(0, std::memory_order_relaxed);
        for (auto& w : coordinator_.shard_watermarks) w.v.store(0, std::memory_order_relaxed);
        for (auto& a : scatter_max_pbr_) a.store(0, std::memory_order_relaxed);
        for (uint32_t i = 0; i < config::MAX_BROKERS; ++i) last_advanced_pbr_[i] = 0;

        shards_.clear();
        shards_.reserve(config_.num_shards);
        for (uint32_t i = 0; i < config_.num_shards; ++i) {
            auto s = std::make_unique<ShardState>();
            s->shard_id = i;
            s->input_queue = std::make_unique<ShardQueue<BatchInfo>>(config_.shard_queue_size);
            s->running.store(true);
            s->current_epoch.store(0, std::memory_order_relaxed);
            s->epoch_complete.store(false, std::memory_order_relaxed);
            s->dedup.set_skip(config_.skip_dedup);
            shards_.push_back(std::move(s));
        }

        for (uint32_t i = 0; i < config_.num_shards; ++i) {
            shards_[i]->worker_thread = std::thread(&Sequencer::shard_worker_loop, this, i);
        }

        std::vector<std::vector<int>> assignments(config_.num_collectors);
        for (uint32_t i = 0; i < config_.num_brokers; ++i) {
            assignments[i % config_.num_collectors].push_back(int(i));
        }
        for (uint32_t i = 0; i < config_.num_collectors; ++i) {
            collector_threads_.emplace_back(&Sequencer::collector_loop_scatter, this,
                                            int(i), std::move(assignments[i]));
        }

        // Phase 2: Use committed_seq_updater instead of central writer (shards write GOI directly)
        writer_thread_ = std::thread(&Sequencer::committed_seq_updater, this);
        timer_thread_ = std::thread(&Sequencer::timer_loop_scatter, this);
        return;
    }

    // Single-threaded path
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

    if (config_.mode == SequencerMode::SCATTER_GATHER) {
        for (auto& s : shards_) {
            s->epoch_cv.notify_all();
            s->running.store(false, std::memory_order_release);
        }
        writer_cv_.notify_all();
        if (timer_thread_.joinable()) timer_thread_.join();
        for (auto& t : collector_threads_) if (t.joinable()) t.join();
        collector_threads_.clear();
        for (auto& s : shards_) {
            if (s->worker_thread.joinable()) s->worker_thread.join();
        }
        if (writer_thread_.joinable()) writer_thread_.join();
        return;
    }

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
        // Pipeline full: yield to avoid timer spinning at 100% (debug: reduced segfault on next run).
        freeze_collection_.store(true, std::memory_order_release);
        std::this_thread::sleep_for(std::chrono::microseconds(config_.epoch_us));
        return;
    }

    if (!cur_buf.seal()) {
        return;  // Not in COLLECTING state
    }
    // Never reset a buffer that might still have collectors (avoid heap-use-after-free in collector_loop).
    while (next_buf.active_collectors() != 0)
        std::this_thread::sleep_for(std::chrono::microseconds(10));

    next_buf.reset_and_start(next);
    freeze_collection_.store(false, std::memory_order_release);
    current_epoch_.store(next, std::memory_order_release);
    epoch_cv_.notify_all();
}

void Sequencer::collector_loop(int id, std::vector<int> pbr_ids) {
    const size_t pbr_mask = config_.pbr_entries - 1;

    while (!shutdown_.load(std::memory_order_acquire)) {
        if (freeze_collection_.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lk(epoch_mtx_);
            epoch_cv_.wait_for(lk, std::chrono::milliseconds(1));
            continue;
        }
        uint64_t epoch = current_epoch_.load(std::memory_order_acquire);
        auto& buf = epoch_bufs_[epoch % config::PIPELINE_DEPTH];
        // Avoid use-after-free: only collect into buffer that still matches current epoch
        // (buffer may have been reset for reuse if we raced with advance_epoch).
        if (buf.epoch_number != epoch) continue;
        if (!buf.enter_collection()) {
            std::unique_lock<std::mutex> lk(epoch_mtx_);
            epoch_cv_.wait_for(lk, std::chrono::milliseconds(1));
            continue;
        }

        const uint64_t collecting_epoch = epoch;
        auto& my_batches = buf.collectors[id].batches;

        for (int pbr_id : pbr_ids) {
            if (buf.epoch_number != collecting_epoch) break;  // Buffer was reset under us; exit and re-enter
            auto& ring = *pbr_rings_[pbr_id];
            auto& state = pbr_state_[pbr_id];
            uint64_t tail = ring.tail();
            PBREntry* base = pbr_ + (pbr_id * config_.pbr_entries);

            uint64_t scan = state.scan_pos.load(std::memory_order_relaxed);
            for (uint32_t i = 0; i < config::PREFETCH_DISTANCE && scan + i < tail; ++i) {
                PREFETCH_L1(&base[(scan + i) & pbr_mask]);
            }

            uint64_t highest_committed = state.committed_pos;

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

                // Critical: do NOT skip non-VALID slots unless timeout (dead producer).
                if (!(f & flags::VALID)) {
                    uint64_t now = now_ns();
                    if (state.hole_wait_start_ns == 0)
                        state.hole_wait_start_ns = now;
                    if ((now - state.hole_wait_start_ns) >
                        static_cast<uint64_t>(config::MAX_HOLE_WAIT_US) * 1000) {
                        stats_.pbr_holes_timed_out.fetch_add(1, std::memory_order_relaxed);
                        state.hole_wait_start_ns = 0;
                        ++scan;
                        state.skipped_hole_watermark = std::max(state.skipped_hole_watermark, scan);
                        continue;
                    }
                    break;  // Stall; retry same slot next iteration
                }
                state.hole_wait_start_ns = 0;  // Valid slot: reset wait

                // Document §4.2.1: skip stale epoch (zombie broker or recovery artifact)
                uint16_t cur_epoch_16 = static_cast<uint16_t>(
                    control_->epoch.load(std::memory_order_relaxed));
                uint16_t created = e.epoch_created;
                int age = static_cast<int>(cur_epoch_16) - static_cast<int>(created);
                if (age < 0) age += 65536;
                if (age > static_cast<int>(config::MAX_EPOCH_AGE)) {
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

            state.scan_pos.store(scan, std::memory_order_release);
            uint64_t head_watermark = std::max(highest_committed, state.skipped_hole_watermark);
            if (head_watermark > state.committed_pos) {
                state.committed_pos = head_watermark;
                buf.broker_committed_pbr[pbr_id].store(head_watermark, std::memory_order_release);
            }
        }

        buf.exit_collection();
    }
}

void Sequencer::collector_loop_scatter(int collector_id, std::vector<int> pbr_ids) {
    const size_t pbr_mask = config_.pbr_entries - 1;
    const uint32_t shard_mask = config_.num_shards - 1;

    while (!shutdown_.load(std::memory_order_acquire)) {
        if (freeze_collection_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(config::WAIT_SLEEP_US));
            continue;
        }
        for (int pbr_id : pbr_ids) {
            auto& ring = *pbr_rings_[pbr_id];
            auto& state = pbr_state_[pbr_id];
            uint64_t tail = ring.tail();
            PBREntry* base = pbr_ + (pbr_id * config_.pbr_entries);

            while (state.scan_pos.load(std::memory_order_acquire) < tail) {
                uint64_t pos = state.scan_pos.load(std::memory_order_relaxed);
                PBREntry& e = base[pos & pbr_mask];
                uint32_t f = e.flags.load(std::memory_order_acquire);

                // Bounded wait on !VALID: skip after timeout (dead producer).
                if (!(f & flags::VALID)) {
                    uint64_t now = now_ns();
                    if (state.hole_wait_start_ns == 0)
                        state.hole_wait_start_ns = now;
                    if ((now - state.hole_wait_start_ns) >
                        static_cast<uint64_t>(config::MAX_HOLE_WAIT_US) * 1000) {
                        stats_.pbr_holes_timed_out.fetch_add(1, std::memory_order_relaxed);
                        state.hole_wait_start_ns = 0;
                        state.scan_pos.store(pos + 1, std::memory_order_release);
                        continue;
                    }
                    break;  // Stall; retry same slot next iteration
                }
                state.hole_wait_start_ns = 0;

                uint16_t cur_epoch_16 = static_cast<uint16_t>(
                    control_->epoch.load(std::memory_order_relaxed));
                int age = static_cast<int>(cur_epoch_16) - static_cast<int>(e.epoch_created);
                if (age < 0) age += 65536;
                if (age > static_cast<int>(config::MAX_EPOCH_AGE)) {
                    state.scan_pos.store(pos + 1, std::memory_order_release);
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
                info.pbr_index = static_cast<uint32_t>(pos);
                info.timestamp_ns = e.timestamp_ns;

                state.scan_pos.store(pos + 1, std::memory_order_release);

                uint32_t shard_id;
                if (info.is_level5()) {
                    shard_id = static_cast<uint32_t>(hash64(info.client_id) & shard_mask);
                } else {
                    shard_id = static_cast<uint32_t>(pos & shard_mask);
                }

                auto& shard = *shards_[shard_id];
                int retries = 0;
                uint64_t backoff_us = 10;
                while (!shard.input_queue->try_push(std::move(info))) {
                    if (config_.allow_drops) {
                        if (++retries > 1000) {
                            // Stress-only: allow drops when shard queue saturated.
                            stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
                            break;
                        }
                        CPU_PAUSE();
                    } else {
                        std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                        backoff_us = std::min<uint64_t>(backoff_us * 2, 1000);
                    }
                }
            }
        }
        std::this_thread::yield();
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
        uint64_t epochs_done = stats_.epochs_completed.fetch_add(1, std::memory_order_relaxed) + 1;

        // Assessment §2.1: Phase profile every 100 epochs (diagnostic; set SEQUENCER_PROFILE_PHASE=1 to enable).
        if (epochs_done % 100 == 0 && epochs_done >= 100) {
            uint64_t merge_ns = stats_.merge_time_ns.load(std::memory_order_relaxed);
            uint64_t part_ns = stats_.phase_partition_ns.load(std::memory_order_relaxed);
            uint64_t l0_ns = stats_.phase_l0_ns.load(std::memory_order_relaxed);
            uint64_t l5_ns = stats_.phase_l5_ns.load(std::memory_order_relaxed);
            uint64_t hold_ns = stats_.hold_phase_ns.load(std::memory_order_relaxed);
            uint64_t l5_reorder_ns = stats_.phase_l5_reorder_ns.load(std::memory_order_relaxed);
            uint64_t l0_dedup_ns = stats_.phase_l0_dedup_ns.load(std::memory_order_relaxed);
            uint64_t window = 100;
            std::cerr << "[PROFILE] merge=" << (merge_ns / window / 1000)
                      << " partition=" << (part_ns / window / 1000)
                      << " l0=" << (l0_ns / window / 1000)
                      << " l5=" << (l5_ns / window / 1000)
                      << " l5_reorder=" << (l5_reorder_ns / window / 1000)
                      << " l0_dedup=" << (l0_dedup_ns / window / 1000)
                      << " hold=" << (hold_ns / window / 1000)
                      << " μs/epoch (avg over " << window << " epochs)\n";
            stats_.merge_time_ns.store(0, std::memory_order_relaxed);
            stats_.phase_partition_ns.store(0, std::memory_order_relaxed);
            stats_.phase_l0_ns.store(0, std::memory_order_relaxed);
            stats_.phase_l5_ns.store(0, std::memory_order_relaxed);
            stats_.phase_l5_reorder_ns.store(0, std::memory_order_relaxed);
            stats_.phase_l0_dedup_ns.store(0, std::memory_order_relaxed);
            stats_.hold_phase_ns.store(0, std::memory_order_relaxed);
        }

#ifdef SEQUENCER_DEBUG_TIMING
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
#endif

        for (uint32_t i = 0; i < config_.num_brokers; ++i) {
            uint64_t committed = buf.broker_committed_pbr[i].load(std::memory_order_acquire);
            if (committed > 0) {
                pbr_rings_[i]->advance_head(committed);
            }
        }

        // Assessment §3.2: Backlog monitoring (sequencer falling behind timer).
        uint64_t current = current_epoch_.load(std::memory_order_acquire);
        uint64_t processed = stats_.epochs_completed.load(std::memory_order_relaxed);
        uint64_t backlog = (current > processed) ? (current - processed) : 0;
        stats_.backlog_epochs.store(backlog, std::memory_order_relaxed);
        if (backlog > 10) {
            std::cerr << "[WARN] Sequencer backlog: " << backlog << " epochs ("
                      << (backlog * config_.epoch_us / 1000.0) << " ms)\n";
        }

        buf.mark_sequenced();
        ++next;
    }
}

void Sequencer::process_epoch(EpochBuffer& buf) {
    merge_buffer_.clear();
    ready_buffer_.clear();

    // Phase 2: avoid realloc in hot path — reserve once up to max
    if (merge_buffer_.capacity() < config::MAX_BATCHES_PER_EPOCH) merge_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH);
    if (ready_buffer_.capacity() < config::MAX_BATCHES_PER_EPOCH) ready_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH);
    if (l0_buffer_.capacity() < config::MAX_BATCHES_PER_EPOCH) l0_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH);
    if (l5_buffer_.capacity() < config::MAX_BATCHES_PER_EPOCH) l5_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH);

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
    // Process this epoch's L5 first so we never emit expired/gap (drain/age) before this epoch's lower client_seqs.
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
    size_t out_before_hold = out.size();
    drain_hold(out, epoch);
    age_hold(out, epoch);
    if (!clients_with_held_batches_.empty())
        drain_hold(out, epoch);  // Second drain only if some client still has held batches
    for (size_t i = out_before_hold; i < out.size(); ++i) {
        out[i].global_seq = global_seq_.fetch_add(1, std::memory_order_relaxed);
        stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
    }
    stats_.hold_phase_ns.fetch_add(now_ns() - t_hold, std::memory_order_relaxed);
}

void Sequencer::sequence_batches_per_epoch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch) {
    uint64_t t_part0 = now_ns();
    l0_buffer_.clear();
    l5_buffer_.clear();
    for (auto& b : in) {
        if (b.is_level5() && config_.level5_enabled) {
            l5_buffer_.push_back(std::move(b));
        } else {
            l0_buffer_.push_back(std::move(b));
        }
    }
    stats_.phase_partition_ns.fetch_add(now_ns() - t_part0, std::memory_order_relaxed);

    uint64_t t_l0 = now_ns();
    process_level0(l0_buffer_, out);
    stats_.phase_l0_ns.fetch_add(now_ns() - t_l0, std::memory_order_relaxed);

    // Step 3: Collect all L5 batches (from epoch + hold buffers) for single sort
    std::vector<BatchInfo> l5_ready;
    l5_ready.reserve(l5_buffer_.size() + hold_size_ + 1024);

    uint64_t t_l5 = now_ns();
    if (!l5_buffer_.empty()) process_level5(l5_buffer_, l5_ready, epoch);
    stats_.phase_l5_ns.fetch_add(now_ns() - t_l5, std::memory_order_relaxed);

    uint64_t t_hold = now_ns();
    drain_hold(l5_ready, epoch);
    age_hold(l5_ready, epoch);
    if (!clients_with_held_batches_.empty())
        drain_hold(l5_ready, epoch);  // Second drain only if some client still has held batches
    stats_.hold_phase_ns.fetch_add(now_ns() - t_hold, std::memory_order_relaxed);

    // Step 3: Single sort of ALL L5 batches (this epoch's + drained from hold buffer)
    if (config_.level5_enabled && !l5_ready.empty()) {
        uint64_t t_reorder = now_ns();
        std::sort(l5_ready.begin(), l5_ready.end(), [](const BatchInfo& a, const BatchInfo& b) {
            if (a.client_id != b.client_id) return a.client_id < b.client_id;
            return a.client_seq < b.client_seq;
        });
        stats_.phase_l5_reorder_ns.fetch_add(now_ns() - t_reorder, std::memory_order_relaxed);
        
        // Append sorted L5 to output
        out.insert(out.end(),
                   std::make_move_iterator(l5_ready.begin()),
                   std::make_move_iterator(l5_ready.end()));
        l5_ready.clear();
    }

    if (!out.empty()) {
        uint64_t base = global_seq_.fetch_add(out.size(), std::memory_order_relaxed);
        stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < out.size(); ++i) out[i].global_seq = base + i;
    }
}

// Level 0 = no per-client order; Level 5 = strong per-client order (see Order levels comment above).
void Sequencer::process_level0(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out) {
    uint64_t t0 = now_ns();
    for (auto& b : in) {
        if (!dedup_.check_and_insert(b.batch_id)) {
            out.push_back(std::move(b));
        } else {
            stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
        }
    }
    stats_.phase_l0_dedup_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}

void Sequencer::process_level5(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch) {
    if (!deferred_l5_.empty()) {
        in.insert(in.end(),
                  std::make_move_iterator(deferred_l5_.begin()),
                  std::make_move_iterator(deferred_l5_.end()));
        deferred_l5_.clear();
    }
    stats_.level5_batches.fetch_add(in.size(), std::memory_order_relaxed);

    // Step 3: Do NOT sort here - collect and sort once at the end with drain_hold/age_hold results
    // For now, just do per-client ordering validation and hold buffer logic
    
    // Group by client_id (but don't sort globally - that happens in sequence_batches_per_epoch)
    std::sort(in.begin(), in.end(), [](const BatchInfo& a, const BatchInfo& b) {
        return a.client_id < b.client_id;
    });

    auto it = in.begin();
    while (it != in.end()) {
        uint64_t cid = it->client_id;
        auto start = it;
        while (it != in.end() && it->client_id == cid) ++it;

        // Sort this client's batches by client_seq
        sorter_.sort_by_client_seq(&*start, &*it);

        ClientState& st = get_state(cid);
        st.last_epoch = epoch;

        for (auto curr = start; curr != it; ++curr) {
            auto& b = *curr;
            if (st.is_duplicate(b.client_seq)) {
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Already committed (retry or late arrival): reject duplicate, do not advance next_expected
            uint64_t& hc = get_hc(cid);
            if (b.client_seq <= hc) {
                st.mark_sequenced(b.client_seq);
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // seq > hc and !is_duplicate => seq >= next_expected. Emit in order or hold.
            if (b.client_seq == st.next_expected) {
                if (!dedup_.check_and_insert(b.batch_id)) {
                    out.push_back(std::move(b));
                    hc = std::max(hc, b.client_seq);
                    st.mark_sequenced(b.client_seq);
                    st.advance_next_expected();
                } else {
                    st.mark_sequenced(b.client_seq);
                    st.advance_next_expected();
                    stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                add_to_hold(cid, std::move(b), epoch);
            }
        }
    }
}

void Sequencer::add_to_hold(uint64_t client_id, BatchInfo&& batch, uint64_t current_epoch) {
    if (hold_size_ >= config::HOLD_BUFFER_MAX) {
        if (config_.allow_drops) {
            // Stress-only: allow drops under overload.
            stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Backpressure path: defer and retry next epoch (no loss).
            if (deferred_l5_.size() >= config::DEFERRED_L5_MAX) {
                // Avoid deadlock: sequencer thread must not block on its own backlog.
                stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                deferred_l5_.push_back(std::move(batch));
            }
        }
        return;
    }

    uint64_t seq = batch.client_seq;
    auto& client_map = get_hold_buf(client_id);
    // Check for duplicate (client_id, client_seq) before moving batch; try_emplace would move
    // the batch even when insertion fails, losing data and not counting the duplicate.
    if (client_map.find(seq) != client_map.end()) {
        stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t expiry_epoch = current_epoch + config::HOLD_MAX_WAIT_EPOCHS;
    client_map.emplace(seq, HoldEntry{std::move(batch), 0});
    ++hold_size_;
    expiry_queue_.emplace(expiry_epoch, std::make_pair(client_id, seq));
    clients_with_held_batches_.insert(client_id);
    stats_.hold_buffer_size.store(hold_size_, std::memory_order_relaxed);
}

void Sequencer::drain_hold(std::vector<BatchInfo>& out, uint64_t epoch) {
    for (auto sit = clients_with_held_batches_.begin(); sit != clients_with_held_batches_.end(); ) {
        uint64_t cid = *sit;
        std::map<uint64_t, HoldEntry>& held = get_hold_buf(cid);
        if (held.empty()) {
            sit = clients_with_held_batches_.erase(sit);
            continue;
        }

        ClientState& st = get_state(cid);
        st.last_epoch = epoch;

        auto it = held.begin();
        while (it != held.end() && it->first == st.next_expected) {
            uint64_t seq = it->first;
            uint64_t& hc = get_hc(cid);
            if (seq <= hc) {
                st.mark_sequenced(seq);
                st.advance_next_expected();
                it = held.erase(it);
                --hold_size_;
                continue;
            }
            bool is_dup = dedup_.check_and_insert(it->second.batch.batch_id);
            if (!is_dup) {
                out.push_back(std::move(it->second.batch));
                hc = std::max(hc, seq);
            } else {
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
            }
            st.mark_sequenced(seq);
            st.advance_next_expected();
            it = held.erase(it);
            --hold_size_;
        }

        if (held.empty()) {
            sit = clients_with_held_batches_.erase(sit);
        } else {
            ++sit;
        }
    }
    stats_.hold_buffer_size.store(hold_size_, std::memory_order_relaxed);
}

void Sequencer::age_hold(std::vector<BatchInfo>& out, uint64_t current_epoch) {
    // Collect expired entries so we can emit in (client_id, client_seq) order.
    // Emitting in expiry order would violate per-client order (e.g. seq 10 before seq 9 for same client).
    age_hold_expired_buffer_.clear();
    auto end_it = expiry_queue_.upper_bound(current_epoch);
    for (auto it = expiry_queue_.begin(); it != end_it; ) {
        uint64_t cid = it->second.first;
        uint64_t seq = it->second.second;
        it = expiry_queue_.erase(it);

        std::map<uint64_t, HoldEntry>& cmap = get_hold_buf(cid);
        auto entry_it = cmap.find(seq);
        if (entry_it == cmap.end()) continue;

        BatchInfo batch = std::move(entry_it->second.batch);
        cmap.erase(entry_it);
        if (cmap.empty()) {
            clients_with_held_batches_.erase(cid);
        }
        --hold_size_;
        age_hold_expired_buffer_.push_back({cid, seq, std::move(batch)});
    }

    std::sort(age_hold_expired_buffer_.begin(), age_hold_expired_buffer_.end(),
              [](const ExpiredEntry& a, const ExpiredEntry& b) {
                  if (a.cid != b.cid) return a.cid < b.cid;
                  return a.seq < b.seq;
              });

    for (ExpiredEntry& ent : age_hold_expired_buffer_) {
        ClientState& st = get_state(ent.cid);
        uint64_t& hc = get_hc(ent.cid);
        if (ent.seq < st.next_expected || ent.seq <= hc) {
            st.mark_sequenced(ent.seq);
            // Advance next_expected so later batches (e.g. seq+1) can drain; otherwise
            // we'd leave next_expected behind and hold valid in-order batches forever.
            if (ent.seq >= st.next_expected)
                st.next_expected = ent.seq + 1;
            continue;
        }
        stats_.gaps_skipped.fetch_add(ent.seq - st.next_expected, std::memory_order_relaxed);
        st.next_expected = ent.seq + 1;
        st.mark_sequenced(ent.seq);
        if (!dedup_.check_and_insert(ent.batch.batch_id)) {
            out.push_back(std::move(ent.batch));
            hc = std::max(hc, ent.seq);
        }
    }

    // Remove from clients_with_held_batches_ any client whose hold map is now empty
    std::vector<uint64_t> to_remove;
    for (uint64_t cid : clients_with_held_batches_) {
        if (get_hold_buf(cid).empty()) to_remove.push_back(cid);
    }
    for (uint64_t cid : to_remove) clients_with_held_batches_.erase(cid);
    stats_.hold_buffer_size.store(hold_size_, std::memory_order_relaxed);
}

void Sequencer::client_gc(uint64_t cur_epoch) {
    if ((cur_epoch & (config::CLIENT_GC_EPOCH_INTERVAL - 1)) != 0) return;

    if (use_dense()) {
        // Dense path: iterate states_dense_, reset inactive clients (do NOT clear client_highest_committed_dense_)
        for (size_t cid = 0; cid < states_dense_.size(); ++cid) {
            ClientState& st = states_dense_[cid];
            if (cur_epoch - st.last_epoch > config::CLIENT_TTL_EPOCHS) {
                hold_size_ -= hold_buf_dense_[cid].size();
                hold_buf_dense_[cid].clear();
                clients_with_held_batches_.erase(cid);
                st = ClientState{};
            }
        }
        return;
    }

    for (auto& shard : client_shards_) {
        if (shard.states.size() <= config::MAX_CLIENTS_PER_SHARD) continue;

        std::vector<uint64_t> evict;
        for (const auto& [cid, st] : shard.states) {
            if (cur_epoch - st.last_epoch > config::CLIENT_TTL_EPOCHS) {
                evict.push_back(cid);
            }
        }
        for (uint64_t c : evict) {
            // Step 4: Clean hold buffer for evicted client. Use lazy deletion for expiry_queue.
            auto it = hold_buf_.find(c);
            if (it != hold_buf_.end()) {
                hold_size_ -= it->second.size();
                hold_buf_.erase(it);
            }
            clients_with_held_batches_.erase(c);
            // NOTE: Stale expiry_queue entries for evicted clients are lazily skipped in age_hold()
            // when hold_buf_.find(client_id) returns end(). This avoids O(n²) scan per evicted client.
            shard.states.erase(c);
            // Do NOT erase client_highest_committed_[c]. It is the durability barrier: late/retry
            // packets with client_seq <= hc must be rejected.
        }
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
            uint64_t count = validation_batch_count_.fetch_add(1, std::memory_order_relaxed);
            bool record = config_.validate_ordering || (count & config::VALIDATION_SAMPLE_MASK) == 0;
            if (record)
                validation_ring_.record(b.global_seq, b.batch_id, b.client_id, b.client_seq, b.is_level5());
        }

        if (!buf.sequenced.empty()) {
            control_->committed_seq.store(max_seq, std::memory_order_release);
        }

        buf.sequenced.clear();
        buf.mark_committed();
        ++next;
    }
}

// ----- Scatter-gather: shard helpers (plan §3.2) -----

void Sequencer::add_to_hold_shard(ShardState& shard, BatchInfo&& batch, uint64_t current_epoch) {
    if (shard.hold_size >= config::HOLD_BUFFER_MAX) {
        if (config_.allow_drops) {
            // Stress-only: allow drops under overload.
            stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
        } else {
            if (shard.deferred_l5.size() >= config::DEFERRED_L5_MAX) {
                // Avoid deadlock: shard worker must not block on its own backlog.
                stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                shard.deferred_l5.push_back(std::move(batch));
            }
        }
        return;
    }
    uint64_t client_id = batch.client_id;
    uint64_t seq = batch.client_seq;
    uint64_t expiry_epoch = current_epoch + config::HOLD_MAX_WAIT_EPOCHS;

    auto& client_map = shard.hold_buf[client_id];
    auto [iter, inserted] = client_map.try_emplace(seq, HoldEntry{std::move(batch), 0});
    if (inserted) {
        ++shard.hold_size;
        shard.expiry_heap.push(HoldExpiryKey{expiry_epoch, client_id, seq});
    } else {
        // Duplicate (client_id, client_seq) already in hold buffer - count it
        // (Sync with single-threaded add_to_hold path.)
        shard.duplicates.fetch_add(1, std::memory_order_relaxed);
        stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
    }
}

void Sequencer::drain_hold_shard(ShardState& shard, std::vector<BatchInfo>& out, uint64_t epoch) {
    for (auto cit = shard.hold_buf.begin(); cit != shard.hold_buf.end(); ) {
        uint64_t cid = cit->first;
        auto& held = cit->second;
        auto& st = shard.client_states[cid];
        uint64_t& hc = shard.client_highest_committed[cid];
        st.last_epoch = epoch;

        // Sync next_expected with hc if state was recreated after GC
        if (st.next_expected == 1 && hc > 0) {
            st.next_expected = hc + 1;
            st.highest_sequenced = hc;
            st.window_base = hc + 1;
        }

        auto it = held.begin();
        while (it != held.end() && it->first == st.next_expected) {
            uint64_t seq = it->first;
            if (seq <= hc) {
                st.mark_sequenced(seq);
                st.advance_next_expected();
                it = held.erase(it);
                --shard.hold_size;
                continue;
            }
            bool is_dup = shard.dedup.check_and_insert(it->second.batch.batch_id);
            if (!is_dup) {
                out.push_back(std::move(it->second.batch));
                hc = std::max(hc, seq);
            } else {
                shard.duplicates.fetch_add(1, std::memory_order_relaxed);
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
            }
            st.mark_sequenced(seq);
            st.advance_next_expected();
            it = held.erase(it);
            --shard.hold_size;
        }
        if (held.empty())
            cit = shard.hold_buf.erase(cit);
        else
            ++cit;
    }
}

// SHARD_PATH: Must stay in sync with single-threaded age_hold (hc, dedup, ordering, gaps_skipped formula).
void Sequencer::age_hold_shard(ShardState& shard, std::vector<BatchInfo>& out, uint64_t current_epoch) {
    // Collect expired so we emit in (client_id, client_seq) order; expiry order can violate per-client order.
    struct ExpiredEntry { uint64_t cid; uint64_t seq; BatchInfo batch; };
    std::vector<ExpiredEntry> expired;
    while (!shard.expiry_heap.empty() && shard.expiry_heap.top().expiry_epoch <= current_epoch) {
        HoldExpiryKey k = shard.expiry_heap.top();
        shard.expiry_heap.pop();

        auto cit = shard.hold_buf.find(k.client_id);
        if (cit == shard.hold_buf.end()) continue;
        auto entry_it = cit->second.find(k.client_seq);
        if (entry_it == cit->second.end()) continue;

        BatchInfo batch = std::move(entry_it->second.batch);
        cit->second.erase(entry_it);
        if (cit->second.empty()) shard.hold_buf.erase(cit);
        --shard.hold_size;
        expired.push_back({k.client_id, k.client_seq, std::move(batch)});
    }

    std::sort(expired.begin(), expired.end(), [](const ExpiredEntry& a, const ExpiredEntry& b) {
        if (a.cid != b.cid) return a.cid < b.cid;
        return a.seq < b.seq;
    });

    for (ExpiredEntry& ent : expired) {
        auto& st = shard.client_states[ent.cid];
        uint64_t& hc = shard.client_highest_committed[ent.cid];

        // Sync next_expected with hc if state was recreated after GC
        if (st.next_expected == 1 && hc > 0) {
            st.next_expected = hc + 1;
            st.highest_sequenced = hc;
            st.window_base = hc + 1;
        }

        if (ent.seq < st.next_expected || ent.seq <= hc) {
            st.mark_sequenced(ent.seq);
            // Advance next_expected so later batches (e.g. seq+1) can drain; otherwise
            // we'd leave next_expected behind and hold valid in-order batches forever.
            // (Sync with single-threaded age_hold path.)
            if (ent.seq >= st.next_expected)
                st.next_expected = ent.seq + 1;
            continue;
        }
        uint64_t delta = ent.seq - st.next_expected;
        shard.gaps_skipped.fetch_add(delta, std::memory_order_relaxed);
        stats_.gaps_skipped.fetch_add(delta, std::memory_order_relaxed);
        st.next_expected = ent.seq + 1;
        st.mark_sequenced(ent.seq);
        if (!shard.dedup.check_and_insert(ent.batch.batch_id)) {
            out.push_back(std::move(ent.batch));
            hc = std::max(hc, ent.seq);
        }
    }
}

// SHARD_PATH: Client GC for scatter-gather mode. Must stay in sync with single-threaded client_gc.
// Evicts inactive client states to prevent memory leak in long-running benchmarks.
// NOTE: Do NOT erase client_highest_committed[cid] — it is the durability barrier for retries.
void Sequencer::client_gc_shard(ShardState& shard, uint64_t cur_epoch) {
    // Run every CLIENT_GC_EPOCH_INTERVAL epochs to amortize overhead
    if ((cur_epoch & (config::CLIENT_GC_EPOCH_INTERVAL - 1)) != 0) return;

    // Only run if shard has many clients (avoid overhead when not needed)
    if (shard.client_states.size() <= config::MAX_CLIENTS_PER_SHARD) return;

    std::vector<uint64_t> evict;
    for (const auto& [cid, st] : shard.client_states) {
        if (cur_epoch - st.last_epoch > config::CLIENT_TTL_EPOCHS) {
            evict.push_back(cid);
        }
    }

    for (uint64_t cid : evict) {
        // Clean hold buffer for evicted client
        auto it = shard.hold_buf.find(cid);
        if (it != shard.hold_buf.end()) {
            shard.hold_size -= it->second.size();
            shard.hold_buf.erase(it);
        }
        // Note: expiry_heap entries for evicted clients are handled lazily in age_hold_shard
        // (entry_it == cmap.end() check skips stale entries)
        shard.client_states.erase(cid);
        // Do NOT erase client_highest_committed[cid]. It is the durability barrier.
    }
}

// SHARD_PATH: Must stay in sync with single-threaded process_level5 (hc, dedup, in-order emit, hold).
void Sequencer::process_level5_shard(ShardState& shard, std::vector<BatchInfo>& batches, uint64_t epoch) {
    if (!shard.deferred_l5.empty()) {
        batches.insert(batches.end(),
                       std::make_move_iterator(shard.deferred_l5.begin()),
                       std::make_move_iterator(shard.deferred_l5.end()));
        shard.deferred_l5.clear();
    }
    std::sort(batches.begin(), batches.end(),
              [](const BatchInfo& a, const BatchInfo& b) { return a.client_id < b.client_id; });

    auto it = batches.begin();
    while (it != batches.end()) {
        uint64_t cid = it->client_id;
        auto start = it;
        while (it != batches.end() && it->client_id == cid) ++it;

        std::sort(start, it, [](const BatchInfo& a, const BatchInfo& b) { return a.client_seq < b.client_seq; });

        auto& state = shard.client_states[cid];
        uint64_t& hc = shard.client_highest_committed[cid];
        state.last_epoch = epoch;

        // Sync next_expected with hc if client state was newly created but hc was preserved
        // (e.g., after GC cleared client_states but kept client_highest_committed).
        // (Sync with single-threaded get_state lazy initialization.)
        if (state.next_expected == 1 && hc > 0) {
            state.next_expected = hc + 1;
            state.highest_sequenced = hc;
            state.window_base = hc + 1;
        }

        for (auto curr = start; curr != it; ++curr) {
            auto& b = *curr;
            if (state.is_duplicate(b.client_seq)) {
                shard.duplicates.fetch_add(1, std::memory_order_relaxed);
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Already committed (retry or late arrival): reject duplicate; do not advance next_expected (align with single-threaded path).
            if (b.client_seq <= hc) {
                state.mark_sequenced(b.client_seq);
                shard.duplicates.fetch_add(1, std::memory_order_relaxed);
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (b.client_seq < state.next_expected) {
                state.mark_sequenced(b.client_seq);
                continue;
            }
            if (b.client_seq == state.next_expected) {
                if (!shard.dedup.check_and_insert(b.batch_id)) {
                    shard.ready.push_back(std::move(b));
                    hc = std::max(hc, b.client_seq);
                    state.mark_sequenced(b.client_seq);
                    state.advance_next_expected();
                } else {
                    state.mark_sequenced(b.client_seq);
                    state.advance_next_expected();
                    shard.duplicates.fetch_add(1, std::memory_order_relaxed);
                    stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                add_to_hold_shard(shard, std::move(b), epoch);
            }
        }
    }
}

void Sequencer::shard_worker_loop(uint32_t shard_id) {
    ShardState& shard = *shards_[shard_id];
    std::vector<BatchInfo> input_buffer;
    input_buffer.reserve(16384);

    while (!shutdown_.load(std::memory_order_acquire)) {
        {
            std::unique_lock<std::mutex> lk(shard.epoch_mutex);
            shard.epoch_cv.wait(lk, [&] {
                return shutdown_.load(std::memory_order_acquire) ||
                       coordinator_.current_epoch.load(std::memory_order_acquire) >
                       shard.current_epoch.load(std::memory_order_relaxed);
            });
        }
        if (shutdown_.load(std::memory_order_acquire)) break;

        uint64_t epoch = coordinator_.current_epoch.load(std::memory_order_acquire);
        shard.current_epoch.store(epoch, std::memory_order_relaxed);
        shard.epoch_complete.store(false, std::memory_order_relaxed);

        input_buffer.clear();
        shard.input_queue->drain_to(input_buffer);

        shard.ready.clear();
        shard.ready.reserve(input_buffer.size() + config::SHARD_READY_EXTRA);

        uint64_t t_part0 = now_ns();
        shard.l0_batches.clear();
        shard.l5_batches.clear();
        for (auto& b : input_buffer) {
            if (b.is_level5() && config_.level5_enabled)
                shard.l5_batches.push_back(std::move(b));
            else
                shard.l0_batches.push_back(std::move(b));
        }
        shard.phase_partition_ns.fetch_add(now_ns() - t_part0, std::memory_order_relaxed);

        uint64_t t_l0 = now_ns();
        for (auto& b : shard.l0_batches) {
            if (!shard.dedup.check_and_insert(b.batch_id))
                shard.ready.push_back(std::move(b));
            else {
                shard.duplicates.fetch_add(1, std::memory_order_relaxed);
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
            }
        }
        shard.phase_l0_ns.fetch_add(now_ns() - t_l0, std::memory_order_relaxed);

        uint64_t t_l5 = now_ns();
        if (!shard.l5_batches.empty())
            process_level5_shard(shard, shard.l5_batches, epoch);
        shard.phase_l5_ns.fetch_add(now_ns() - t_l5, std::memory_order_relaxed);

        uint64_t t_hold = now_ns();
        drain_hold_shard(shard, shard.ready, epoch);
        age_hold_shard(shard, shard.ready, epoch);
        shard.phase_hold_ns.fetch_add(now_ns() - t_hold, std::memory_order_relaxed);

        // Client GC to prevent memory leak in long-running benchmarks
        client_gc_shard(shard, epoch);

        // §1.1: Per-client ordering before global_seq (same as single-thread path).
        if (config_.level5_enabled && !shard.ready.empty()) {
            auto l5_start = std::stable_partition(shard.ready.begin(), shard.ready.end(),
                [](const BatchInfo& b) { return !b.is_level5(); });
            if (l5_start != shard.ready.end()) {
                std::sort(l5_start, shard.ready.end(), [](const BatchInfo& a, const BatchInfo& b) {
                    if (a.client_id != b.client_id) return a.client_id < b.client_id;
                    return a.client_seq < b.client_seq;
                });
            }
        }

        // Phase 2: Decentralized GOI writes — shard writes directly to GOI and updates PBR watermarks
        size_t n_ready = shard.ready.size();
        if (n_ready != 0) {
            uint64_t t_write = now_ns();
            const size_t goi_mask = config_.goi_entries - 1;
            uint64_t base = coordinator_.global_seq.fetch_add(n_ready, std::memory_order_relaxed);
            uint64_t bytes = 0;
            uint64_t now_ns_val = now_ns();
            for (size_t i = 0; i < n_ready; ++i) {
                BatchInfo& b = shard.ready[i];
                uint64_t seq = base + i;
                b.global_seq = seq;
                GOIEntry& e = goi_[seq & goi_mask];
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
                e.global_seq.store(seq, std::memory_order_release);
                cxl_write_delay();
                bytes += b.payload_size;
                if (b.timestamp_ns > 0)
                    stats_.latency_ring.record(now_ns_val - b.timestamp_ns);
                uint64_t count = validation_batch_count_.fetch_add(1, std::memory_order_relaxed);
                bool record = config_.validate_ordering || (count & config::VALIDATION_SAMPLE_MASK) == 0;
                if (record)
                    validation_ring_.record(seq, b.batch_id, b.client_id, b.client_seq, b.is_level5());
                // Update max PBR per broker for committed_seq_updater to advance heads
                uint16_t bid = b.broker_id;
                if (bid < config_.num_brokers) {
                    uint64_t new_val = static_cast<uint64_t>(b.pbr_index + 1);
                    for (uint64_t prev = scatter_max_pbr_[bid].load(std::memory_order_relaxed);
                         new_val > prev && !scatter_max_pbr_[bid].compare_exchange_weak(prev, new_val,
                                 std::memory_order_relaxed, std::memory_order_relaxed); )
                        ;
                }
            }
            shard.watermark.store(base + n_ready - 1, std::memory_order_release);
            shard.batches_processed.fetch_add(n_ready, std::memory_order_relaxed);
            shard.ready.clear();
            shard.phase_write_ns.fetch_add(now_ns() - t_write, std::memory_order_relaxed);
        }

        stats_.level5_batches.fetch_add(shard.l5_batches.size(), std::memory_order_relaxed);

        shard.epoch_complete.store(true, std::memory_order_release);
        shard.epochs_processed.fetch_add(1, std::memory_order_relaxed);
    }
}

void Sequencer::committed_seq_updater() {
    // Phase 2: committed_seq is updated by timer_loop_scatter after epoch completion.
    // This thread only advances PBR heads from scatter_max_pbr_.
    int sleep_us = config::WAIT_SLEEP_US;
    while (!shutdown_.load(std::memory_order_acquire)) {
        bool did_work = false;
        for (uint32_t i = 0; i < config_.num_brokers; ++i) {
            uint64_t new_head = scatter_max_pbr_[i].load(std::memory_order_acquire);
            if (new_head > last_advanced_pbr_[i]) {
                pbr_rings_[i]->advance_head(new_head);
                last_advanced_pbr_[i] = new_head;
                did_work = true;
            }
        }
        if (did_work)
            sleep_us = config::WAIT_SLEEP_US;
        else
            sleep_us = std::min(1000, sleep_us + (sleep_us / 2));
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
}

void Sequencer::timer_loop_scatter() {
    using Clock = std::chrono::steady_clock;
    const auto epoch_duration = std::chrono::microseconds(config_.epoch_us);
    auto next_tick = Clock::now() + epoch_duration;

    while (!shutdown_.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        if (now < next_tick)
            std::this_thread::sleep_for(next_tick - now);
        next_tick += epoch_duration;

        // Phase 2: Wait for all shards to finish epoch (no central writer; shards write GOI directly)
        uint64_t cur = coordinator_.current_epoch.load(std::memory_order_acquire);
        uint64_t min_epoch = cur;
        for (uint32_t i = 0; i < config_.num_shards; ++i) {
            min_epoch = std::min(min_epoch, shards_[i]->current_epoch.load(std::memory_order_relaxed));
        }
        uint64_t backlog = (cur > min_epoch) ? (cur - min_epoch) : 0;
        stats_.backlog_epochs.store(backlog, std::memory_order_relaxed);
        if (cur > 0) {
            while (!shutdown_.load(std::memory_order_acquire)) {
                bool all_complete = true;
                for (uint32_t i = 0; i < config_.num_shards; ++i) {
                    if (!shards_[i]->epoch_complete.load(std::memory_order_acquire)) {
                        all_complete = false;
                        break;
                    }
                }
                if (all_complete) break;
                std::this_thread::sleep_for(std::chrono::microseconds(config::WAIT_SLEEP_US));
            }
            // After all shards complete: contiguous prefix is [0, global_seq-1] (not min(watermarks)).
            uint64_t final_seq = coordinator_.global_seq.load(std::memory_order_acquire);
            if (final_seq > 0)
                control_->committed_seq.store(final_seq - 1, std::memory_order_release);

            if ((cur % 100) == 0) {
                uint64_t part = 0, l0 = 0, l5 = 0, hold = 0, write = 0, epochs = 0;
                size_t max_q = 0, max_hold = 0, max_deferred = 0, max_ready = 0;
                for (uint32_t i = 0; i < config_.num_shards; ++i) {
                    epochs += shards_[i]->epochs_processed.exchange(0, std::memory_order_relaxed);
                    part += shards_[i]->phase_partition_ns.exchange(0, std::memory_order_relaxed);
                    l0 += shards_[i]->phase_l0_ns.exchange(0, std::memory_order_relaxed);
                    l5 += shards_[i]->phase_l5_ns.exchange(0, std::memory_order_relaxed);
                    hold += shards_[i]->phase_hold_ns.exchange(0, std::memory_order_relaxed);
                    write += shards_[i]->phase_write_ns.exchange(0, std::memory_order_relaxed);
                    max_q = std::max(max_q, shards_[i]->input_queue->size());
                    max_hold = std::max(max_hold, shards_[i]->hold_size);
                    max_deferred = std::max(max_deferred, shards_[i]->deferred_l5.size());
                    max_ready = std::max(max_ready, shards_[i]->ready.size());
                }
                if (epochs > 0) {
                    std::cerr << "[SG_PROFILE] partition=" << (part / epochs / 1000)
                              << " l0=" << (l0 / epochs / 1000)
                              << " l5=" << (l5 / epochs / 1000)
                              << " hold=" << (hold / epochs / 1000)
                              << " write=" << (write / epochs / 1000)
                              << " μs/epoch (avg over " << epochs << " epochs)\n";
                    std::cerr << "[SG_QUEUES] max_q=" << max_q
                              << " max_hold=" << max_hold
                              << " max_deferred=" << max_deferred
                              << " max_ready=" << max_ready << "\n";
                }
            }
        }
        if (shutdown_.load(std::memory_order_acquire)) break;

        coordinator_.current_epoch.fetch_add(1, std::memory_order_release);
        for (uint32_t i = 0; i < config_.num_shards; ++i)
            shards_[i]->epoch_cv.notify_one();
    }
}

InjectResult Sequencer::inject_batch(uint16_t broker_id, uint64_t batch_id,
                              uint32_t payload_size, uint32_t msg_count,
                              uint32_t extra_flags,
                              uint64_t client_id, uint64_t client_seq) {
    if (broker_id >= config_.num_brokers) return InjectResult::RING_FULL;
    auto& ring = *pbr_rings_[broker_id];
    
    // Step 1: Proactive backpressure check - BEFORE reserve()
    uint64_t head = ring.head();
    uint64_t tail = ring.tail();
    uint64_t used = tail - head;
    double usage = static_cast<double>(used) / config_.pbr_entries;
    
    if (usage >= config::PBR_HIGH_WATERMARK_PCT) {
        stats_.backpressure_events.fetch_add(1, std::memory_order_relaxed);
        return InjectResult::BACKPRESSURE;
    }
    
    auto slot = ring.reserve();
    if (!slot) {
        stats_.pbr_full_count.fetch_add(1, std::memory_order_relaxed);
        return InjectResult::RING_FULL;
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
    stats_.total_injected.fetch_add(1, std::memory_order_relaxed);
    return InjectResult::SUCCESS;
}

bool Sequencer::validate_ordering() {
    return validate_ordering_reason().empty();
}

/** Query: Is broker_id's PBR below low watermark? For adaptive backoff in producer. */
bool Sequencer::below_low_watermark(uint16_t broker_id) const {
    if (broker_id >= config_.num_brokers) return true;
    auto& ring = *pbr_rings_[broker_id];
    uint64_t used = ring.tail() - ring.head();
    double usage = static_cast<double>(used) / config_.pbr_entries;
    return usage < config::PBR_LOW_WATERMARK_PCT;
}

/** PBR usage snapshot: average (used/capacity)*100 across all brokers. */
double Sequencer::get_pbr_usage_pct() const {
    if (config_.num_brokers == 0 || pbr_rings_.empty()) return 0.0;
    double sum = 0.0;
    for (uint32_t i = 0; i < config_.num_brokers; ++i) {
        auto& ring = *pbr_rings_[i];
        uint64_t used = ring.tail() - ring.head();
        sum += static_cast<double>(used) / static_cast<double>(config_.pbr_entries);
    }
    return 100.0 * sum / static_cast<double>(config_.num_brokers);
}

// Call after stop(); drains lock-free ring (no mutex on hot path).
std::string Sequencer::validate_ordering_reason() {
    std::vector<ValidationEntry> sorted = validation_ring_.drain();
    if (sorted.empty()) return "";
    std::sort(sorted.begin(), sorted.end(),
              [](const ValidationEntry& a, const ValidationEntry& b) { return a.global_seq < b.global_seq; });
    uint64_t prev_seq = 0;
    for (const auto& e : sorted) {
        if (e.global_seq <= prev_seq && prev_seq > 0) {
            return "global_seq non-increasing: prev=" + std::to_string(prev_seq) + " cur=" + std::to_string(e.global_seq);
        }
        prev_seq = e.global_seq;
    }
    // Per-client order (Level 5): only L5 batches guarantee per-client order; L0 batches are not ordered per-client.
    std::map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> by_client;
    for (const auto& e : sorted) {
        if (e.is_level5)
            by_client[e.client_id].emplace_back(e.global_seq, e.client_seq);
    }
    for (auto& [cid, vec] : by_client) {
        std::sort(vec.begin(), vec.end());  // by global_seq
        for (size_t i = 1; i < vec.size(); ++i) {
            if (vec[i].second <= vec[i - 1].second) {
                return "per-client order violation client_id=" + std::to_string(cid)
                    + " global_seq " + std::to_string(vec[i - 1].first) + " client_seq " + std::to_string(vec[i - 1].second)
                    + " then global_seq " + std::to_string(vec[i].first) + " client_seq " + std::to_string(vec[i].second);
            }
        }
    }
    return "";
}

}  // namespace embarcadero

// ============================================================================
// Benchmark
// ============================================================================

namespace bench {

using namespace embarcadero;
using Clock = std::chrono::steady_clock;

enum class ValidityMode {
    ALGORITHM,      // Strict: algorithm comparison (no loss, steady-state, low PBR)
    MAX_THROUGHPUT, // Open-loop max throughput (no loss, low PBR, latency warnings only)
    STRESS          // Lossy allowed (explicit stress test only)
};

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
    // Scatter-gather (plan §4.2)
    bool scatter_gather = false;
    uint32_t num_shards = 8;
    // Steady-state detection (paper-grade runs)
    bool steady_state = true;
    uint64_t steady_state_window_ms = 200;
    uint32_t steady_state_windows = 5;
    double steady_state_cv_pct = 5.0;
    uint64_t steady_state_timeout_ms = 5000;
    uint64_t steady_state_max_backlog_epochs = 10;
    // Run validity thresholds (paper-grade)
    double pbr_full_valid_threshold_pct = 0.1;  // 0.1% for paper; loosen for dev if needed
    ValidityMode validity_mode = ValidityMode::MAX_THROUGHPUT;
    bool validity_mode_explicit = false;  // True if user set via --validity=
    // Suite selection (paper runs often skip stress/scalability)
    bool run_ablation1 = true;
    bool run_levels = true;
    bool run_scalability = true;
    bool run_epoch_test = true;
    bool run_broker_test = true;
    bool run_scatter_gather = true;
    bool skip_dedup = false;  // If true, disable dedup for ablation (benchmark has no retries)
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
    uint64_t backpressure_events = 0;  // Inject attempts deferred by high watermark (controlled backpressure)
    double pbr_usage_pct = 0;  // Snapshot: avg (used/capacity)*100 across brokers at end of run
    double duration = 0;
    double efficiency_pct = 0;  // 100 * achieved / (21 GB/s theoretical max)
    uint64_t epoch_us = 500;  // Target epoch interval (for reporting)
    bool pbr_saturated = false;  // true if PBR full % > threshold (run invalid for algorithm comparison)
    bool steady_state_reached = false;
    double steady_state_cv_pct = 0;
    uint64_t steady_state_windows = 0;
    uint64_t backlog_max = 0;
    uint64_t validation_overwrites = 0;
    bool valid = true;

    double pbr_full_pct() const {
        uint64_t total = batches + pbr_full;
        return (total > 0) ? (100.0 * static_cast<double>(pbr_full) / static_cast<double>(total)) : 0;
    }
};

struct StatResult {
    double median = 0;
    double min_val = 0;
    double max_val = 0;
    double stddev = 0;
    double mean = 0;

    static StatResult compute(std::vector<double>& values) {
        StatResult out;
        if (values.empty()) return out;
        std::sort(values.begin(), values.end());
        const size_t n = values.size();
        out.median = (n % 2) ? values[n / 2] : (values[n / 2 - 1] + values[n / 2]) / 2.0;
        out.min_val = values.front();
        out.max_val = values.back();
        const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(n);
        out.mean = mean;
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
    uint64_t valid_runs = 0;
    uint64_t total_runs = 0;
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
        sc.mode = cfg_.scatter_gather ? SequencerMode::SCATTER_GATHER : cfg_.mode;
        sc.use_radix_sort = cfg_.use_radix_sort;
        sc.validate_ordering = cfg_.validate;  // Full trace when validating; sampling when not
        if (cfg_.pbr_entries > 0) sc.pbr_entries = cfg_.pbr_entries;
        if (cfg_.scatter_gather) {
            sc.scatter_gather_enabled = true;
            sc.num_shards = cfg_.num_shards;
        }
        sc.skip_dedup = cfg_.skip_dedup;
        // Phase 2: dense client state (O(1) lookup) when client_id in [0, clients)
        sc.max_clients = cfg_.clients;
        sc.allow_drops = (cfg_.validity_mode == ValidityMode::STRESS);
        // Validation coverage: size ring for full-trace when validating (throughput * duration).
        // Stress test: use fixed 1M to avoid huge allocation and long validation; non-stress: cap at 4M.
        if (cfg_.validate) {
            if (cfg_.validity_mode == ValidityMode::STRESS) {
                sc.validation_ring_size = config::VALIDATION_RING_SIZE;  // 1M
            } else {
                size_t expected_batches = 3000000ULL * cfg_.duration_sec;
                sc.validation_ring_size = next_pow2(expected_batches);
                sc.validation_ring_size = std::min(sc.validation_ring_size, config::VALIDATION_RING_CAP);
                // Long runs: validate only the last VALIDATION_RING_CAP batches to keep validation time bounded.
            }
        }

        Sequencer seq(sc);
        if (!seq.initialize()) throw std::runtime_error("Init failed");
        seq.start();

        ProducerGroup producers;
        start_producers(seq, producers);

        std::cout << "  Warmup..." << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.warmup_ms));
        std::cout << " done\n";

        SteadyStateInfo ss{};
        if (cfg_.steady_state) {
            std::cout << "  Steady-state..." << std::flush;
            ss = wait_for_steady_state(seq);
            std::cout << (ss.reached ? " reached" : " timeout");
            std::cout << " (cv=" << std::fixed << std::setprecision(1) << ss.cv_pct
                      << "%, backlog_max=" << ss.backlog_max << ")\n";
        }

        auto base_b = seq.stats().batches_sequenced.load();
        auto base_bytes = seq.stats().bytes_sequenced.load();
        auto base_e = seq.stats().epochs_completed.load();
        auto base_level5 = seq.stats().level5_batches.load();
        auto base_gaps = seq.stats().gaps_skipped.load();
        auto base_dropped = seq.stats().batches_dropped.load();
        auto base_atomics = seq.stats().atomics_executed.load();
        auto base_pbr_full = seq.stats().pbr_full_count.load();
        auto base_backpressure = seq.stats().backpressure_events.load();
        seq.reset_validation_overwrites();
        seq.stats().reset_latency();

        std::cout << "  Running..." << std::flush;
        auto t0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.duration_sec * 1000));
        auto t1 = Clock::now();
        stop_producers(producers, nullptr);
        // Sample stats AFTER stopping producers to avoid race where producers inject more batches
        // between snapshot and stop, causing accounted > injected (completeness failure).
        uint64_t injected_at_t1 = seq.stats().total_injected.load();
        uint64_t batches_at_t1 = seq.stats().batches_sequenced.load();
        uint64_t bytes_at_t1 = seq.stats().bytes_sequenced.load();
        uint64_t dropped_at_t1 = seq.stats().batches_dropped.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << " done\n";

        seq.stop();

        if (cfg_.validate && cfg_.validity_mode != ValidityMode::STRESS) {
            std::cout << "  Validating..." << std::flush;
            std::string reason;
            std::atomic<bool> validation_done{false};
            auto t_validate_start = Clock::now();
            std::thread validation_thread([&] {
                reason = seq.validate_ordering_reason();
                validation_done.store(true);
            });
            const int timeout_sec = 60;
            for (int s = 0; s < timeout_sec && !validation_done.load(); ++s) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (!validation_done.load()) std::cout << "." << std::flush;
            }
            validation_thread.join();
            auto t_validate_end = Clock::now();
            double validate_sec = std::chrono::duration<double>(t_validate_end - t_validate_start).count();
            if (reason.empty()) {
                std::cout << " PASSED";
                std::cout << " (" << std::fixed << std::setprecision(2) << validate_sec << " s)\n";
            } else {
                std::cout << " FAILED\n";
                std::cerr << "  Validation reason: " << reason << "\n";
                std::cerr << "  Validation took " << std::fixed << std::setprecision(2) << validate_sec << " s\n";
            }
        } else if (cfg_.validate && cfg_.validity_mode == ValidityMode::STRESS) {
            std::cout << "  Validation skipped (stress test)\n";
        }

        Result r;
        r.epoch_us = cfg_.epoch_us;
        r.duration = std::chrono::duration<double>(t1 - t0).count();
        r.batches = batches_at_t1 - base_b;
        r.epochs = seq.stats().epochs_completed.load() - base_e;
        r.level5 = seq.stats().level5_batches.load() - base_level5;
        r.gaps = seq.stats().gaps_skipped.load() - base_gaps;
        r.dropped = dropped_at_t1 - base_dropped;  // Use t1 snapshot for consistency with completeness window
        r.atomics = seq.stats().atomics_executed.load() - base_atomics;
        r.pbr_full = seq.stats().pbr_full_count.load() - base_pbr_full;
        r.backpressure_events = seq.stats().backpressure_events.load() - base_backpressure;
        r.pbr_usage_pct = seq.get_pbr_usage_pct();  // Snapshot after stop
        r.steady_state_reached = ss.reached;
        r.steady_state_cv_pct = ss.cv_pct;
        r.steady_state_windows = ss.windows;
        r.backlog_max = ss.backlog_max;
        r.validation_overwrites = seq.validation_overwrites();

        auto bytes = bytes_at_t1 - base_bytes;
        r.batches_sec = r.batches / r.duration;
        r.mb_sec = (bytes / 1e6) / r.duration;
        r.msgs_sec = (r.batches * cfg_.msgs_per_batch) / r.duration;

        r.avg_seq_ns = seq.stats().avg_seq_ns();
        r.min_seq_ns = seq.stats().seq_min_ns.load();
        r.max_seq_ns = seq.stats().seq_max_ns.load();

        // Phase 3.1: Report latency from sequencer latency_ring (inject-to-commit), not producer injection time
        std::vector<uint64_t> ns_samples = seq.stats().drain_latency_ring();
        if (!ns_samples.empty()) {
            std::sort(ns_samples.begin(), ns_samples.end());
            const size_t n = ns_samples.size();
            const double ns_to_us = 1e-3;
            r.p50_us = ns_samples[n / 2] * ns_to_us;
            r.p95_us = ns_samples[n * 95 / 100] * ns_to_us;
            r.p99_us = ns_samples[n * 99 / 100] * ns_to_us;
            r.p999_us = ns_samples[std::min(n * 999 / 1000, n - 1)] * ns_to_us;
        }

        const ValidityMode mode = cfg_.validity_mode;
        const bool allow_loss = (mode == ValidityMode::STRESS);
        r.valid = true;
        if (r.batches_sec == 0 && r.duration > 0) {
            std::cerr << "  WARNING: Zero throughput. PBR_full=" << r.pbr_full
                      << " (if high: PBR saturated - try larger pbr_entries or fewer producers).\n";
            if (!allow_loss) {
                std::cerr << "  [INVALID RUN] Zero throughput in non-stress mode.\n";
                r.valid = false;
            }
        }
        // Valid experiment for algorithm comparison: PBR_full below configured threshold (paper-grade default 0.1%).
        const double pbr_valid_threshold = cfg_.pbr_full_valid_threshold_pct;
        uint64_t total_attempts = r.batches + r.pbr_full;
        r.pbr_saturated = (total_attempts > 0 && r.pbr_full_pct() > pbr_valid_threshold);
        if (r.pbr_saturated) {
            std::cerr << "  WARNING: PBR_full=" << r.pbr_full << " (" << std::fixed << std::setprecision(1)
                      << r.pbr_full_pct() << "% of attempts). Run may be saturation-bound; "
                      << "consider larger pbr_entries or fewer producers for interpretable throughput.\n";
            // Assessment §6.1: PBR saturation is invalid for ALGORITHM (implies we can't compare fairly),
            // but expected for MAX_THROUGHPUT (open-loop peak capacity measurement).
            if (!allow_loss && mode != ValidityMode::MAX_THROUGHPUT) {
                std::cerr << "  [INVALID RUN] PBR saturation > " << std::fixed << std::setprecision(1)
                          << pbr_valid_threshold << "%; results do not reflect sequencer algorithm performance.\n";
                r.valid = false;
            }
        }

        // Efficiency vs theoretical max (21 GB/s CXL bandwidth)
        const double theoretical_max_batches_sec = 21e9 / static_cast<double>(cfg_.batch_size);
        r.efficiency_pct = (theoretical_max_batches_sec > 0)
            ? (100.0 * r.batches_sec / theoretical_max_batches_sec) : 0;

        // Assessment §5.2: Completeness - no silent loss.
        // Use global invariant so backlog drain doesn't cause false failures.
        uint64_t accounted_total = batches_at_t1 + dropped_at_t1;
        int64_t global_diff = static_cast<int64_t>(injected_at_t1) - static_cast<int64_t>(accounted_total);
        uint64_t in_flight = (global_diff > 0) ? static_cast<uint64_t>(global_diff) : 0;
        uint64_t threshold = std::max<uint64_t>(1000, injected_at_t1 / 100);
        if (global_diff < 0) {
            std::cerr << "  [COMPLETENESS FAIL] accounted > injected: injected=" << injected_at_t1
                      << " sequenced+dropped=" << accounted_total << " (diff " << global_diff << ")\n";
            r.valid = false;
        } else if (in_flight > threshold) {
            std::cerr << "  [COMPLETENESS WARN] large in-flight at t1: " << in_flight
                      << " (injected=" << injected_at_t1 << ", accounted=" << accounted_total << ")\n";
            if (in_flight > 1000000) {
                std::cerr << "  [SATURATION] Very high in-flight suggests system saturated; "
                          << "throughput may be capped by PBR/consumption, not sequencer.\n";
            }
        }

        // Assessment §6.2: Sanity checks on results.
        if (r.p99_us > 100000.0) {
            std::cerr << "  [SANITY WARN] P99 latency " << (r.p99_us / 1000.0)
                      << " ms is high; system is backlogged.\n";
            if (mode == ValidityMode::ALGORITHM) {
                std::cerr << "  [INVALID RUN] Latency exceeds algorithm-comparison threshold.\n";
                r.valid = false;
            }
        }
        if (cfg_.producers > 1 && r.batches_sec < 500000.0 * cfg_.producers) {
            std::cerr << "  [SANITY WARN] Throughput " << r.batches_sec
                      << " << expected min ~" << (500000u * cfg_.producers) << " batches/s.\n";
        }
        if (r.batches + r.pbr_full > 0) {
            double pbr_full_pct = 100.0 * static_cast<double>(r.pbr_full) / static_cast<double>(r.batches + r.pbr_full);
            if (pbr_full_pct > cfg_.pbr_full_valid_threshold_pct) {
                std::cerr << "  [SANITY WARN] PBR full " << std::fixed << std::setprecision(1) << pbr_full_pct
                          << "% exceeds threshold " << cfg_.pbr_full_valid_threshold_pct << "%.\n";
            }
        }
        if (r.batches > 0 && r.dropped > 0) {
            double drop_pct = 100.0 * static_cast<double>(r.dropped) / static_cast<double>(r.batches);
            std::cerr << "  [SANITY WARN] Drop rate " << std::fixed << std::setprecision(2) << drop_pct
                      << "% is too high for a log system.\n";
            if (!allow_loss) {
                std::cerr << "  [INVALID RUN] Drops not allowed for this validity mode.\n";
                r.valid = false;
            }
        }
        if (cfg_.steady_state && !r.steady_state_reached) {
            std::cerr << "  [STEADY-STATE WARN] Not reached (cv=" << std::fixed << std::setprecision(1)
                      << r.steady_state_cv_pct << "%, backlog_max=" << r.backlog_max << ").\n";
            if (mode == ValidityMode::ALGORITHM) {
                std::cerr << "  [INVALID RUN] Steady-state required for algorithm comparison.\n";
                r.valid = false;
            }
        }

        return r;
    }

private:
    struct SteadyStateInfo {
        bool reached = false;
        double cv_pct = 0;
        uint64_t windows = 0;
        uint64_t backlog_max = 0;
    };

    struct ProducerGroup {
        std::atomic<bool> go{true};
        std::vector<std::thread> threads;
        std::vector<std::vector<double>> tlats;
    };

    void start_producers(Sequencer& seq, ProducerGroup& group) {
        group.go.store(true, std::memory_order_relaxed);
        group.threads.clear();
        group.tlats.assign(cfg_.producers, {});
        global_batch_counter_.store(0, std::memory_order_relaxed);

        // Producer affinity: each producer owns a disjoint subset of clients so (client_id, client_seq)
        // is unique and strictly increasing per client (no duplicate seq from different producers).
        const uint32_t clients_per_producer = std::max(1u, (cfg_.clients + cfg_.producers - 1) / cfg_.producers);

        for (uint32_t tid = 0; tid < cfg_.producers; ++tid) {
            group.threads.emplace_back([&, tid] {
                thread_local std::mt19937_64 rng(tid + 42);
                std::uniform_real_distribution<> ldist(0, 1);

                const uint32_t client_base = tid * clients_per_producer;
                const uint32_t my_clients = std::min(clients_per_producer, cfg_.clients - client_base);
                std::uniform_int_distribution<uint32_t> cdist(0, my_clients > 0 ? my_clients - 1 : 0);
                std::vector<uint64_t> cseqs(my_clients, 0);

                auto& mylats = group.tlats[tid];

                while (group.go.load(std::memory_order_relaxed)) {
                    // Assessment §4.2: Backpressure when sequencer backlog is high.
                    if (seq.stats().backlog_epochs.load(std::memory_order_relaxed) > 50) {
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                    }
                    if (!seq.config().allow_drops &&
                        seq.stats().hold_buffer_size.load(std::memory_order_relaxed) > config::HOLD_BUFFER_SOFT_LIMIT) {
                        std::this_thread::sleep_for(std::chrono::microseconds(1));
                    }
                    auto t0 = Clock::now();
                    uint32_t ef = 0;
                    uint64_t cid = 0, cseq_val = 0;
                    if (cfg_.level5 && my_clients > 0 && ldist(rng) < cfg_.level5_ratio) {
                        ef = flags::STRONG_ORDER;
                        uint32_t local_c = cdist(rng);
                        cid = client_base + local_c;  // 0-based for dense state [0, max_clients)
                        cseq_val = ++cseqs[local_c];
                    }
                    uint64_t my_batch = global_batch_counter_.fetch_add(1, std::memory_order_relaxed);
                    uint16_t broker_id = static_cast<uint16_t>(my_batch % cfg_.brokers);
                    uint64_t batch_id = (uint64_t(tid) << 48) | my_batch;
                    
                    // Step 1: Adaptive backpressure with exponential backoff
                    uint64_t backoff_us = config::BACKPRESSURE_MIN_SLEEP_US;
                    uint64_t consecutive_bp = 0;
                    InjectResult result;
                    
                    while (true) {
                        result = seq.inject_batch(broker_id, batch_id,
                                cfg_.batch_size, cfg_.msgs_per_batch, ef, cid, cseq_val);
                        
                        if (result == InjectResult::SUCCESS) {
                            auto t1 = Clock::now();
                            if (mylats.size() < config::LATENCY_SAMPLE_LIMIT) {
                                mylats.push_back(
                                    std::chrono::duration<double, std::micro>(t1 - t0).count());
                            }
                            break;  // Successfully injected
                        } else if (result == InjectResult::BACKPRESSURE) {
                            consecutive_bp++;
                            // Exponential backoff with cap
                            std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                            backoff_us = std::min(backoff_us * 2, config::BACKPRESSURE_MAX_SLEEP_US);
                            
                            // If sustained backpressure, wait for low watermark
                            if (consecutive_bp > 10) {
                                while (!seq.below_low_watermark(broker_id)) {
                                    std::this_thread::sleep_for(
                                        std::chrono::microseconds(config::BACKPRESSURE_MAX_SLEEP_US));
                                    if (!group.go.load(std::memory_order_acquire)) return;
                                }
                            }
                            continue;  // RETRY
                        } else {
                            // RING_FULL: transient CAS failure, brief pause
                            CPU_PAUSE();
                            continue;  // RETRY
                        }
                    }

                    if ((my_batch & 0x3F) == 0) std::this_thread::yield();
                }
            });
        }
    }

    void stop_producers(ProducerGroup& group, std::vector<double>* lats) {
        group.go.store(false, std::memory_order_relaxed);
        for (auto& t : group.threads) t.join();
        if (lats) {
            for (auto& tl : group.tlats) {
                lats->insert(lats->end(), tl.begin(), tl.end());
            }
        }
    }

    SteadyStateInfo wait_for_steady_state(Sequencer& seq) {
        SteadyStateInfo out;
        std::vector<double> window_rates;
        window_rates.reserve(cfg_.steady_state_windows);
        uint64_t backlog_max = 0;

        auto t_start = Clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t_start).count()
               < static_cast<long long>(cfg_.steady_state_timeout_ms)) {
            uint64_t base_b = seq.stats().batches_sequenced.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.steady_state_window_ms));
            uint64_t cur_b = seq.stats().batches_sequenced.load(std::memory_order_relaxed);
            double window_sec = cfg_.steady_state_window_ms / 1000.0;
            double rate = (cur_b - base_b) / window_sec;
            window_rates.push_back(rate);
            if (window_rates.size() > cfg_.steady_state_windows)
                window_rates.erase(window_rates.begin());

            uint64_t backlog = seq.stats().backlog_epochs.load(std::memory_order_relaxed);
            backlog_max = std::max(backlog_max, backlog);

            if (window_rates.size() >= cfg_.steady_state_windows) {
                double mean = std::accumulate(window_rates.begin(), window_rates.end(), 0.0) / window_rates.size();
                double sq_sum = 0;
                for (double v : window_rates) sq_sum += (v - mean) * (v - mean);
                double stddev = std::sqrt(sq_sum / window_rates.size());
                double cv_pct = (mean > 0) ? (100.0 * stddev / mean) : 0.0;
                if (cv_pct <= cfg_.steady_state_cv_pct &&
                    backlog <= cfg_.steady_state_max_backlog_epochs) {
                    out.reached = true;
                    out.cv_pct = cv_pct;
                    out.windows = cfg_.steady_state_windows;
                    out.backlog_max = backlog_max;
                    return out;
                }
                out.cv_pct = cv_pct;
            }
        }
        out.reached = false;
        out.windows = window_rates.size();
        out.backlog_max = backlog_max;
        return out;
    }

    void produce(Sequencer& seq, uint64_t ms, std::vector<double>* lats) {
        std::atomic<bool> go{true};
        // Assessment §1.2: Round-robin brokers so no single PBR is overwhelmed (fixes zero throughput at 2 threads).
        std::atomic<uint64_t> global_batch_counter{0};
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

                auto& mylats = tlats[tid];

                while (go.load(std::memory_order_relaxed)) {
                    // Assessment §4.2: Backpressure when sequencer backlog is high.
                    if (seq.stats().backlog_epochs.load(std::memory_order_relaxed) > 50) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    if (!seq.config().allow_drops &&
                        seq.stats().hold_buffer_size.load(std::memory_order_relaxed) > config::HOLD_BUFFER_SOFT_LIMIT) {
                        std::this_thread::sleep_for(std::chrono::microseconds(100));
                    }
                    auto t0 = Clock::now();
                    uint32_t ef = 0;
                    uint64_t cid = 0, cseq_val = 0;
                    if (cfg_.level5 && my_clients > 0 && ldist(rng) < cfg_.level5_ratio) {
                        ef = flags::STRONG_ORDER;
                        uint32_t local_c = cdist(rng);
                        cid = client_base + local_c;  // 0-based for dense state [0, max_clients)
                        cseq_val = ++cseqs[local_c];
                    }
                    uint64_t my_batch = global_batch_counter.fetch_add(1, std::memory_order_relaxed);
                    uint16_t broker_id = static_cast<uint16_t>(my_batch % cfg_.brokers);
                    uint64_t batch_id = (uint64_t(tid) << 48) | my_batch;
                    
                    // Step 1: Adaptive backpressure with exponential backoff
                    uint64_t backoff_us = config::BACKPRESSURE_MIN_SLEEP_US;
                    uint64_t consecutive_bp = 0;
                    InjectResult result;
                    
                    while (true) {
                        result = seq.inject_batch(broker_id, batch_id,
                                cfg_.batch_size, cfg_.msgs_per_batch, ef, cid, cseq_val);
                        
                        if (result == InjectResult::SUCCESS) {
                            auto t1 = Clock::now();
                            if (mylats.size() < config::LATENCY_SAMPLE_LIMIT) {
                                mylats.push_back(
                                    std::chrono::duration<double, std::micro>(t1 - t0).count());
                            }
                            break;  // Successfully injected
                        } else if (result == InjectResult::BACKPRESSURE) {
                            consecutive_bp++;
                            // Exponential backoff with cap
                            std::this_thread::sleep_for(std::chrono::microseconds(backoff_us));
                            backoff_us = std::min(backoff_us * 2, config::BACKPRESSURE_MAX_SLEEP_US);
                            
                            // If sustained backpressure, wait for low watermark
                            if (consecutive_bp > 10) {
                                while (!seq.below_low_watermark(broker_id)) {
                                    std::this_thread::sleep_for(
                                        std::chrono::microseconds(config::BACKPRESSURE_MAX_SLEEP_US));
                                    if (!go.load(std::memory_order_acquire)) return;
                                }
                            }
                            continue;  // RETRY
                        } else {
                            // RING_FULL: transient CAS failure, brief pause
                            CPU_PAUSE();
                            continue;  // RETRY
                        }
                    }

                    if ((my_batch & 0x3F) == 0) std::this_thread::yield();
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
    std::atomic<uint64_t> global_batch_counter_{0};
};

void sep(char c = '=', int w = 65) { std::cout << std::string(w, c) << '\n'; }

MultiRunResult run_multiple(const Config& cfg) {
    // Warm-up run (discarded) to stabilize cache/allocator
    Runner(cfg).run();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<double> throughputs, latencies, mb_secs, efficiencies;
    uint64_t atomics = 0, epochs = 0, gaps = 0, dropped = 0, pbr_full = 0;
    uint64_t valid_runs = 0;

    const double theoretical_max = (cfg.batch_size > 0) ? (21e9 / static_cast<double>(cfg.batch_size)) : 0;

    for (int i = 0; i < cfg.num_runs; ++i) {
        Result r = Runner(cfg).run();
        if (r.valid) {
            ++valid_runs;
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
        } else {
            std::cerr << "  [RUN SKIPPED] invalid run (steady-state/saturation/sanity failure)\n";
        }
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
    const uint64_t nr = std::max<uint64_t>(1, valid_runs);
    out.total_atomics = atomics / nr;
    out.total_epochs = epochs / nr;
    out.total_gaps = gaps / nr;
    out.total_dropped = dropped / nr;
    out.total_pbr_full = pbr_full / nr;
    out.valid_runs = valid_runs;
    out.total_runs = static_cast<uint64_t>(cfg.num_runs);
    if (valid_runs < static_cast<uint64_t>(cfg.num_runs)) {
        std::cerr << "  [WARN] Valid runs " << valid_runs << "/" << cfg.num_runs
                  << " (invalid runs excluded from stats)\n";
    }
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
    // Assessment §6.1: Use ALGORITHM mode by default for fair algorithm comparison, but respect explicit CLI override.
    if (!cfg.validity_mode_explicit) {
        c_baseline.validity_mode = ValidityMode::ALGORITHM;
    }
    Config c_optimized = cfg;
    c_optimized.level5 = false;
    c_optimized.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;
    if (!cfg.validity_mode_explicit) {
        c_optimized.validity_mode = ValidityMode::ALGORITHM;
    }
    // For algorithm comparison: use large PBR so sequencer is bottleneck, not ring (paper-ready results).
    if (cfg.pbr_entries == 0) {
        c_baseline.pbr_entries = config::CLEAN_ABLATION_PBR_ENTRIES;
        c_optimized.pbr_entries = config::CLEAN_ABLATION_PBR_ENTRIES;
    }

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
    uint64_t base_valid = 0, opt_valid = 0;
    const double theoretical_max = (cfg.batch_size > 0) ? (21e9 / static_cast<double>(cfg.batch_size)) : 0;

    for (int i = 0; i < cfg.num_runs; ++i) {
        Result r_base = Runner(c_baseline).run();
        Result r_opt = Runner(c_optimized).run();
        if (r_base.valid) {
            ++base_valid;
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
        }
        if (r_opt.valid) {
            ++opt_valid;
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
        }
        // Only include speedup when both runs are valid and not PBR-bound.
        if (r_base.valid && r_opt.valid && r_base.batches_sec > 0 &&
            !r_base.pbr_saturated && !r_opt.pbr_saturated)
            speedups.push_back(r_opt.batches_sec / r_base.batches_sec);
    }

    const uint64_t base_nr = std::max<uint64_t>(1, base_valid);
    const uint64_t opt_nr = std::max<uint64_t>(1, opt_valid);
    Ablation1Result out;
    out.baseline.throughput = StatResult::compute(base_throughputs);
    out.baseline.latency_p99 = StatResult::compute(base_latencies);
    out.baseline.total_atomics = base_atomics / base_nr;
    out.baseline.total_epochs = base_epochs / base_nr;
    out.baseline.total_gaps = base_gaps / base_nr;
    out.baseline.total_dropped = base_dropped / base_nr;
    out.baseline.total_pbr_full = base_pbr / base_nr;
    out.baseline.valid_runs = base_valid;
    out.baseline.total_runs = static_cast<uint64_t>(cfg.num_runs);
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
    out.optimized.total_atomics = opt_atomics / opt_nr;
    out.optimized.total_epochs = opt_epochs / opt_nr;
    out.optimized.total_gaps = opt_gaps / opt_nr;
    out.optimized.total_dropped = opt_dropped / opt_nr;
    out.optimized.total_pbr_full = opt_pbr / opt_nr;
    out.optimized.valid_runs = opt_valid;
    out.optimized.total_runs = static_cast<uint64_t>(cfg.num_runs);
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
    uint64_t effective_runs = r.valid_runs > 0 ? r.valid_runs : static_cast<uint64_t>(num_runs);
    std::cout << "  " << name << " (" << r.valid_runs << "/" << num_runs << " valid runs)\n";
    sep('-');
    double cv_pct = (r.throughput.median > 0) ? (100.0 * r.throughput.stddev / r.throughput.median) : 0;
    double ci95 = (effective_runs > 1) ? (1.96 * r.throughput.stddev / std::sqrt(static_cast<double>(effective_runs))) : 0.0;
    std::cout << std::setprecision(2)
              << "  Throughput: " << (r.throughput.median / 1e6) << " M/s "
              << "[" << (r.throughput.min_val / 1e6) << ", " << (r.throughput.max_val / 1e6) << "] "
              << "±" << (r.throughput.stddev / 1e6) << " M/s "
              << "(cv " << std::setprecision(1) << cv_pct << "%, 95% CI ±" << (ci95 / 1e6) << " M/s)\n"
              << std::setprecision(2)
              << "  P99 Latency: " << r.latency_p99.median << " μs "
              << "[" << r.latency_p99.min_val << ", " << r.latency_p99.max_val << "]\n"
              << std::setprecision(0)
              << "  Atomics: " << r.total_atomics << "  Epochs: " << r.total_epochs << "  Gaps: " << r.total_gaps;
    if (r.total_dropped > 0) std::cout << "  Dropped: " << r.total_dropped;
    std::cout << "  PBR_full: " << r.total_pbr_full;
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
              << " max=" << (r.max_seq_ns / 1000.0) << " μs/epoch (target: " << r.epoch_us << " μs)\n"
              << "  Stats:      " << r.batches << " batches, " << r.epochs << " epochs, "
              << r.level5 << " L5, " << r.gaps << " gaps";
    if (r.atomics > 0) std::cout << ", " << r.atomics << " atomics";
    if (r.dropped > 0) std::cout << ", " << r.dropped << " dropped";
    std::cout << ", PBR_full: " << r.pbr_full
              << ", validation_overwrites: " << r.validation_overwrites << "\n";
    uint64_t total_inject_attempts = r.batches + r.pbr_full + r.backpressure_events;
    if (total_inject_attempts > 0) {
        double bp_pct = 100.0 * static_cast<double>(r.backpressure_events) / static_cast<double>(total_inject_attempts);
        std::cout << "  Backpressure: " << r.backpressure_events << " events ("
                  << std::fixed << std::setprecision(1) << bp_pct << "% of attempts)\n";
    }
    std::cout << "  PBR usage (snapshot): " << std::fixed << std::setprecision(1) << r.pbr_usage_pct << "%\n";
    std::cout << "  Steady-state: " << (r.steady_state_reached ? "yes" : "no")
              << " (cv=" << std::setprecision(1) << r.steady_state_cv_pct
              << "%, windows=" << r.steady_state_windows
              << ", backlog_max=" << r.backlog_max << ")\n";
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
        if (t >= 8) c.pbr_entries = 1024 * 1024;  // Avoid PBR saturation with 8+ producers
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

// Scatter-gather ablation: single-thread vs scatter-gather (plan §4.2, §6.2)
void scatter_gather_scaling_test(const Config& base) {
    std::cout << "\n"; sep();
    std::cout << "  ABLATION: Single-Thread vs Scatter-Gather\n"; sep();
    std::cout << std::setw(8) << "Shards"
              << std::setw(14) << "Throughput"
              << std::setw(10) << "Speedup"
              << std::setw(12) << "Efficiency\n";
    sep('-');

    Config cfg_single = base;
    cfg_single.scatter_gather = false;
    cfg_single.mode = SequencerMode::PER_EPOCH_ATOMIC;
    cfg_single.validity_mode = ValidityMode::MAX_THROUGHPUT;
    Result r_single = Runner(cfg_single).run();
    const double baseline = r_single.batches_sec;
    const double theoretical_max = (base.batch_size > 0) ? (21e9 / static_cast<double>(base.batch_size)) : 0;

    std::cout << std::fixed << std::setw(8) << 1
              << std::setw(14) << std::setprecision(0) << baseline
              << std::setw(10) << "1.00×"
              << std::setw(12) << std::setprecision(1)
              << (theoretical_max > 0 ? (100.0 * baseline / theoretical_max) : 0) << "%\n";

    for (uint32_t shards : {2u, 4u, 8u, 16u}) {
        Config cfg_sg = base;
        cfg_sg.scatter_gather = true;
        cfg_sg.num_shards = shards;
        cfg_sg.validity_mode = ValidityMode::MAX_THROUGHPUT;
        Result r_sg = Runner(cfg_sg).run();
        double speedup = (baseline > 0) ? (r_sg.batches_sec / baseline) : 0;
        double efficiency = (theoretical_max > 0 && r_sg.batches_sec > 0)
            ? (100.0 * r_sg.batches_sec / theoretical_max) : 0;
        std::cout << std::fixed << std::setw(8) << shards
                  << std::setw(14) << std::setprecision(0) << r_sg.batches_sec
                  << std::setw(10) << std::setprecision(2) << speedup << "×"
                  << std::setw(12) << std::setprecision(1) << efficiency << "%\n";
    }
}

}  // namespace bench

// ============================================================================
// Main
// ============================================================================

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog
              << " [brokers] [producers] [duration_sec] [level5_ratio] [num_runs] [use_radix_sort] [scatter_gather] [num_shards] [scatter_gather_only] [pbr_entries]\n"
              << "  brokers            1..32 (default 4)\n"
              << "  producers          1+ (default 8)\n"
              << "  duration_sec       1+ (default 10)\n"
              << "  level5_ratio       0.0..1.0 (default 0.1)\n"
              << "  num_runs           1+ (default 5); >=2 reports median [min,max]\n"
              << "  use_radix_sort     0|1 (default 0)\n"
              << "  scatter_gather     0|1 (default 0); 1 enables scatter-gather mode\n"
              << "  num_shards         1..32 (default 8); used when scatter_gather=1\n"
              << "  scatter_gather_only  0|1 (default 0); 1 runs only scatter-gather scaling test\n"
              << "  pbr_entries        0=auto (default); >0 = entries per broker (e.g. 4194304 for 4M)\n"
              << "  --clean            paper-ready: producers=2, pbr_entries=4M (sequencer as bottleneck)\n"
              << "  --paper            algorithm-limited: 1 producer, 60s, 64K PBR (target PBR_full<0.1%, valid speedup)\n"
              << "  --suite=LIST        run only selected suites (comma-separated): ablation,levels,scalability,epoch,broker,sg,all\n"
              << "  --no-steady-state   disable steady-state detection\n"
              << "  --no-validate      skip ordering validation (faster smoke runs)\n"
              << "  --ss-window=MS     steady-state window size (ms)\n"
              << "  --ss-cv=PCT         steady-state coefficient of variation threshold (percent)\n"
              << "  --ss-timeout=MS     steady-state detection timeout (ms)\n"
              << "  --ss-backlog=N      steady-state max backlog epochs\n"
              << "  --pbr-threshold=PCT PBR_full validity threshold (percent, e.g. 0.1)\n"
              << "  --validity=MODE     validity mode: algo|max|stress (default max)\n"
              << "  --skip-dedup        disable deduplication (for ablation speedup)\n"
              << "  --help, -h          print this and exit\n"
              << "  --test              minimal correctness test (ordering + per-client); exit 0 pass, 1 fail\n";
}

static const char* validity_mode_name(bench::ValidityMode mode) {
    switch (mode) {
        case bench::ValidityMode::ALGORITHM: return "algo";
        case bench::ValidityMode::MAX_THROUGHPUT: return "max";
        case bench::ValidityMode::STRESS: return "stress";
    }
    return "max";
}

/** Minimal correctness test: inject batches, run briefly, assert validate_ordering() (includes per-client). */
static bool run_correctness_test() {
    using namespace embarcadero;
    SequencerConfig sc;
    sc.num_brokers = 4;
    sc.num_collectors = 4;
    sc.epoch_us = 500;
    sc.level5_enabled = true;
    sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
    sc.validate();

    Sequencer seq(sc);
    if (!seq.initialize()) return false;
    seq.start();

    const uint32_t brokers = 4;
    const uint32_t batch_size = 1024;
    const uint32_t msgs_per_batch = 10;
    uint64_t batch_id = 0;
    std::vector<uint64_t> client_seqs(20, 0);

    for (int i = 0; i < 2000; ++i) {
        uint32_t flags = 0;
        uint64_t cid = 0, cseq = 0;
        if (i % 10 == 0) {
            flags = flags::STRONG_ORDER;
            cid = (i / 10) % 20 + 1;
            cseq = ++client_seqs[(cid - 1) % 20];
        }
        InjectResult result;
        do {
            result = seq.inject_batch(static_cast<uint16_t>(i % brokers), batch_id++, batch_size, msgs_per_batch, flags, cid, cseq);
            if (result != InjectResult::SUCCESS) {
                std::this_thread::sleep_for(std::chrono::microseconds(5));
            }
        } while (result != InjectResult::SUCCESS);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    seq.stop();
    bool ok = seq.validate_ordering();
    if (!ok) return false;

    /* Scatter-gather correctness: must use SCATTER_GATHER mode so collector_loop_scatter + shard_worker_loop run.
     * L5 batches with client_id=0 must route by hash(client_id) to a single shard (not position); per-client order validated. */
    SequencerConfig sc_sg;
    sc_sg.num_brokers = 4;
    sc_sg.num_collectors = 4;
    sc_sg.epoch_us = 500;
    sc_sg.level5_enabled = true;
    sc_sg.scatter_gather_enabled = true;
    sc_sg.num_shards = 2;
    sc_sg.mode = SequencerMode::SCATTER_GATHER;
    sc_sg.validate_ordering = true;
    sc_sg.validate();

    Sequencer seq_sg(sc_sg);
    if (!seq_sg.initialize()) return false;
    seq_sg.start();

    batch_id = 0;
    std::vector<uint64_t> sg_client_seqs(5, 0);
    for (int i = 0; i < 1500; ++i) {
        uint32_t flags = (i % 5 == 0) ? flags::STRONG_ORDER : 0;
        uint64_t cid = (i % 5 == 0) ? (i / 5) % 5 : 0;  // L5 clients 0..4 (client_id=0 explicitly included)
        uint64_t cseq = (cid < 5) ? ++sg_client_seqs[cid] : 0;
        InjectResult result;
        do {
            result = seq_sg.inject_batch(static_cast<uint16_t>(i % brokers), batch_id++, batch_size, msgs_per_batch, flags, cid, cseq);
            if (result != InjectResult::SUCCESS) {
                std::this_thread::sleep_for(std::chrono::microseconds(5));
            }
        } while (result != InjectResult::SUCCESS);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    seq_sg.stop();
    std::string sg_reason = seq_sg.validate_ordering_reason();
    if (!sg_reason.empty()) {
        std::cerr << "  Scatter-gather validation: " << sg_reason << "\n";
        return false;
    }
    return true;
}

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    bool clean_ablation_preset = false;
    bool paper_ablation_preset = false;
    bool suite_specified = false;
    std::vector<std::string> suites;
    bool steady_state = true;
    uint64_t ss_window_ms = 200;
    double ss_cv_pct = 5.0;
    uint64_t ss_timeout_ms = 5000;
    uint64_t ss_backlog = 10;
    double pbr_threshold_pct = 0.1;
    bench::ValidityMode validity_mode = bench::ValidityMode::MAX_THROUGHPUT;
    bool validity_specified = false;
    bool validate = true;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--test") {
            std::cout << "  Running minimal correctness test...\n";
            bool pass = run_correctness_test();
            std::cout << "  " << (pass ? "PASSED" : "FAILED") << "\n";
            return pass ? 0 : 1;
        }
        if (arg == "--clean") {
            clean_ablation_preset = true;
        }
        if (arg == "--paper") {
            paper_ablation_preset = true;
        }
        if (arg.rfind("--suite=", 0) == 0) {
            suite_specified = true;
            std::string list = arg.substr(std::string("--suite=").size());
            std::stringstream ss(list);
            std::string item;
            while (std::getline(ss, item, ',')) {
                if (!item.empty()) suites.push_back(item);
            }
        }
        if (arg == "--no-steady-state") {
            steady_state = false;
        }
        if (arg == "--no-validate") {
            validate = false;
        }
        if (arg.rfind("--ss-window=", 0) == 0) {
            ss_window_ms = static_cast<uint64_t>(std::stoull(arg.substr(std::string("--ss-window=").size())));
        }
        if (arg.rfind("--ss-cv=", 0) == 0) {
            ss_cv_pct = std::stod(arg.substr(std::string("--ss-cv=").size()));
        }
        if (arg.rfind("--ss-timeout=", 0) == 0) {
            ss_timeout_ms = static_cast<uint64_t>(std::stoull(arg.substr(std::string("--ss-timeout=").size())));
        }
        if (arg.rfind("--ss-backlog=", 0) == 0) {
            ss_backlog = static_cast<uint64_t>(std::stoull(arg.substr(std::string("--ss-backlog=").size())));
        }
        if (arg.rfind("--pbr-threshold=", 0) == 0) {
            pbr_threshold_pct = std::stod(arg.substr(std::string("--pbr-threshold=").size()));
        }
        if (arg.rfind("--validity=", 0) == 0) {
            std::string mode = arg.substr(std::string("--validity=").size());
            validity_specified = true;
            if (mode == "algo") {
                validity_mode = bench::ValidityMode::ALGORITHM;
            } else if (mode == "max") {
                validity_mode = bench::ValidityMode::MAX_THROUGHPUT;
            } else if (mode == "stress") {
                validity_mode = bench::ValidityMode::STRESS;
            } else {
                std::cerr << "Unknown validity mode: " << mode << "\n";
                print_usage(argv[0]);
                return 1;
            }
        }
    }

    std::cout << "\n";
    bench::sep();
    std::cout << "  EMBARCADERO SEQUENCER - ABLATION STUDY\n";
    bench::sep();

    bench::Config cfg;
    bool scatter_gather_only = false;
    if (argc > 1) { int v = std::atoi(argv[1]); cfg.brokers = static_cast<uint32_t>(std::max(1, v)); }
    if (argc > 2) { int v = std::atoi(argv[2]); cfg.producers = static_cast<uint32_t>(std::max(1, v)); }
    if (argc > 3) { int v = std::atoi(argv[3]); cfg.duration_sec = static_cast<uint64_t>(std::max(1, v)); }
    if (argc > 4) { double v = std::atof(argv[4]); cfg.level5_ratio = std::clamp(v, 0.0, 1.0); }
    if (argc > 5) { int v = std::atoi(argv[5]); cfg.num_runs = std::max(1, v); }
    if (argc > 6) cfg.use_radix_sort = (std::atoi(argv[6]) != 0);
    if (argc > 7) cfg.scatter_gather = (std::atoi(argv[7]) != 0);
    if (argc > 8) { int v = std::atoi(argv[8]); cfg.num_shards = std::clamp(static_cast<uint32_t>(v), 1u, 32u); }
    if (argc > 9) scatter_gather_only = (std::atoi(argv[9]) != 0);
    if (argc > 10) {
        size_t v = static_cast<size_t>(std::atoll(argv[10]));
        if (v > 0) cfg.pbr_entries = v;
    }
    if (clean_ablation_preset) {
        cfg.producers = 2;
        cfg.pbr_entries = embarcadero::config::CLEAN_ABLATION_PBR_ENTRIES;
    }
    if (paper_ablation_preset) {
        cfg.brokers = 4;
        cfg.producers = 1;
        cfg.duration_sec = 60;
        cfg.pbr_entries = 64 * 1024;  // 64K per broker: algorithm-limited, target PBR_full < 0.1%
    }
    if (!validity_specified && (clean_ablation_preset || paper_ablation_preset)) {
        validity_mode = bench::ValidityMode::ALGORITHM;
    }
    // Panel: <0.1% PBR saturation. Use larger PBR when 8+ producers (1M for full benchmark headroom).
    if (cfg.pbr_entries == 0 && cfg.producers >= 8) cfg.pbr_entries = 1024 * 1024;

    cfg.steady_state = steady_state;
    cfg.validate = validate;
    cfg.steady_state_window_ms = ss_window_ms;
    cfg.steady_state_cv_pct = ss_cv_pct;
    cfg.steady_state_timeout_ms = ss_timeout_ms;
    cfg.steady_state_max_backlog_epochs = ss_backlog;
    cfg.pbr_full_valid_threshold_pct = pbr_threshold_pct;
    cfg.validity_mode = validity_mode;
    cfg.validity_mode_explicit = validity_specified;

    if (suite_specified) {
        cfg.run_ablation1 = false;
        cfg.run_levels = false;
        cfg.run_scalability = false;
        cfg.run_epoch_test = false;
        cfg.run_broker_test = false;
        cfg.run_scatter_gather = false;
        for (const auto& s : suites) {
            if (s == "all") {
                cfg.run_ablation1 = cfg.run_levels = cfg.run_scalability = cfg.run_epoch_test =
                    cfg.run_broker_test = cfg.run_scatter_gather = true;
            } else if (s == "ablation") {
                cfg.run_ablation1 = true;
            } else if (s == "levels") {
                cfg.run_levels = true;
            } else if (s == "scalability") {
                cfg.run_scalability = true;
            } else if (s == "epoch") {
                cfg.run_epoch_test = true;
            } else if (s == "broker") {
                cfg.run_broker_test = true;
            } else if (s == "sg") {
                cfg.run_scatter_gather = true;
            }
        }
    }

    std::cout << "\nConfig: " << cfg.brokers << " brokers, " << cfg.producers
              << " producers, " << cfg.duration_sec << "s, "
              << int(cfg.level5_ratio * 100) << "% L5, "
              << cfg.num_runs << " runs, "
              << (cfg.use_radix_sort ? "radix" : "std::sort")
              << ", validate=" << (cfg.validate ? "yes" : "no")
              << ", steady_state=" << (cfg.steady_state ? "yes" : "no")
              << ", pbr_threshold=" << cfg.pbr_full_valid_threshold_pct << "%"
              << ", validity=" << validity_mode_name(cfg.validity_mode);
    if (cfg.scatter_gather) std::cout << ", scatter_gather=" << cfg.num_shards << " shards";
    if (scatter_gather_only) std::cout << ", scatter_gather_only=yes";
    if (cfg.pbr_entries > 0) std::cout << ", pbr_entries=" << cfg.pbr_entries;
    if (clean_ablation_preset) std::cout << " (--clean)";
    if (paper_ablation_preset) std::cout << " (--paper, target PBR_full<0.1%)";
    if (cfg.skip_dedup) std::cout << " (--skip-dedup)";
    if (suite_specified) {
        std::cout << " --suite=";
        for (size_t i = 0; i < suites.size(); ++i) {
            if (i > 0) std::cout << ",";
            std::cout << suites[i];
        }
    }
    std::cout << "\n";

    try {
        if (scatter_gather_only) {
            bench::scatter_gather_scaling_test(cfg);
            std::cout << "\n"; bench::sep();
            std::cout << "  Complete (scatter-gather only)\n"; bench::sep(); std::cout << "\n";
            return 0;
        }

        const bool multi_run = (cfg.num_runs >= 2);

        if (multi_run) {
            // ================================================================
            // ABLATION 1: Per-Batch vs Per-Epoch (paired runs, speedup distribution)
            // ================================================================
            if (cfg.run_ablation1) {
                std::cout << "\n"; bench::sep();
                std::cout << "  ABLATION 1: Per-Batch vs Per-Epoch (paired, " << cfg.num_runs << " runs)\n"; bench::sep();
                std::cout << "  (Epochs = epoch buffers fully processed by sequencer; atomics = epochs in per-epoch mode.\n"
                          << "   When Epoch CPU >> τ, sequencer is bottleneck so epochs << duration/τ.)\n";
                bench::Ablation1Result a1 = bench::run_ablation1_paired(cfg);
                bench::print_stat_result("Per-Batch Atomic (Baseline)", a1.baseline, cfg.num_runs);
                bench::print_stat_result("Per-Epoch Atomic (Optimized)", a1.optimized, cfg.num_runs);
                std::cout << std::fixed << std::setprecision(2);
                if (a1.speedup.median > 0) {
                    std::cout << "\n  >>> SPEEDUP: " << a1.speedup.median << "× "
                              << "[" << a1.speedup.min_val << ", " << a1.speedup.max_val << "] "
                              << "(±" << a1.speedup.stddev << ")\n";
                } else {
                    std::cout << "\n  >>> SPEEDUP: [INVALID COMPARISON] One or both modes were PBR-bound; "
                              << "speedup only computed when PBR saturation < "
                              << cfg.pbr_full_valid_threshold_pct << "%.\n";
                }
                std::cout << "  >>> ATOMIC REDUCTION: " << std::setprecision(0) << a1.atomic_reduction << "×\n";
            }

            // ================================================================
            // TEST 2-4: Ordering levels (multi-run)
            // ================================================================
            if (cfg.run_levels) {
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
                    if (t.ratio >= 1.0) c.validity_mode = bench::ValidityMode::STRESS;
                    bench::MultiRunResult r = bench::run_multiple(c);
                    bench::print_stat_result(t.name, r, cfg.num_runs);
                }
            }

            // Scalability: median of num_runs when num_runs >= 3
            if (cfg.run_scalability && cfg.num_runs >= 3) {
                std::cout << "\n"; bench::sep();
                std::cout << "  SCALABILITY: Threads (median of " << cfg.num_runs << " runs)\n"; bench::sep();
                std::cout << std::setw(8) << "Threads" << std::setw(14) << "Batches/s"
                          << std::setw(12) << "[min, max]" << std::setw(10) << "MB/s"
                          << std::setw(10) << "P99 μs" << '\n';
                bench::sep('-');
                for (uint32_t t : {1u, 2u, 4u, 8u, 16u}) {
                    auto c = cfg; c.producers = t; c.duration_sec = 5;
                    if (t >= 8) c.pbr_entries = 1024 * 1024;  // Avoid PBR saturation with 8+ producers
                    bench::MultiRunResult r = bench::run_multiple(c);
                    std::cout << std::fixed << std::setw(8) << t
                              << std::setw(14) << std::setprecision(0) << r.throughput.median
                              << " [" << std::setprecision(0) << r.throughput.min_val
                              << ", " << r.throughput.max_val << "]"
                              << std::setw(10) << std::setprecision(2) << r.median_mb_sec << " MB/s "
                              << std::setprecision(1) << r.latency_p99.median << " μs\n";
                }
            } else if (cfg.run_scalability) {
                bench::scalability_test(cfg);
            }
            if (cfg.run_epoch_test) bench::epoch_test(cfg);
            if (cfg.run_broker_test) bench::broker_test(cfg);
            if (cfg.run_scatter_gather) bench::scatter_gather_scaling_test(cfg);
        } else {
            // Single run: original format (force single-threaded for ablation tests)
            bench::Result baseline, optimized;
            if (cfg.run_ablation1) {
                std::cout << "\n"; bench::sep();
                std::cout << "  ABLATION 1a: Per-Batch Atomic (Baseline)\n"; bench::sep();
                auto c = cfg; c.scatter_gather = false; c.level5 = false; c.mode = embarcadero::SequencerMode::PER_BATCH_ATOMIC;
                baseline = bench::Runner(c).run();
                bench::print("Per-Batch Atomic", baseline);
            }
            if (cfg.run_ablation1) {
                std::cout << "\n"; bench::sep();
                std::cout << "  ABLATION 1b: Per-Epoch Atomic (Optimized)\n"; bench::sep();
                auto c = cfg; c.scatter_gather = false; c.level5 = false; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;
                optimized = bench::Runner(c).run();
                bench::print("Per-Epoch Atomic", optimized);
            }
            if (cfg.run_ablation1 && baseline.batches_sec > 0 && optimized.batches_sec > 0) {
                double atomic_red = (baseline.atomics > 0 && optimized.atomics > 0)
                    ? static_cast<double>(baseline.atomics) / optimized.atomics : 0;
                if (!baseline.pbr_saturated && !optimized.pbr_saturated) {
                    double speedup = optimized.batches_sec / baseline.batches_sec;
                    std::cout << "\n  >>> SPEEDUP: " << std::fixed << std::setprecision(2) << speedup << "×  ";
                } else {
                    std::cout << "\n  >>> SPEEDUP: [INVALID COMPARISON] One or both runs PBR-bound (saturation > "
                              << cfg.pbr_full_valid_threshold_pct << "%).  ";
                }
                std::cout << "ATOMIC REDUCTION: " << std::setprecision(0) << atomic_red << "×\n";
            }

            if (cfg.run_levels) {
                std::cout << "\n"; bench::sep();
                std::cout << "  TEST 2: Level 0\n"; bench::sep();
                { auto c = cfg; c.scatter_gather = false; c.level5 = false; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Level 0", bench::Runner(c).run()); }

                std::cout << "\n"; bench::sep();
                std::cout << "  TEST 3: Mixed 10% L5\n"; bench::sep();
                { auto c = cfg; c.scatter_gather = false; c.level5_ratio = 0.1; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Mixed", bench::Runner(c).run()); }

                if (cfg.stress_test) {
                    std::cout << "\n"; bench::sep();
                    std::cout << "  Stress Test (100% L5)\n"; bench::sep();
                    { auto c = cfg; c.scatter_gather = false; c.level5_ratio = 1.0; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Stress Test (100% L5)", bench::Runner(c).run()); }
                }
            }

            if (cfg.run_scalability) { auto c = cfg; c.scatter_gather = false; bench::scalability_test(c); }
            if (cfg.run_epoch_test) { auto c = cfg; c.scatter_gather = false; bench::epoch_test(c); }
            if (cfg.run_broker_test) { auto c = cfg; c.scatter_gather = false; bench::broker_test(c); }
        }

        std::cout << "\n"; bench::sep();
        std::cout << "  Complete\n"; bench::sep(); std::cout << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
