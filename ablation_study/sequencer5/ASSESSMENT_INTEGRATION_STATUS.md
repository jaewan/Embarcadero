# Assessment Integration: Implementation Status

This document maps the "Assessment Integration: Implementation Reality vs. Paper Claims" suggestions to what was actually implemented in the codebase.

---

## 1. Silent Drops vs. Paper Semantics

**Suggestion:** Replace silent drops with backpressure or explicit loss notification; at minimum document limitation in the paper's Discussion.

**Implemented:**
- **Backpressure behavior added** for paper-grade modes:
  - `add_to_hold()` and `add_to_hold_shard()` now **defer** instead of dropping when full (non-stress modes).
  - Scatter-gather collector **waits** for shard queue space instead of dropping (non-stress modes).
- **Stress-only loss**: drops are still allowed when `--validity=stress` is selected, and runs are labeled as stress.

**Gap:** Paper should still document overload semantics explicitly; stress-mode drops are permitted and must not be used for headline claims.

---

## 2. Scatter-Gather L5 Divergence from Single-Threaded Semantics

**Suggestion:** Fix the bug where scatter-gather advanced `next_expected` on `seq <= hc`, which could skip legitimate sequences and violate per-client order.

**Implemented:**
- **Fixed.** In `process_level5_shard()`, the branch for `b.client_seq <= hc` (already committed / duplicate) no longer calls `state.advance_next_expected()`. It only calls `state.mark_sequenced(b.client_seq)` and increments the duplicate counter, matching the single-threaded path.
- Comment added: *"Already committed (retry or late arrival): reject duplicate; do not advance next_expected (align with single-threaded path)."*

**Result:** Scatter-gather L5 semantics now align with single-threaded Order 5 for this edge case. The assessment’s recommendation to "fix the bug or restrict the paper to single-sequencer mode" is addressed by the fix; paper can still cautiously claim scatter-gather L5 if evaluation is run and validated.

---

## 3. Validation Ring Stall on Hot Path

**Suggestion:** Make validation ring lossy so it never blocks: if slot is ready (ring full), skip recording (and optionally count `dropped_validations_`) instead of spinning forever.

**Implemented:**
- **Stall removed.** `ValidationRing::record()` now uses a **bounded spin** (`VALIDATION_RING_MAX_SPIN = 10000`). If the slot is still ready after that, the code **overwrites** the slot with the new record (no return/skip). So the hot path never blocks indefinitely; at most one validation entry is lost when the ring is full.
- Comment added: *"If ring is full (slot still ready after VALIDATION_RING_MAX_SPIN spins), overwrites slot to avoid blocking the hot path; one validation entry may be lost."*
- **Not implemented:** A separate `dropped_validations_` counter (the assessment’s optional suggestion). Overwrite was chosen so validation still sees a contiguous stream of entries, with possible loss under saturation.

**Result:** Scatter-gather (and any path calling `record()`) is no longer subject to unbounded stall due to a full validation ring.

---

## 4. Benchmark Methodology

**Suggestion:** Steady-state detection, 95% confidence intervals, 0.1% PBR saturation threshold for "clean" numbers, and clear latency semantics.

**Implemented:**
- **Unbuffered output:** `std::cout << std::unitbuf` and `std::cerr << std::unitbuf` in `main()` so logs are not lost when piping/redirecting.
- **PBR saturation threshold:** Default **0.1%** with CLI override (`--pbr-threshold=`).
- **Steady-state detection:** Implemented and reported; enforced for algorithm-comparison validity mode.
- **95% confidence intervals:** Reported in multi-run summaries.
- **Validity modes:** `--validity=algo|max|stress` to align gating with experiment type.
- **Latency semantics:** P99 is reported; max-throughput mode treats high latency as a warning, not an invalidation.

**Gap:** Paper text should still state that latency is inject→commit in-process (not end-to-end).

---

## 5. Other Code Improvements (From Earlier Panel Review)

- **PBR single-owner invariant:** Documented for `PBRReadState` (each broker read by exactly one collector; if shared, make fields atomic).
- **EpochBuffer::seal():** Comment that early state check is optimization only; CAS is the real gate (TOCTOU safe).
- **Unitbuf:** As above, in `main()`.

---

## Summary Table

| Assessment item                         | Status        | Notes |
|----------------------------------------|---------------|--------|
| Silent drops → backpressure or notify  | **Fixed**     | Deferred/blocked in non-stress modes; stress allows drops |
| Scatter-gather L5 `next_expected` bug  | **Fixed**     | No advance on `seq <= hc`; matches single-threaded path |
| Validation ring stall                  | **Fixed**     | Bounded spin + overwrite; no infinite stall |
| Steady-state detection                 | **Fixed**     | Enforced in algorithm mode |
| 95% confidence intervals              | **Fixed**     | Reported in multi-run summary |
| 0.1% PBR saturation threshold         | **Fixed**     | Default 0.1%; configurable via CLI |
| Latency = queueing + algorithm         | Partial       | Output warns on high P99; paper text still needed |

---

## Recommended Next Steps (If Targeting Paper Submission)

1. **Paper / docs:** Add a short "Limitations" or "Implementation notes" stating that stress mode allows loss and headline numbers are produced in non-lossy modes.
2. **Optional code:** Add a `dropped_validations` counter and/or make the validation ring "skip and return" instead of overwrite, if you want to report how many validations were lost.
3. **Evaluation:** Implement steady-state detection and 95% CI (e.g. 10+ runs), and consider a 0.1% PBR saturation option for "clean" algorithm comparison runs.
4. **Scope:** The single-sequencer design and scatter-gather L5 fix support reporting both modes with aligned semantics; keep scatter-gather claims consistent with the evaluation you actually run and validate.
