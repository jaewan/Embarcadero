// ============================================================================
// Embarcadero — in-vitro CXL non-coherence fault-injection harness (Track 04/W6)
// ============================================================================
//
// A standalone, cluster-free driver that reconstructs the exact CXL access
// patterns the protocol relies on (a two-cache-line record like BatchHeader, a
// sequencer readiness poll) and runs them under the three adversaries with a
// concurrent writer/reader. Because the harness owns *both* sides of every
// access, it can genuinely produce stale and torn reads on a single *coherent*
// host — something the live pipeline cannot do without real cross-host memory.
//
// For each adversary it runs a matched pair:
//   * a CORRECT protocol (proper flush / fence / publish-commit-last / re-poll)
//     which must survive the adversary  -> "confirmed-safe"
//   * a BUGGY protocol (missing fence / commit-before-payload / read-once) which
//     the adversary must expose         -> "caught"
// The test fails (nonzero exit) if either expectation is unmet, so it doubles as
// a self-test that the harness actually bites.
//
// CIRCULARITY CAVEAT: injection validates the design against the assumptions it
// was built on; it cannot surface an unmodeled hardware behavior. Pair every
// result with the Track 05 silicon evidence and the Track 02 TLA+ model.
//
// Needs no testbed lock (no /dev/shm, no ports, no CXL segment).
//
// Build: only compiled when -DCXL_FAULT_INJECTION=ON.
// Run:   ./faultinj_harness [iterations]     (default 200000)
// ============================================================================
#include "common/performance_utils.h"
#include "faultinj/cxl_fault_inject.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace fi = Embarcadero::faultinj;
namespace cxl = Embarcadero::CXL;

