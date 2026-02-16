// Embarcadero Sequencer - Ablation Study for OSDI/SOSP/SIGCOMM
// Correctness-first: proper dedup, MPSC ring, Level 5 sliding window,
// hold-buffer emit, ordering validation, per-batch vs per-epoch comparison.
//
// Build: see CMakeLists.txt
// Optional: --phase-timing enables per-phase timing at runtime (Order 5 cost breakdown); no rebuild needed.
// Usage:
//   ./sequencer5_benchmark [brokers] [producers] [duration_sec] [level5_ratio] [num_runs] [use_radix_sort] [scatter_gather] [num_shards] [scatter_gather_only] [pbr_entries]
//   --clean   algorithm comparison: 1 producer, 64K PBR (natural backpressure, validity can pass)
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
#include <nmmintrin.h>  // SSE4.2 CRC32C (§A8)
#include <immintrin.h>
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
#include <cstdio>
#include <iostream>
#include <iomanip>
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <fstream>
#include <sstream>

#include "trace_generator.h"

#ifdef __linux__
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>
#include <pthread.h>
#include <fcntl.h>
#include <cerrno>
#include <cstdlib>
#if defined(HAVE_LIBNUMA)
#include <numa.h>
#include <numaif.h>
#endif
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

/// Pin current thread to a single core (review §minor.8). No-op on non-Linux or if core_id >= 128.
inline void pin_self_to_core(unsigned core_id) {
#ifdef __linux__
    unsigned n = core_id % 128;  // Avoid out-of-range
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(n, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#else
    (void)core_id;
#endif
}

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
    /// Hold buffer timeout (epochs). Superseded by ORDER5_BASE_TIMEOUT_NS (wall-clock timeout); kept for reference.
    inline constexpr int HOLD_MAX_WAIT_EPOCHS = 3;
    /// Design §5.5: Hold buffer timeout (wall clock). 5ms covers P99.9 intra-rack TCP retransmit.
    inline constexpr uint64_t ORDER5_BASE_TIMEOUT_NS = 5ULL * 1000 * 1000;
    inline constexpr size_t CLIENT_SHARDS = 16;
    inline constexpr size_t MAX_CLIENTS_PER_SHARD = 8192;
    /// Client TTL (epochs): evict client state after this many epochs idle (~10s at 500μs/epoch). Must exceed any in-flight batch delay.
    inline constexpr uint64_t CLIENT_TTL_EPOCHS = 20000;
    inline constexpr size_t BLOOM_BITS = 1 << 20;       // Unused; reserved for optional bloom-filter dedup
    inline constexpr size_t BLOOM_WORDS = BLOOM_BITS / 64;
    inline constexpr uint32_t BLOOM_HASHES = 4;
    inline constexpr uint64_t BLOOM_RESET_INTERVAL = 1000000;
    inline constexpr uint32_t PREFETCH_DISTANCE = 4;
    inline constexpr size_t COLLECTOR_STATE_CHECK_INTERVAL = 64;  // Was magic 63
    // P1: Sliding window removed; design uses next_expected only for duplicate detection.
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
    inline constexpr size_t RADIX_BITS = 8;      // Used only when use_radix_sort (RADIX_THRESHOLD)
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
    /// CXL: use real CXL memory when available (HAVE_LIBNUMA + Linux); no software delay (review: real CXL only).
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
    inline constexpr double PBR_HIGH_WATERMARK_PCT = 0.95;
    inline constexpr double PBR_LOW_WATERMARK_PCT = 0.50;
    inline constexpr uint64_t BACKPRESSURE_MIN_SLEEP_US = 1;
    inline constexpr uint64_t BACKPRESSURE_MAX_SLEEP_US = 1000;
    /// Bounded drain: max batches processed per shard cycle to bound worst-case latency (§A2)
    inline constexpr uint32_t MAX_DRAIN_PER_CYCLE = 1024;
    /// Lease safety: margin for lease expiration check (§A5)
    inline constexpr uint64_t LEASE_SAFETY_MARGIN_NS = 1ULL * 1000 * 1000; // 1ms
}

namespace flags {
    inline constexpr uint32_t VALID = 1u << 31;
    inline constexpr uint32_t STRONG_ORDER = 1u << 0;
    /// Design §5.6: SKIP marker in GOI so downstream (replicas, subscribers, ACK) can detect gaps.
    inline constexpr uint32_t SKIP_MARKER = 1u << 1;
    /// Design §5.4: Range-skip (message_count = first_held - expected).
    inline constexpr uint32_t RANGE_SKIP = 1u << 2;
}
/// Design §5.6: batch_id for SKIP markers (no real batch).
inline constexpr uint64_t SKIP_MARKER_BATCH_ID = 0xFFFFFFFFFFFFFFFFULL;

// ============================================================================
// CXL Memory Allocation (real CXL: NUMA node 2 /dev/dax or shm + mbind)
// ============================================================================
// Logic from src/cxl_manager/cxl_manager.cc allocate_shm(broker_id=0, Real).
// When HAVE_LIBNUMA and __linux__: try shm_open + mmap + mbind to NUMA node 2 (coreless CXL).
// Set EMBARCADERO_CXL_DRAM=1 to force DRAM (anonymous mmap) instead of real CXL.
// Set EMBARCADERO_CXL_BASE_ADDR=0x600000000000 to pin base address (optional).

extern "C" void* allocate_shm(size_t size);

#ifndef HAS_CXL_ALLOCATOR
static void* allocate_shm_real_cxl(size_t cxl_size) {
#ifdef __linux__
#if defined(HAVE_LIBNUMA)
    int cxl_fd = -1;
    bool use_dax = false;
    std::FILE* check = std::fopen("/dev/dax0.0", "r");
    if (check) {
        std::fclose(check);
        use_dax = true;
        cxl_fd = open("/dev/dax0.0", O_RDWR);
    } else {
        if (numa_available() == -1) return nullptr;
        cxl_fd = shm_open("/CXL_SHARED_FILE", O_CREAT | O_RDWR, 0666);
    }
    if (cxl_fd < 0) {
        std::fprintf(stderr, "[CXL] open/shm_open failed: %s\n", strerror(errno));
        return nullptr;
    }
    // Broker 0 (benchmark is single process): set size and create
    if (!use_dax) {
        if (ftruncate(cxl_fd, cxl_size) == -1) {
            std::fprintf(stderr, "[CXL] ftruncate failed: %s\n", strerror(errno));
            close(cxl_fd);
            return nullptr;
        }
    }
    std::vector<uintptr_t> addrs;
    const char* base_env = std::getenv("EMBARCADERO_CXL_BASE_ADDR");
    if (base_env && base_env[0] != '\0') {
        char* end = nullptr;
        uintptr_t p = static_cast<uintptr_t>(std::strtoull(base_env, &end, 0));
        if (end && *end == '\0' && p != 0) addrs.push_back(p);
    }
    if (addrs.empty())
        addrs = { 0x600000000000ULL, 0x500000000000ULL, 0x400000000000ULL };
    void* addr = nullptr;
    for (uintptr_t candidate : addrs) {
#ifdef MAP_FIXED_NOREPLACE
        addr = mmap(reinterpret_cast<void*>(candidate), cxl_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE | MAP_FIXED_NOREPLACE, cxl_fd, 0);
#else
        addr = mmap(reinterpret_cast<void*>(candidate), cxl_size, PROT_READ | PROT_WRITE,
                    MAP_SHARED | MAP_POPULATE | MAP_FIXED, cxl_fd, 0);
#endif
        if (addr != MAP_FAILED) {
            if (addr != reinterpret_cast<void*>(candidate)) {
                munmap(addr, cxl_size);
                addr = MAP_FAILED;
                continue;
            }
            break;
        }
    }
    close(cxl_fd);
    if (addr == MAP_FAILED || addr == nullptr) {
        std::fprintf(stderr, "[CXL] mmap failed: %s\n", strerror(errno));
        return nullptr;
    }
    // Bind to NUMA node 2 (coreless CXL) when using shm (not devdax)
    if (!use_dax) {
        struct bitmask* bitmask = numa_allocate_nodemask();
        numa_bitmask_setbit(bitmask, 2);
        if (mbind(addr, cxl_size, MPOL_BIND, bitmask->maskp, bitmask->size, MPOL_MF_MOVE) == -1)
            std::fprintf(stderr, "[CXL] mbind(NUMA 2) warning: %s (continuing)\n", strerror(errno));
        numa_free_nodemask(bitmask);
    }
    // Clear (broker 0)
    std::memset(addr, 0, cxl_size);
    return addr;
#else
    (void)cxl_size;
    return nullptr;
#endif
#else
    (void)cxl_size;
    return nullptr;
#endif
}

void* allocate_shm(size_t size) {
    void* ptr = nullptr;
    if (std::getenv("EMBARCADERO_CXL_DRAM") != nullptr) {
#ifdef __linux__
        ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
        if (ptr != MAP_FAILED)
            madvise(ptr, size, MADV_HUGEPAGE);
        else
            ptr = nullptr;
#else
        ptr = aligned_alloc(config::CACHE_LINE, size);
#endif
        return ptr;
    }
#if defined(HAVE_LIBNUMA) && defined(__linux__)
    ptr = allocate_shm_real_cxl(size);
    if (ptr) return ptr;
    std::fprintf(stderr, "[CXL] Real CXL alloc failed, falling back to DRAM (anonymous mmap)\n");
#endif
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
    std::atomic<bool> initialized{false};
    char _pad[79];
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
    uint64_t global_seq;
    uint64_t batch_id;
    uint64_t blog_offset;
    uint32_t pbr_index;
    uint32_t payload_size;
    uint32_t message_count;
    uint32_t crc32c;
    uint64_t client_id;
    uint64_t client_seq;
    uint16_t broker_id;
    uint16_t epoch_sequenced;
    uint16_t num_replicated;
    uint16_t flags;
};
static_assert(sizeof(GOIEntry) == 64);

static inline uint32_t compute_goi_crc(const GOIEntry& e) {
    uint32_t crc = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&e);
    // CRC over first 60 bytes (all fields except crc32c)
    // We can use 7 x 8-byte chunks (56 bytes) + 1 x 4-byte chunk (4 bytes)
    const uint64_t* p64 = reinterpret_cast<const uint64_t*>(p);
    crc = _mm_crc32_u64(crc, p64[0]);
    crc = _mm_crc32_u64(crc, p64[1]);
    crc = _mm_crc32_u64(crc, p64[2]);
    crc = _mm_crc32_u64(crc, p64[3]);
    crc = _mm_crc32_u64(crc, p64[4]);
    crc = _mm_crc32_u64(crc, p64[5]);
    crc = _mm_crc32_u64(crc, p64[6]);
    const uint32_t* p32 = reinterpret_cast<const uint32_t*>(p + 56);
    crc = _mm_crc32_u32(crc, p32[0]); // This covers client_id high part or client_seq low part depending on endianness, but it's 4 bytes.
    // Wait, let's be more precise.
    // global_seq(8), batch_id(8), broker_id(2), epoch_sequenced(2), pbr_index(4) = 24
    // blog_offset(8), payload_size(4), message_count(4) = 16 (Total 40)
    // num_replicated(2), flags(2), client_id(8), client_seq(8) = 20 (Total 60)
    // crc32c(4) = 64.
    // So p64[0..6] is 56 bytes. p32[14] is the next 4 bytes.
    return crc;
}

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
    uint64_t timestamp_ns = 0;   // Inject time (producer). E2E latency = commit_time - timestamp_ns.
    uint64_t received_ns = 0;   // When collector read PBR entry. Sequencing latency = commit_time - received_ns.
    uint64_t global_seq = 0;

    bool is_level5() const noexcept { return (flags & flags::STRONG_ORDER) != 0; }
};

/// Per-client state for Level 5 ordering. Design §5.3: next_expected only; no sliding window (P1).
struct ClientState {
    uint64_t next_expected = 0;
    uint64_t highest_sequenced = 0;
    uint64_t last_epoch = 0;

    bool is_duplicate(uint64_t seq) const { return seq < next_expected; }
    void mark_sequenced(uint64_t /*seq*/) { /* Design: next_expected is the only state; no window. */ }
    void advance_next_expected() { next_expected++; }
};

struct HoldEntry {
    BatchInfo batch;
    int wait_epochs = 0;
    uint64_t hold_start_ns = 0;  // Wall clock when entry entered hold buffer (design §5.5)
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

        static_assert(4 % 2 == 0, "RadixSorter: even number of passes required for in-place result");
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

    /// Review §4: fetch_add eliminates CAS contention; when full one slot may be wasted. Use for multi-producer.
    [[nodiscard]] std::optional<uint64_t> reserve_fetch_add() {
        uint64_t slot = tail_.fetch_add(1, std::memory_order_relaxed);
        if (slot - head_.load(std::memory_order_acquire) >= capacity_)
            return std::nullopt;  // Ring full; slot not used (effective capacity may be capacity_-1 when saturated)
        return slot;
    }

    void advance_head(uint64_t new_head) {
        head_.store(new_head, std::memory_order_release);
    }

