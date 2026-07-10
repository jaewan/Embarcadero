# RF=2 > RF=1 throughput anomaly = deterministic first-run warm-up, not ambient contention

**Date:** 2026-07-10. **Host:** moscxl (shared 512-core).

## Symptom
In the local baseline matrix, SCALOG (and CORFU) showed RF=2 *publish* goodput **above** RF=1
(e.g. SCALOG 6027.8 vs 4199.6 MB/s) — impossible as a replication *cost* (more replication should
degrade, not improve, throughput). A prior investigation ruled out CPU frequency scaling (fixed
governor/clock), THP (globally `never`), and disk-file staleness (A/B inconclusive, direction flipped),
and concluded **"ambient contention on the shared host."**

## Correction: the dominant cause is a deterministic first-run warm-up
"Ambient contention" is under-supported and contradicted by the data:

1. **The matrix's own data is a monotonic ramp, not random scatter.** CORFU RF=1 trials were
   **2902 → 11298 → 12633 MB/s** — monotonically increasing. Random contention is never monotonic; a
   monotonic first-trial-cold ramp is a textbook warm-up.
2. **The reversed A/B showed "second config always faster in all 3 rounds"** — a *consistent*
   order effect. Consistency is the warm-up tell; contention would flip direction (and did, in the
   disk-file A/B — which is why that test was inconclusive).
3. **Controlled probe (this doc): 6× identical SCALOG RF=1, back-to-back.** Publish goodput:
   `2979 → 3611 → 3588 → 3210 → 3195 → 3225 MB/s`. **Trial 1 (2979) is the slowest, and it ran while
   the host was idle (load 0.01)** — contention cannot make the *idle* first trial the cold one; only
   a deterministic warm-up can. (Raw: `subscriber-tail/data/warmup_probe/`.)

So there are **two** effects, now separated:
- **Deterministic first-run warm-up (primary):** the first trial after a fresh cluster start is cold
  (cold connections / socket buffers / CPU caches, and — if `MAP_POPULATE` is off — CXL first-touch).
  ~12–20% for SCALOG, ~4× for CORFU. It is deterministic (slowest even on an idle host).
- **Ambient contention (secondary, real):** in the same probe, load spiked 0.01 → 26 mid-run (a
  concurrent session) and trials 4–6 sagged ~11% below the warm peak. This is genuine shared-host
  noise — but it is *scatter*, not the cause of the RF ordering.

## Why RF=2 looked faster
The matrix ran **all RF=1, then all RF=2** per sequencer. RF=1 absorbed the machine's coldest state;
RF=2 ran on an already-warmed machine → higher mean. It is **run-order warm-up, not replication cost.**

## Fix (deterministic → cleanly correctable; exclusive host NOT required for the RF *direction*)
1. **Discard the first (cold) trial** from every aggregate (implemented: `aggregate_e2e_throughput.py
   --warmup-trials`, default 1, wired via `WARMUP_TRIALS` in `run_e2e_throughput_benchmark.sh`; raw
   trials preserved). Applies to *all* metrics, not just RF.
2. **Interleave / randomize RF order** (never all-RF1-then-all-RF2) so residual order effects cancel.
3. **Exclusive host** (cgroups / exclusive scheduling) — only to tighten CIs against the *secondary*
   ambient-contention scatter; not needed to restore the RF *direction* (RF=2 ≤ RF=1).

With (1)+(2), the RF comparison is valid on the shared host; (3) is for publication-grade CIs.

## Not fully pinned
The exact warmed subsystem (connection vs socket-buffer vs cache vs CXL first-touch) was not isolated
to one — that needs sudo-gated probes (`drop_caches`, `tcp_metrics flush`, `MAP_POPULATE` toggle) on a
quiet host. The *determinism* (and therefore the fix) is established regardless.