namespace {

// Faithful mirror of the readiness-critical shape of BatchHeader: 128 bytes,
// payload + guard in cache line 0, the publish-commit token in cache line 1.
struct alignas(64) TwoLineRecord {
    // ---- cache line 0 : payload + guard --------------------------------
    volatile uint64_t magic;            // +0
    volatile uint64_t seq;              // +8   (stands in for total_order)
    volatile uint64_t payload_a;        // +16
    volatile uint64_t payload_b;        // +24
    volatile uint64_t checksum;         // +32  guard over the payload above
    volatile uint32_t batch_complete;   // +40  readiness flag (line 0)
    uint32_t          _p0;              // +44
    uint8_t           _pad0[16];        // +48 -> 64
    // ---- cache line 1 : publish-commit token ---------------------------
    volatile uint64_t publish_commit;   // +64  committed == pbr_absolute_index
    volatile uint64_t pbr_absolute_index; // +72
    uint8_t           _pad1[48];        // +80 -> 128
};
static_assert(sizeof(TwoLineRecord) == 128, "TwoLineRecord must be 128B (two cache lines)");
static_assert(alignof(TwoLineRecord) == 64, "TwoLineRecord must be 64B aligned");
// line1_of() and the two-cache-line model depend on these offsets, not just size:
static_assert(offsetof(TwoLineRecord, batch_complete) < 64, "readiness flag must be in line 0");
static_assert(offsetof(TwoLineRecord, checksum) < 64, "payload guard must be in line 0");
static_assert(offsetof(TwoLineRecord, publish_commit) == 64, "commit token must start line 1");

inline void* line1_of(TwoLineRecord* r) {
    return reinterpret_cast<uint8_t*>(r) + 64;
}
inline uint64_t guard_of(uint64_t m, uint64_t s, uint64_t a, uint64_t b) {
    return m ^ s ^ (a * 0x9e3779b97f4a7c15ULL) ^ (b + 0xd1b54a32d192ed03ULL);
}

// ----------------------------------------------------------------------------
// Common controls
// ----------------------------------------------------------------------------
void configure(uint32_t mask, const fi::Params& p) {
    fi::SetParams(p);
    fi::SetMask(mask);
    fi::ResetStats();
}

// ============================================================================
// Adversary 1 — DELAYED FLUSH
//   Correct writer flushes both lines and fences  -> deferred set always drained
//   Buggy writer flushes but never fences         -> deferred set overflows,
//                                                    which the harness flags as a
//                                                    missing-fence discipline bug.
// ============================================================================
bool run_delayed_flush(uint64_t iters) {
    fi::Params p;
    p.seed = 0xD31A9;            // deterministic
    p.flush_defer_ppm = 900000;  // defer 90% of flushes
    printf("\n== Adversary 1: delayed-flush ==\n");

    // --- CORRECT: flush + fence every publish (safe) ---
    configure(fi::kDelayedFlush, p);
    {
        alignas(64) TwoLineRecord rec{};
        for (uint64_t i = 0; i < iters; ++i) {
            rec.magic = 0xABCDEF; rec.seq = i;
            rec.payload_a = i * 3; rec.payload_b = i ^ 0x55;
            rec.checksum = guard_of(rec.magic, rec.seq, rec.payload_a, rec.payload_b);
            cxl::flush_cacheline(&rec);
            cxl::flush_cacheline(line1_of(&rec));
            cxl::store_fence();     // drains the deferred set
        }
    }
    fi::Stats safe = fi::SnapshotStats();
    bool safe_ok = (safe.pending_overflows == 0);
    // RecordCheck tracks *unexpected* invariant breaks only. The correct writer
    // must never overflow; the buggy-writer catch below is the expected outcome
    // and is reported via the return value, not as a "violation".
    fi::RecordCheck("delayed-flush", "correct-writer-no-overflow", safe_ok);
    printf("  correct writer : deferred=%llu drains=%llu overflows=%llu -> %s\n",
           (unsigned long long)safe.deferred_flushes, (unsigned long long)safe.drains,
           (unsigned long long)safe.pending_overflows,
           safe_ok ? "confirmed-safe" : "UNEXPECTED (overflow on correct writer!)");

    // --- BUGGY: flush but never fence (missing store_fence) ---
    configure(fi::kDelayedFlush, p);
    {
        alignas(64) TwoLineRecord rec{};
        for (uint64_t i = 0; i < iters; ++i) {
            rec.magic = 0xABCDEF; rec.seq = i;
            rec.payload_a = i * 3; rec.payload_b = i ^ 0x55;
            rec.checksum = guard_of(rec.magic, rec.seq, rec.payload_a, rec.payload_b);
            cxl::flush_cacheline(&rec);
            cxl::flush_cacheline(line1_of(&rec));
            // BUG: no store_fence() — deferred flushes are never drained here.
        }
    }
    // Drain any tail so we don't leak the thread-local buffer into later tests.
    cxl::store_fence();
    fi::Stats buggy = fi::SnapshotStats();
    bool caught = (buggy.pending_overflows > 0);
    printf("  buggy writer   : deferred=%llu overflows=%llu -> %s\n",
           (unsigned long long)buggy.deferred_flushes,
           (unsigned long long)buggy.pending_overflows,
           caught ? "CAUGHT missing-fence" : "not caught (increase iters)");

    return safe_ok && caught;
}

// ============================================================================
// Adversary 2 — STALE READ  (sequencer readiness poll)
//   Correct reader re-polls via acquire_load until it observes readiness ->
//     staleness only delays, never corrupts (confirmed-safe).
//   Buggy reader reads the flag exactly once (assumes coherence) -> a stale
//     read makes it miss / mis-order the update (caught).
// ============================================================================
bool run_stale_read(uint64_t rounds) {
    fi::Params p;
    p.seed = 0x57A1E;
    p.stale_ppm = 700000;   // 70% chance to serve stale while in-window
    p.stale_window = 16;    // up to 16 stale reads after a change
    printf("\n== Adversary 2: stale-read ==\n");

    // Shared record + a control flag driving one publish per round.
    alignas(64) TwoLineRecord rec{};
    std::atomic<uint64_t> round{0};
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> correct_reads{0}, correct_wrong{0};
    std::atomic<uint64_t> onceReads{0}, onceMissed{0};

    // ---- CORRECT reader: re-poll until readiness, then read seq ----
    configure(fi::kStaleRead, p);
    auto correct_reader = [&] {
        // Start at 0 so the reader ignores the initial round==0 state and only
        // acts once the writer publishes round>=1. Starting at ~0 would process a
        // phantom round 0 before any publish, desyncing the writer/reader
        // handshake and producing false wrong-reads.
        uint64_t last = 0;
        while (!stop.load(std::memory_order_relaxed)) {
            uint64_t r = round.load(std::memory_order_acquire);
            if (r == last) { cxl::cpu_pause(); continue; }
            // Poll readiness through the read seam until it flips to committed.
            uint64_t spins = 0;
            bool ready = true;
            while (cxl::acquire_load(&rec.batch_complete) != 1u) {
                cxl::invalidate_cacheline_for_read(&rec);
                cxl::full_fence();
                if (++spins > 1000000ull) { ready = false; break; }  // liveness backstop
                cxl::cpu_pause();
            }
            // Per the design discipline, the payload is read *fresh* after the
            // readiness flag: invalidate + fence, then load. Staleness affects the
            // polled flag (handled by re-polling above), not this guarded read.
            cxl::invalidate_cacheline_for_read(&rec);
            cxl::full_fence();
            uint64_t s = rec.seq;
            correct_reads.fetch_add(1, std::memory_order_relaxed);
            // Only judge consistency when readiness was actually observed. A backstop
            // exit (should never happen given the bounded stale window) must not be
            // counted as a wrong-read — that would be an artifact of the spin cap,
            // not a protocol violation.
            if (ready && s != r) correct_wrong.fetch_add(1, std::memory_order_relaxed);
            last = r;
        }
    };
    std::thread ct(correct_reader);
    for (uint64_t i = 1; i <= rounds; ++i) {
        // reset then publish
        __atomic_store_n(&rec.batch_complete, 0u, __ATOMIC_RELEASE);
        cxl::store_fence();
        rec.seq = i;
        cxl::flush_cacheline(&rec); cxl::store_fence();
        __atomic_store_n(&rec.batch_complete, 1u, __ATOMIC_RELEASE);
        cxl::flush_cacheline(&rec); cxl::store_fence();
        round.store(i, std::memory_order_release);
        // give the reader time to observe (bounded)
        while (correct_reads.load(std::memory_order_acquire) < i &&
               !stop.load(std::memory_order_relaxed)) {
            cxl::cpu_pause();
        }
    }
    stop.store(true, std::memory_order_release);
    ct.join();
    bool safe_ok = (correct_wrong.load() == 0);
    fi::RecordCheck("stale-read", "repolling-reader-consistent", safe_ok);
    printf("  correct reader : reads=%llu wrong-seq=%llu -> %s\n",
           (unsigned long long)correct_reads.load(), (unsigned long long)correct_wrong.load(),
           safe_ok ? "confirmed-safe (staleness only delayed)" : "UNEXPECTED wrong read");

    // ---- BUGGY reader: read the flag ONCE per round (no re-poll) ----
    // Demonstrates that a reader assuming coherence misses updates under stale
    // reads. Single-threaded, deterministic: right after publish, read once.
    configure(fi::kStaleRead, p);
    {
        alignas(64) TwoLineRecord b{};
        for (uint64_t i = 1; i <= rounds; ++i) {
            __atomic_store_n(&b.batch_complete, 0u, __ATOMIC_RELEASE);
            cxl::store_fence();
            // prime the reader's view of the "old" value (0) so staleness has a prior
            (void)cxl::acquire_load(&b.batch_complete);
            b.seq = i;
            __atomic_store_n(&b.batch_complete, 1u, __ATOMIC_RELEASE);
            cxl::flush_cacheline(&b); cxl::store_fence();
            // BUG: read exactly once, trust it.
            uint32_t seen = cxl::acquire_load(&b.batch_complete);
            onceReads.fetch_add(1, std::memory_order_relaxed);
            if (seen != 1u) onceMissed.fetch_add(1, std::memory_order_relaxed);
        }
    }
    bool caught = (onceMissed.load() > 0);
    printf("  buggy reader   : reads=%llu missed-update=%llu -> %s\n",
           (unsigned long long)onceReads.load(), (unsigned long long)onceMissed.load(),
           caught ? "CAUGHT stale read (missed a committed update)"
                  : "not caught (raise CXL_FI_STALE_PPM/WINDOW)");

    return safe_ok && caught;
}

// ============================================================================
// Adversary 3 — TORN WINDOW  (two-cache-line record over a slot ring)
//   Correct writer publishes commit LAST (payload+fence, then commit+fence):
//     a reader that gates on the commit token never sees a torn payload (safe).
//   Buggy writer publishes commit FIRST: a concurrent reader observes the commit
//     token with the slot's *previous-lap* (or partial) payload -> caught.
//
//   A RING of slots is essential: with a single reused record the free-running
//   writer would overwrite the payload between the reader's commit-gate and its
//   payload-read, flagging the *correct* writer as torn. The ring reproduces the
//   real design's property — a committed slot is not reused for N rounds — so a
//   gated read is stable. (N must exceed how far the writer can lap the reader.)
// ============================================================================
bool run_torn_window(uint64_t iters) {
    fi::Params p;
    p.seed = 0x70A2;
    p.torn_ppm = 500000;
    p.torn_gap_ns = 1500;   // widen the partial-visibility window
    printf("\n== Adversary 3: torn-window ==\n");

    constexpr uint64_t N = 4096;  // ring slots (reuse distance)

    auto run = [&](bool commit_last) -> uint64_t {
        configure(fi::kTornWindow, p);
        // over-aligned TwoLineRecord => aligned allocation (C++17), value-init to 0.
        std::vector<TwoLineRecord> ring(N);
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> torn{0}, accepted{0};
        std::atomic<uint64_t> writer_idx{0};
        std::atomic<uint64_t> reader_progress{0};  // last round the reader consumed

        // Reader consumes rounds strictly in order; slot for round r is ring[r % N].
        auto reader = [&] {
            uint64_t next = 1;
            uint64_t idle = 0;
            while (next <= iters) {
                TwoLineRecord* slot = &ring[next % N];
                cxl::invalidate_cacheline_for_read(line1_of(slot));
                cxl::full_fence();
                uint64_t pc = cxl::acquire_load(&slot->publish_commit);
                uint64_t pid = cxl::acquire_load(&slot->pbr_absolute_index);
                if (pc != next || pid != next) {
                    // Not yet committed for this round. If the writer is finished and
                    // we've been idle a long time, this round was lapped/skipped: stop.
                    if (stop.load(std::memory_order_acquire) &&
                        writer_idx.load(std::memory_order_acquire) >= iters) {
                        if (++idle > 2000000ull) break;
                    }
                    cxl::cpu_pause();
                    continue;
                }
                idle = 0;
                cxl::invalidate_cacheline_for_read(slot);
                cxl::full_fence();
                uint64_t m = slot->magic, s = slot->seq, a = slot->payload_a, b = slot->payload_b;
                uint64_t cs = slot->checksum;
                accepted.fetch_add(1, std::memory_order_relaxed);
                // Torn/inconsistent if the intra-record guard fails (partial write)
                // OR the payload identity does not match the commit token (commit
                // visible before its payload — the dominant observable of the
                // commit-before-payload bug: a clean but stale prior-lap record).
                if (cs != guard_of(m, s, a, b) || s != next) {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                next++;
                reader_progress.store(next - 1, std::memory_order_release);
            }
        };
        std::thread rt(reader);
        for (uint64_t i = 1; i <= iters; ++i) {
            TwoLineRecord* slot = &ring[i % N];
            uint64_t m = 0xABCDEF, s = i, a = i * 7 + 1, b = (i << 3) ^ 0x1234;
            uint64_t cs = guard_of(m, s, a, b);
            if (commit_last) {
                // payload first, fence, THEN commit token, fence
                slot->magic = m; slot->seq = s; slot->payload_a = a; slot->payload_b = b; slot->checksum = cs;
                cxl::flush_cacheline(slot); cxl::store_fence();
                __atomic_store_n(&slot->pbr_absolute_index, i, __ATOMIC_RELEASE);
                __atomic_store_n(&slot->publish_commit, i, __ATOMIC_RELEASE);
                cxl::flush_cacheline(line1_of(slot)); cxl::store_fence();
            } else {
                // BUG: commit token first, payload after (no publish-commit-last)
                __atomic_store_n(&slot->pbr_absolute_index, i, __ATOMIC_RELEASE);
                __atomic_store_n(&slot->publish_commit, i, __ATOMIC_RELEASE);
                cxl::flush_cacheline(line1_of(slot)); cxl::store_fence();
                slot->magic = m; slot->seq = s; slot->payload_a = a; slot->payload_b = b; slot->checksum = cs;
                cxl::flush_cacheline(slot); cxl::store_fence();
            }
            writer_idx.store(i, std::memory_order_release);
            // Structural anti-lap throttle: keep the writer within N/2 rounds of the
            // reader so a committed slot is never overwritten while the reader reads
            // it. This makes the commit-last safety hold by construction rather than
            // by the torn-gap timing accident (a reviewer-flagged robustness gap).
            while ((i - reader_progress.load(std::memory_order_acquire)) >= N / 2)
                cxl::cpu_pause();
        }
        stop.store(true, std::memory_order_release);
        rt.join();
        uint64_t t = torn.load();
        printf("  %-14s: accepted=%llu torn=%llu\n",
               commit_last ? "commit-last" : "commit-first",
               (unsigned long long)accepted.load(), (unsigned long long)t);
        return t;
    };

    uint64_t torn_safe = run(true);
    // The commit-first catch is a genuine race outcome; retry a few times so a
    // single unlucky schedule (e.g. under a loaded CI runner) does not flake the
    // test to red. Any attempt observing torn>0 is a catch.
    uint64_t torn_buggy = 0;
    for (int attempt = 0; attempt < 3 && torn_buggy == 0; ++attempt) torn_buggy = run(false);
    bool safe_ok = (torn_safe == 0);
    bool caught = (torn_buggy > 0);
    fi::RecordCheck("torn-window", "commit-last-holds", safe_ok);
    printf("  commit-last -> %s ; commit-first -> %s\n",
           safe_ok ? "confirmed-safe (guard held)" : "UNEXPECTED torn under correct writer",
           caught ? "CAUGHT torn read" : "not caught (raise iters/torn_gap)");
    return safe_ok && caught;
}

}  // namespace

int main(int argc, char** argv) {
    uint64_t iters = (argc > 1) ? strtoull(argv[1], nullptr, 0) : 200000ull;
    printf("Embarcadero CXL fault-injection harness (in-vitro)  iters=%llu\n",
           (unsigned long long)iters);

    bool ok = true;
    ok &= run_delayed_flush(iters);
    ok &= run_stale_read(iters / 20 + 100);   // rounds; each round syncs
    ok &= run_torn_window(iters);

    // NOTE: stats are reset per subrun (configure() calls ResetStats), so this
    // dump reflects ONLY the final subrun (torn-window commit-first). The
    // per-adversary lines printed above are the authoritative counts.
    fi::DumpSummary("last-subrun-only (torn commit-first)");
    printf("\n==== %s ====\n", ok ? "ALL ADVERSARIES BEHAVED AS EXPECTED"
                                  : "FAILURE: an adversary did not produce its expected verdict");
    return ok ? 0 : 1;
}
