# Track 04 — Non-coherence fault-injection harness (W6)

**Read `sessions/00-BACKGROUND.md` first.** Track id: `04-fault-injection`. Branch:
`session/04-fault-injection`.

**Goal.** A harness that interposes at the CXL accessor layer to run the full pipeline under
adversarial non-coherence schedules: **delayed-flush injection, stale-read injection, torn-window
adversary.** Independent — start now.

**Framing (v4 — read carefully).** In v4, the *primary* in-silicon validation of the coherence-free
discipline is the **RDMA variants (Track 05)** — one-sided RDMA to remote memory is itself cross-host
non-coherent, so the discipline is exercised on real hardware there. This harness is a **supplement**:
it stresses the design against *its own fault model* on the single CXL machine.
- **State the circularity caveat in the harness docs and the paper (panel #7):** injection validates
  the design against the assumptions it was built on and **cannot surface an unmodeled hardware
  behavior by construction.** It is a supplement, never a substitute for silicon. A fresh reviewer
  will (correctly) write "validated in model and injection" — so pair every result with the Track 05
  silicon evidence and Track 02 TLA⁺.

**Plan sections:** `Paper/improvement_plan.md` → W6, G4 (process- vs host-level), D8 (flush
discipline), I.4 claim 2 (coherence-free discipline).

## You OWN
- A new `src/cxl_transport/faultinj/` or `test/faultinj/` layer (create it) that wraps the CXL
  accessors (loads/stores/clwb/sfence used across `src/cxl_manager/` and `src/embarlet/`).
- A build option to enable it (e.g. CMake `-DCXL_FAULT_INJECTION=ON`) that swaps the accessor
  implementation without editing protocol logic.
- `docs/experiments/fault_injection.md` (new): adversary models, how to run, what each catches.

## Do NOT touch
- Protocol logic in `src/embarlet/topic.{cc,h}` (Track 01) or baseline code (Track 03). Interpose
  via a **wrapper / compile-time shim**, don't modify the call sites' logic. If the accessors aren't
  already behind a single seam, add the seam as a thin new header and coordinate the one-line
  include change with Track 01 (keep it minimal).
- Any `Paper/Text/*.tex`.

## Scope
- **Delayed-flush:** reorder/delay clwb visibility to expose missing-flush bugs.
- **Stale-read:** serve a reader an older cache-line version within a bounded window (no coherence).
- **Torn-window adversary:** interleave a writer's cache-line update with a reader mid-record;
  verify spatial guard + wrap-fence hold (reviewer C praised these — prove them under adversity).
- Drive the harness through the existing pipeline (throughput/e2e smoke) under adversarial
  schedules; assert the guarantee invariants don't break.

## Build/test (BACKGROUND protocol)
- Build in `~/Embarcadero-sessions/04-fault-injection/` with `-DCXL_FAULT_INJECTION=ON` (`-j 64`).
- Running the pipeline under injection (uses `/dev/shm`/CXL segments): **take the `flock` lock**.
- Pure-logic unit tests of the adversary (no cluster) need no lock.

## Coordinate
- If you need a seam in a Track-01 file, propose the exact one-line change to that session; keep the
  logic change to zero.
- Findings feed the paper's §5/§6 (hand to Track 07) and complement Track 02's TLA⁺ (same failure
  modes, empirical vs modeled).

## Done criteria
- Harness builds behind the CMake flag on `broker`; all three adversaries runnable; a report in
  `docs/experiments/fault_injection.md` with at least one caught/confirmed-safe result per adversary.
  Handoff note with staged pathspec + commit message.

---

# ✅ COMPLETION REPORT — Track 04 (appended 2026-07-06)

All done-criteria met and **verified on the real x86 testbed (`broker`)**. Full handoff
(files, commit plan, coordination hunks) in `sessions/handoff-04.md`; adversary/how-to/results
detail in `docs/experiments/fault_injection.md`.

## Deliverables (Track-04-owned, in `test/faultinj/`)
- `cxl_fault_inject.{h,cc}` — interposition controller + 3 adversaries (delayed-flush, stale-read,
  torn-window). Lock-free fixed stale table, busy-spin torn gap, deterministic RNG.
- `faultinj_harness.cc` — standalone in-vitro driver; matched correct/buggy pair per adversary.
- `CMakeLists.txt`, `smoke_build.sh` (standalone build+run, no CMake).
- Seam (interim): guarded `#ifdef CXL_FAULT_INJECTION` hooks + `CXL::acquire_load` in
  `src/common/performance_utils.h` (byte-for-byte no-op when OFF).
- Docs: `docs/experiments/fault_injection.md`. Adversarially reviewed (7 dimensions); findings fixed.

## Gates — all PASS on `broker` (x86_64)
| Gate | What | Result |
|---|---|---|
| 1 | x86 harness build+run (`smoke_build.sh`, real `_mm_clflushopt/_mm_clwb/_mm_prefetch`) | ✅ exit 0; all 3 adversaries caught+confirmed-safe |
| 2 | embarlet builds **and links** with `-DCXL_FAULT_INJECTION=ON` across all TUs | ✅ `[100%] Built target embarlet`; `nm` shows `faultinj::{MaybeStale,MaybeTornGap,InterceptFlush,DrainDeferredFlushes}` linked in |
| 3 | live in-vivo run under injection (`mask=0x7`, all 3 adversaries) | ✅ pipeline completes, ACK 100%, **FifoViolations=0, ScannerTimeoutSkips=0, order5_commit_order_violations=0** |
| OFF-perf | Track-04 present but flag OFF → no perf disruption | ✅ OFF embarlet **md5-identical** to session-1 canonical (0 faultinj symbols) |

## Perf-regression numbers (flag OFF, vs references)
- Loopback O0 (control): 12.31 / 12.41 GB/s (ref ~12.46), ACK 100%.
- Loopback O5 (5/5, no-retry): 12.48/12.64/12.17/12.15/11.97 GB/s (ref 11.46–12.11), ACK 100%, all anomaly counters 0.
- Real-wire c3/100G O0: 7.01/7.74/7.26 GB/s (ref 7.38/7.43/7.65).
- Real-wire c3/100G O5: intermittent `incomplete_or_missing_bandwidth` — **pre-existing, NOT Track 04**
  (identical md5 binary + config; session-1's own canonical O5 real-wire is equally flaky; root cause
  = hugepages under-provisioned → THP fallback, shared by all sessions).
- In-vivo under injection: 11.68/12.23 GB/s (mild, safe injection overhead), invariants intact.

## Broker artifacts (session `04-fault-injection` dir)
- OFF build (= session-1 canonical): `build/bin/embarlet`.
- ON build: `build_fi/bin/{embarlet,faultinj_harness}`.
- Testbed left clean: no orphan procs, lock released, activity.log annotated.

## Remaining (NOT Track-04-owned or awaiting sign-off)
1. **Track 01 — CMake integration** at merge: root `option(CXL_FAULT_INJECTION …)`, add the faultinj
   subdir **before** the embarlet target (verified working via
   `add_subdirectory(${CMAKE_SOURCE_DIR}/test/faultinj ${CMAKE_BINARY_DIR}/faultinj_build)`), and
   `target_link_libraries(embarlet cxl_faultinj)`. Exact hunks in handoff-04.
2. **Track 01 — single `RegionAccessor` seam.** The `performance_utils.h` hooks are interim; migrate
   onto the shared interface (also used by Track 05) at design-freeze. Coordinate via the human.
3. **Track 01 — adopt `CXL::acquire_load`** at the `topic.cc` readiness polls (one-liners in handoff)
   so in-vivo stale-read serves stale *values*, not just suppressed invalidations.
4. **Human — commit + CI sign-off** (nothing committed yet; pathspec/message in handoff-04).

**Status: Track 04 is functionally complete and testbed-verified (in-vitro + in-vivo, OFF + ON).
The only open items are cross-track integration (Track 01) and the commit/sign-off.**