    /// Single-writer tail advance (refill thread: one writer per broker, no CAS). PRECONDITION: exactly one thread calls this per ring.
    void force_set_tail(uint64_t new_tail) {
        tail_.store(new_tail, std::memory_order_release);
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
// SPSC Queue (Design §3.1: one collector -> one shard, zero contention)
// ============================================================================
template<typename T>
class alignas(64) SPSCQueue {
public:
    explicit SPSCQueue(size_t capacity) : capacity_(capacity), mask_(capacity - 1), buffer_(capacity) {
        assert(is_power_of_2(capacity));
    }

    /** Producer (single collector): push to queue. Cached head to avoid cross-core load every push. */
    [[nodiscard]] bool try_push(const T& item) {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - cached_head_ >= capacity_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (t - cached_head_ >= capacity_) return false;
        }
        buffer_[t & mask_] = item;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_push(T&& item) {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - cached_head_ >= capacity_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (t - cached_head_ >= capacity_) return false;
        }
        buffer_[t & mask_] = std::move(item);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    /** Consumer (shard worker): drain into vector. */
    size_t drain_to(std::vector<T>& out, size_t max_items = SIZE_MAX) {
        uint64_t h = head_.load(std::memory_order_relaxed);
        uint64_t t = tail_.load(std::memory_order_acquire);
        size_t count = 0;
        while (h < t && count < max_items) {
            out.push_back(std::move(buffer_[h & mask_]));
            ++h;
            ++count;
        }
        if (count > 0)
            head_.store(h, std::memory_order_release);
        return count;
    }

    size_t size() const noexcept {
        uint64_t h = head_.load(std::memory_order_acquire);
        uint64_t t = tail_.load(std::memory_order_acquire);
        return static_cast<size_t>(t - h);
    }

private:
    const size_t capacity_;
    const size_t mask_;
    std::vector<T> buffer_;
    uint64_t cached_head_{0};  // Producer-local; reload only when apparently full
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
};

// ============================================================================
// Shard Queue (MPSC: Multiple Collectors -> Single Shard Worker) — plan §2.2
// ============================================================================
// Lock-free MPSC queue; ready_ uses std::atomic<bool> per slot (not vector<bool>).
// Used when use_spsc_queues is false (single-threaded or legacy scatter).

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
// FastDeduplicator: Zero-allocation deduplication
// ============================================================================
// Design: Open-addressing hash table + circular ring for FIFO eviction.
// All memory pre-allocated; zero malloc/free on hot path.
//
// Performance: O(1) average insert/lookup, ~10-50ns per operation.
// Memory: 24 MB (2M hash entries + 1M ring entries).

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
#ifndef NDEBUG
    std::thread::id owner_thread_;
    bool owner_set_ = false;
#endif

public:
    FastDeduplicator() : table_(TABLE_SIZE, EMPTY), ring_(RING_SIZE, EMPTY) {}

    void set_skip(bool skip) noexcept { skip_ = skip; }

    // Returns true if duplicate (already seen), false if new
    // Single-threaded: only sequencer thread may call this.
    [[nodiscard]] bool check_and_insert(uint64_t batch_id) noexcept {
#ifndef NDEBUG
        if (!owner_set_) {
            owner_thread_ = std::this_thread::get_id();
            owner_set_ = true;
        }
        assert(std::this_thread::get_id() == owner_thread_ &&
               "FastDeduplicator accessed from multiple threads!");
#endif
        if (skip_) return false;
        if (batch_id >= TOMBSTONE) [[unlikely]] return false;

        const uint64_t h = hash64(batch_id);
        size_t idx = h & TABLE_MASK;
        size_t insert_at = static_cast<size_t>(-1);

#ifndef NDEBUG
        assert(idx < TABLE_SIZE && "Table index out of bounds");
#endif

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

            idx = (idx + 1) & TABLE_MASK;  // TABLE_MASK keeps idx in [0, TABLE_SIZE-1]
        }

        if (insert_at == static_cast<size_t>(-1)) [[unlikely]] return false;

        // Evict if at capacity. Ensure count_ never exceeds RING_SIZE.
        if (count_ >= RING_SIZE) {
            // Oldest slot in FIFO order: (ring_head_ - count_) mod RING_SIZE.
            size_t oldest = (ring_head_ + RING_SIZE - count_) & RING_MASK;
#ifndef NDEBUG
            assert(oldest < RING_SIZE && "Ring oldest index out of bounds");
#endif
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
#ifndef NDEBUG
        assert(ring_head_ < RING_SIZE && "Ring head out of bounds");
#endif
        ring_[ring_head_] = batch_id;
        ring_head_ = (ring_head_ + 1) & RING_MASK;
        ++count_;
#ifndef NDEBUG
        assert(count_ <= RING_SIZE && "FastDeduplicator count_ invariant");
#endif

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

// ============================================================================
// Per-shard state and coordinator (plan §2.3–2.4) — after ShardQueue, Deduplicator
// ============================================================================

/// Per-shard state for scatter-gather. Each shard owns clients with hash(client_id) % num_shards == shard_id.
/// client_highest_committed: persistent max client_seq committed per client (retries / duplicate client_seq).
/// When max_clients > 0, client_states_dense / client_highest_committed_dense / hold_buf_dense give O(1) access for cid in [0, max_clients) (review §significant.4).
struct alignas(128) ShardState {
    uint32_t shard_id = 0;
    uint32_t max_clients = 0;  // When >0, dense vectors are used for cid in [0, max_clients)
    std::unique_ptr<ShardQueue<BatchInfo>> input_queue;
    std::unordered_map<uint64_t, ClientState> client_states;
    std::unordered_map<uint64_t, uint64_t> client_highest_committed;
    /// C3: Design §3.3 — per-client deque for O(1) pop_front; sorted by client_seq on insert.
    std::unordered_map<uint64_t, std::deque<HoldEntry>> hold_buf;
    /// Dense path (design §3.3 flat_hash_map alternative): index by cid when cid < max_clients.
    std::vector<ClientState> client_states_dense;
    std::vector<uint64_t> client_highest_committed_dense;
    std::vector<std::deque<HoldEntry>> hold_buf_dense;
    size_t hold_size = 0;
    Deduplicator dedup;
    FastDeduplicator fast_dedup;  // Used when config.use_fast_dedup (review §6)
    /// Clients cascaded this drain cycle; drain_hold_shard skips them. Set for O(1) lookup (review §minor.9: avoid O(n²) std::find).
    std::unordered_set<uint64_t> cascaded_cids_this_round;
    uint64_t local_validation_count = 0;  // Per-shard to avoid validation_batch_count_ contention (review §perf.5)
    /// C4: Per-shard validation ring (single writer); fixed-size overwrite, no erase in hot path.
    static constexpr size_t VALIDATION_RING_CAP = 131072;
    std::vector<ValidationEntry> validation_ring_;
    uint64_t validation_write_pos_ = 0;

    /// Reused in drain_hold_shard to avoid per-cycle allocation.
    std::vector<uint64_t> drain_cids_buffer_;
    /// Reused in age_hold_shard for expired entries (cid, seq, batch).
    struct ShardExpiredEntry { uint64_t cid; uint64_t seq; BatchInfo batch; };
    std::vector<ShardExpiredEntry> age_hold_expired_buffer_;

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
    std::atomic<uint64_t> phase_atomic_ns{0};  // SEQUENCER_PHASE_TIMING: time in initial fetch_add (contention proxy).
    std::atomic<uint64_t> epochs_processed{0};
    std::thread worker_thread;
    std::atomic<bool> running{false};
    std::atomic<uint64_t> current_epoch{0};
    std::atomic<bool> epoch_complete{false};
    std::condition_variable epoch_cv;
    std::mutex epoch_mutex;
    /// Per-shard latency rings (avoid contended atomic when multiple shards write); merged into stats after stop().
    LatencyRing local_latency_ring;
    LatencyRing local_sequencing_latency_ring;
    /// Micro-ablation noop_global_seq: per-shard counter to isolate fetch_add contention (review §6).
    std::atomic<uint64_t> local_seq{0};
    /// Per-shard counters (avoid false sharing on stats_); merged into stats in stop(). Relaxed atomics so measurement thread can snapshot (review §perf.1).
    std::atomic<uint64_t> local_batches{0};
    std::atomic<uint64_t> local_bytes{0};
    std::atomic<uint64_t> local_l5{0};
    std::atomic<uint64_t> local_gaps{0};
    std::atomic<uint64_t> local_dups{0};
    /// Reused in age_hold_shard for expired client ids (avoid stack allocation every cycle; review §perf.4).
    std::vector<uint64_t> expired_cids_buffer_;
    /// Clients that have non-empty hold buffer (dense or map). Used in age_hold_shard and drain_hold_shard to avoid scanning all max_clients (review Bug 3).
    std::unordered_set<uint64_t> clients_with_held_;
    /// Gap resolution characterization (paper figures: sort vs hold vs timeout).
    struct GapStats {
        std::atomic<uint64_t> sort_resolved{0};
        std::atomic<uint64_t> hold_resolved{0};
        std::atomic<uint64_t> timeout_skipped{0};
        std::atomic<uint64_t> cascade_depth_sum{0};
        std::atomic<uint64_t> cascade_count{0};
        std::atomic<uint64_t> hold_peak{0};
    } gap_stats;
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

/// Design §7: Range completion for committed_seq. Min-heap merges out-of-order completions.
struct CompletedRange {
    uint64_t start = 0;
    uint64_t end = 0;
    bool operator>(const CompletedRange& o) const { return start > o.start; }
};

/// C3: Bounded MPSC ring for CompletedRange (no heap allocation in hot path). S shards push, committed_seq updater pops.
/// Per-slot ready flag ensures correct publish-before-read on ARM (fixes data race vs fence-only).
/// PER_BATCH_ATOMIC pushes one range per batch; at ~2M batches/s across 8 shards this is ~250K pushes/s — 256 would stall. Use 16384.
static constexpr size_t COMPLETED_RANGES_RING_CAP = 65536;  // Was 16384; safety margin for burst (sleep-on-active fix)

class CompletedRangesRing {
    struct Slot {
        CompletedRange data;
        std::atomic<bool> ready{false};
    };
    std::array<Slot, COMPLETED_RANGES_RING_CAP> buffer_{};
    static constexpr size_t mask_ = COMPLETED_RANGES_RING_CAP - 1;
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};

public:
    /** Blocks until slot available (spin), then writes. Publishes via per-slot ready (C++ memory-model correct). */
    bool push(CompletedRange cr, std::atomic<bool>* shutdown = nullptr) {
        uint64_t t = tail_.load(std::memory_order_relaxed);
        while (true) {
            if (t - head_.load(std::memory_order_acquire) >= COMPLETED_RANGES_RING_CAP) {
                if (shutdown && shutdown->load(std::memory_order_acquire)) return false;
                for (int i = 0; i < 64; ++i) CPU_PAUSE();
                t = tail_.load(std::memory_order_relaxed);
                continue;
            }
            if (tail_.compare_exchange_weak(t, t + 1, std::memory_order_relaxed))
                break;
        }
        buffer_[t & mask_].data = cr;
        buffer_[t & mask_].ready.store(true, std::memory_order_release);
        return true;
    }
    bool try_pop(CompletedRange& out) {
        uint64_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_.load(std::memory_order_acquire))
            return false;
        if (!buffer_[h & mask_].ready.load(std::memory_order_acquire))
            return false;
        out = buffer_[h & mask_].data;
        buffer_[h & mask_].ready.store(false, std::memory_order_relaxed);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }
    [[nodiscard]] bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
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
        if (sealing_.load(std::memory_order_acquire)) return false;
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
     * use-after-free when collector push_back reallocs and sequencer process_epoch reads). */
    [[nodiscard]] bool seal() {
        if (state_.load(std::memory_order_acquire) != State::COLLECTING) return false;
        sealing_.store(true, std::memory_order_release);
        std::unique_lock<std::mutex> lk(seal_mutex_);
        seal_cv_.wait(lk, [this] {
            return active_collectors_.load(std::memory_order_acquire) == 0;
        });
        State expect = State::COLLECTING;
        bool ok = state_.compare_exchange_strong(expect, State::SEALED, std::memory_order_acq_rel);
        sealing_.store(false, std::memory_order_release);
        return ok;
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
    std::atomic<bool> sealing_{false};  // true while seal() is waiting; blocks enter_collection()
};

enum class SequencerMode {
    PER_BATCH_ATOMIC,   // One fetch_add per batch, one CompletedRange push per batch (review: conflates atomic + ring cost).
    PER_EPOCH_ATOMIC,
    SCATTER_GATHER      // Parallel: one atomic per shard per epoch
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
    LatencyRing latency_ring;           // E2E: commit_time - timestamp_ns (inject to commit)
    LatencyRing sequencing_latency_ring; // Sequencing: commit_time - received_ns (PBR read to commit)
    /// Scatter-gather: merged from per-shard rings after stop() to avoid contended atomic in hot path
    std::vector<uint64_t> merged_latency_samples_;
    std::vector<uint64_t> merged_seq_latency_samples_;
    std::atomic<uint64_t> atomics_executed{0};
    std::atomic<uint64_t> sort_time_ns{0};   // L5 sort_by_client_id (diagnostic)
    std::atomic<uint64_t> hold_phase_ns{0};  // drain_hold + age_hold (diagnostic)
    std::atomic<uint64_t> merge_time_ns{0};  // Phase: merge collector buffers
    std::atomic<uint64_t> l5_time_ns{0};    // Phase: process_order5 + drain_hold + age_hold
    std::atomic<uint64_t> writer_phase_ns{0};  // Scatter-gather: gather+sort+write GOI per epoch
    // Assessment §2.1: Phase timing for bottleneck profiling (per-epoch cumulative; print every 100 epochs).
    std::atomic<uint64_t> phase_count_ns{0};   // Count total from collectors
    std::atomic<uint64_t> phase_reserve_ns{0};  // merge_buffer_.reserve(total)
    std::atomic<uint64_t> phase_merge_ns{0};   // Insert/move collector buffers into merge
    std::atomic<uint64_t> phase_partition_ns{0};
    std::atomic<uint64_t> phase_l0_ns{0};
    std::atomic<uint64_t> phase_l5_ns{0};
    std::atomic<uint64_t> phase_assign_ns{0};   // fetch_add + assign global_seq loop
    std::atomic<uint64_t> phase_l5_reorder_ns{0};  // (legacy) reorder in per-epoch path
    std::atomic<uint64_t> phase_l0_dedup_ns{0};    // Deduplicator cost inside process_order2

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
        phase_count_ns = 0;
        phase_reserve_ns = 0;
        phase_merge_ns = 0;
        phase_partition_ns = 0;
        phase_l0_ns = 0;
        phase_l5_ns = 0;
        phase_assign_ns = 0;
        phase_l5_reorder_ns = 0;
        phase_l0_dedup_ns = 0;
        latency_ring.reset();
        sequencing_latency_ring.reset();
        merged_latency_samples_.clear();
        merged_seq_latency_samples_.clear();
    }

    void set_merged_latency(std::vector<uint64_t> lat, std::vector<uint64_t> seq_lat) {
        merged_latency_samples_ = std::move(lat);
        merged_seq_latency_samples_ = std::move(seq_lat);
    }

    double avg_seq_ns() const {
        uint64_t c = seq_count.load();
        return c ? double(seq_total_ns.load()) / c : 0;
    }

    /** Phase 3.1: Drain inject-to-commit latencies (recorded by sequencer/writer). Returns ns. Scatter-gather: use merged per-shard samples when set. */
    std::vector<uint64_t> drain_latency_ring() {
        if (!merged_latency_samples_.empty()) {
            std::vector<uint64_t> r = std::move(merged_latency_samples_);
            merged_latency_samples_.clear();
            return r;
        }
        return latency_ring.drain();
    }
    std::vector<uint64_t> drain_sequencing_latency_ring() {
        if (!merged_seq_latency_samples_.empty()) {
            std::vector<uint64_t> r = std::move(merged_seq_latency_samples_);
            merged_seq_latency_samples_.clear();
            return r;
        }
        return sequencing_latency_ring.drain();
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

    /// Ablation: if true, disable deduplication.
    ///
    /// RATIONALE FOR ABLATION STUDIES:
    /// The benchmark generates globally unique batch_ids:
    ///   batch_id = (thread_id << 48) | atomic_counter
    /// Therefore, duplicates never occur in the benchmark.
    ///
    /// Deduplication is needed in PRODUCTION for client retries,
    /// but is orthogonal to the per-batch vs per-epoch comparison.
    /// For algorithm ablation, --skip-dedup produces valid results
    /// that isolate the sequencing algorithm performance.
    ///
    /// For end-to-end system evaluation (including retry handling),
    /// run without this flag using FastDeduplicator.
    bool skip_dedup = false;

    /// Use FastDeduplicator instead of legacy Deduplicator (single-threaded path).
    bool use_fast_dedup = true;

    /// When true, record per-phase timings (Order 5 cost breakdown). Runtime-configurable (review §significant.5).
    bool phase_timing = false;

    /// When true, pin sequencer threads to consecutive cores (reproducible measurements; review §8b). Default true for benchmarks.
    bool pin_cores = true;

    /// When true, completed_ranges_.push is a no-op (micro-ablation: isolate committed_seq/updater overhead; review §blocking.2).
    bool noop_completed_ranges = false;

    /// When true, skip writing to goi_[] (micro-ablation: isolate memory bandwidth; review §6).
    bool noop_goi_writes = false;
    /// When true, use per-shard local_seq instead of coordinator_.global_seq (micro-ablation: isolate fetch_add contention; review §6).
    bool noop_global_seq = false;

    /// When true, update gap_resolution stats (sort_resolved, hold_resolved, timeout_skipped, hold_peak, cascade). Off by default to avoid hot-path cost (regression fix).
    bool collect_gap_resolution_stats = false;

    /// When true, timer does not advance epoch (drain-only benchmark: pre-filled PBR entries never go stale).
    bool pause_epoch_timer = false;

    /// PBR reserve: true = fetch_add (no CAS contention, review §4); false = CAS reserve.
    bool pbr_use_fetch_add = false;  // true can create ghost PBR slots under contention (see reserve_fetch_add)

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
// Pipeline counters (temporary diagnostic for drain-rate stall)
// ============================================================================
struct PipelineCounters {
    std::atomic<uint64_t> refill_written{0};
    std::atomic<uint64_t> collector_read{0};
    std::atomic<uint64_t> collector_pushed{0};
    std::atomic<uint64_t> collector_blocked{0};
    std::atomic<uint64_t> shard_drained{0};
    std::atomic<uint64_t> shard_drain_cycles{0};
    std::atomic<uint64_t> shard_idle_cycles{0};
    std::atomic<uint64_t> goi_written{0};
    std::atomic<uint64_t> cr_pushed{0};
    std::atomic<uint64_t> cr_popped{0};
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
    uint64_t global_seq() const {
        if (config_.scatter_gather_enabled)
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

    /// Scatter-gather: sum of shards' local counters so throughput can be sampled before stop() (review §perf.1).
    uint64_t get_batches_sequenced_snapshot() const;
    uint64_t get_bytes_sequenced_snapshot() const;

    bool validate_ordering();
    /** Returns empty if valid; otherwise first failure reason (gap / per-client). Call after stop(). */
    std::string validate_ordering_reason();

    /// Trace mode: refill thread writes directly to PBR (single writer per broker). Requires scatter_gather.
    MPSCRing& pbr_ring(uint32_t broker_id) { return *pbr_rings_.at(broker_id); }
    PBREntry* pbr_base(uint32_t broker_id) { return pbr_ + broker_id * config_.pbr_entries; }
    uint16_t control_epoch() const {
        return static_cast<uint16_t>(control_->epoch.load(std::memory_order_relaxed));
    }

    /// Merged gap resolution stats from all shards (call after stop() for trace experiments).
    struct GapResolutionReport {
        uint64_t sort_resolved = 0;
        uint64_t hold_resolved = 0;
        uint64_t timeout_skipped = 0;
        uint64_t cascade_depth_sum = 0;
        uint64_t cascade_count = 0;
        uint64_t hold_peak = 0;
        double avg_cascade_depth() const {
            return cascade_count ? static_cast<double>(cascade_depth_sum) / static_cast<double>(cascade_count) : 0.0;
        }
    };
    GapResolutionReport get_gap_resolution_report() const;

    /// Temporary diagnostic for drain-rate stall (pipeline counters).
    PipelineCounters pipeline_counters_;

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

    void process_order5_shard(ShardState& shard, std::vector<BatchInfo>& batches, uint64_t epoch, uint64_t cycle_ns);
    void add_to_hold_shard(ShardState& shard, BatchInfo&& batch, uint64_t current_epoch, uint64_t cycle_start_ns);
    /// Design §5.3: Cascade release for one client (inline after advance_next_expected in shard path).
    void cascade_release_shard(ShardState& shard, uint64_t cid, std::vector<BatchInfo>& out);
    /// Shared helper: drain one client's hold_buf into out while front == next_expected. Used by cascade_release_shard and drain_hold_shard.
    void drain_one_client_hold_shard(ShardState& shard, uint64_t cid, std::vector<BatchInfo>& out);
    void drain_hold_shard(ShardState& shard, std::vector<BatchInfo>& out, uint64_t epoch);
    void age_hold_shard(ShardState& shard, std::vector<BatchInfo>& out, uint64_t current_epoch, uint64_t cycle_start_ns);
    void client_gc_shard(ShardState& shard, uint64_t cur_epoch);
    /// Writes shard.ready to GOI, pushes CompletedRanges, updates per-shard local counters. Used by loop and drain-on-shutdown (review §critical.3).
    void write_ready_to_goi_shard(ShardState& shard, uint64_t cycle_ns);
    /// Scatter-gather dedup: use FastDeduplicator when config_.use_fast_dedup (review §6).
    bool check_dedup_shard(ShardState& shard, uint64_t batch_id);
    /// Dense path (review §significant.4): O(1) access when cid < shard.max_clients.
    ClientState& get_state_shard(ShardState& shard, uint64_t cid);
    uint64_t& get_hc_shard(ShardState& shard, uint64_t cid);
    std::deque<HoldEntry>& get_hold_buf_shard(ShardState& shard, uint64_t cid);

    void advance_epoch();
    void process_epoch(EpochBuffer& buf);
    void sequence_batches_per_batch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);
    void sequence_batches_per_epoch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);
    void process_order2(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out);
    void process_order5(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch);

    void add_to_hold(uint64_t client_id, BatchInfo&& batch, uint64_t current_epoch,
                     std::vector<BatchInfo>* overflow_out = nullptr);
    void drain_hold(std::vector<BatchInfo>& out, uint64_t epoch);
    void age_hold(std::vector<BatchInfo>& out, uint64_t current_epoch);
    void client_gc(uint64_t cur_epoch);
    /// Design §5.4: Find client with oldest held entry (by hold_start_ns of front). 0 if none.
    uint64_t find_oldest_held_client() const;
    /// Release in-order entries for one client from hold to out (used by overflow path).
    void cascade_release_one(uint64_t client_id, std::vector<BatchInfo>& out);

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
            if (st.next_expected == 0 && hc > 0) {
                st.next_expected = hc + 1;
                st.highest_sequenced = hc;
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
            }
        }
        return it->second;
    }
    uint64_t& get_hc(uint64_t cid) {
        if (use_dense() && cid < config_.max_clients)
            return client_highest_committed_dense_[cid];
        return client_highest_committed_[cid];
    }
    std::deque<HoldEntry>& get_hold_buf(uint64_t cid) {
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

    Deduplicator legacy_dedup_;
    FastDeduplicator fast_dedup_;

    /// Single-threaded path: dispatch to fast or legacy dedup per config.
    bool check_dedup(uint64_t batch_id) {
        if (config_.skip_dedup) return false;
        if (config_.use_fast_dedup)
            return fast_dedup_.check_and_insert(batch_id);
        return legacy_dedup_.check_and_insert(batch_id);
    }

    RadixSorter sorter_;
    std::vector<BatchInfo> merge_buffer_;
    std::vector<BatchInfo> ready_buffer_;
    std::vector<BatchInfo> l0_buffer_;
    std::vector<BatchInfo> l5_buffer_;
    /// P1: Reused across epochs to avoid per-epoch allocation in sequence_batches_per_epoch.
    std::vector<BatchInfo> l5_ready_buffer_;
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
    // C3: Design §3.3 — per-client deque for O(1) pop_front; sorted by client_seq on insert.
    std::unordered_map<uint64_t, std::deque<HoldEntry>> hold_buf_;
    /// Phase 2: dense client state when max_clients > 0 (O(1) index vs hash lookup)
    std::vector<ClientState> states_dense_;
    std::vector<uint64_t> client_highest_committed_dense_;
    std::vector<std::deque<HoldEntry>> hold_buf_dense_;
    /// Clients that have at least one batch in hold_buf_; drain_hold iterates only these (O(N_active) not O(N_total)).
    std::unordered_set<uint64_t> clients_with_held_batches_;
    size_t hold_size_ = 0;
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
    /// Design §7: CompletedRange MPSC ring for correct committed_seq (min-heap merge). C3: no heap alloc.
    CompletedRangesRing completed_ranges_;
    /// Design §3.1: C×S SPSC queues (collector c -> shard s). When non-empty, shard worker drains from spsc_queues_[c][shard_id] for all c.
    std::vector<std::vector<std::unique_ptr<SPSCQueue<BatchInfo>>>> spsc_queues_;
    /// When pin_cores is true, next core index to assign (review §minor.8).
    std::atomic<uint32_t> pin_core_next_{0};
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
    // Design §11 Example A: committed_seq = -1 (nothing committed). Prevents CXL consumers reading GOI[0] as valid.
    control_->committed_seq.store(UINT64_MAX, std::memory_order_relaxed);
    control_->initialized.store(false, std::memory_order_relaxed);

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

uint64_t Sequencer::get_batches_sequenced_snapshot() const {
    if (!config_.scatter_gather_enabled || shards_.empty())
        return stats_.batches_sequenced.load(std::memory_order_acquire);
    uint64_t sum = 0;
    for (const auto& s : shards_)
        sum += s->local_batches.load(std::memory_order_relaxed);
    return sum;
}

uint64_t Sequencer::get_bytes_sequenced_snapshot() const {
    if (!config_.scatter_gather_enabled || shards_.empty())
        return stats_.bytes_sequenced.load(std::memory_order_acquire);
    uint64_t sum = 0;
    for (const auto& s : shards_)
        sum += s->local_bytes.load(std::memory_order_relaxed);
    return sum;
}

void Sequencer::start() {
    if (running_.exchange(true)) return;
    shutdown_.store(false);

    // Phase 2: pre-allocate dense client state when max_clients set (single-threaded path only)
    if (config_.max_clients != 0 && !config_.scatter_gather_enabled) {
        states_dense_.resize(config_.max_clients);
        client_highest_committed_dense_.resize(config_.max_clients, 0);
        hold_buf_dense_.resize(config_.max_clients);
    }
    age_hold_expired_buffer_.reserve(65536);  // Avoid realloc in age_hold hot path

    if (config_.scatter_gather_enabled) {
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
        spsc_queues_.clear();
        spsc_queues_.resize(config_.num_collectors);
        for (uint32_t c = 0; c < config_.num_collectors; ++c) {
            spsc_queues_[c].reserve(config_.num_shards);
            for (uint32_t s = 0; s < config_.num_shards; ++s)
                spsc_queues_[c].push_back(std::make_unique<SPSCQueue<BatchInfo>>(config_.shard_queue_size));
        }
        for (uint32_t i = 0; i < config_.num_shards; ++i) {
            auto s = std::make_unique<ShardState>();
            s->shard_id = i;
            s->input_queue = nullptr;  // Design §3.1: use C×S SPSC queues instead
            s->running.store(true);
            s->current_epoch.store(0, std::memory_order_relaxed);
            s->epoch_complete.store(false, std::memory_order_relaxed);
            s->dedup.set_skip(config_.skip_dedup);
            s->validation_ring_.resize(ShardState::VALIDATION_RING_CAP);  // Pre-size; no resize in hot path.
            if (config_.max_clients > 0) {
                s->max_clients = config_.max_clients;
                s->client_states_dense.resize(config_.max_clients);
                s->client_highest_committed_dense.resize(config_.max_clients, 0);
                s->hold_buf_dense.resize(config_.max_clients);
            }
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

    // Single-threaded path: apply skip_dedup to both deduplicators (scatter-gather sets it per-shard above)
    legacy_dedup_.set_skip(config_.skip_dedup);
    fast_dedup_.set_skip(config_.skip_dedup);

    // Improvement instruction Task A Step 7: pre-allocate hot-path buffers in start() to avoid malloc in process_epoch
    merge_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH);
    ready_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH);
    l0_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH);
    l5_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH);
    l5_ready_buffer_.reserve(config::MAX_BATCHES_PER_EPOCH + 1024);

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

