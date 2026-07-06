// ============================================================================
// Embarcadero — CXL non-coherence fault-injection controller  (Track 04 / W6)
// ============================================================================
//
// This header declares the interposition controller that perturbs the CXL
// accessor seam (Embarcadero::CXL::* in common/performance_utils.h) so the full
// pipeline can be exercised under adversarial non-coherence schedules:
//
//   * delayed-flush : defer/reorder clflushopt visibility (expose missing flush)
//   * stale-read    : serve a reader an older value within a bounded window
//   * torn-window   : widen the gap between multi-cacheline flushes so a
//                     concurrent reader can observe a half-updated record
//
// DESIGN CONTRACT
//   - When the CMake flag CXL_FAULT_INJECTION is *not* defined, none of this is
//     compiled into the tree: performance_utils.h guards every include/hook with
//     `#ifdef CXL_FAULT_INJECTION`, so production builds are byte-for-byte the
//     same as before. This layer never edits protocol logic; it only substitutes
//     the accessor implementation behind the existing seam.
//
// CIRCULARITY CAVEAT (state this everywhere — see docs/experiments/fault_injection.md)
//   Injection validates the design against *the assumptions it was built on*. It
//   cannot, by construction, surface an unmodeled hardware behavior. It is a
//   supplement to — never a substitute for — the in-silicon RDMA evidence
//   (Track 05) and the TLA+ model (Track 02). A fresh reviewer will correctly
//   read a green result here as "validated in model and injection," so every
//   result must be paired with that silicon/model evidence.
//
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace Embarcadero {
namespace faultinj {

// --------------------------------------------------------------------------
// Adversary bit flags (an active-mask combines them; kAll enables all three).
// --------------------------------------------------------------------------
enum Adversary : uint32_t {
    kNone         = 0u,
    kDelayedFlush = 1u << 0,
    kStaleRead    = 1u << 1,
    kTornWindow   = 1u << 2,
    kAll          = kDelayedFlush | kStaleRead | kTornWindow,
};

// Fast-path enable mask. Loaded once from the environment (or overridden by the
// harness via SetMask). The inline seam hooks read this with relaxed ordering
// and take the slow (out-of-line) path only when the relevant bit is set.
extern std::atomic<uint32_t> g_active_mask;

inline bool Enabled(uint32_t adv) {
    return (g_active_mask.load(std::memory_order_relaxed) & adv) != 0u;
}

// --------------------------------------------------------------------------
// Lifecycle / configuration
// --------------------------------------------------------------------------

// Parse configuration from the environment exactly once (idempotent, thread-safe).
// Recognised variables (all optional):
//   CXL_FI_MODE               off|delayed_flush|stale_read|torn_window|all
//                             (comma-separated list also accepted, e.g.
//                              "stale_read,torn_window")
//   CXL_FI_SEED               uint64 RNG seed (default 0x5eed)
//   CXL_FI_FLUSH_DEFER_PPM    delayed-flush: P(defer a flush), parts-per-million
//   CXL_FI_STALE_PPM          stale-read:    P applied to BOTH stale draws — the
//                                            per-read serve (in MaybeStale) AND the
//                                            invalidation-skip (InterceptInvalidateForRead).
//                                            A readiness poll that does both per spin
//                                            therefore sees a compounded (~ppm^2) effect.
//   CXL_FI_STALE_WINDOW       stale-read:    max #reads a stale value is served
//                                            after a change is observed
//   CXL_FI_TORN_PPM           torn-window:   P(inject a gap on a flush)
//   CXL_FI_TORN_GAP_NS        torn-window:   busy-spin nanoseconds to widen the gap
//                                            (spin, not sleep; capped at 1ms)
//   CXL_FI_LOG                path to an event log (appended); empty = stderr summary only
//   CXL_FI_ABORT_ON_VIOLATION 1 = abort() on an asserted invariant violation
void InitFromEnvOnce();

// Runtime overrides used by the in-vitro harness (bypass the environment).
struct Params {
    uint64_t seed              = 0x5eedULL;
    uint32_t flush_defer_ppm   = 0;
    uint32_t stale_ppm         = 0;
    uint32_t stale_window      = 0;
    uint32_t torn_ppm          = 0;
    uint32_t torn_gap_ns       = 0;   // busy-spin gap; capped at 1ms
    bool     abort_on_violation = false;
};
// THREADING CONTRACT: SetMask/SetParams (and GetParams) are for single-threaded
// setup only — call them before any injected CXL::* access runs (e.g. the harness
// configures a scenario before spawning its threads and after joining the prior
// one). They mutate a plain global; calling them concurrently with live accessors
// is a non-modeled race on the config itself and is unsupported.
void SetMask(uint32_t mask);
void SetParams(const Params& p);
const Params& GetParams();

// --------------------------------------------------------------------------
// Seam hooks — invoked from Embarcadero::CXL::* in performance_utils.h.
// Each is a no-op unless its adversary bit is set in g_active_mask.
// --------------------------------------------------------------------------

// delayed-flush: called from CXL::flush_cacheline(). Returns true if the physical
// flush was *deferred* (caller must NOT issue the real clflushopt); false to flush
// normally. Deferred lines drain (shuffled) at the next fence. NOTE: on a single
// coherent host, deferring a physical flush does not itself make a reader see
// stale data; the observable signal is a *missing fence* — a thread issuing
// >kMaxPending flushes with no intervening fence trips a pending-buffer overflow.
bool InterceptFlush(const void* addr);

// Drain any deferred flushes for this thread. Called from CXL::store_fence()/
// full_fence() *before* the caller issues the real fence, so a correct design
// stays correct (the fence orders prior clflushopt).
void DrainDeferredFlushes();

// stale-read: called from CXL::invalidate_cacheline_for_read(). Returns true if
// the invalidation should be *skipped* (model: reader keeps its stale line).
// Gated by stale_ppm (see CXL_FI_STALE_PPM note above re: compounding).
bool InterceptInvalidateForRead(const void* addr);

// stale-read: called from CXL::acquire_load(). `live` is the freshly-loaded scalar
// (<=8B) zero-extended into a uint64. Returns either `live` or a previously-observed
// value, per the bounded-staleness model. Reaches ONLY loads a call site has been
// converted to CXL::acquire_load; raw __atomic_load_n polls are not perturbed.
uint64_t MaybeStale(const void* addr, uint64_t live);

// torn-window: called from CXL::flush_cacheline(). Busy-spins to widen the timing
// window in which a concurrent reader can observe a multi-cacheline record between
// its two line writes. NOTE: this is a timing amplifier for a store-publish-ORDER
// bug (commit visible before payload); on a coherent host it does not produce a
// physically half-flushed line, and the ordering bug it exposes is observable even
// with torn_ppm=0 (the gap only raises the catch probability). No-op unless kTornWindow.
void MaybeTornGap(const void* addr);

// --------------------------------------------------------------------------
// Invariant reporting (used by the harness and by opt-in in-vivo checks).
// --------------------------------------------------------------------------

// Record that an invariant held / was violated for a named adversary+check.
// Increments counters and (if CXL_FI_ABORT_ON_VIOLATION) aborts on violation.
void RecordCheck(const char* adversary, const char* check, bool ok);

// --------------------------------------------------------------------------
// Statistics
// --------------------------------------------------------------------------
struct Stats {
    uint64_t flushes            = 0;  // CXL::flush_cacheline calls seen
    uint64_t deferred_flushes   = 0;  // flushes deferred by delayed-flush
    uint64_t drains             = 0;  // fence-triggered drains
    uint64_t drained_lines      = 0;  // physical flushes issued at drain
    uint64_t pending_overflows  = 0;  // deferred buffer overflow (missing fence!)
    uint64_t acquire_loads      = 0;  // CXL::acquire_load calls seen (stale-read on)
    uint64_t stale_served       = 0;  // reads served a stale value
    uint64_t invalidations_skip = 0;  // invalidations suppressed
    uint64_t torn_gaps          = 0;  // torn-window gaps injected
    uint64_t checks_ok          = 0;  // invariant checks that held
    uint64_t checks_violated    = 0;  // invariant checks that failed
};
Stats SnapshotStats();
void   ResetStats();

// Emit a human-readable summary (to CXL_FI_LOG if set, else stderr).
void DumpSummary(const char* tag);

}  // namespace faultinj
}  // namespace Embarcadero
