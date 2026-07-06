// ============================================================================
// Embarcadero — CXL non-coherence fault-injection controller  (Track 04 / W6)
// Implementation. See cxl_fault_inject.h for the design contract and the
// circularity caveat.
// ============================================================================
#include "faultinj/cxl_fault_inject.h"

#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

#ifdef __x86_64__
#include <immintrin.h>
#endif

namespace Embarcadero {
namespace faultinj {

// ---- global state ---------------------------------------------------------
std::atomic<uint32_t> g_active_mask{kNone};

namespace {

// CONFIG THREADING CONTRACT: g_params is a plain global. SetMask/SetParams must
// be called only during single-threaded setup, before any injected CXL::* access
// runs (the harness configures each scenario before spawning its threads and
// after joining the previous one). Live reconfiguration while accessors run is a
// non-modeled race on the config itself and is unsupported.
Params g_params;
std::once_flag g_init_flag;

constexpr uint32_t kPpmScale = 1000000u;  // parts-per-million denominator

// ---- statistics -----------------------------------------------------------
// The two hottest counters get their own cache line to avoid false sharing with
// the cold counters (and with each other) when many threads bump them.
struct AtomicStats {
    alignas(64) std::atomic<uint64_t> flushes{0};
    alignas(64) std::atomic<uint64_t> acquire_loads{0};
    std::atomic<uint64_t> deferred_flushes{0};
    std::atomic<uint64_t> drains{0};
    std::atomic<uint64_t> drained_lines{0};
    std::atomic<uint64_t> pending_overflows{0};
    std::atomic<uint64_t> stale_served{0};
    std::atomic<uint64_t> invalidations_skip{0};
    std::atomic<uint64_t> torn_gaps{0};
    std::atomic<uint64_t> checks_ok{0};
    std::atomic<uint64_t> checks_violated{0};
};
AtomicStats g_stats;

inline void bump(std::atomic<uint64_t>& c, uint64_t n = 1) {
    c.fetch_add(n, std::memory_order_relaxed);
}

// ---- event log ------------------------------------------------------------
std::mutex g_log_mu;
FILE* g_log = nullptr;  // nullptr => stderr

FILE* log_sink() { return g_log ? g_log : stderr; }

// ---- CPU relax (local; this TU intentionally does not include the CXL seam) -
inline void cpu_relax() {
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

// ---- per-thread deterministic RNG ----------------------------------------
std::atomic<uint32_t> g_thread_counter{0};
// Bumped (release) by SetParams so a thread that already seeded re-seeds with the
// new seed; rng_state() loads it (acquire) so the new g_params.seed is visible.
std::atomic<uint64_t> g_seed_gen{0};

uint32_t thread_index() {
    static thread_local uint32_t idx = g_thread_counter.fetch_add(1, std::memory_order_relaxed);
    return idx;
}

uint64_t& rng_state() {
    static thread_local uint64_t s = 0;
    static thread_local uint64_t seen_gen = ~0ull;
    uint64_t gen = g_seed_gen.load(std::memory_order_acquire);
    if (seen_gen != gen) {
        s = g_params.seed ^ (0x9e3779b97f4a7c15ULL * (uint64_t(thread_index()) + 1));
        if (s == 0) s = 0x1234567deadbeefULL;
        seen_gen = gen;
    }
    return s;
}

uint64_t next_rand() {
    uint64_t& s = rng_state();  // one TLS/atomic touch per draw
    uint64_t x = s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    s = x;
    return x;
}

// Division-free Bernoulli(ppm/1e6) via a multiply-shift reduction of the RNG.
bool coin_ppm(uint32_t ppm) {
    if (ppm == 0) return false;
    if (ppm >= kPpmScale) return true;
    uint32_t sample = static_cast<uint32_t>(((next_rand() >> 32) * uint64_t(kPpmScale)) >> 32);
    return sample < ppm;
}

// ---- physical cache-line flush --------------------------------------------
// Raw arch body ONLY. MUST NOT call CXL::flush_cacheline here: that seam calls
// MaybeTornGap + InterceptFlush first, so delegating would re-enter InterceptFlush,
// re-append to tls_pending, and recurse / never physically flush. Kept in sync
// with CXL::flush_cacheline's real body in common/performance_utils.h by hand.
inline void physical_flush(const void* addr) {
#ifdef __x86_64__
    _mm_clflushopt(const_cast<void*>(addr));
#elif defined(__aarch64__)
    const uintptr_t a = reinterpret_cast<uintptr_t>(addr) & ~63UL;
    __builtin___clear_cache(reinterpret_cast<char*>(a), reinterpret_cast<char*>(a + 64));
#else
    (void)addr;
#endif
}

// ---- delayed-flush: per-thread deferred set -------------------------------
constexpr size_t kMaxPending = 256;
struct PendingBuf {
    const void* lines[kMaxPending];
    size_t n = 0;
};
thread_local PendingBuf tls_pending;

void drain_pending() {
    PendingBuf& p = tls_pending;
    if (p.n == 0) return;
    // Reorder the drain to model clflushopt's weak ordering (Fisher-Yates).
    uint64_t& s = rng_state();
    for (size_t i = p.n; i > 1; --i) {
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        size_t j = static_cast<size_t>(s % i);
        const void* tmp = p.lines[i - 1];
        p.lines[i - 1] = p.lines[j];
        p.lines[j] = tmp;
    }
    for (size_t i = 0; i < p.n; ++i) physical_flush(p.lines[i]);
    bump(g_stats.drains);
    bump(g_stats.drained_lines, p.n);
    p.n = 0;
}

// ---- stale-read: fixed-size, lock-free, direct-mapped bounded-staleness ----
// A fixed table (no mutex, no allocation, bounded memory) keyed by cache-line
// address. Cross-thread updates to a slot are racy by construction, but all
// fields are atomic (no torn/UB) and mis-attribution on a collision is
// acceptable for a probabilistic fault model. This is the hot read path.
struct StaleSlot {
    std::atomic<const void*> key{nullptr};
    std::atomic<uint64_t>    last_live{0};
    std::atomic<uint64_t>    stale_val{0};
    std::atomic<uint32_t>    budget{0};
};
constexpr size_t kStaleSlots = 4096;
static_assert((kStaleSlots & (kStaleSlots - 1)) == 0, "kStaleSlots must be a power of two");
StaleSlot g_stale[kStaleSlots];

inline size_t stale_index(const void* addr) {
    uintptr_t h = reinterpret_cast<uintptr_t>(addr);
    h ^= h >> 20;                          // mix high bits down
    return (h >> 6) & (kStaleSlots - 1);   // >>6: index by cache line
}

// ---- env parsing ----------------------------------------------------------
uint32_t parse_mode(const char* s) {
    if (!s || !*s) return kNone;
    uint32_t mask = kNone;
    std::string in(s);
    size_t start = 0;
    while (start <= in.size()) {
        size_t comma = in.find(',', start);
        std::string tok = in.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        size_t b = tok.find_first_not_of(" \t");
        size_t e = tok.find_last_not_of(" \t");
        if (b != std::string::npos) tok = tok.substr(b, e - b + 1);
        for (auto& c : tok) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (tok == "off" || tok == "none" || tok.empty()) {
            // no-op
        } else if (tok == "all") {
            mask |= kAll;
        } else if (tok == "delayed_flush" || tok == "delayed-flush" || tok == "flush") {
            mask |= kDelayedFlush;
        } else if (tok == "stale_read" || tok == "stale-read" || tok == "stale") {
            mask |= kStaleRead;
        } else if (tok == "torn_window" || tok == "torn-window" || tok == "torn") {
            mask |= kTornWindow;
        } else {
            fprintf(stderr, "[CXL_FI] unknown CXL_FI_MODE token: '%s'\n", tok.c_str());
        }
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return mask;
}

uint64_t getenv_num(const char* name, uint64_t dflt, uint64_t max) {
    const char* v = getenv(name);
    if (!v || !*v) return dflt;
    char* end = nullptr;
    errno = 0;
    unsigned long long r = strtoull(v, &end, 0);
    if (end == v || *end != '\0' || errno == ERANGE || r > max) {
        fprintf(stderr, "[CXL_FI] invalid %s='%s', using default %llu\n",
                name, v, (unsigned long long)dflt);
        return dflt;
    }
    return static_cast<uint64_t>(r);
}
uint32_t getenv_u32(const char* name, uint32_t dflt) {
    return static_cast<uint32_t>(getenv_num(name, dflt, 0xffffffffULL));
}
uint64_t getenv_u64(const char* name, uint64_t dflt) {
    return getenv_num(name, dflt, ~0ULL);
}

}  // namespace

// ---------------------------------------------------------------------------
// Lifecycle / configuration
// ---------------------------------------------------------------------------
void InitFromEnvOnce() {
    std::call_once(g_init_flag, [] {
        uint32_t mask = parse_mode(getenv("CXL_FI_MODE"));

        g_params.seed            = getenv_u64("CXL_FI_SEED", 0x5eedULL);
        // Defaults chosen so that merely selecting a mode does something visible.
        g_params.flush_defer_ppm = getenv_u32("CXL_FI_FLUSH_DEFER_PPM", (mask & kDelayedFlush) ? 500000u : 0u);
        g_params.stale_ppm       = getenv_u32("CXL_FI_STALE_PPM",       (mask & kStaleRead)    ? 300000u : 0u);
        g_params.stale_window    = getenv_u32("CXL_FI_STALE_WINDOW",    (mask & kStaleRead)    ? 8u      : 0u);
        g_params.torn_ppm        = getenv_u32("CXL_FI_TORN_PPM",        (mask & kTornWindow)   ? 200000u : 0u);
        // Cap the gap well under 1s: it is a busy-spin deadline, not a sleep.
        g_params.torn_gap_ns     = getenv_u32("CXL_FI_TORN_GAP_NS",     (mask & kTornWindow)   ? 2000u   : 0u);
        if (g_params.torn_gap_ns > 1000000u) g_params.torn_gap_ns = 1000000u;  // <= 1ms
        g_params.abort_on_violation = getenv_u32("CXL_FI_ABORT_ON_VIOLATION", 0u) != 0u;

        const char* logpath = getenv("CXL_FI_LOG");
        if (logpath && *logpath) {
            FILE* f = fopen(logpath, "a");
            if (f) {
                g_log = f;
            } else {
                fprintf(stderr, "[CXL_FI] could not open CXL_FI_LOG='%s', using stderr\n", logpath);
            }
        }

        g_active_mask.store(mask, std::memory_order_release);

        if (mask != kNone) {
            fprintf(log_sink(),
                    "[CXL_FI] ENABLED mask=0x%x seed=0x%llx flush_defer_ppm=%u "
                    "stale_ppm=%u stale_window=%u torn_ppm=%u torn_gap_ns=%u abort=%d\n",
                    mask, (unsigned long long)g_params.seed, g_params.flush_defer_ppm,
                    g_params.stale_ppm, g_params.stale_window, g_params.torn_ppm,
                    g_params.torn_gap_ns, (int)g_params.abort_on_violation);
            fflush(log_sink());
        }
    });
}

void SetMask(uint32_t mask) { g_active_mask.store(mask, std::memory_order_release); }
void SetParams(const Params& p) {
    g_params = p;
    g_seed_gen.fetch_add(1, std::memory_order_release);  // publish + force RNG re-seed
}
const Params& GetParams() { return g_params; }

// ---------------------------------------------------------------------------
// Seam hooks
// ---------------------------------------------------------------------------
bool InterceptFlush(const void* addr) {
    bump(g_stats.flushes);
    if (!Enabled(kDelayedFlush)) return false;
    if (!coin_ppm(g_params.flush_defer_ppm)) return false;

    PendingBuf& p = tls_pending;
    if (p.n >= kMaxPending) {
        // Deferred set is full without a fence draining it. On a correct design a
        // store_fence()/full_fence() drains long before this. Overflow signals a
        // *missing fence* on some path — flush everything and record it.
        bump(g_stats.pending_overflows);
        drain_pending();
    }
    p.lines[p.n++] = addr;
    bump(g_stats.deferred_flushes);
    return true;  // caller must NOT issue the physical flush now
}

void DrainDeferredFlushes() {
    if (!Enabled(kDelayedFlush)) return;
    drain_pending();
}

bool InterceptInvalidateForRead(const void* addr) {
    (void)addr;
    if (!Enabled(kStaleRead)) return false;
    if (!coin_ppm(g_params.stale_ppm)) return false;  // reuses stale_ppm intentionally
    bump(g_stats.invalidations_skip);
    return true;  // caller must NOT invalidate (reader keeps stale line)
}

uint64_t MaybeStale(const void* addr, uint64_t live) {
    if (!Enabled(kStaleRead)) return live;  // guard first, then count
    bump(g_stats.acquire_loads);

    StaleSlot& slot = g_stale[stale_index(addr)];
    if (slot.key.load(std::memory_order_relaxed) != addr) {
        // First touch or collision-eviction: (re)claim the slot; treat as first sight.
        slot.key.store(addr, std::memory_order_relaxed);
        slot.last_live.store(live, std::memory_order_relaxed);
        slot.budget.store(0, std::memory_order_relaxed);
        return live;
    }
    uint64_t last = slot.last_live.load(std::memory_order_relaxed);
    if (last != live) {
        // A change just became visible; remember the prior value as "stale".
        slot.stale_val.store(last, std::memory_order_relaxed);
        slot.budget.store(g_params.stale_window, std::memory_order_relaxed);
        slot.last_live.store(live, std::memory_order_relaxed);
    }
    uint32_t b = slot.budget.load(std::memory_order_relaxed);
    if (b > 0 && coin_ppm(g_params.stale_ppm)) {
        slot.budget.store(b - 1, std::memory_order_relaxed);
        bump(g_stats.stale_served);
        return slot.stale_val.load(std::memory_order_relaxed);  // serve older value
    }
    return live;
}

void MaybeTornGap(const void* addr) {
    (void)addr;
    if (!Enabled(kTornWindow)) return;
    if (!coin_ppm(g_params.torn_ppm)) return;
    bump(g_stats.torn_gaps);
    uint32_t ns = g_params.torn_gap_ns;
    if (ns == 0) return;
    // Busy-spin to a deadline. nanosleep cannot deliver sub-microsecond gaps
    // (deschedule + timer slack overshoot by 10-100x); a spin widens the window
    // by ~ns without yielding, keeping the injected timing faithful.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::nanoseconds(ns);
    while (std::chrono::steady_clock::now() < deadline) cpu_relax();
}

// ---------------------------------------------------------------------------
// Invariant reporting
// ---------------------------------------------------------------------------
void RecordCheck(const char* adversary, const char* check, bool ok) {
    if (ok) {
        bump(g_stats.checks_ok);
    } else {
        bump(g_stats.checks_violated);
        std::lock_guard<std::mutex> g(g_log_mu);
        fprintf(log_sink(), "[CXL_FI] VIOLATION adversary=%s check=%s\n",
                adversary ? adversary : "?", check ? check : "?");
        fflush(log_sink());
        if (g_params.abort_on_violation) abort();
    }
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------
Stats SnapshotStats() {
    Stats s;
    s.flushes            = g_stats.flushes.load(std::memory_order_relaxed);
    s.deferred_flushes   = g_stats.deferred_flushes.load(std::memory_order_relaxed);
    s.drains             = g_stats.drains.load(std::memory_order_relaxed);
    s.drained_lines      = g_stats.drained_lines.load(std::memory_order_relaxed);
    s.pending_overflows  = g_stats.pending_overflows.load(std::memory_order_relaxed);
    s.acquire_loads      = g_stats.acquire_loads.load(std::memory_order_relaxed);
    s.stale_served       = g_stats.stale_served.load(std::memory_order_relaxed);
    s.invalidations_skip = g_stats.invalidations_skip.load(std::memory_order_relaxed);
    s.torn_gaps          = g_stats.torn_gaps.load(std::memory_order_relaxed);
    s.checks_ok          = g_stats.checks_ok.load(std::memory_order_relaxed);
    s.checks_violated    = g_stats.checks_violated.load(std::memory_order_relaxed);
    return s;
}

void ResetStats() {
    g_stats.flushes.store(0);
    g_stats.deferred_flushes.store(0);
    g_stats.drains.store(0);
    g_stats.drained_lines.store(0);
    g_stats.pending_overflows.store(0);
    g_stats.acquire_loads.store(0);
    g_stats.stale_served.store(0);
    g_stats.invalidations_skip.store(0);
    g_stats.torn_gaps.store(0);
    g_stats.checks_ok.store(0);
    g_stats.checks_violated.store(0);
    // Also clear the stale-read tracking state so a reset is a true clean slate.
    for (auto& slot : g_stale) {
        slot.key.store(nullptr, std::memory_order_relaxed);
        slot.budget.store(0, std::memory_order_relaxed);
    }
}

void DumpSummary(const char* tag) {
    Stats s = SnapshotStats();
    std::lock_guard<std::mutex> g(g_log_mu);
    FILE* out = log_sink();
    fprintf(out, "[CXL_FI] SUMMARY %s mask=0x%x\n", tag ? tag : "", g_active_mask.load());
    fprintf(out, "  flushes=%llu deferred=%llu drains=%llu drained_lines=%llu overflows=%llu\n",
            (unsigned long long)s.flushes, (unsigned long long)s.deferred_flushes,
            (unsigned long long)s.drains, (unsigned long long)s.drained_lines,
            (unsigned long long)s.pending_overflows);
    fprintf(out, "  acquire_loads=%llu stale_served=%llu invalidations_skipped=%llu torn_gaps=%llu\n",
            (unsigned long long)s.acquire_loads, (unsigned long long)s.stale_served,
            (unsigned long long)s.invalidations_skip, (unsigned long long)s.torn_gaps);
    fprintf(out, "  checks_ok=%llu checks_violated=%llu\n",
            (unsigned long long)s.checks_ok, (unsigned long long)s.checks_violated);
    fflush(out);
}

// ---------------------------------------------------------------------------
// Auto-initialise from the environment when this TU is linked into a process,
// and close the event log at exit.
// ---------------------------------------------------------------------------
namespace {
struct AutoInit {
    AutoInit() { InitFromEnvOnce(); }
    ~AutoInit() {
        std::lock_guard<std::mutex> g(g_log_mu);
        if (g_log) { fflush(g_log); fclose(g_log); g_log = nullptr; }
    }
};
AutoInit g_auto_init;
}  // namespace

}  // namespace faultinj
}  // namespace Embarcadero