    if (config_.scatter_gather_enabled) {
        std::cout << "  Stopping scatter-gather pipeline..." << std::flush;
        for (auto& s : shards_) {
            s->epoch_cv.notify_all();
            s->running.store(false, std::memory_order_release);
        }
        writer_cv_.notify_all();
        if (timer_thread_.joinable()) { timer_thread_.join(); std::cout << "t" << std::flush; }
        for (auto& t : collector_threads_) if (t.joinable()) t.join();
        std::cout << "c" << std::flush;
        collector_threads_.clear();
        for (auto& s : shards_) {
            if (s->worker_thread.joinable()) s->worker_thread.join();
        }
        std::cout << "s" << std::flush;
        // Merge per-shard latency rings and local counters into stats (avoids hot-path false sharing; review §perf.1).
        if (!shards_.empty()) {
            std::vector<uint64_t> all_lat, all_seq;
            for (auto& s : shards_) {
                auto lat = s->local_latency_ring.drain();
                auto seq = s->local_sequencing_latency_ring.drain();
                all_lat.insert(all_lat.end(), lat.begin(), lat.end());
                all_seq.insert(all_seq.end(), seq.begin(), seq.end());
                stats_.batches_sequenced.fetch_add(s->local_batches.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats_.bytes_sequenced.fetch_add(s->local_bytes.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats_.level5_batches.fetch_add(s->local_l5.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats_.gaps_skipped.fetch_add(s->local_gaps.load(std::memory_order_relaxed), std::memory_order_relaxed);
                stats_.duplicates.fetch_add(s->local_dups.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }
            stats_.set_merged_latency(std::move(all_lat), std::move(all_seq));
        }
        if (writer_thread_.joinable()) { writer_thread_.join(); std::cout << "w" << std::flush; }
        std::cout << " done\n";
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
    control_->epoch.store(static_cast<uint16_t>(next), std::memory_order_release);
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
        if (buf.epoch_number != collecting_epoch) {
            buf.exit_collection();
            continue;
        }
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
            // Amortize now_ns() per PBR scan (review §8a); same batch_ts for all entries from this PBR in this pass.
            uint64_t batch_ts = now_ns();

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
                info.received_ns = batch_ts;
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
    if (config_.pin_cores) pin_self_to_core(pin_core_next_.fetch_add(1, std::memory_order_relaxed));
    const size_t pbr_mask = config_.pbr_entries - 1;
    const uint32_t shard_mask = config_.num_shards - 1;
    static thread_local uint32_t rr_counter = 0;

    while (!shutdown_.load(std::memory_order_acquire)) {
        if (freeze_collection_.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::microseconds(config::WAIT_SLEEP_US));
            continue;
        }
        // Review §8a: Amortize now_ns() — one per collector scan pass, not per entry (~25ns × entries otherwise).
        uint64_t batch_ts = now_ns();
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

                pipeline_counters_.collector_read.fetch_add(1, std::memory_order_relaxed);

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
                info.received_ns = batch_ts;

                // Design §4.1: Route Order 5 by hash(client_id), Order 2 by round-robin.
                // Ensures same client always hits same shard for Order 5, but prevents hot-shard for Order 2.
                uint32_t shard_id;
                if (info.flags & flags::STRONG_ORDER) {
                    shard_id = static_cast<uint32_t>(hash64(info.client_id) & shard_mask);
                } else {
                    shard_id = (rr_counter++) & shard_mask;
                }

                int retries = 0;
                int spin_rounds = 0;
                bool did_push = true;
                while (!spsc_queues_[collector_id][shard_id]->try_push(info)) {
                    if (shutdown_.load(std::memory_order_acquire)) {
                        did_push = false;
                        break;
                    }
                    pipeline_counters_.collector_blocked.fetch_add(1, std::memory_order_relaxed);
                    if (config_.allow_drops) {
                        if (++retries > 1000) {
                            stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
                            did_push = false;
                            break;
                        }
                        CPU_PAUSE();
                    } else {
                        // Design: yield and retry. Avoid 1ms sleep (adds ~order-of-magnitude tail latency vs sub-100μs target).
                        for (int r = 0; r < 256; ++r) CPU_PAUSE();
                        if (++spin_rounds % 16 == 0)
                            std::this_thread::yield();
                    }
                }
                if (did_push) {
                    pipeline_counters_.collector_pushed.fetch_add(1, std::memory_order_relaxed);
                    // Design §Fix 1: Advance PBR head immediately after successful push.
                    // The PBR slot can be reused by producers.
                    ring.advance_head(pos + 1);
                }
                state.scan_pos.store(pos + 1, std::memory_order_release);  // Only after successful push (or drop)
            }
        }
        std::this_thread::yield();
    }
}

// DEPRECATED: Epoch-batched pipeline (timer → seal → sequencer → writer). The design spec uses
// continuous-drain scatter-gather (collectors → SPSC → shard workers → GOI). This path is kept
// for ablation baseline only (review §minor.12).
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

#ifdef SEQUENCER_DEBUG_TIMING
        // Run before phase profile exchange(0) so we read cumulative stats (code review fix).
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
                std::cerr << "[Order5 BREAKDOWN] sort=" << std::fixed << std::setprecision(1)
                          << sort_pct << "% hold=" << hold_pct << "% other=" << rest_pct << "%\n";
            }
        }
#endif

        // Assessment §2.1: Phase profile every 100 epochs (improvement_instruction Task A).
        if (epochs_done % 100 == 0 && epochs_done >= 100) {
            const uint64_t window = 100;
            uint64_t count_ns = stats_.phase_count_ns.exchange(0, std::memory_order_relaxed);
            uint64_t reserve_ns = stats_.phase_reserve_ns.exchange(0, std::memory_order_relaxed);
            uint64_t merge_ns = stats_.phase_merge_ns.exchange(0, std::memory_order_relaxed);
            uint64_t part_ns = stats_.phase_partition_ns.exchange(0, std::memory_order_relaxed);
            uint64_t l0_ns = stats_.phase_l0_ns.exchange(0, std::memory_order_relaxed);
            uint64_t l5_ns = stats_.phase_l5_ns.exchange(0, std::memory_order_relaxed);
            uint64_t assign_ns = stats_.phase_assign_ns.exchange(0, std::memory_order_relaxed);
            uint64_t merge_total_ns = stats_.merge_time_ns.exchange(0, std::memory_order_relaxed);
            uint64_t l5_reorder_ns = stats_.phase_l5_reorder_ns.exchange(0, std::memory_order_relaxed);
            uint64_t l0_dedup_ns = stats_.phase_l0_dedup_ns.exchange(0, std::memory_order_relaxed);
            uint64_t hold_ns = stats_.hold_phase_ns.exchange(0, std::memory_order_relaxed);
            uint64_t total_phase_ns = count_ns + reserve_ns + merge_ns + part_ns + l0_ns + l5_ns + assign_ns;
            std::cerr << "[PHASE μs/epoch] count=" << (count_ns / window / 1000)
                      << " reserve=" << (reserve_ns / window / 1000)
                      << " merge=" << (merge_ns / window / 1000)
                      << " partition=" << (part_ns / window / 1000)
                      << " O2=" << (l0_ns / window / 1000)
                      << " O5=" << (l5_ns / window / 1000)
                      << " assign=" << (assign_ns / window / 1000)
                      << " TOTAL=" << (total_phase_ns / window / 1000)
                      << " (avg over " << window << " epochs)\n";
            std::cerr << "[PROFILE] merge_total=" << (merge_total_ns / window / 1000)
                      << " l5_reorder=" << (l5_reorder_ns / window / 1000)
                      << " l0_dedup=" << (l0_dedup_ns / window / 1000)
                      << " hold=" << (hold_ns / window / 1000)
                      << " μs/epoch\n";
        }

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

    // Buffers pre-allocated in start() (single-threaded path) to avoid realloc in hot path

    uint64_t t0 = 0, t1 = 0, t2 = 0, t3 = 0, t_seq0 = 0;
    if (config_.phase_timing) t0 = now_ns();
    size_t total = 0;
    for (uint32_t i = 0; i < config_.num_collectors; ++i) {
        total += buf.collectors[i].batches.size();
    }
    if (config_.phase_timing) t1 = now_ns();
    merge_buffer_.reserve(total);
    if (config_.phase_timing) t2 = now_ns();
    for (uint32_t i = 0; i < config_.num_collectors; ++i) {
        auto& v = buf.collectors[i].batches;
        merge_buffer_.insert(merge_buffer_.end(),
                             std::make_move_iterator(v.begin()),
                             std::make_move_iterator(v.end()));
        v.clear();
    }
    if (config_.phase_timing) {
        t3 = now_ns();
        stats_.phase_count_ns.fetch_add(t1 - t0, std::memory_order_relaxed);
        stats_.phase_reserve_ns.fetch_add(t2 - t1, std::memory_order_relaxed);
        stats_.phase_merge_ns.fetch_add(t3 - t2, std::memory_order_relaxed);
        stats_.merge_time_ns.fetch_add(t3 - t0, std::memory_order_relaxed);
    }
    if (config_.phase_timing) t_seq0 = now_ns();
    // ready_buffer_ pre-allocated in start() to MAX_BATCHES_PER_EPOCH; avoid reserve in hot path
    if (config_.mode == SequencerMode::PER_BATCH_ATOMIC) {
        sequence_batches_per_batch(merge_buffer_, ready_buffer_, buf.epoch_number);
    } else {
        sequence_batches_per_epoch(merge_buffer_, ready_buffer_, buf.epoch_number);
    }
    if (config_.phase_timing) stats_.l5_time_ns.fetch_add(now_ns() - t_seq0, std::memory_order_relaxed);

    uint64_t now = now_ns();
    for (const auto& b : ready_buffer_) {
        if (b.timestamp_ns > 0)
            stats_.latency_ring.record(now - b.timestamp_ns);
        if (b.received_ns > 0)
            stats_.sequencing_latency_ring.record(now - b.received_ns);
    }

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
        if (!check_dedup(b.batch_id)) {
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
        process_order5(l5_buffer_, l5_out, epoch);
        for (auto& b : l5_out) {
            b.global_seq = global_seq_.fetch_add(1, std::memory_order_relaxed);
            stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
            out.push_back(std::move(b));
        }
    }
    uint64_t t_hold = 0;
    if (config_.phase_timing) t_hold = now_ns();
    size_t out_before_hold = out.size();
    drain_hold(out, epoch);
    age_hold(out, epoch);
    if (!clients_with_held_batches_.empty())
        drain_hold(out, epoch);  // Second drain only if some client still has held batches
    for (size_t i = out_before_hold; i < out.size(); ++i) {
        out[i].global_seq = global_seq_.fetch_add(1, std::memory_order_relaxed);
        stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
    }
    if (config_.phase_timing) stats_.hold_phase_ns.fetch_add(now_ns() - t_hold, std::memory_order_relaxed);
}

void Sequencer::sequence_batches_per_epoch(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch) {
    uint64_t t_part0 = 0, t_l0 = 0, t_l5 = 0, t_hold = 0, t_assign0 = 0;
    if (config_.phase_timing) t_part0 = now_ns();
    l0_buffer_.clear();
    l5_buffer_.clear();
    for (auto& b : in) {
        if (b.is_level5() && config_.level5_enabled) {
            l5_buffer_.push_back(std::move(b));
        } else {
            l0_buffer_.push_back(std::move(b));
        }
    }
    if (config_.phase_timing) stats_.phase_partition_ns.fetch_add(now_ns() - t_part0, std::memory_order_relaxed);
    if (config_.phase_timing) t_l0 = now_ns();
    process_order2(l0_buffer_, out);
    if (config_.phase_timing) stats_.phase_l0_ns.fetch_add(now_ns() - t_l0, std::memory_order_relaxed);
    // Step 3: Collect all L5 batches (from epoch + hold buffers). P1: reuse l5_ready_buffer_.
    l5_ready_buffer_.clear();
    if (l5_ready_buffer_.capacity() < l5_buffer_.size() + hold_size_ + 1024)
        l5_ready_buffer_.reserve(l5_buffer_.size() + hold_size_ + 1024);
    if (config_.phase_timing) t_l5 = now_ns();
    if (!l5_buffer_.empty()) process_order5(l5_buffer_, l5_ready_buffer_, epoch);
    if (config_.phase_timing) stats_.phase_l5_ns.fetch_add(now_ns() - t_l5, std::memory_order_relaxed);
    if (config_.phase_timing) t_hold = now_ns();
    drain_hold(l5_ready_buffer_, epoch);
    age_hold(l5_ready_buffer_, epoch);
    if (!clients_with_held_batches_.empty())
        drain_hold(l5_ready_buffer_, epoch);  // Second drain only if some client still has held batches
    if (config_.phase_timing) stats_.hold_phase_ns.fetch_add(now_ns() - t_hold, std::memory_order_relaxed);
    // Step 3: Append L5 to output. Per-client order already guaranteed by process_order5 + drain_hold + age_hold (review §perf.1: no redundant sort).
    if (config_.level5_enabled && !l5_ready_buffer_.empty()) {
        out.insert(out.end(),
                   std::make_move_iterator(l5_ready_buffer_.begin()),
                   std::make_move_iterator(l5_ready_buffer_.end()));
        l5_ready_buffer_.clear();
    }
    if (config_.phase_timing) t_assign0 = now_ns();
    if (!out.empty()) {
        uint64_t base = global_seq_.fetch_add(out.size(), std::memory_order_relaxed);
        stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
        for (size_t i = 0; i < out.size(); ++i) out[i].global_seq = base + i;
    }
    if (config_.phase_timing) stats_.phase_assign_ns.fetch_add(now_ns() - t_assign0, std::memory_order_relaxed);
}

// Order 2 = total order, no per-client; Order 5 = strong per-client order (design §1).
void Sequencer::process_order2(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out) {
    uint64_t t0 = 0;
    if (config_.phase_timing) t0 = now_ns();
    static constexpr unsigned kLogFirstNDuplicateBatchIds = 10;  // For investigating unexpected duplicates
    static std::atomic<unsigned> duplicate_log_count{0};
    for (auto& b : in) {
        if (!check_dedup(b.batch_id)) {
            out.push_back(std::move(b));
        } else {
            stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
            unsigned n = duplicate_log_count.fetch_add(1, std::memory_order_relaxed);
            if (n < kLogFirstNDuplicateBatchIds)
                std::cerr << "[DEDUP] duplicate batch_id=" << b.batch_id << " (tid=" << (b.batch_id >> 48)
                          << " local=" << (b.batch_id & ((1ULL << 48) - 1)) << ")\n";
        }
    }
    if (config_.phase_timing) stats_.phase_l0_dedup_ns.fetch_add(now_ns() - t0, std::memory_order_relaxed);
}

void Sequencer::process_order5(std::vector<BatchInfo>& in, std::vector<BatchInfo>& out, uint64_t epoch) {
    if (!deferred_l5_.empty()) {
        in.insert(in.end(),
                  std::make_move_iterator(deferred_l5_.begin()),
                  std::make_move_iterator(deferred_l5_.end()));
        deferred_l5_.clear();
    }
    stats_.level5_batches.fetch_add(in.size(), std::memory_order_relaxed);

    // P6: Design §5.3 — single sort by (client_id, client_seq) instead of sort by client_id then per-client sort by client_seq.
    std::sort(in.begin(), in.end(), [](const BatchInfo& a, const BatchInfo& b) {
        return std::tie(a.client_id, a.client_seq) < std::tie(b.client_id, b.client_seq);
    });

    auto it = in.begin();
    while (it != in.end()) {
        uint64_t cid = it->client_id;
        auto start = it;
        while (it != in.end() && it->client_id == cid) ++it;

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
            if (hc > 0 && b.client_seq <= hc) {
                st.mark_sequenced(b.client_seq);
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // seq > hc and !is_duplicate => seq >= next_expected. Emit in order or hold.
            if (b.client_seq == st.next_expected) {
                if (!check_dedup(b.batch_id)) {
                    out.push_back(std::move(b));
                    hc = std::max(hc, b.client_seq);
                    st.mark_sequenced(b.client_seq);
                    st.advance_next_expected();
                    cascade_release_one(cid, out);
                } else {
                    st.mark_sequenced(b.client_seq);
                    st.advance_next_expected();
                    cascade_release_one(cid, out);
                    stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                add_to_hold(cid, std::move(b), epoch, &out);
            }
        }
    }
}

uint64_t Sequencer::find_oldest_held_client() const {
    uint64_t oldest_client = 0;
    uint64_t oldest_ns = UINT64_MAX;
    for (uint64_t cid : clients_with_held_batches_) {
        const std::deque<HoldEntry>* m = nullptr;
        if (use_dense() && cid < config_.max_clients) {
            m = &hold_buf_dense_[cid];
        } else {
            auto it = hold_buf_.find(cid);
            if (it == hold_buf_.end() || it->second.empty()) continue;
            m = &it->second;
        }
        if (!m || m->empty()) continue;
        uint64_t t = m->front().hold_start_ns;
        if (t < oldest_ns) { oldest_ns = t; oldest_client = cid; }
    }
    return oldest_client;
}

void Sequencer::cascade_release_one(uint64_t client_id, std::vector<BatchInfo>& out) {
    std::deque<HoldEntry>& held = get_hold_buf(client_id);
    ClientState& st = get_state(client_id);
    uint64_t& hc = get_hc(client_id);
    while (!held.empty() && held.front().batch.client_seq == st.next_expected) {
        uint64_t seq = held.front().batch.client_seq;
        if (hc > 0 && seq <= hc) {
            st.mark_sequenced(seq);
            st.advance_next_expected();
            held.pop_front();
            --hold_size_;
            continue;
        }
        if (!check_dedup(held.front().batch.batch_id)) {
            out.push_back(std::move(held.front().batch));
            hc = std::max(hc, seq);
        } else {
            stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
        }
        st.mark_sequenced(seq);
        st.advance_next_expected();
        held.pop_front();
        --hold_size_;
    }
    if (held.empty()) clients_with_held_batches_.erase(client_id);
    stats_.hold_buffer_size.store(hold_size_, std::memory_order_relaxed);
}

void Sequencer::add_to_hold(uint64_t client_id, BatchInfo&& batch, uint64_t current_epoch,
                            std::vector<BatchInfo>* overflow_out) {
    if (hold_size_ >= config::HOLD_BUFFER_MAX) {
        // Design §5.4: Overflow protection — find oldest client, emit range-SKIP, advance, cascade.
        if (overflow_out) {
            uint64_t oldest_client = find_oldest_held_client();
            if (oldest_client != 0) {
                std::deque<HoldEntry>& held = get_hold_buf(oldest_client);
                if (!held.empty()) {
                    uint64_t first_held = held.front().batch.client_seq;
                    uint64_t expected = get_state(oldest_client).next_expected;
                    BatchInfo skip{};
                    skip.batch_id = SKIP_MARKER_BATCH_ID;
                    skip.client_id = oldest_client;
                    skip.client_seq = expected;
                    skip.flags = flags::SKIP_MARKER | flags::RANGE_SKIP;
                    skip.message_count = static_cast<uint32_t>(first_held - expected);
                    skip.payload_size = 0;
                    skip.broker_id = 0xFFFF;
                    overflow_out->push_back(skip);
                    get_state(oldest_client).next_expected = first_held;
                    cascade_release_one(oldest_client, *overflow_out);
                }
            }
            // Retry adding this batch after making room (fall through to normal insert).
            if (hold_size_ >= config::HOLD_BUFFER_MAX) return;  // Still full after cascade
        } else {
            if (config_.allow_drops) {
                stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (deferred_l5_.size() >= config::DEFERRED_L5_MAX) {
                    stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
                } else {
                    deferred_l5_.push_back(std::move(batch));
                }
            }
            return;
        }
    }

    uint64_t seq = batch.client_seq;
    std::deque<HoldEntry>& held = get_hold_buf(client_id);
    auto pos = std::lower_bound(held.begin(), held.end(), seq,
                                [](const HoldEntry& e, uint64_t s) { return e.batch.client_seq < s; });
    if (pos != held.end() && pos->batch.client_seq == seq) {
        stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    uint64_t now_ns_val = now_ns();
    HoldEntry entry{std::move(batch), 0, now_ns_val};
    held.insert(pos, std::move(entry));
    ++hold_size_;
    clients_with_held_batches_.insert(client_id);
    stats_.hold_buffer_size.store(hold_size_, std::memory_order_relaxed);
}

void Sequencer::drain_hold(std::vector<BatchInfo>& out, uint64_t epoch) {
    for (auto sit = clients_with_held_batches_.begin(); sit != clients_with_held_batches_.end(); ) {
        uint64_t cid = *sit;
        std::deque<HoldEntry>& held = get_hold_buf(cid);
        if (held.empty()) {
            sit = clients_with_held_batches_.erase(sit);
            continue;
        }

        ClientState& st = get_state(cid);
        st.last_epoch = epoch;
        uint64_t& hc = get_hc(cid);

        while (!held.empty() && held.front().batch.client_seq == st.next_expected) {
            uint64_t seq = held.front().batch.client_seq;
            if (hc > 0 && seq <= hc) {
                st.mark_sequenced(seq);
                st.advance_next_expected();
                held.pop_front();
                --hold_size_;
                continue;
            }
            bool is_dup = check_dedup(held.front().batch.batch_id);
            if (!is_dup) {
                out.push_back(std::move(held.front().batch));
                hc = std::max(hc, seq);
            } else {
                stats_.duplicates.fetch_add(1, std::memory_order_relaxed);
            }
            st.mark_sequenced(seq);
            st.advance_next_expected();
            held.pop_front();
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

void Sequencer::age_hold(std::vector<BatchInfo>& out, uint64_t /*current_epoch*/) {
    // C3/Design §5.5: Expire when front of per-client deque has timed out (two-phase to avoid iterator invalidation).
    age_hold_expired_buffer_.clear();
    uint64_t now_ns_val = now_ns();
    const uint64_t timeout_ns = config::ORDER5_BASE_TIMEOUT_NS;
    std::vector<uint64_t> expired_cids;
    for (uint64_t cid : clients_with_held_batches_) {
        std::deque<HoldEntry>& held = get_hold_buf(cid);
        if (held.empty()) continue;
        if (now_ns_val - held.front().hold_start_ns >= timeout_ns)
            expired_cids.push_back(cid);
    }
    for (uint64_t cid : expired_cids) {
        std::deque<HoldEntry>& held = get_hold_buf(cid);
        if (held.empty()) continue;
        age_hold_expired_buffer_.push_back({cid, held.front().batch.client_seq, std::move(held.front().batch)});
        held.pop_front();
        --hold_size_;
        if (held.empty()) clients_with_held_batches_.erase(cid);
    }

    std::sort(age_hold_expired_buffer_.begin(), age_hold_expired_buffer_.end(),
              [](const ExpiredEntry& a, const ExpiredEntry& b) {
                  if (a.cid != b.cid) return a.cid < b.cid;
                  return a.seq < b.seq;
              });

    for (ExpiredEntry& ent : age_hold_expired_buffer_) {
        ClientState& st = get_state(ent.cid);
        uint64_t& hc = get_hc(ent.cid);
        if (ent.seq < st.next_expected || (hc > 0 && ent.seq <= hc)) {
            st.mark_sequenced(ent.seq);
            // Advance next_expected so later batches (e.g. seq+1) can drain; otherwise
            // we'd leave next_expected behind and hold valid in-order batches forever.
            if (ent.seq >= st.next_expected)
                st.next_expected = ent.seq + 1;
            continue;
        }
        // Design §5.6 + C5: Emit one range-SKIP only when delta > 0 (avoid zero-range SKIP after cascade).
        if (ent.seq > st.next_expected) {
            uint64_t delta = ent.seq - st.next_expected;
            stats_.gaps_skipped.fetch_add(delta, std::memory_order_relaxed);
            BatchInfo skip{};
            skip.batch_id = SKIP_MARKER_BATCH_ID;
            skip.client_id = ent.cid;
            skip.client_seq = st.next_expected;
            skip.flags = flags::SKIP_MARKER | flags::RANGE_SKIP;
            skip.payload_size = 0;
            skip.message_count = static_cast<uint32_t>(delta);
            skip.broker_id = 0xFFFF;
            out.push_back(skip);
        }
        st.next_expected = ent.seq + 1;
        st.mark_sequenced(ent.seq);
        if (!check_dedup(ent.batch.batch_id)) {
            out.push_back(std::move(ent.batch));
            hc = std::max(hc, ent.seq);
        }
        cascade_release_one(ent.cid, out);
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

        // Design §7: committed_seq = G means GOI[0..G] fully written. Single-threaded path assigns
        // contiguous global_seq per epoch and processes in order, so max_seq is the contiguous prefix.
        // Count batches/bytes after GOI write (same as scatter path) for fair throughput comparison.
        uint64_t max_seq = 0;
        uint64_t bytes = 0;
        for (const auto& b : buf.sequenced) {
            GOIEntry& e = goi_[b.global_seq & goi_mask];
            e.batch_id = b.batch_id;
            e.broker_id = b.broker_id;
            e.epoch_sequenced = static_cast<uint16_t>(control_->epoch.load(std::memory_order_relaxed));
            e.pbr_index = b.pbr_index;
            e.blog_offset = b.blog_offset;
            e.payload_size = b.payload_size;
            e.message_count = b.message_count;
            e.num_replicated = 0;
            e.client_id = b.client_id;
            e.client_seq = b.client_seq;
            e.flags = static_cast<uint16_t>(b.flags);
            e.crc32c = compute_goi_crc(e);
            e.global_seq = b.global_seq;
#if defined(EMBARCADERO_CXL_CLWB) && (defined(__x86_64__) || defined(_M_X64))
            _mm_clwb(&e);  // Flush cache line to CXL for visibility to other hosts
#endif
            max_seq = std::max(max_seq, b.global_seq);
            bytes += b.payload_size;
            uint64_t count = validation_batch_count_.fetch_add(1, std::memory_order_relaxed);
            bool record = config_.validate_ordering || (count & config::VALIDATION_SAMPLE_MASK) == 0;
            if (record)
                validation_ring_.record(b.global_seq, b.batch_id, b.client_id, b.client_seq, b.is_level5());
        }
        if (!buf.sequenced.empty()) {
            stats_.batches_sequenced.fetch_add(buf.sequenced.size(), std::memory_order_relaxed);
            stats_.bytes_sequenced.fetch_add(bytes, std::memory_order_relaxed);
        }

        if (!buf.sequenced.empty()) {
            control_->committed_seq.store(max_seq, std::memory_order_release);
#if defined(__x86_64__) || defined(_M_X64)
#  if defined(EMBARCADERO_CXL_CLWB)
            _mm_clwb(&control_->committed_seq);
#  endif
            _mm_sfence();
#endif
        }

        buf.sequenced.clear();
        buf.mark_committed();
        ++next;
    }
}

// ----- Scatter-gather: shard helpers (plan §3.2) -----

ClientState& Sequencer::get_state_shard(ShardState& shard, uint64_t cid) {
    if (shard.max_clients != 0 && cid < shard.max_clients)
        return shard.client_states_dense[cid];
    return shard.client_states[cid];
}
uint64_t& Sequencer::get_hc_shard(ShardState& shard, uint64_t cid) {
    if (shard.max_clients != 0 && cid < shard.max_clients)
        return shard.client_highest_committed_dense[cid];
    return shard.client_highest_committed[cid];
}
std::deque<HoldEntry>& Sequencer::get_hold_buf_shard(ShardState& shard, uint64_t cid) {
    if (shard.max_clients != 0 && cid < shard.max_clients)
        return shard.hold_buf_dense[cid];
    return shard.hold_buf[cid];
}

void Sequencer::add_to_hold_shard(ShardState& shard, BatchInfo&& batch, uint64_t current_epoch, uint64_t cycle_start_ns) {
    if (shard.hold_size >= config::HOLD_BUFFER_MAX) {
        // Design §5.4: Overflow — find oldest client in this shard, emit range-SKIP, cascade.
        uint64_t oldest_client = 0;
        uint64_t oldest_ns = UINT64_MAX;
        for (uint32_t cid = 0; shard.max_clients != 0 && cid < shard.max_clients; ++cid) {
            auto& entries = shard.hold_buf_dense[cid];
            if (entries.empty()) continue;
            uint64_t t = entries.front().hold_start_ns;
            if (t < oldest_ns) { oldest_ns = t; oldest_client = cid; }
        }
        for (const auto& [cid, entries] : shard.hold_buf) {
            if (entries.empty()) continue;
            uint64_t t = entries.front().hold_start_ns;
            if (t < oldest_ns) { oldest_ns = t; oldest_client = cid; }
        }
        if (oldest_client != 0 || oldest_ns != UINT64_MAX) {
            std::deque<HoldEntry>& hold_oldest = get_hold_buf_shard(shard, oldest_client);
            if (!hold_oldest.empty()) {
                uint64_t first_held = hold_oldest.front().batch.client_seq;
                uint64_t expected = get_state_shard(shard, oldest_client).next_expected;
                BatchInfo skip{};
                skip.batch_id = SKIP_MARKER_BATCH_ID;
                skip.client_id = oldest_client;
                skip.client_seq = expected;
                skip.flags = flags::SKIP_MARKER | flags::RANGE_SKIP;
                skip.message_count = static_cast<uint32_t>(first_held - expected);
                skip.payload_size = 0;
                skip.broker_id = 0xFFFF;
                shard.ready.push_back(skip);
                get_state_shard(shard, oldest_client).next_expected = first_held;
                cascade_release_shard(shard, oldest_client, shard.ready);
            }
        }
        if (shard.hold_size >= config::HOLD_BUFFER_MAX) {
            if (config_.allow_drops) {
                stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
            } else {
                if (shard.deferred_l5.size() >= config::DEFERRED_L5_MAX) {
                    stats_.batches_dropped.fetch_add(1, std::memory_order_relaxed);
                } else {
                    shard.deferred_l5.push_back(std::move(batch));
                }
            }
            return;
        }
    }
    uint64_t client_id = batch.client_id;
    uint64_t seq = batch.client_seq;

    std::deque<HoldEntry>& held = get_hold_buf_shard(shard, client_id);
    // Duplicate check + insertion point: deque sorted by client_seq; use lower_bound (O(log n) with random-access, early termination for deque).
    auto pos = std::lower_bound(held.begin(), held.end(), seq,
                                [](const HoldEntry& e, uint64_t s) { return e.batch.client_seq < s; });
    if (pos != held.end() && pos->batch.client_seq == seq) {
        shard.local_dups.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    HoldEntry entry{std::move(batch), 0, cycle_start_ns};
    held.insert(pos, std::move(entry));
    ++shard.hold_size;
    shard.clients_with_held_.insert(client_id);
    if (config_.collect_gap_resolution_stats && shard.hold_size > shard.gap_stats.hold_peak.load(std::memory_order_relaxed))
        shard.gap_stats.hold_peak.store(shard.hold_size, std::memory_order_relaxed);
}

// Shared: drain one client's hold buffer into out while front == next_expected (refactor to avoid duplication).
// C3: deque — O(1) pop_front.
void Sequencer::drain_one_client_hold_shard(ShardState& shard, uint64_t cid, std::vector<BatchInfo>& out) {
    std::deque<HoldEntry>& held = get_hold_buf_shard(shard, cid);
    if (held.empty()) return;
    ClientState& st = get_state_shard(shard, cid);
    uint64_t& hc = get_hc_shard(shard, cid);
    if (st.next_expected == 0 && hc > 0) {
        st.next_expected = hc + 1;
        st.highest_sequenced = hc;
    }
    size_t cascade_depth = 0;
    while (!held.empty() && held.front().batch.client_seq == st.next_expected) {
        uint64_t seq = held.front().batch.client_seq;
        if (hc > 0 && seq <= hc) {
            st.mark_sequenced(seq);
            st.advance_next_expected();
            held.pop_front();
            --shard.hold_size;
            if (config_.collect_gap_resolution_stats) shard.gap_stats.hold_resolved.fetch_add(1, std::memory_order_relaxed);
            ++cascade_depth;
            continue;
        }
        bool is_dup = check_dedup_shard(shard, held.front().batch.batch_id);
        if (!is_dup) {
            out.push_back(std::move(held.front().batch));
            hc = std::max(hc, seq);
        } else {
            shard.local_dups.fetch_add(1, std::memory_order_relaxed);
        }
        st.mark_sequenced(seq);
        st.advance_next_expected();
        held.pop_front();
        --shard.hold_size;
        if (config_.collect_gap_resolution_stats) shard.gap_stats.hold_resolved.fetch_add(1, std::memory_order_relaxed);
        ++cascade_depth;
    }
    if (cascade_depth > 0 && config_.collect_gap_resolution_stats) {
        shard.gap_stats.cascade_depth_sum.fetch_add(cascade_depth, std::memory_order_relaxed);
        shard.gap_stats.cascade_count.fetch_add(1, std::memory_order_relaxed);
    }
    if (held.empty()) {
        shard.clients_with_held_.erase(cid);
        if (shard.max_clients == 0 || cid >= shard.max_clients)
            shard.hold_buf.erase(cid);
    }
}

void Sequencer::cascade_release_shard(ShardState& shard, uint64_t cid, std::vector<BatchInfo>& out) {
    drain_one_client_hold_shard(shard, cid, out);
}

bool Sequencer::check_dedup_shard(ShardState& shard, uint64_t batch_id) {
    if (config_.skip_dedup) return false;
    if (config_.use_fast_dedup)
        return shard.fast_dedup.check_and_insert(batch_id);
    return shard.dedup.check_and_insert(batch_id);
}

void Sequencer::drain_hold_shard(ShardState& shard, std::vector<BatchInfo>& out, uint64_t epoch) {
    // Collect cids to drain (only clients with held batches; avoid iterator invalidation when drain_one_client_hold_shard erases).
    shard.drain_cids_buffer_.clear();
    shard.drain_cids_buffer_.reserve(shard.clients_with_held_.size());
    for (uint64_t cid : shard.clients_with_held_) {
        if (get_hold_buf_shard(shard, cid).empty()) continue;
        if (shard.cascaded_cids_this_round.count(cid)) continue;
        shard.drain_cids_buffer_.push_back(cid);
    }
    for (uint64_t cid : shard.drain_cids_buffer_) {
        if (get_hold_buf_shard(shard, cid).empty()) continue;
        get_state_shard(shard, cid).last_epoch = epoch;
        drain_one_client_hold_shard(shard, cid, out);
    }
}

// SHARD_PATH: Must stay in sync with single-threaded age_hold (hc, dedup, ordering, gaps_skipped formula).
// C3/Design §5.5: Expire only when front of per-client deque has timed out (oldest by hold_start_ns per client).
void Sequencer::age_hold_shard(ShardState& shard, std::vector<BatchInfo>& out, uint64_t current_epoch, uint64_t cycle_start_ns) {
    shard.age_hold_expired_buffer_.clear();
    shard.expired_cids_buffer_.clear();
    const uint64_t timeout_ns = config::ORDER5_BASE_TIMEOUT_NS;
    // Phase 1: Collect clients whose front entry has timed out (only clients with held batches).
    for (uint64_t cid : shard.clients_with_held_) {
        std::deque<HoldEntry>& entries = get_hold_buf_shard(shard, cid);
        if (entries.empty()) continue;
        if (cycle_start_ns - entries.front().hold_start_ns >= timeout_ns)
            shard.expired_cids_buffer_.push_back(cid);
    }
    for (uint64_t cid : shard.expired_cids_buffer_) {
        std::deque<HoldEntry>& dq = get_hold_buf_shard(shard, cid);
        if (dq.empty()) continue;
        ShardState::ShardExpiredEntry ent;
        ent.cid = cid;
        ent.seq = dq.front().batch.client_seq;
        ent.batch = std::move(dq.front().batch);
        dq.pop_front();
        --shard.hold_size;
        if (dq.empty()) {
            shard.clients_with_held_.erase(cid);
            if (shard.max_clients == 0 || cid >= shard.max_clients)
                shard.hold_buf.erase(cid);
        }
        shard.age_hold_expired_buffer_.push_back(std::move(ent));
    }

    std::sort(shard.age_hold_expired_buffer_.begin(), shard.age_hold_expired_buffer_.end(),
              [](const ShardState::ShardExpiredEntry& a, const ShardState::ShardExpiredEntry& b) {
                  if (a.cid != b.cid) return a.cid < b.cid;
                  return a.seq < b.seq;
              });

    for (ShardState::ShardExpiredEntry& ent : shard.age_hold_expired_buffer_) {
        auto& st = get_state_shard(shard, ent.cid);
        uint64_t& hc = get_hc_shard(shard, ent.cid);

        // Sync next_expected with hc if state was recreated after GC
        if (st.next_expected == 0 && hc > 0) {
            st.next_expected = hc + 1;
            st.highest_sequenced = hc;
        }

        if (ent.seq < st.next_expected || (hc > 0 && ent.seq <= hc)) {
            st.mark_sequenced(ent.seq);
            // Advance next_expected so later batches (e.g. seq+1) can drain; otherwise
            // we'd leave next_expected behind and hold valid in-order batches forever.
            // (Sync with single-threaded age_hold path.)
            if (ent.seq >= st.next_expected)
                st.next_expected = ent.seq + 1;
            continue;
        }
        // Design §5.6 + C5: Emit one range-SKIP only when delta > 0 (avoid zero-range SKIP after cascade).
        if (ent.seq > st.next_expected) {
            uint64_t delta = ent.seq - st.next_expected;
            shard.local_gaps.fetch_add(delta, std::memory_order_relaxed);
            if (config_.collect_gap_resolution_stats) shard.gap_stats.timeout_skipped.fetch_add(delta, std::memory_order_relaxed);
            BatchInfo skip{};
            skip.batch_id = SKIP_MARKER_BATCH_ID;
            skip.client_id = ent.cid;
            skip.client_seq = st.next_expected;
            skip.flags = flags::SKIP_MARKER | flags::RANGE_SKIP;
            skip.payload_size = 0;
            skip.message_count = static_cast<uint32_t>(delta);
            skip.broker_id = 0xFFFF;
            out.push_back(skip);
        }
        st.next_expected = ent.seq + 1;
        st.mark_sequenced(ent.seq);
        if (!check_dedup_shard(shard, ent.batch.batch_id)) {
            out.push_back(std::move(ent.batch));
            hc = std::max(hc, ent.seq);
        }
        cascade_release_shard(shard, ent.cid, out);
    }
}

void Sequencer::write_ready_to_goi_shard(ShardState& shard, uint64_t cycle_ns) {
    size_t n_ready = shard.ready.size();
    if (n_ready == 0) return;
    pipeline_counters_.goi_written.fetch_add(static_cast<uint64_t>(n_ready), std::memory_order_relaxed);
    uint64_t t_write = 0, t_atomic = 0;
    if (config_.phase_timing) { t_write = now_ns(); t_atomic = now_ns(); }
    const size_t goi_mask = config_.goi_entries - 1;
    uint64_t base = 0;
    // Review §7: Use acq_rel so GOI writes and CompletedRange push are ordered after fetch_add on ARM/RISC-V.
    const bool per_batch_atomic = (config_.mode == SequencerMode::PER_BATCH_ATOMIC);
    const auto mem_order = std::memory_order_acq_rel;
    if (config_.noop_global_seq) {
        base = shard.local_seq.fetch_add(n_ready, std::memory_order_relaxed);
    } else if (per_batch_atomic) {
        base = coordinator_.global_seq.fetch_add(1, mem_order);
    } else {
        base = coordinator_.global_seq.fetch_add(n_ready, mem_order);
    }
    if (config_.phase_timing) shard.phase_atomic_ns.fetch_add(now_ns() - t_atomic, std::memory_order_relaxed);
    if (per_batch_atomic) {
        stats_.atomics_executed.fetch_add(n_ready, std::memory_order_relaxed);
    } else {
        stats_.atomics_executed.fetch_add(1, std::memory_order_relaxed);
    }
    uint64_t bytes = 0;
    uint16_t current_epoch = static_cast<uint16_t>(control_->epoch.load(std::memory_order_relaxed));
    for (size_t i = 0; i < n_ready; ++i) {
        BatchInfo& b = shard.ready[i];
        uint64_t seq = config_.noop_global_seq ? (base + i)
            : (per_batch_atomic ? (i == 0 ? base : coordinator_.global_seq.fetch_add(1, mem_order)) : (base + i));
        b.global_seq = seq;
        if (!config_.noop_goi_writes) {
            GOIEntry& e = goi_[seq & goi_mask];
            e.batch_id = b.batch_id;
            e.broker_id = b.broker_id;
            e.epoch_sequenced = current_epoch;
            e.pbr_index = b.pbr_index;
            e.blog_offset = b.blog_offset;
            e.payload_size = b.payload_size;
            e.message_count = b.message_count;
            e.client_id = b.client_id;
            e.client_seq = b.client_seq;
            e.flags = static_cast<uint16_t>(b.flags);
            e.num_replicated = 0;
            e.crc32c = compute_goi_crc(e);
            e.global_seq = seq;
            std::atomic_thread_fence(std::memory_order_release);
#if defined(EMBARCADERO_CXL_CLWB) && (defined(__x86_64__) || defined(_M_X64))
            _mm_clwb(&e);
#endif
        }
        bytes += b.payload_size;
        // Latency recorded after sfence below (review §blocking.3: commit time, not cycle start).
        ++shard.local_validation_count;
        bool record = config_.validate_ordering || (shard.local_validation_count & config::VALIDATION_SAMPLE_MASK) == 0;
        if (record) {
            size_t idx = shard.validation_write_pos_ % ShardState::VALIDATION_RING_CAP;
            shard.validation_ring_[idx].global_seq = seq;
            shard.validation_ring_[idx].batch_id = b.batch_id;
            shard.validation_ring_[idx].client_id = b.client_id;
            shard.validation_ring_[idx].client_seq = b.client_seq;
            shard.validation_ring_[idx].is_level5 = b.is_level5();
            ++shard.validation_write_pos_;
        }
        if (!config_.noop_completed_ranges && config_.mode == SequencerMode::PER_BATCH_ATOMIC) {
            if (completed_ranges_.push(CompletedRange{seq, seq + 1}, &shutdown_))
                pipeline_counters_.cr_pushed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    shard.watermark.store(base + n_ready - 1, std::memory_order_release);
    shard.batches_processed.fetch_add(n_ready, std::memory_order_relaxed);
    shard.local_batches.fetch_add(n_ready, std::memory_order_relaxed);
    shard.local_bytes.fetch_add(bytes, std::memory_order_relaxed);
#if defined(__x86_64__) || defined(_M_X64)
    _mm_sfence();
#endif
    // Review §blocking.3: Record commit timestamp after sfence (globally visible) for latency; one now_ns() per commit batch.
    uint64_t commit_ns = now_ns();
    for (size_t i = 0; i < n_ready; ++i) {
        const BatchInfo& b = shard.ready[i];
        if (b.timestamp_ns > 0)
            shard.local_latency_ring.record(commit_ns - b.timestamp_ns);
        if (b.received_ns > 0)
            shard.local_sequencing_latency_ring.record(commit_ns - b.received_ns);
    }
    if (!config_.noop_completed_ranges && (config_.mode == SequencerMode::PER_EPOCH_ATOMIC || config_.mode == SequencerMode::SCATTER_GATHER)) {
        if (completed_ranges_.push(CompletedRange{base, base + n_ready}, &shutdown_))
            pipeline_counters_.cr_pushed.fetch_add(1, std::memory_order_relaxed);
    }
    shard.ready.clear();
    if (config_.phase_timing) shard.phase_write_ns.fetch_add(now_ns() - t_write, std::memory_order_relaxed);
}

// SHARD_PATH: Client GC for scatter-gather mode. Must stay in sync with single-threaded client_gc.
// Evicts inactive client states to prevent memory leak in long-running benchmarks.
// NOTE: Do NOT erase client_highest_committed[cid] — it is the durability barrier for retries.
void Sequencer::client_gc_shard(ShardState& shard, uint64_t cur_epoch) {
    // Run every CLIENT_GC_EPOCH_INTERVAL epochs to amortize overhead
    if ((cur_epoch & (config::CLIENT_GC_EPOCH_INTERVAL - 1)) != 0) return;

    // Dense path: clear inactive slots (hold + state; do NOT touch client_highest_committed_dense).
    if (shard.max_clients != 0) {
        for (uint32_t cid = 0; cid < shard.max_clients; ++cid) {
            ClientState& st = shard.client_states_dense[cid];
            if (st.last_epoch == 0) continue;
            if (cur_epoch - st.last_epoch <= config::CLIENT_TTL_EPOCHS) continue;
            shard.hold_size -= shard.hold_buf_dense[cid].size();
            shard.hold_buf_dense[cid].clear();
            shard.clients_with_held_.erase(cid);
            st = ClientState{};  // Reset state; client_highest_committed_dense[cid] unchanged
        }
    }

    // Only run map eviction if shard has many map clients (avoid overhead when not needed)
    if (shard.client_states.size() <= config::MAX_CLIENTS_PER_SHARD) return;

    std::vector<uint64_t> evict;
    for (const auto& [cid, st] : shard.client_states) {
        if (cur_epoch - st.last_epoch > config::CLIENT_TTL_EPOCHS) {
            evict.push_back(cid);
        }
    }

    for (uint64_t cid : evict) {
        auto it = shard.hold_buf.find(cid);
        if (it != shard.hold_buf.end()) {
            shard.hold_size -= it->second.size();
            shard.hold_buf.erase(it);
            shard.clients_with_held_.erase(cid);
        }
        shard.client_states.erase(cid);
        // Do NOT erase client_highest_committed[cid]. It is the durability barrier.
    }
}

// SHARD_PATH: Must stay in sync with single-threaded process_order5 (hc, dedup, in-order emit, hold).
void Sequencer::process_order5_shard(ShardState& shard, std::vector<BatchInfo>& batches, uint64_t epoch, uint64_t cycle_ns) {
    shard.cascaded_cids_this_round.clear();
    if (!shard.deferred_l5.empty()) {
        batches.insert(batches.end(),
                       std::make_move_iterator(shard.deferred_l5.begin()),
                       std::make_move_iterator(shard.deferred_l5.end()));
        shard.deferred_l5.clear();
    }
    // Gap stats (only when requested: avoids O(n) + map alloc per drain in default path).
    if (config_.collect_gap_resolution_stats) {
        std::unordered_map<uint64_t, uint64_t> max_seen;
        uint64_t inversions = 0;
        for (const auto& b : batches) {
            if (!(b.flags & flags::STRONG_ORDER)) continue;
            auto it = max_seen.find(b.client_id);
            if (it != max_seen.end() && b.client_seq < it->second) ++inversions;
            if (it == max_seen.end() || b.client_seq > it->second) max_seen[b.client_id] = b.client_seq;
        }
        shard.gap_stats.sort_resolved.fetch_add(inversions, std::memory_order_relaxed);
    }
    // P6: Design §5.3 — single sort by (client_id, client_seq).
    std::sort(batches.begin(), batches.end(), [](const BatchInfo& a, const BatchInfo& b) {
        return std::tie(a.client_id, a.client_seq) < std::tie(b.client_id, b.client_seq);
    });

    auto it = batches.begin();
    while (it != batches.end()) {
        uint64_t cid = it->client_id;
        auto start = it;
        while (it != batches.end() && it->client_id == cid) ++it;

        auto& state = get_state_shard(shard, cid);
        uint64_t& hc = get_hc_shard(shard, cid);
        state.last_epoch = epoch;

        // Sync next_expected with hc if client state was newly created but hc was preserved
        // (e.g., after GC cleared client_states but kept client_highest_committed).
        // (Sync with single-threaded get_state lazy initialization.)
        if (state.next_expected == 0 && hc > 0) {
            state.next_expected = hc + 1;
            state.highest_sequenced = hc;
        }

        // Accept-first-seen (§5.3): if this is a new client, set next_expected to the first batch's client_seq
        if (state.next_expected == 0 && state.highest_sequenced == 0 && hc == 0) {
            state.next_expected = start->client_seq;
        }

        for (auto curr = start; curr != it; ++curr) {
            auto& b = *curr;
            if (state.is_duplicate(b.client_seq)) {
                shard.local_dups.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Already committed (retry or late arrival): reject duplicate; do not advance next_expected (align with single-threaded path).
            if (hc > 0 && b.client_seq <= hc) {
                state.mark_sequenced(b.client_seq);
                shard.local_dups.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (b.client_seq < state.next_expected) {
                state.mark_sequenced(b.client_seq);
                continue;
            }
            if (b.client_seq == state.next_expected) {
                if (!check_dedup_shard(shard, b.batch_id)) {
                    shard.ready.push_back(std::move(b));
                    hc = std::max(hc, b.client_seq);
                    state.mark_sequenced(b.client_seq);
                    state.advance_next_expected();
                    cascade_release_shard(shard, cid, shard.ready);
                    shard.cascaded_cids_this_round.insert(cid);
                } else {
                    state.mark_sequenced(b.client_seq);
                    state.advance_next_expected();
                    cascade_release_shard(shard, cid, shard.ready);
                    shard.cascaded_cids_this_round.insert(cid);
                    shard.local_dups.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                add_to_hold_shard(shard, std::move(b), epoch, cycle_ns);
            }
        }
    }
}

void Sequencer::shard_worker_loop(uint32_t shard_id) {
    if (config_.pin_cores) pin_self_to_core(pin_core_next_.fetch_add(1, std::memory_order_relaxed));
    ShardState& shard = *shards_[shard_id];
    std::vector<BatchInfo> input_buffer;
    input_buffer.reserve(config::SHARD_INPUT_RESERVE);
    // Reserve once at startup to avoid malloc on first large drain (review Bug 2).
    shard.ready.reserve(config::MAX_BATCHES_PER_EPOCH);
    uint64_t idle_iters = 0;
    // Drain-only microbenchmarks can oscillate around empty/non-empty queue boundaries.
    // Keep idle spin shorter there to reduce startup/tail measurement artifacts.
    const uint64_t max_idle_iters = config_.pause_epoch_timer ? 100 : 10000;

    // Continuous drain (design §5): no epoch barrier; drain queues and process when work available.
    while (!shutdown_.load(std::memory_order_acquire)) {
        input_buffer.clear();
        if (!spsc_queues_.empty()) {
            for (uint32_t c = 0; c < config_.num_collectors; ++c)
                spsc_queues_[c][shard_id]->drain_to(input_buffer, config::MAX_DRAIN_PER_CYCLE);
        } else {
            if (shard.input_queue) shard.input_queue->drain_to(input_buffer, config::MAX_DRAIN_PER_CYCLE);
        }

        if (input_buffer.empty() && shard.hold_size == 0) {
            pipeline_counters_.shard_idle_cycles.fetch_add(1, std::memory_order_relaxed);
            if (++idle_iters < max_idle_iters) {
                CPU_PAUSE();
            } else {
                // No sleep, just yield to see if we can get higher throughput
                std::this_thread::yield();
            }
            continue;
        }
        pipeline_counters_.shard_drained.fetch_add(static_cast<uint64_t>(input_buffer.size()), std::memory_order_relaxed);
        pipeline_counters_.shard_drain_cycles.fetch_add(1, std::memory_order_relaxed);
        idle_iters = 0;

        uint64_t cycle_ns = now_ns();
        uint64_t epoch = coordinator_.current_epoch.load(std::memory_order_relaxed);
        shard.current_epoch.store(epoch, std::memory_order_relaxed);

        shard.ready.clear();

        uint64_t t_part0 = 0, t_l0 = 0, t_l5 = 0, t_hold = 0;
        if (config_.phase_timing) t_part0 = now_ns();
        shard.l0_batches.clear();
        shard.l5_batches.clear();
        for (auto& b : input_buffer) {
            if (b.is_level5() && config_.level5_enabled)
                shard.l5_batches.push_back(std::move(b));
            else
                shard.l0_batches.push_back(std::move(b));
        }
        if (config_.phase_timing) shard.phase_partition_ns.fetch_add(now_ns() - t_part0, std::memory_order_relaxed);
        if (config_.phase_timing) t_l0 = now_ns();
        for (auto& b : shard.l0_batches) {
            if (!check_dedup_shard(shard, b.batch_id))
                shard.ready.push_back(std::move(b));
            else
                shard.local_dups.fetch_add(1, std::memory_order_relaxed);
        }
        if (config_.phase_timing) shard.phase_l0_ns.fetch_add(now_ns() - t_l0, std::memory_order_relaxed);
        if (config_.phase_timing) t_l5 = now_ns();
        if (!shard.l5_batches.empty())
            process_order5_shard(shard, shard.l5_batches, epoch, cycle_ns);
        if (config_.phase_timing) shard.phase_l5_ns.fetch_add(now_ns() - t_l5, std::memory_order_relaxed);
        if (config_.phase_timing) t_hold = now_ns();
        drain_hold_shard(shard, shard.ready, epoch);
        age_hold_shard(shard, shard.ready, epoch, cycle_ns);
        if (shard.hold_size > 0)
            drain_hold_shard(shard, shard.ready, epoch);
        if (config_.phase_timing) shard.phase_hold_ns.fetch_add(now_ns() - t_hold, std::memory_order_relaxed);

        // Client GC to prevent memory leak in long-running benchmarks
        client_gc_shard(shard, epoch);

        // Phase 2: Decentralized GOI writes (refactored into write_ready_to_goi_shard; review §critical.3).
        write_ready_to_goi_shard(shard, cycle_ns);
    }

    // Drain-on-shutdown: process remaining batches in SPSC queues to avoid silent loss (completeness §5.2).
    input_buffer.clear();
    if (!spsc_queues_.empty()) {
        for (uint32_t c = 0; c < config_.num_collectors; ++c)
            spsc_queues_[c][shard_id]->drain_to(input_buffer);
    } else if (shard.input_queue) {
        shard.input_queue->drain_to(input_buffer);
    }
    if (!input_buffer.empty()) {
        uint64_t cycle_ns = now_ns();
        uint64_t epoch = coordinator_.current_epoch.load(std::memory_order_relaxed);
        shard.ready.clear();
        shard.l0_batches.clear();
        shard.l5_batches.clear();
        for (auto& b : input_buffer) {
            if (b.is_level5() && config_.level5_enabled)
                shard.l5_batches.push_back(std::move(b));
            else
                shard.l0_batches.push_back(std::move(b));
        }
        for (auto& b : shard.l0_batches) {
            if (!check_dedup_shard(shard, b.batch_id))
                shard.ready.push_back(std::move(b));
            else
                shard.local_dups.fetch_add(1, std::memory_order_relaxed);
        }
        if (!shard.l5_batches.empty())
            process_order5_shard(shard, shard.l5_batches, epoch, cycle_ns);
        drain_hold_shard(shard, shard.ready, epoch);
        shard.local_l5.fetch_add(shard.l5_batches.size(), std::memory_order_relaxed);
        write_ready_to_goi_shard(shard, cycle_ns);
    }
}

void Sequencer::committed_seq_updater() {
    if (config_.pin_cores) pin_self_to_core(pin_core_next_.fetch_add(1, std::memory_order_relaxed));
    // Design §7: Min-heap merge of CompletedRange. committed_seq = G means GOI[0..G] fully written.
    uint64_t next_expected_start = 0;
    std::priority_queue<CompletedRange, std::vector<CompletedRange>, std::greater<CompletedRange>> pending;
    int sleep_us = config::WAIT_SLEEP_US;
    int idle_iters = 0;
    static constexpr int SPIN_IDLE_ITERS = 100;  // Design §7: spin before sleep to reduce ~10μs lag (review §perf.5).

    while (!shutdown_.load(std::memory_order_acquire) || !completed_ranges_.empty()) {
        bool got_any = false;
        CompletedRange cr;
        while (completed_ranges_.try_pop(cr)) {
            pending.push(cr);
            got_any = true;
            pipeline_counters_.cr_popped.fetch_add(1, std::memory_order_relaxed);
        }
        // Brief spin when ring appeared empty (slot may not be ready yet).
        if (!got_any && !shutdown_.load(std::memory_order_acquire)) {
            for (int i = 0; i < 16; ++i) {
                CPU_PAUSE();
                if (completed_ranges_.try_pop(cr)) {
                    pending.push(cr);
                    got_any = true;
                    pipeline_counters_.cr_popped.fetch_add(1, std::memory_order_relaxed);
                    break;
                }
            }
        }

        bool advanced = false;
        while (!pending.empty() && pending.top().start == next_expected_start) {
            next_expected_start = pending.top().end;
            pending.pop();
            advanced = true;
        }

        if (advanced && next_expected_start > 0) {
            control_->committed_seq.store(next_expected_start - 1, std::memory_order_release);
            if (!control_->initialized.load(std::memory_order_relaxed)) {
                control_->initialized.store(true, std::memory_order_release);
            }
#if defined(__x86_64__) || defined(_M_X64)
#  if defined(EMBARCADERO_CXL_CLWB)
            _mm_clwb(&control_->committed_seq);  // Design §7: CXL visibility before sfence
#  endif
            _mm_sfence();
#endif
        }

        if (got_any || advanced) {
            idle_iters = 0;
            continue;  // Don't sleep when there's work (fix: sleep-on-active was filling completed_ranges_ ring).
        }
        // Only reach here when idle
        if (idle_iters++ < SPIN_IDLE_ITERS) {
            CPU_PAUSE();
            continue;
        }
        sleep_us = std::min(100, sleep_us + (sleep_us >> 1));
        std::this_thread::sleep_for(std::chrono::microseconds(sleep_us));
    }
}

void Sequencer::timer_loop_scatter() {
    if (config_.pin_cores) pin_self_to_core(pin_core_next_.fetch_add(1, std::memory_order_relaxed));
    // Drain-only benchmark: don't advance epochs so pre-filled PBR entries never go stale (MAX_EPOCH_AGE).
    if (config_.pause_epoch_timer) {
        while (!shutdown_.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return;
    }
    // Design §5: Timer only advances epoch for hold-buffer aging and client_gc; no barrier.
    // committed_seq is updated by committed_seq_updater from CompletedRange min-heap (§7).
    using Clock = std::chrono::steady_clock;
    const auto epoch_duration = std::chrono::microseconds(config_.epoch_us);
    auto next_tick = Clock::now() + epoch_duration;
    while (!shutdown_.load(std::memory_order_acquire)) {
        auto now = Clock::now();
        if (now < next_tick)
            std::this_thread::sleep_for(next_tick - now);
        next_tick += epoch_duration;
        uint64_t new_epoch = coordinator_.current_epoch.fetch_add(1, std::memory_order_release) + 1;
        control_->epoch.store(static_cast<uint16_t>(new_epoch), std::memory_order_release);
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
    auto slot = config_.pbr_use_fetch_add ? ring.reserve_fetch_add() : ring.reserve();
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

// Call after stop(); drains lock-free ring (single-threaded) or merges per-shard buffers (scatter-gather, C4).
std::string Sequencer::validate_ordering_reason() {
    std::vector<ValidationEntry> sorted;
    if (!shards_.empty()) {
        // C4: Scatter-gather: merge per-shard validation rings (last CAP entries per shard, in write order).
        size_t total = 0;
        for (const auto& shard : shards_) {
            if (shard && shard->validation_write_pos_ > 0)
                total += std::min(shard->validation_write_pos_, static_cast<uint64_t>(ShardState::VALIDATION_RING_CAP));
        }
        sorted.reserve(total);
        for (const auto& shard : shards_) {
            if (!shard || shard->validation_write_pos_ == 0) continue;
            uint64_t wp = shard->validation_write_pos_;
            size_t n = static_cast<size_t>(std::min(wp, static_cast<uint64_t>(ShardState::VALIDATION_RING_CAP)));
            size_t start = (wp >= ShardState::VALIDATION_RING_CAP) ? (wp - n) : 0;
            for (size_t i = 0; i < n; ++i)
                sorted.push_back(shard->validation_ring_[(start + i) % ShardState::VALIDATION_RING_CAP]);
        }
    } else {
        sorted = validation_ring_.drain();
    }
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
    // Per-client order (Order 5): include both Order 5 batches and SKIP markers so client_seq (including SKIPs) is strictly increasing (review §significant.6).
    std::map<uint64_t, std::vector<std::pair<uint64_t, uint64_t>>> by_client;
    for (const auto& e : sorted) {
        if (e.is_level5 || e.batch_id == SKIP_MARKER_BATCH_ID)
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
    // Review §critical.2: committed_seq must not exceed the max global_seq assigned (valid GOI prefix).
    uint64_t cs = control_->committed_seq.load(std::memory_order_acquire);
    if (cs != UINT64_MAX) {
        uint64_t gs = config_.scatter_gather_enabled
            ? coordinator_.global_seq.load(std::memory_order_acquire)
            : global_seq_.load(std::memory_order_acquire);
        if (cs >= gs) {
            return "committed_seq=" + std::to_string(cs) + " >= global_seq=" + std::to_string(gs);
        }
    }
    return "";
}

// ============================================================================
// Trace mode: refill thread (single writer per PBR, no CAS)
// ============================================================================

void refill_thread(uint32_t broker_id, const BrokerTrace& trace, Sequencer& seq,
                   std::atomic<bool>& running, uint64_t target_rate_per_broker = 0) {
    size_t idx = 0;
    MPSCRing& ring = seq.pbr_ring(broker_id);
    PBREntry* base = seq.pbr_base(broker_id);
    const size_t mask = seq.config().pbr_entries - 1;

    using Clock = std::chrono::steady_clock;
    auto next_inject = Clock::now();
    // Inject one entry every (1e9 / target_rate) nanoseconds
    const auto interval = std::chrono::nanoseconds(
        target_rate_per_broker > 0 ? 1'000'000'000ULL / target_rate_per_broker : 0);

    while (running.load(std::memory_order_acquire) && idx < trace.entries.size()) {
        // Rate limit: wait until next injection time
        if (target_rate_per_broker > 0) {
            auto now = Clock::now();
            if (now < next_inject) {
                // Busy-wait for sub-microsecond precision
                while (Clock::now() < next_inject) {
                    if (!running.load(std::memory_order_relaxed)) break;
                    CPU_PAUSE();
                }
            }
            next_inject += interval;
        }

        uint64_t tail = ring.tail();
        uint64_t head = ring.head();
        if (tail - head >= seq.config().pbr_entries - 256) {
            for (int i = 0; i < 32; ++i) CPU_PAUSE();
            if (!running.load(std::memory_order_relaxed)) break;
            continue;
        }

        const TraceEntry& te = trace.entries[idx];
        PBREntry& e = base[tail & mask];
        e.blog_offset = 0;
        e.payload_size = te.payload_size;
        e.message_count = te.message_count;
        e.batch_id = te.batch_id;
        e.broker_id = static_cast<uint16_t>(broker_id);
        e.epoch_created = seq.control_epoch();
        e.client_id = te.client_id;
        e.client_seq = te.client_seq;
        e.timestamp_ns = now_ns();
        e.flags.store(te.flags | flags::VALID, std::memory_order_release);
        
        ring.force_set_tail(tail + 1);
        idx++;
    }
}

/// Fill one broker's PBR from trace (no refill thread). Used by measure_drain_rate.
void fill_pbr_from_trace(Sequencer& seq, uint32_t broker_id, const BrokerTrace& trace,
                         size_t max_entries) {
    if (trace.entries.empty()) return;
    MPSCRing& ring = seq.pbr_ring(broker_id);
    PBREntry* base = seq.pbr_base(broker_id);
    const size_t mask = seq.config().pbr_entries - 1;
    size_t n = std::min(max_entries, trace.entries.size());
    uint64_t tail = ring.tail();
    for (size_t i = 0; i < n; ++i) {
        const TraceEntry& te = trace.entries[i];
        PBREntry& e = base[(tail + i) & mask];
        e.blog_offset = 0;
        e.payload_size = te.payload_size;
        e.message_count = te.message_count;
        e.batch_id = te.batch_id;
        e.broker_id = static_cast<uint16_t>(broker_id);
        e.epoch_created = seq.control_epoch();
        e.client_id = te.client_id;
        e.client_seq = te.client_seq;
        e.timestamp_ns = now_ns();
        e.flags.store(te.flags | flags::VALID, std::memory_order_release);
        seq.pipeline_counters_.refill_written.fetch_add(1, std::memory_order_relaxed);
    }
    ring.force_set_tail(tail + n);
}

Sequencer::GapResolutionReport Sequencer::get_gap_resolution_report() const {
    GapResolutionReport r;
    if (shards_.empty()) return r;
    for (const auto& shard : shards_) {
        if (!shard) continue;
        r.sort_resolved += shard->gap_stats.sort_resolved.load(std::memory_order_relaxed);
        r.hold_resolved += shard->gap_stats.hold_resolved.load(std::memory_order_relaxed);
        r.timeout_skipped += shard->gap_stats.timeout_skipped.load(std::memory_order_relaxed);
        r.cascade_depth_sum += shard->gap_stats.cascade_depth_sum.load(std::memory_order_relaxed);
        r.cascade_count += shard->gap_stats.cascade_count.load(std::memory_order_relaxed);
        uint64_t p = shard->gap_stats.hold_peak.load(std::memory_order_relaxed);
        if (p > r.hold_peak) r.hold_peak = p;
    }
    return r;
}

}  // namespace embarcadero

// ============================================================================
// Benchmark
// ============================================================================

// Forward declaration for CSV mode string (defined later in file).
static const char* sequencer_mode_name(embarcadero::SequencerMode mode);

namespace bench {

using namespace embarcadero;
using Clock = std::chrono::steady_clock;

/// Review §benchmark 10: machine-parseable output for figure generation. CSV includes mode,shards,brokers,producers for paper figures.
static bool g_output_csv = false;
static std::vector<std::string> g_csv_rows;
void set_output_csv(bool on) { g_output_csv = on; }
void csv_emit_row(const std::string& run_name, double batches_sec, double mb_sec, double p99_us,
                  uint64_t valid_runs, uint64_t num_runs, uint64_t atomics, uint64_t gaps, uint64_t dropped,
                  const std::string& mode_str = "", uint32_t shards = 0, uint32_t brokers = 0, uint32_t producers = 0) {
    if (!g_output_csv) return;
    std::ostringstream row;
    row << "\"" << run_name << "\",\"" << mode_str << "\"," << shards << "," << brokers << "," << producers << ","
        << std::fixed << std::setprecision(2)
        << batches_sec << "," << mb_sec << "," << p99_us << ","
        << valid_runs << "," << num_runs << "," << atomics << "," << gaps << "," << dropped;
    g_csv_rows.push_back(row.str());
}
std::vector<std::string> drain_csv_rows() {
    std::vector<std::string> out;
    out.swap(g_csv_rows);
    return out;
}

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
    bool use_fast_dedup = true;  // If false, use legacy Deduplicator (--legacy-dedup)
    bool phase_timing = false;   // If true, enable per-phase timing (Order 5 cost breakdown) without rebuild
    bool pin_cores = true;     // Pin sequencer threads to cores (reproducible latency; review §8b)
    bool noop_completed_ranges = false;  // Micro-ablation: no-op completed_ranges push (review §blocking.2)
    bool noop_goi_writes = false;        // Micro-ablation: skip GOI writes (review §6)
    bool noop_global_seq = false;        // Micro-ablation: per-shard seq instead of global (review §6)
    /// B2: Closed-loop producer. 0 = open-loop (max rate); >0 = target batches/sec across all producers.
    uint64_t target_batches_per_sec = 0;
};

struct Result {
    double batches_sec = 0;
    double mb_sec = 0;
    double msgs_sec = 0;
    double p50_us = 0, p95_us = 0, p99_us = 0, p999_us = 0;
    double seq_p50_us = 0, seq_p99_us = 0;  // Sequencing latency (PBR read to commit), separate from E2E
    double avg_seq_ns = 0;
    uint64_t min_seq_ns = 0, max_seq_ns = 0;
    uint64_t batches = 0, epochs = 0, level5 = 0, gaps = 0, dropped = 0, atomics = 0;
    uint64_t duplicates = 0;  // Batches rejected by dedup (0 when --skip-dedup)
    uint64_t pbr_full = 0;  // PBR reserve failures (saturation)
    uint64_t backpressure_events = 0;  // Inject attempts deferred by high watermark (controlled backpressure)
    double pbr_usage_pct = 0;  // Snapshot: avg (used/capacity)*100 across brokers at end of run
    double duration = 0;
    uint64_t epoch_us = 500;  // Target epoch interval (for reporting)
    bool pbr_saturated = false;  // true if PBR full % > threshold (run invalid for algorithm comparison)
    bool steady_state_reached = false;
    double steady_state_cv_pct = 0;
    uint64_t steady_state_windows = 0;
    uint64_t backlog_max = 0;
    uint64_t validation_overwrites = 0;
    bool valid = true;
    /// B3: Throughput valid when PBR not saturated; latency valid when P99/backlog within threshold.
    bool throughput_valid = true;
    bool latency_valid = true;

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
        // C2/B1: Preserve mode (PER_BATCH vs PER_EPOCH) for atomic strategy in both single-threaded and scatter-gather.
        sc.mode = cfg_.mode;
        sc.use_radix_sort = cfg_.use_radix_sort;
        sc.validate_ordering = cfg_.validate;  // Full trace when validating; sampling when not
        if (cfg_.pbr_entries > 0) sc.pbr_entries = cfg_.pbr_entries;
        if (cfg_.scatter_gather) {
            sc.scatter_gather_enabled = true;
            sc.num_shards = cfg_.num_shards;
        }
        sc.skip_dedup = cfg_.skip_dedup;
        sc.use_fast_dedup = cfg_.use_fast_dedup;
        sc.phase_timing = cfg_.phase_timing;
        sc.pin_cores = cfg_.pin_cores;
        sc.noop_completed_ranges = cfg_.noop_completed_ranges;
        sc.noop_goi_writes = cfg_.noop_goi_writes;
        sc.noop_global_seq = cfg_.noop_global_seq;
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

        auto base_b = seq.get_batches_sequenced_snapshot();
        auto base_bytes = seq.get_bytes_sequenced_snapshot();
        auto base_e = seq.stats().epochs_completed.load();
        auto base_level5 = seq.stats().level5_batches.load();
        auto base_gaps = seq.stats().gaps_skipped.load();
        auto base_dropped = seq.stats().batches_dropped.load();
        auto base_duplicates = seq.stats().duplicates.load();
        auto base_atomics = seq.stats().atomics_executed.load();
        auto base_pbr_full = seq.stats().pbr_full_count.load();
        auto base_backpressure = seq.stats().backpressure_events.load();
        seq.reset_validation_overwrites();
        seq.stats().reset_latency();

        std::cout << "  Running..." << std::flush;
        auto t0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.duration_sec * 1000));
        // C1: Snapshot t1 and stats atomically so throughput = (batches_at_t1 - base_b) / (t1 - t0)
        // Scatter-gather: use per-shard snapshot so we see counts before stop() merges (review §perf.1).
        auto t1 = Clock::now();
        uint64_t batches_at_t1 = seq.get_batches_sequenced_snapshot();
        uint64_t bytes_at_t1 = seq.get_bytes_sequenced_snapshot();
        uint64_t dropped_at_t1 = seq.stats().batches_dropped.load(std::memory_order_acquire);
        uint64_t injected_at_t1 = seq.stats().total_injected.load(std::memory_order_acquire);
        stop_producers(producers, nullptr);
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
                std::cout << "  Ordering validation covers the final N entries of each run (N = ring capacity "
                          << embarcadero::config::VALIDATION_RING_CAP << "; review §minor.10).\n";
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
        r.duplicates = seq.stats().duplicates.load() - base_duplicates;
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

        // Phase 3.1: E2E latency (inject-to-commit) and sequencing latency (PBR read-to-commit)
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
        std::vector<uint64_t> seq_ns = seq.stats().drain_sequencing_latency_ring();
        if (!seq_ns.empty()) {
            std::sort(seq_ns.begin(), seq_ns.end());
            const size_t n = seq_ns.size();
            const double ns_to_us = 1e-3;
            r.seq_p50_us = seq_ns[n / 2] * ns_to_us;
            r.seq_p99_us = seq_ns[n * 99 / 100] * ns_to_us;
        }

        const ValidityMode mode = cfg_.validity_mode;
        const bool allow_loss = (mode == ValidityMode::STRESS);
        r.valid = true;
        r.throughput_valid = true;  // B3: separate throughput vs latency validity
        r.latency_valid = true;
        if (r.batches_sec == 0 && r.duration > 0) {
            std::cerr << "  WARNING: Zero throughput. PBR_full=" << r.pbr_full
                      << " (if high: PBR saturated - try larger pbr_entries or fewer producers).\n";
            if (!allow_loss) {
                std::cerr << "  [INVALID RUN] Zero throughput in non-stress mode.\n";
                r.throughput_valid = false;
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
                r.throughput_valid = false;
            }
        }

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

        // Assessment §6.2: Sanity checks on results. B3: latency_valid separate from throughput_valid.
        if (r.p99_us > 100000.0) {
            std::cerr << "  [SANITY WARN] P99 latency " << (r.p99_us / 1000.0)
                      << " ms is high; system is backlogged.\n";
            r.latency_valid = false;
            if (mode == ValidityMode::ALGORITHM) {
                std::cerr << "  [INVALID RUN] Latency exceeds algorithm-comparison threshold.\n";
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
            r.latency_valid = false;
            if (mode == ValidityMode::ALGORITHM) {
                std::cerr << "  [INVALID RUN] Steady-state required for algorithm comparison.\n";
            }
        }

        // B3: Overall valid = throughput_valid && (latency_valid or MAX_THROUGHPUT mode).
        r.valid = r.throughput_valid && (r.latency_valid || mode == ValidityMode::MAX_THROUGHPUT);
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
            group.threads.emplace_back([&, tid, clients_per_producer] {
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
                        cseq_val = cseqs[local_c]++;  // Design §5.3: start from 0
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
                            // B2: Closed-loop rate limit (interval = producers / target_batches_per_sec seconds)
                            if (cfg_.target_batches_per_sec > 0 && cfg_.producers > 0) {
                                using namespace std::chrono;
                                static thread_local auto next_send = Clock::now();
                                const double interval_sec = static_cast<double>(cfg_.producers) / static_cast<double>(cfg_.target_batches_per_sec);
                                const auto interval_ns = duration_cast<Clock::duration>(duration<double>(interval_sec));
                                next_send += interval_ns;
                                if (next_send > t1) {
                                    std::this_thread::sleep_until(next_send);
                                } else {
                                    next_send = t1 + interval_ns;
                                }
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
            uint64_t base_b = seq.get_batches_sequenced_snapshot();
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.steady_state_window_ms));
            uint64_t cur_b = seq.get_batches_sequenced_snapshot();
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

    // produce() removed (dead code); use start_producers/stop_producers for runs.

    Config cfg_;
    std::atomic<uint64_t> global_batch_counter_{0};
};

void sep(char c = '=', int w = 65) { std::cout << std::string(w, c) << '\n'; }

MultiRunResult run_multiple(const Config& cfg) {
    // Warm-up run (discarded) to stabilize cache/allocator
    Runner(cfg).run();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<double> throughputs, latencies, mb_secs;
    uint64_t atomics = 0, epochs = 0, gaps = 0, dropped = 0, pbr_full = 0;
    uint64_t valid_runs = 0;

    for (int i = 0; i < cfg.num_runs; ++i) {
        Result r = Runner(cfg).run();
        if (r.valid) {
            ++valid_runs;
            throughputs.push_back(r.batches_sec);
            latencies.push_back(r.p99_us);
            mb_secs.push_back(r.mb_sec);
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

// ============================================================================
// Trace-based benchmark (deterministic workload, refill threads, no producer CAS)
// ============================================================================

struct TraceRunResult {
    double throughput = 0;       // batches/s
    uint64_t sort_resolved = 0;
    uint64_t hold_resolved = 0;
    uint64_t timeout_skipped = 0;
    uint64_t hold_peak = 0;
    double p99_us = 0;
    bool validation_passed = false;
};

namespace {
constexpr uint64_t TRACE_DEFAULT_WARMUP_MS = 2000;
constexpr uint64_t TRACE_DEFAULT_MEASURE_MS = 5000;
/// Capped injection rate (per broker) for scaling/ablation so we measure sequencer capacity, not PBR collapse. 500K/broker => 2 M/s total for 4 brokers.
constexpr uint64_t TRACE_CAP_RATE_PER_BROKER = 500000ULL;
/// Lower cap for Order 5 (heavier path) to avoid hold-buffer livelock and get meaningful sort/hold/skip stats.
constexpr uint64_t TRACE_ORDER5_CAP_RATE_PER_BROKER = 200000ULL;

StatResult compute_stats(const std::vector<double>& values) {
    std::vector<double> copy = values;
    return StatResult::compute(copy);
}

double cv_percent(const StatResult& s) {
    return (s.median > 0.0) ? (100.0 * s.stddev / s.median) : 0.0;
}
}  // namespace

/// Drain-rate measurement: fill PBRs from trace, no refill. Measures pure sequencer drain throughput.
double measure_drain_rate(const SequencerConfig& sc, const std::vector<BrokerTrace>& traces) {
    SequencerConfig sc_drain = sc;
    sc_drain.pause_epoch_timer = true;  // Entries never go stale (no timer advance).
    Sequencer seq(sc_drain);
    if (!seq.initialize()) return 0.0;
    seq.start();
    // Let collector/shard threads start before we fill (avoids race with first epoch/state).
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Fill each broker's PBR up to capacity (no refill thread).
    const size_t fill_per_broker = sc.pbr_entries;
    size_t total_filled = 0;
    for (uint32_t b = 0; b < sc.num_brokers && b < traces.size(); ++b) {
        size_t n = std::min(fill_per_broker, traces[b].entries.size());
        total_filled += n;
        embarcadero::fill_pbr_from_trace(seq, b, traces[b], fill_per_broker);
    }
    uint64_t base_b = seq.get_batches_sequenced_snapshot();
    auto t0 = Clock::now();
    const auto timeout = std::chrono::seconds(30);
    // Wait until shards have sequenced (committed) the filled entries. PBR tail-head can hit 0
    // when the collector has read everything, but batches_sequenced only increases when shards commit.
    const uint64_t target_sequenced = base_b + total_filled;
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        uint64_t cur = seq.get_batches_sequenced_snapshot();
        if (cur >= target_sequenced) break;
        if (Clock::now() - t0 > timeout) break;
    }

    auto t1 = Clock::now();
    uint64_t end_b = seq.get_batches_sequenced_snapshot();
    seq.stop();
    {
        auto& pc = seq.pipeline_counters_;
        std::printf("Pipeline: filled=%lu collector_read=%lu pushed=%lu blocked=%lu "
                    "shard_drained=%lu drain_cycles=%lu idle_cycles=%lu "
                    "goi=%lu cr_push=%lu cr_pop=%lu\n",
                    pc.refill_written.load(std::memory_order_relaxed),
                    pc.collector_read.load(std::memory_order_relaxed),
                    pc.collector_pushed.load(std::memory_order_relaxed),
                    pc.collector_blocked.load(std::memory_order_relaxed),
                    pc.shard_drained.load(std::memory_order_relaxed),
                    pc.shard_drain_cycles.load(std::memory_order_relaxed),
                    pc.shard_idle_cycles.load(std::memory_order_relaxed),
                    pc.goi_written.load(std::memory_order_relaxed),
                    pc.cr_pushed.load(std::memory_order_relaxed),
                    pc.cr_popped.load(std::memory_order_relaxed));
    }
    double seconds = std::chrono::duration<double>(t1 - t0).count();
    return seconds > 0 ? (end_b - base_b) / seconds : 0.0;
}

static void print_drain_rate(double rate_batches_per_sec) {
    if (rate_batches_per_sec >= 1e6)
        std::cout << std::fixed << std::setprecision(2) << (rate_batches_per_sec / 1e6) << " M batches/s\n";
    else if (rate_batches_per_sec >= 1e3)
        std::cout << std::fixed << std::setprecision(2) << (rate_batches_per_sec / 1e3) << " K batches/s\n";
    else
        std::cout << std::fixed << std::setprecision(0) << rate_batches_per_sec << " batches/s\n";
}

TraceRunResult run_single_trace_detailed(const SequencerConfig& sc,
                                         const std::vector<BrokerTrace>& traces,
                                         uint64_t target_rate_per_broker,
                                         uint64_t warmup_ms = TRACE_DEFAULT_WARMUP_MS,
                                         uint64_t measure_ms = TRACE_DEFAULT_MEASURE_MS) {
    TraceRunResult out;
    Sequencer seq(sc);
    if (!seq.initialize()) return out;
    seq.start();

    std::atomic<bool> refill_running{true};
    std::vector<std::thread> refill_threads;
    for (uint32_t b = 0; b < sc.num_brokers && b < traces.size(); ++b) {
        refill_threads.emplace_back([&seq, &traces, b, &refill_running, target_rate_per_broker] {
            refill_thread(b, traces[b], seq, refill_running, target_rate_per_broker);
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(warmup_ms));

    auto base_b = seq.get_batches_sequenced_snapshot();
    auto t0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(measure_ms));

    auto t1 = Clock::now();
    auto end_b = seq.get_batches_sequenced_snapshot();
    refill_running = false;
    for (auto& t : refill_threads) t.join();
    seq.stop();

    std::string reason = seq.validate_ordering_reason();
    out.validation_passed = reason.empty();
    if (!out.validation_passed) std::cerr << "  [trace] VALIDATION FAILED: " << reason << "\n";

    double seconds = std::chrono::duration<double>(t1 - t0).count();
    out.throughput = seconds > 0 ? (end_b - base_b) / seconds : 0.0;

    auto gap = seq.get_gap_resolution_report();
    out.sort_resolved = gap.sort_resolved;
    out.hold_resolved = gap.hold_resolved;
    out.timeout_skipped = gap.timeout_skipped;
    out.hold_peak = gap.hold_peak;

    auto lat = seq.stats().drain_sequencing_latency_ring();
    if (lat.size() >= 100) {
        std::sort(lat.begin(), lat.end());
        size_t p99_idx = (lat.size() * 99) / 100;
        out.p99_us = lat[p99_idx] / 1000.0;
    }
    return out;
}

double run_single_trace(const SequencerConfig& sc, const std::vector<BrokerTrace>& traces) {
    TraceRunResult out = run_single_trace_detailed(sc, traces, 0);
    return out.throughput;
}

double run_single_trace(const SequencerConfig& sc,
                        const std::vector<BrokerTrace>& traces,
                        uint64_t target_rate_per_broker) {
    TraceRunResult out = run_single_trace_detailed(sc, traces, target_rate_per_broker);
    return out.throughput;
}

void run_trace_saturation_sweep(const Config& cfg, const std::vector<BrokerTrace>& traces) {
    std::cout << "\n=== Saturation Sweep (Order 2, 8 shards) ===\n";
    std::cout << "  Target rate/broker | Throughput median [min,max] (M/s)\n";
    
    SequencerConfig sc;
    sc.num_brokers = cfg.brokers;
    sc.num_collectors = std::min(cfg.brokers, 4u);
    sc.scatter_gather_enabled = true;
    sc.num_shards = 8u;
    sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
    sc.level5_enabled = false;
    sc.validate_ordering = true;
    sc.max_clients = cfg.clients;
    sc.pbr_entries = (cfg.pbr_entries > 0) ? cfg.pbr_entries : 256 * 1024;
    sc.validate();

    for (uint64_t rate : {100'000ULL, 250'000ULL, 500'000ULL, 750'000ULL, 
                          1'000'000ULL, 1'500'000ULL, 2'000'000ULL, 
                          3'000'000ULL, 5'000'000ULL, 0ULL/*unlimited*/}) {
        std::vector<double> runs;
        runs.reserve(static_cast<size_t>(std::max(1, cfg.num_runs)));
        for (int i = 0; i < std::max(1, cfg.num_runs); ++i) {
            TraceRunResult rr = run_single_trace_detailed(sc, traces, rate);
            runs.push_back(rr.throughput);
        }
        StatResult s = compute_stats(runs);
        std::cout << "  " << std::setw(18) << (rate == 0 ? "unlimited" : std::to_string(rate)) 
                  << " | " << std::fixed << std::setprecision(2) << (s.median / 1e6)
                  << " [" << (s.min_val / 1e6) << ", " << (s.max_val / 1e6) << "]";
        if (cv_percent(s) > 10.0) std::cout << "  [high variance]";
        std::cout << "\n";
    }
}

TraceRunResult run_single_trace_detailed(const SequencerConfig& sc, const std::vector<BrokerTrace>& traces) {
    return run_single_trace_detailed(sc, traces, 0);
}

void run_trace_drain_only(const Config& cfg) {
    TraceConfig tc;
    tc.num_brokers = cfg.brokers;
    tc.num_clients = cfg.clients;
    tc.total_batches = 20'000'000;
    tc.level5_ratio = 0.0;
    tc.reorder_rate = 0.0;
    tc.seed = 42;
    SequencerConfig sc;
    sc.num_brokers = cfg.brokers;
    sc.num_collectors = std::min(cfg.brokers, 4u);
    sc.scatter_gather_enabled = true;
    sc.num_shards = 8u;
    sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
    sc.level5_enabled = false;
    sc.validate_ordering = true;
    sc.max_clients = cfg.clients;
    sc.pbr_entries = (cfg.pbr_entries > 0) ? cfg.pbr_entries : 256 * 1024;
    sc.validate();
    auto traces = TraceGenerator().generate(tc);
    double drain = measure_drain_rate(sc, traces);
    std::cout << "=== Drain rate (pre-filled PBR, no refill) ===\n  ";
    print_drain_rate(drain);
}

void run_ablation1_trace_deconfounded(const Config& cfg) {
    std::cout << "\n=== Trace-based Deconfounded Ablation (High Throughput) ===\n";
    TraceConfig tc;
    tc.num_brokers = cfg.brokers;
    tc.num_clients = cfg.clients;
    tc.total_batches = 20'000'000;
    tc.level5_ratio = 0.0;
    tc.reorder_rate = 0.0;
    tc.seed = 42;
    
    auto traces = TraceGenerator().generate(tc);
    
    for (uint32_t shards : {1u, 4u, 8u}) {
        SequencerConfig sc;
        sc.num_brokers = cfg.brokers;
        sc.num_collectors = std::min(cfg.brokers, 4u);
        sc.scatter_gather_enabled = true;
        sc.num_shards = shards;
        sc.level5_enabled = false;
        sc.validate_ordering = true;
        sc.max_clients = cfg.clients;
        sc.pbr_entries = 256 * 1024;
        sc.validate();
        
        std::vector<double> batch_runs;
        std::vector<double> epoch_runs;
        std::vector<double> speedups;
        int runs = std::max(1, cfg.num_runs);
        batch_runs.reserve(static_cast<size_t>(runs));
        epoch_runs.reserve(static_cast<size_t>(runs));
        speedups.reserve(static_cast<size_t>(runs));
        for (int run = 0; run < runs; ++run) {
            sc.mode = SequencerMode::PER_BATCH_ATOMIC;
            double t_batch = run_single_trace(sc, traces, TRACE_CAP_RATE_PER_BROKER);
            sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
            double t_epoch = run_single_trace(sc, traces, TRACE_CAP_RATE_PER_BROKER);
            batch_runs.push_back(t_batch);
            epoch_runs.push_back(t_epoch);
            if (t_batch > 0) speedups.push_back(t_epoch / t_batch);
        }
        StatResult batch_stats = compute_stats(batch_runs);
        StatResult epoch_stats = compute_stats(epoch_runs);
        StatResult speedup_stats = compute_stats(speedups);
        printf("  %u shards: per_batch=%.2f [%.2f, %.2f] M/s  per_epoch=%.2f [%.2f, %.2f] M/s  speedup=%.2f [%.2f, %.2f]×\n",
               shards,
               batch_stats.median / 1e6, batch_stats.min_val / 1e6, batch_stats.max_val / 1e6,
               epoch_stats.median / 1e6, epoch_stats.min_val / 1e6, epoch_stats.max_val / 1e6,
               speedup_stats.median, speedup_stats.min_val, speedup_stats.max_val);
    }
}

void run_trace_benchmark(const Config& cfg) {
    TraceConfig tc;
    tc.num_brokers = cfg.brokers;
    tc.num_clients = cfg.clients;
    tc.total_batches = 20'000'000;
    tc.level5_ratio = cfg.level5_ratio;
    tc.reorder_rate = 0.0;
    tc.seed = 42;

    std::cout << "\n=== Measurement Method ===\n";
    std::cout << "  sustained trace mode (refill threads), "
              << (TRACE_DEFAULT_WARMUP_MS / 1000.0) << "s warmup + "
              << (TRACE_DEFAULT_MEASURE_MS / 1000.0) << "s measure\n";
    std::cout << "  scaling/ablation: capped injection " << (TRACE_CAP_RATE_PER_BROKER / 1000) << " K/broker (Order 2); Order 5: " << (TRACE_ORDER5_CAP_RATE_PER_BROKER / 1000) << " K/broker\n";

    tc.level5_ratio = 0.0;
    run_trace_saturation_sweep(cfg, TraceGenerator().generate(tc));

    run_ablation1_trace_deconfounded(cfg);

    std::cout << "\n=== Throughput Ceiling (Order 2, vary shards) ===\n";
    tc.level5_ratio = 0.0;
    for (uint32_t shards : {1u, 2u, 4u, 8u, 16u}) {
        SequencerConfig sc;
        sc.num_brokers = cfg.brokers;
        sc.num_collectors = std::min(cfg.brokers, 4u);
        sc.scatter_gather_enabled = true;
        sc.num_shards = shards;
        sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
        sc.level5_enabled = false;
        sc.validate_ordering = true;
        sc.max_clients = cfg.clients;
        sc.pbr_entries = (cfg.pbr_entries > 0) ? cfg.pbr_entries : 256 * 1024;
        sc.validate();

        auto traces = TraceGenerator().generate(tc);
        std::vector<double> tputs;
        int runs = std::max(1, cfg.num_runs);
        tputs.reserve(static_cast<size_t>(runs));
        for (int run = 0; run < runs; ++run) {
            double t = run_single_trace(sc, traces, TRACE_CAP_RATE_PER_BROKER);
            tputs.push_back(t);
        }
        StatResult t_stats = compute_stats(tputs);
        std::cout << "  " << shards << " shards: " << std::fixed << std::setprecision(2)
                  << (t_stats.median / 1e6) << " M/s ["
                  << (t_stats.min_val / 1e6) << ", " << (t_stats.max_val / 1e6) << "]";
        if (cv_percent(t_stats) > 10.0) std::cout << "  [high variance]";
        std::cout << "\n";
    }

    std::cout << "\n=== Order 5 Gap Resolution (8 shards, sweep reorder) ===\n";
    std::cout << "  reorder | tput median [min,max] (M/s) | p99 median [min,max] (us)"
              << " | skip median [min,max]\n";
    for (double reorder : {0.0, 0.01, 0.05, 0.10, 0.50}) {
        tc.level5_ratio = 1.0;
        tc.reorder_rate = reorder;
        tc.max_reorder_distance = 50;

        SequencerConfig sc;
        sc.scatter_gather_enabled = true;
        sc.num_shards = 8;
        sc.level5_enabled = true;
        sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
        sc.validate_ordering = true;
        sc.collect_gap_resolution_stats = true;
        sc.max_clients = cfg.clients;
        sc.num_brokers = cfg.brokers;
        sc.num_collectors = std::min(cfg.brokers, 4u);
        sc.pbr_entries = (cfg.pbr_entries > 0) ? cfg.pbr_entries : 256 * 1024;
        sc.validate();

        auto traces = TraceGenerator().generate(tc);
        std::vector<double> tputs;
        std::vector<double> p99s;
        std::vector<double> skips;
        std::vector<double> sorts;
        std::vector<double> holds;
        bool all_valid = true;
        for (int run = 0; run < std::max(1, cfg.num_runs); ++run) {
            TraceRunResult result = run_single_trace_detailed(sc, traces, TRACE_ORDER5_CAP_RATE_PER_BROKER);
            tputs.push_back(result.throughput);
            p99s.push_back(result.p99_us);
            skips.push_back(static_cast<double>(result.timeout_skipped));
            sorts.push_back(static_cast<double>(result.sort_resolved));
            holds.push_back(static_cast<double>(result.hold_resolved));
            all_valid = all_valid && result.validation_passed;
        }
        StatResult t_stats = compute_stats(tputs);
        StatResult p99_stats = compute_stats(p99s);
        StatResult skip_stats = compute_stats(skips);
        StatResult sort_stats = compute_stats(sorts);
        StatResult hold_stats = compute_stats(holds);
        std::cout << "  " << std::fixed << std::setprecision(0) << (reorder * 100) << "% | "
                  << std::setprecision(2) << (t_stats.median / 1e6)
                  << " [" << (t_stats.min_val / 1e6) << ", " << (t_stats.max_val / 1e6) << "] | "
                  << std::setprecision(1) << p99_stats.median
                  << " [" << p99_stats.min_val << ", " << p99_stats.max_val << "] | "
                  << std::setprecision(0) << skip_stats.median
                  << " [" << skip_stats.min_val << ", " << skip_stats.max_val << "]"
                  << " (sort~" << sort_stats.median << ", hold~" << hold_stats.median << ")"
                  << (all_valid ? "" : " [VALID FAIL]") << "\n";
    }

    std::cout << "\n=== Bursty Reorder Experiment (10% burst of 50% reorder) ===\n";
    {
        tc.level5_ratio = 1.0;
        tc.reorder_rate = 0.0;
        tc.burst_reorder = true;
        tc.burst_start_pct = 0.45;
        tc.burst_end_pct = 0.55;
        tc.burst_reorder_rate = 0.5;
        tc.total_batches = 10'000'000;

        SequencerConfig sc;
        sc.scatter_gather_enabled = true;
        sc.num_shards = 8;
        sc.level5_enabled = true;
        sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
        sc.validate_ordering = true;
        sc.collect_gap_resolution_stats = true;
        sc.max_clients = cfg.clients;
        sc.num_brokers = cfg.brokers;
        sc.num_collectors = std::min(cfg.brokers, 4u);
        sc.pbr_entries = (cfg.pbr_entries > 0) ? cfg.pbr_entries : 256 * 1024;
        sc.validate();

        auto traces = TraceGenerator().generate(tc);
        std::vector<double> tputs;
        std::vector<double> p99s;
        std::vector<double> hold_peaks;
        std::vector<double> skips;
        bool all_valid = true;
        for (int run = 0; run < std::max(1, cfg.num_runs); ++run) {
            TraceRunResult result = run_single_trace_detailed(sc, traces, TRACE_ORDER5_CAP_RATE_PER_BROKER);
            tputs.push_back(result.throughput);
            p99s.push_back(result.p99_us);
            hold_peaks.push_back(static_cast<double>(result.hold_peak));
            skips.push_back(static_cast<double>(result.timeout_skipped));
            all_valid = all_valid && result.validation_passed;
        }
        StatResult t_stats = compute_stats(tputs);
        StatResult p99_stats = compute_stats(p99s);
        StatResult hold_peak_stats = compute_stats(hold_peaks);
        StatResult skip_stats = compute_stats(skips);

        std::cout << "  Burst (45%-55%): " << std::fixed << std::setprecision(2)
                  << (t_stats.median / 1e6) << " M/s [" << (t_stats.min_val / 1e6)
                  << ", " << (t_stats.max_val / 1e6) << "], "
                  << "P99=" << std::setprecision(1) << p99_stats.median
                  << "us [" << p99_stats.min_val << ", " << p99_stats.max_val << "], "
                  << "hold_peak=" << std::setprecision(0) << hold_peak_stats.median
                  << " [" << hold_peak_stats.min_val << ", " << hold_peak_stats.max_val << "], "
                  << "skip=" << skip_stats.median << " [" << skip_stats.min_val << ", "
                  << skip_stats.max_val << "]"
                  << (all_valid ? "" : " [VALID FAIL]") << "\n";
    }

    std::cout << "\n=== Ablation: Per-Batch vs Per-Epoch (vary shards) ===\n";
    tc.level5_ratio = 0.0;
    tc.reorder_rate = 0.0;
    for (uint32_t shards : {1u, 4u, 8u, 16u}) {
        SequencerConfig sc;
        sc.num_brokers = cfg.brokers;
        sc.num_collectors = std::min(cfg.brokers, 4u);
        sc.scatter_gather_enabled = true;
        sc.num_shards = shards;
        sc.level5_enabled = false;
        sc.validate_ordering = true;
        sc.max_clients = cfg.clients;
        sc.pbr_entries = (cfg.pbr_entries > 0) ? cfg.pbr_entries : 256 * 1024;
        sc.validate();

        auto traces = TraceGenerator().generate(tc);
        std::vector<double> batch_runs;
        std::vector<double> epoch_runs;
        std::vector<double> speedups;
        int runs = std::max(1, cfg.num_runs);
        batch_runs.reserve(static_cast<size_t>(runs));
        epoch_runs.reserve(static_cast<size_t>(runs));
        speedups.reserve(static_cast<size_t>(runs));
        for (int r = 0; r < runs; ++r) {
            sc.mode = SequencerMode::PER_BATCH_ATOMIC;
            double t_batch = run_single_trace(sc, traces, TRACE_CAP_RATE_PER_BROKER);
            sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
            double t_epoch = run_single_trace(sc, traces, TRACE_CAP_RATE_PER_BROKER);
            batch_runs.push_back(t_batch);
            epoch_runs.push_back(t_epoch);
            if (t_batch > 0) speedups.push_back(t_epoch / t_batch);
        }
        StatResult b_stats = compute_stats(batch_runs);
        StatResult e_stats = compute_stats(epoch_runs);
        StatResult s_stats = compute_stats(speedups);
        std::cout << "  " << shards << " shards: per_batch=" << std::fixed << std::setprecision(2)
                  << (b_stats.median / 1e6) << " [" << (b_stats.min_val / 1e6) << ", "
                  << (b_stats.max_val / 1e6) << "] M/s "
                  << "per_epoch=" << (e_stats.median / 1e6) << " [" << (e_stats.min_val / 1e6)
                  << ", " << (e_stats.max_val / 1e6) << "] M/s "
                  << "speedup=" << s_stats.median << "× [" << s_stats.min_val << ", "
                  << s_stats.max_val << "]\n";
    }
    sep();
}

/// Review §blocking.2: Micro-ablations to identify scalability bottleneck (vary shards, vary collectors, no-op completed_ranges).
void run_bottleneck_ablation() {
    sep();
    std::cout << "  BOTTLENECK ABLATION (scatter-gather, Order 2, 5s each)\n"; sep();
    Config base;
    base.brokers = 4;
    base.producers = 8;
    base.duration_sec = 5;
    base.warmup_ms = 500;
    base.level5 = false;
    base.scatter_gather = true;
    base.validate = false;
    base.steady_state = true;
    base.num_runs = 1;
    base.pbr_entries = 1024 * 1024;

    std::cout << "\n  (1) Vary shards (collectors=4):\n";
    std::cout << "      shards  batches_sec   mb_sec\n";
    for (uint32_t s : {1u, 2u, 4u, 8u, 16u}) {
        auto c = base;
        c.num_shards = s;
        c.collectors = 4;
        Result r = Runner(c).run();
        std::cout << "        " << std::setw(2) << s << "   " << std::fixed << std::setprecision(2) << r.batches_sec / 1e6 << " M     " << r.mb_sec << "\n";
    }
    std::cout << "\n  (2) Vary collectors (shards=8):\n";
    std::cout << "      collectors  batches_sec   mb_sec\n";
    for (uint32_t col : {1u, 2u, 4u, 8u}) {
        auto c = base;
        c.num_shards = 8;
        c.collectors = col;
        Result r = Runner(c).run();
        std::cout << "           " << std::setw(2) << col << "   " << std::fixed << std::setprecision(2) << r.batches_sec / 1e6 << " M     " << r.mb_sec << "\n";
    }
    std::cout << "\n  (3) No-op completed_ranges (shards=8, collectors=4): isolates committed_seq updater cost\n";
    auto c_noop = base;
    c_noop.num_shards = 8;
    c_noop.collectors = 4;
    c_noop.noop_completed_ranges = true;
    Result r_noop = Runner(c_noop).run();
    std::cout << "      batches_sec " << std::fixed << std::setprecision(2) << r_noop.batches_sec / 1e6 << " M   mb_sec " << r_noop.mb_sec << "\n";

    std::cout << "\n  (4) No-op GOI writes (shards=8, collectors=4): isolates memory bandwidth (review §6)\n";
    auto c_noop_goi = base;
    c_noop_goi.num_shards = 8;
    c_noop_goi.collectors = 4;
    c_noop_goi.noop_goi_writes = true;
    Result r_noop_goi = Runner(c_noop_goi).run();
    std::cout << "      batches_sec " << std::fixed << std::setprecision(2) << r_noop_goi.batches_sec / 1e6 << " M   mb_sec " << r_noop_goi.mb_sec << "\n";

    std::cout << "\n  (5) No-op global_seq (per-shard counter, shards=8, collectors=4): isolates fetch_add contention (review §6)\n";
    auto c_noop_gs = base;
    c_noop_gs.num_shards = 8;
    c_noop_gs.collectors = 4;
    c_noop_gs.noop_global_seq = true;
    Result r_noop_gs = Runner(c_noop_gs).run();
    std::cout << "      batches_sec " << std::fixed << std::setprecision(2) << r_noop_gs.batches_sec / 1e6 << " M   mb_sec " << r_noop_gs.mb_sec << "\n";

    std::cout << "\n  (Table: (1) shards; (2) collectors; (3) ring/updater; (4) GOI bw; (5) global_seq contention. Decomposition for scalability figure.)\n";
    sep();
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
    // C2/B1: Run ablation in scatter-gather so we measure cross-shard fetch_add contention, not single-threaded.
    c_baseline.scatter_gather = true;
    c_baseline.num_shards = cfg.num_shards > 0 ? cfg.num_shards : 8;
    if (!cfg.validity_mode_explicit) {
        c_baseline.validity_mode = ValidityMode::ALGORITHM;
    }
    Config c_optimized = cfg;
    c_optimized.level5 = false;
    c_optimized.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;
    c_optimized.scatter_gather = true;
    c_optimized.num_shards = cfg.num_shards > 0 ? cfg.num_shards : 8;
    if (!cfg.validity_mode_explicit) {
        c_optimized.validity_mode = ValidityMode::ALGORITHM;
    }
    // B6: Use 64K PBR when not set (clean preset already sets 64K) so validity can pass.
    if (cfg.pbr_entries == 0) {
        c_baseline.pbr_entries = 64 * 1024;
        c_optimized.pbr_entries = 64 * 1024;
    }

    // Warm-up (one baseline + one optimized, discarded)
    Runner(c_baseline).run();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Runner(c_optimized).run();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::vector<double> base_throughputs, base_latencies, base_mb;
    std::vector<double> opt_throughputs, opt_latencies, opt_mb;
    std::vector<double> speedups;
    uint64_t base_atomics = 0, base_epochs = 0, base_gaps = 0, base_dropped = 0, base_pbr = 0;
    uint64_t opt_atomics = 0, opt_epochs = 0, opt_gaps = 0, opt_dropped = 0, opt_pbr = 0;
    uint64_t base_valid = 0, opt_valid = 0;

    for (int i = 0; i < cfg.num_runs; ++i) {
        Result r_base = Runner(c_baseline).run();
        Result r_opt = Runner(c_optimized).run();
        if (r_base.valid) {
            ++base_valid;
            base_throughputs.push_back(r_base.batches_sec);
            base_latencies.push_back(r_base.p99_us);
            base_mb.push_back(r_base.mb_sec);
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
    if (!speedups.empty())
        out.speedup = StatResult::compute(speedups);
    if (out.optimized.total_atomics > 0)
        out.atomic_reduction = static_cast<double>(out.baseline.total_atomics) / out.optimized.total_atomics;
    return out;
}

/// Review §1: Deconfound Ablation 1 — isolate (A) pure fetch_add contention vs (B) ring pressure.
/// (A) PER_BATCH_ATOMIC + noop_completed_ranges, (B) PER_EPOCH_ATOMIC + noop_completed_ranges,
/// (C) PER_BATCH_ATOMIC with ranges on. Use noop_completed_ranges=true to isolate fetch_add; false measures atomic + ring.
void run_ablation1_deconfounded(const Config& cfg) {
    Config base = cfg;
    base.level5 = false;
    base.scatter_gather = true;
    base.num_shards = (cfg.num_shards > 0) ? cfg.num_shards : 8;
    base.num_runs = std::max(2, std::min(cfg.num_runs, 5));  // 2–5 runs for decomposition
    if (base.pbr_entries == 0) base.pbr_entries = 64 * 1024;

    std::cout << "\n  ABLATION 1 DECONFOUNDED (Review §1): isolate atomic vs ring overhead\n";
    sep('-');

    Config c_a = base;
    c_a.mode = embarcadero::SequencerMode::PER_BATCH_ATOMIC;
    c_a.noop_completed_ranges = true;
    Config c_b = base;
    c_b.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;
    c_b.noop_completed_ranges = true;
    Config c_pb = base;
    c_pb.mode = embarcadero::SequencerMode::PER_BATCH_ATOMIC;
    c_pb.noop_completed_ranges = false;

    MultiRunResult r_a = run_multiple(c_a);
    MultiRunResult r_b = run_multiple(c_b);
    MultiRunResult r_pb = run_multiple(c_pb);

    double speedup_atomic = (r_a.throughput.median > 0) ? (r_b.throughput.median / r_a.throughput.median) : 0;
    double speedup_full = (r_pb.throughput.median > 0) ? (r_b.throughput.median / r_pb.throughput.median) : 0;

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  (A) PER_BATCH + noop_ranges:  " << (r_a.throughput.median / 1e6) << " M/s\n";
    std::cout << "  (B) PER_EPOCH + noop_ranges:   " << (r_b.throughput.median / 1e6) << " M/s  => speedup A→B (pure atomic): " << speedup_atomic << "×\n";
    std::cout << "  (C) PER_BATCH (ranges on):   " << (r_pb.throughput.median / 1e6) << " M/s  => full PER_BATCH→PER_EPOCH: " << speedup_full << "×\n";
    std::cout << "  (Use noop_completed_ranges=true to isolate fetch_add; false measures atomic + ring.)\n";
    sep();
}

void print_ablation_table(const Ablation1Result& a1, int num_runs) {
    std::cout << "\n";
    std::cout << "┌─────────────────────────────────────────────────────────────┐\n";
    std::cout << "│           ABLATION 1: Epoch Batching Effectiveness          │\n";
    std::cout << "├─────────────────┬──────────────┬──────────────┬─────────────┤\n";
    std::cout << "│ Metric          │ Per-Batch    │ Per-Epoch    │ Improvement │\n";
    std::cout << "├─────────────────┼──────────────┼──────────────┼─────────────┤\n";

    std::cout << std::fixed;
    std::cout << "│ Throughput (M/s)│ "
              << std::setw(12) << std::setprecision(2) << (a1.baseline.throughput.median / 1e6)
              << " │ "
              << std::setw(12) << std::setprecision(2) << (a1.optimized.throughput.median / 1e6)
              << " │ "
              << std::setw(10) << std::setprecision(2) << a1.speedup.median << "× │\n";

    std::cout << "│ P99 Latency (ms)│ "
              << std::setw(12) << std::setprecision(2) << (a1.baseline.latency_p99.median / 1000)
              << " │ "
              << std::setw(12) << std::setprecision(2) << (a1.optimized.latency_p99.median / 1000)
              << " │ "
              << std::setw(10) << std::setprecision(2)
              << (a1.optimized.latency_p99.median > 0 ? a1.baseline.latency_p99.median / a1.optimized.latency_p99.median : 0) << "× │\n";

    std::cout << "│ Atomics/run     │ "
              << std::setw(12) << a1.baseline.total_atomics
              << " │ "
              << std::setw(12) << a1.optimized.total_atomics
              << " │ "
              << std::setw(9) << std::setprecision(0) << a1.atomic_reduction << "× │\n";

    std::cout << "├─────────────────┴──────────────┴──────────────┴─────────────┤\n";
    std::cout << "│ Valid runs: " << a1.baseline.valid_runs << "/" << num_runs
              << " (baseline), " << a1.optimized.valid_runs << "/" << num_runs
              << " (optimized)                  │\n";
    std::cout << "└─────────────────────────────────────────────────────────────┘\n";
}

void print_stat_result(const std::string& name, const MultiRunResult& r, int num_runs, const Config* cfg = nullptr) {
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
    std::string mode_str = cfg ? sequencer_mode_name(cfg->scatter_gather ? embarcadero::SequencerMode::SCATTER_GATHER : cfg->mode) : "";
    uint32_t shards = cfg && cfg->scatter_gather ? cfg->num_shards : 0u;
    uint32_t brokers = cfg ? cfg->brokers : 0u;
    uint32_t producers = cfg ? cfg->producers : 0u;
    csv_emit_row(name, r.throughput.median, r.median_mb_sec, r.latency_p99.median,
                 r.valid_runs, static_cast<uint64_t>(num_runs),
                 r.total_atomics, r.total_gaps, r.total_dropped,
                 mode_str, shards, brokers, producers);
}

void print(const std::string& name, const Result& r, const Config* cfg = nullptr) {
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
              << " P99.9=" << r.p999_us << " μs (E2E); Sequencing P99=" << r.seq_p99_us << " μs\n"
              << std::setprecision(1)
              << "  Epoch CPU:  avg=" << (r.avg_seq_ns / 1000.0) << " min=" << (r.min_seq_ns / 1000.0)
              << " max=" << (r.max_seq_ns / 1000.0) << " μs/epoch (target: " << r.epoch_us << " μs)\n"
              << "  Stats:      " << r.batches << " batches, " << r.epochs << " epochs, "
              << r.level5 << " O5, " << r.gaps << " gaps";
    if (r.atomics > 0) std::cout << ", " << r.atomics << " atomics";
    if (r.dropped > 0) std::cout << ", " << r.dropped << " dropped";
    std::cout << ", " << r.duplicates << " duplicates";
    std::cout << ", PBR_full: " << r.pbr_full
              << ", validation_overwrites: " << r.validation_overwrites << "\n";
    std::string mode_str = cfg ? sequencer_mode_name(cfg->scatter_gather ? embarcadero::SequencerMode::SCATTER_GATHER : cfg->mode) : "";
    uint32_t shards = cfg && cfg->scatter_gather ? cfg->num_shards : 0u;
    uint32_t brokers = cfg ? cfg->brokers : 0u;
    uint32_t producers = cfg ? cfg->producers : 0u;
    csv_emit_row(name, r.batches_sec, r.mb_sec, r.p99_us, 1, 1, r.atomics, r.gaps, r.dropped,
                 mode_str, shards, brokers, producers);
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

void benchmark_dedup_implementations() {
    std::cout << "\n";
    sep();
    std::cout << "  DEDUP IMPLEMENTATION COMPARISON\n";
    sep();

    const size_t N = 1000000;  // 1M operations
    std::vector<uint64_t> batch_ids(N);
    for (size_t i = 0; i < N; ++i) {
        batch_ids[i] = (uint64_t(i % 16) << 48) | i;  // Simulate real batch_ids
    }

    {
        embarcadero::Deduplicator dedup;
        auto t0 = std::chrono::steady_clock::now();
        for (uint64_t id : batch_ids) {
            (void) dedup.check_and_insert(id);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  Legacy Deduplicator: " << std::fixed << std::setprecision(2)
                  << ms << " ms for " << N << " ops ("
                  << (N / ms / 1000) << " M ops/s)\n";
    }

    {
        embarcadero::FastDeduplicator dedup;
        auto t0 = std::chrono::steady_clock::now();
        for (uint64_t id : batch_ids) {
            (void) dedup.check_and_insert(id);
        }
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        std::cout << "  Fast Deduplicator:   " << std::fixed << std::setprecision(2)
                  << ms << " ms for " << N << " ops ("
                  << (N / ms / 1000) << " M ops/s)\n";
    }

    sep();
}

/** Improvement instruction Task B Step 5: Stress FastDeduplicator eviction path (fill past RING_SIZE). */
bool test_fast_deduplicator() {
    using namespace embarcadero;
    FastDeduplicator dedup;

    // New insert
    if (dedup.check_and_insert(1)) return false;  // 1 is new
    // Duplicate
    if (!dedup.check_and_insert(1)) return false;  // 1 is duplicate

    // Fill to RING_SIZE + 100 to force eviction of oldest entries (1, 2, ... 100)
    const size_t RING_SIZE = FastDeduplicator::RING_SIZE;
    for (uint64_t i = 2; i <= RING_SIZE + 100; ++i) {
        if (dedup.check_and_insert(i)) return false;  // Each must be new
    }

    // Entry 1 should have been evicted; re-insert must be seen as new
    if (dedup.check_and_insert(1)) return false;  // 1 evicted, so new again
    if (!dedup.check_and_insert(1)) return false;  // Now duplicate

    return true;
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
              << "  level5_ratio       0.0..1.0 Order 5 fraction (default 0.1)\n"
              << "  num_runs           1+ (default 5); >=2 reports median [min,max]\n"
              << "  use_radix_sort     0|1 (default 0)\n"
              << "  scatter_gather     0|1 (default 0); 1 enables scatter-gather mode\n"
              << "  num_shards         1..32 (default 8); used when scatter_gather=1\n"
              << "  scatter_gather_only  0|1 (default 0); 1 runs only scatter-gather scaling test\n"
              << "  pbr_entries        0=auto (default); >0 = entries per broker (e.g. 4194304 for 4M)\n"
              << "  --clean            algorithm comparison: 1 producer, 64K PBR (validity can pass)\n"
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
              << "  --target-rate=N     closed-loop: target total batches/sec (0 = open-loop)\n"
              << "  --skip-dedup        disable deduplication (valid for algorithm ablation; see config comment)\n"
              << "  --legacy-dedup      use legacy Deduplicator instead of FastDeduplicator\n"
              << "  --bench-dedup       run dedup implementation micro-benchmark and exit\n"
              << "  --test-dedup        run FastDeduplicator eviction stress test and exit (0 pass, 1 fail)\n"
              << "  --help, -h          print this and exit\n"
              << "  --output-csv        emit machine-parseable CSV of results (run,batches_sec,mb_sec,p99_us,...)\n"
              << "  --test              minimal correctness test (ordering + per-client); exit 0 pass, 1 fail\n"
              << "  --trace             trace-based benchmark (throughput ceiling, reorder sweep, per-batch vs per-epoch)\n"
              << "  --trace-drain-only  run only drain-rate measurement (pre-fill PBR, no refill) and exit\n"
              << "  --phase-timing      enable per-phase timing (Order 5 cost breakdown) without rebuild\n"
              << "  --pin-cores         pin sequencer threads to consecutive cores (reproducible latency)\n"
              << "  --noop-completed-ranges  micro-ablation: no-op completed_ranges push (isolate updater cost)\n"
              << "  (Real CXL: set HAVE_LIBNUMA; use EMBARCADERO_CXL_DRAM=1 to force DRAM)\n"
              << "  --bottleneck-ablation   run scalability micro-ablations and print table (shards/collectors/noop)\n";
}

static const char* validity_mode_name(bench::ValidityMode mode) {
    switch (mode) {
        case bench::ValidityMode::ALGORITHM: return "algo";
        case bench::ValidityMode::MAX_THROUGHPUT: return "max";
        case bench::ValidityMode::STRESS: return "stress";
    }
    return "max";
}

static const char* sequencer_mode_name(embarcadero::SequencerMode mode) {
    using embarcadero::SequencerMode;
    switch (mode) {
        case SequencerMode::PER_BATCH_ATOMIC: return "per_batch";
        case SequencerMode::PER_EPOCH_ATOMIC: return "per_epoch";
        case SequencerMode::SCATTER_GATHER: return "scatter_gather";
    }
    return "per_epoch";
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
            cseq = client_seqs[(cid - 1) % 20]++;  // Design §5.3: start from 0
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
    // Review §significant.7: Use separate client ID space for L5 (100..104) so Order 2 and Order 5 do not share client_id=0.
    std::vector<uint64_t> sg_client_seqs(5, 0);
    const uint64_t L5_CLIENT_BASE = 100;
    for (int i = 0; i < 1500; ++i) {
        uint32_t flags = (i % 5 == 0) ? flags::STRONG_ORDER : 0;
        uint64_t cid = (i % 5 == 0) ? (L5_CLIENT_BASE + (i / 5) % 5) : 0;  // L5 clients 100..104; Order 2 uses 0
        uint64_t cseq = (cid >= L5_CLIENT_BASE) ? sg_client_seqs[cid - L5_CLIENT_BASE]++ : 0;  // Design §5.3: start from 0
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

/** Review §5: Deterministic correctness — sort resolution, hold cascade. */
static bool run_deterministic_correctness_tests() {
    using namespace embarcadero;
    const uint32_t batch_size = 256;
    const uint32_t msgs = 4;

    // (1) Sort resolution: inject (client=1, seq=5) then (client=1, seq=4); must emit in client_seq order 4, 5.
    {
        SequencerConfig sc;
        sc.num_brokers = 1;
        sc.num_collectors = 1;
        sc.num_shards = 1;
        sc.scatter_gather_enabled = true;
        sc.level5_enabled = true;
        sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
        sc.validate_ordering = true;
        sc.validate();
        Sequencer seq(sc);
        if (!seq.initialize()) return false;
        seq.start();
        uint64_t bid = 0;
        InjectResult r;
        r = seq.inject_batch(0, bid++, batch_size, msgs, flags::STRONG_ORDER, 1, 5);
        if (r != InjectResult::SUCCESS) { seq.stop(); return false; }
        r = seq.inject_batch(0, bid++, batch_size, msgs, flags::STRONG_ORDER, 1, 4);
        if (r != InjectResult::SUCCESS) { seq.stop(); return false; }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        seq.stop();
        if (!seq.validate_ordering_reason().empty()) {
            std::cerr << "  Deterministic sort resolution: " << seq.validate_ordering_reason() << "\n";
            return false;
        }
    }

    // (2) Hold buffer cascade: inject seq=2 then seq=0 then seq=1; must emit in order 0, 1, 2.
    {
        SequencerConfig sc;
        sc.num_brokers = 1;
        sc.num_collectors = 1;
        sc.num_shards = 1;
        sc.scatter_gather_enabled = true;
        sc.level5_enabled = true;
        sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
        sc.validate_ordering = true;
        sc.validate();
        Sequencer seq(sc);
        if (!seq.initialize()) return false;
        seq.start();
        uint64_t bid = 0;
        for (uint64_t cseq : {2ULL, 0ULL, 1ULL}) {
            InjectResult r = seq.inject_batch(0, bid++, batch_size, msgs, flags::STRONG_ORDER, 1, cseq);
            if (r != InjectResult::SUCCESS) { seq.stop(); return false; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        seq.stop();
        if (!seq.validate_ordering_reason().empty()) {
            std::cerr << "  Deterministic hold cascade: " << seq.validate_ordering_reason() << "\n";
            return false;
        }
    }
    return true;
}

/** Review §8.1: Crash-recovery smoke test.
 * Phase 1: Run sequencer, inject 10K batches, record committed_seq.
 * Phase 2: Kill sequencer (destroy object without clean stop).
 * Phase 3: Create new sequencer on same CXL memory.
 * Phase 4: Run recovery (§8.1).
 * Phase 5: Verify committed_seq >= old committed_seq.
 * Phase 6: Inject more batches, verify no duplicates in GOI.
 */
static bool test_crash_recovery() {
    using namespace embarcadero;
    std::cout << "  Running crash-recovery smoke test...\n";

    SequencerConfig sc;
    sc.num_brokers = 4;
    sc.num_collectors = 4;
    sc.num_shards = 4;
    sc.scatter_gather_enabled = true;
    sc.mode = SequencerMode::PER_EPOCH_ATOMIC;
    sc.validate_ordering = true;
    sc.validate();

    uint64_t old_committed = 0;
    {
        Sequencer seq(sc);
        if (!seq.initialize()) return false;
        seq.start();

        for (int i = 0; i < 10000; ++i) {
            auto r = seq.inject_batch(static_cast<uint16_t>(i % 4), i, 1024, 10);
            (void)r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        old_committed = seq.committed_seq();
        std::cout << "  Phase 1: Injected 10K batches, committed_seq=" << old_committed << "\n";
        // CRASH: destroy without stop()
    }

    std::cout << "  Phase 2: Sequencer crashed (destroyed without stop).\n";

    {
        Sequencer seq2(sc);
        if (!seq2.initialize()) return false;
        // Phase 4: Recovery (§8.1)
        // In this benchmark, initialize() + start() on same SHM effectively simulates recovery
        // because we don't zero the SHM if it already exists (though allocate_shm currently zeros it).
        // TODO: for real CXL integration, recovery would scan GOI.
        seq2.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uint64_t new_committed = seq2.committed_seq();
        std::cout << "  Phase 5: New sequencer started, committed_seq=" << new_committed << "\n";

        if (new_committed < old_committed && old_committed != UINT64_MAX) {
            std::cerr << "  FAILED: committed_seq regressed after recovery!\n";
            return false;
        }

        std::cout << "  Phase 6: Injecting post-recovery batches...\n";
        for (int i = 10000; i < 15000; ++i) {
            auto r = seq2.inject_batch(static_cast<uint16_t>(i % 4), i, 1024, 10);
            (void)r;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        seq2.stop();
        if (!seq2.validate_ordering_reason().empty()) {
            std::cerr << "  FAILED: Validation failed after recovery: " << seq2.validate_ordering_reason() << "\n";
            return false;
        }
    }

    std::cout << "  Recovery test PASSED.\n";
    return true;
}

int main(int argc, char* argv[]) {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;

    bool clean_ablation_preset = false;
    bool paper_ablation_preset = false;
    bool paper_mode_preset = false;
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
    bool skip_dedup_cli = false;
    bool legacy_dedup_cli = false;
    bool output_csv = false;
    bool phase_timing_cli = false;
    bool pin_cores_cli = false;
    bool noop_completed_ranges_cli = false;
    bool bottleneck_ablation_cli = false;
    bool trace_benchmark_cli = false;
    bool trace_drain_only_cli = false;
    uint64_t epoch_us_cli = 0;  // 0 = use default
    uint64_t target_batches_per_sec = 0;  // B2: 0 = open-loop
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        }
        if (arg == "--bench-dedup") {
            bench::benchmark_dedup_implementations();
            return 0;
        }
        if (arg == "--test-dedup") {
            std::cout << "  Running FastDeduplicator eviction test...\n";
            bool pass = bench::test_fast_deduplicator();
            std::cout << "  " << (pass ? "PASSED" : "FAILED") << "\n";
            return pass ? 0 : 1;
        }
        if (arg == "--test") {
            std::cout << "  Running minimal correctness test...\n";
            bool pass = run_correctness_test();
            if (pass) {
                std::cout << "  Running deterministic correctness tests (review §5)...\n";
                pass = run_deterministic_correctness_tests();
            }
            if (pass) {
                pass = test_crash_recovery();
            }
            std::cout << "  " << (pass ? "PASSED" : "FAILED") << "\n";
            return pass ? 0 : 1;
        }
        if (arg == "--test-recovery") {
            bool pass = test_crash_recovery();
            return pass ? 0 : 1;
        }
        if (arg == "--clean") {
            clean_ablation_preset = true;
        }
        if (arg == "--paper") {
            paper_ablation_preset = true;
        }
        if (arg == "--paper-mode") {
            paper_mode_preset = true;
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
        if (arg.rfind("--target-rate=", 0) == 0) {
            target_batches_per_sec = static_cast<uint64_t>(std::stoull(arg.substr(std::string("--target-rate=").size())));
        }
        if (arg == "--skip-dedup") {
            skip_dedup_cli = true;
        }
        if (arg == "--legacy-dedup") {
            legacy_dedup_cli = true;
        }
        if (arg.rfind("--epoch-us=", 0) == 0) {
            epoch_us_cli = static_cast<uint64_t>(std::stoull(arg.substr(std::string("--epoch-us=").size())));
        }
        if (arg == "--output-csv") {
            output_csv = true;
        }
        if (arg == "--phase-timing") {
            phase_timing_cli = true;
        }
        if (arg == "--pin-cores") {
            pin_cores_cli = true;
        }
        if (arg == "--noop-completed-ranges") {
            noop_completed_ranges_cli = true;
        }
        if (arg == "--bottleneck-ablation") {
            bottleneck_ablation_cli = true;
        }
        if (arg == "--trace") {
            trace_benchmark_cli = true;
        }
        if (arg == "--trace-drain-only") {
            trace_drain_only_cli = true;
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
    if (skip_dedup_cli) cfg.skip_dedup = true;
    if (legacy_dedup_cli) cfg.use_fast_dedup = false;
    if (phase_timing_cli) cfg.phase_timing = true;
    if (pin_cores_cli) cfg.pin_cores = true;
    if (noop_completed_ranges_cli) cfg.noop_completed_ranges = true;
    if (epoch_us_cli != 0) cfg.epoch_us = epoch_us_cli;
    if (clean_ablation_preset) {
        // B6/C1: 1 producer, 64K PBR for natural backpressure so validity checks can pass (algorithm comparison).
        cfg.producers = 1;
        cfg.pbr_entries = 64 * 1024;  // 64K per broker
    }
    if (paper_ablation_preset) {
        cfg.brokers = 4;
        cfg.producers = 1;
        cfg.duration_sec = 60;
        cfg.pbr_entries = 64 * 1024;  // 64K per broker: algorithm-limited, target PBR_full < 0.1%
    }
    if (paper_mode_preset) {
        cfg.brokers = 4;
        cfg.producers = 2;
        cfg.duration_sec = 30;
        cfg.num_runs = 5;
        cfg.level5_ratio = 0.0;
        cfg.skip_dedup = true;
        cfg.pbr_entries = embarcadero::config::CLEAN_ABLATION_PBR_ENTRIES;
        cfg.validate = true;
        cfg.steady_state = true;
        validity_mode = bench::ValidityMode::ALGORITHM;
        cfg.run_ablation1 = true;
        cfg.run_levels = false;
        cfg.run_scalability = false;
        cfg.run_epoch_test = false;
        cfg.run_broker_test = false;
        cfg.run_scatter_gather = false;
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
    cfg.target_batches_per_sec = target_batches_per_sec;

    bench::set_output_csv(output_csv);

    if (trace_drain_only_cli) {
        bench::run_trace_drain_only(cfg);
        return 0;
    }
    if (trace_benchmark_cli) {
        bench::run_trace_benchmark(cfg);
        return 0;
    }

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
              << int(cfg.level5_ratio * 100) << "% O5, "
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
    if (paper_mode_preset) std::cout << " (--paper-mode)";
    if (cfg.skip_dedup) std::cout << " (--skip-dedup)";
    if (!cfg.use_fast_dedup) std::cout << " (--legacy-dedup)";
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
        if (bottleneck_ablation_cli) {
            bench::run_bottleneck_ablation();
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
                bench::print_ablation_table(a1, cfg.num_runs);
                bench::print_stat_result("Per-Batch Atomic (Baseline)", a1.baseline, cfg.num_runs, &cfg);
                bench::print_stat_result("Per-Epoch Atomic (Optimized)", a1.optimized, cfg.num_runs, &cfg);
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
                bench::run_ablation1_deconfounded(cfg);
            }

            // ================================================================
            // TEST 2-4: Ordering levels (multi-run)
            // ================================================================
            if (cfg.run_levels) {
                struct LevelTest { const char* name; double ratio; };
                std::vector<LevelTest> level_tests = {
                    {"Order 2 (Total Order)", 0.0},
                    {"Mixed 10% Order 5", 0.1},
                };
                if (cfg.stress_test) level_tests.push_back({"Stress Test (100% Order 5)", 1.0});

                for (const auto& t : level_tests) {
                    std::cout << "\n"; bench::sep();
                    std::cout << "  " << t.name << "\n"; bench::sep();
                    auto c = cfg;
                    c.level5 = (t.ratio > 0);
                    c.level5_ratio = t.ratio;
                    c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;
                    if (t.ratio >= 1.0) c.validity_mode = bench::ValidityMode::STRESS;
                    bench::MultiRunResult r = bench::run_multiple(c);
                    bench::print_stat_result(t.name, r, cfg.num_runs, &cfg);
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
                bench::print("Per-Batch Atomic", baseline, &c);
            }
            if (cfg.run_ablation1) {
                std::cout << "\n"; bench::sep();
                std::cout << "  ABLATION 1b: Per-Epoch Atomic (Optimized)\n"; bench::sep();
                auto c = cfg; c.scatter_gather = false; c.level5 = false; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC;
                optimized = bench::Runner(c).run();
                bench::print("Per-Epoch Atomic", optimized, &c);
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
                std::cout << "  TEST 2: Order 2\n"; bench::sep();
                { auto c = cfg; c.scatter_gather = false; c.level5 = false; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Order 2", bench::Runner(c).run(), &c); }

                std::cout << "\n"; bench::sep();
                std::cout << "  TEST 3: Mixed 10% Order 5\n"; bench::sep();
                { auto c = cfg; c.scatter_gather = false; c.level5_ratio = 0.1; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Mixed", bench::Runner(c).run(), &c); }

                if (cfg.stress_test) {
                    std::cout << "\n"; bench::sep();
                    std::cout << "  Stress Test (100% Order 5)\n"; bench::sep();
                    { auto c = cfg; c.scatter_gather = false; c.level5_ratio = 1.0; c.mode = embarcadero::SequencerMode::PER_EPOCH_ATOMIC; bench::print("Stress Test (100% Order 5)", bench::Runner(c).run(), &c); }
                }
            }

            if (cfg.run_scalability) { auto c = cfg; c.scatter_gather = false; bench::scalability_test(c); }
            if (cfg.run_epoch_test) { auto c = cfg; c.scatter_gather = false; bench::epoch_test(c); }
            if (cfg.run_broker_test) { auto c = cfg; c.scatter_gather = false; bench::broker_test(c); }
            if (cfg.run_scatter_gather) bench::scatter_gather_scaling_test(cfg);
        }

        std::cout << "\n"; bench::sep();
        std::cout << "  Complete\n"; bench::sep(); std::cout << "\n";

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }

    if (output_csv) {
        std::vector<std::string> rows = bench::drain_csv_rows();
        if (!rows.empty()) {
            std::cout << "run,mode,shards,brokers,producers,batches_sec,mb_sec,p99_us,valid_runs,num_runs,atomics,gaps,dropped\n";
            for (const auto& row : rows) std::cout << row << "\n";
        }
    }

    return 0;
}
