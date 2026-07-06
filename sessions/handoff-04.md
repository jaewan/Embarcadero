# Handoff — Track 04 (fault-injection / W6)

Branch: `session/04-fault-injection` (based off `chore/repo-reorg` HEAD).
Status: **in-vitro harness build-verified & PASSING locally (arm64)**, reviewed
adversarially (7 dimensions) with the findings applied, and **relocated to
`test/faultinj/` per the updated BACKGROUND ownership rules**. x86 intrinsic
paths + CMake integration + embarlet link still to confirm on `broker`.

## What it is

A CXL non-coherence fault-injection harness. Three adversaries, each a matched
correct/buggy pair (confirmed-safe + caught):
- **delayed-flush** — defer/reorder `clflushopt`, drain at fences; on a coherent
  host the observable is a *missing-fence* buffer overflow (documented scope).
- **stale-read** — bounded-staleness via the `CXL::acquire_load` read seam +
  suppressed invalidations; catches read-once-assumes-coherence.
- **torn-window** — busy-spin gap widening a store-publish-order window over a slot
  ring; catches commit-before-payload (publish-commit-last must hold).

Design + how-to + circularity caveat + limitations: `docs/experiments/fault_injection.md`.

## Compliance with updated BACKGROUND rules (this pass)

BACKGROUND.md was updated mid-flight; I brought the layer into line:
- **Location:** moved `src/cxl_transport/faultinj/` → **`test/faultinj/`**
  (`src/cxl_transport/` is Track 03's CXL-mailbox dir).
- **CMake integrator = Track 01:** I **reverted** my edits to root `CMakeLists.txt`
  and `src/CMakeLists.txt`. The layer does not self-wire. Exact hunks for Track 01
  are below. Build/run today via `test/faultinj/smoke_build.sh` (no CMake needed).
- **One shared accessor seam:** the `common/performance_utils.h` hooks are flagged
  as **INTERIM** in-code; per the rule, Tracks 04 & 05 must converge on the single
  `RegionAccessor` interface Track 01 exposes at design-freeze. Migrate then;
  coordinate the one interpose point via the human.

## Files

New, under `test/faultinj/` (Track-04-owned):
- `cxl_fault_inject.h` / `cxl_fault_inject.cc` — controller + 3 adversaries
- `faultinj_harness.cc` — standalone in-vitro driver
- `CMakeLists.txt` — targets (NOT wired into the build; Track 01 adds the subdir)
- `smoke_build.sh` — standalone build+run of record (no CMake/gRPC)

Other new:
- `docs/experiments/fault_injection.md`, `sessions/handoff-04.md`

Modified (guarded, zero-footprint when OFF):
- `src/common/performance_utils.h` — interim `#ifdef CXL_FAULT_INJECTION` hooks in
  flush_cacheline/store_fence/full_fence/invalidate_cacheline_for_read + the new
  `CXL::acquire_load` read seam (`static_assert`ed to integer/pointer T).

NOT modified anymore (reverted to respect the CMake-integrator rule):
- `CMakeLists.txt`, `src/CMakeLists.txt` — clean of Track-04 changes.

Intended commit (do NOT commit until the human asks):
```
git add test/faultinj src/common/performance_utils.h \
        docs/experiments/fault_injection.md sessions/handoff-04.md
git commit -m "Add CXL non-coherence fault-injection harness (Track 04/W6)

Interim seam hooks in common/performance_utils.h behind -DCXL_FAULT_INJECTION
(off by default; production build byte-for-byte unchanged) + CXL::acquire_load
read seam. Controller + 3 adversaries + standalone faultinj_harness under
test/faultinj/. CMake integration left to Track 01 (see handoff).

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

## Verified (fresh, this session)

- **In-vitro harness builds and PASSES on arm64** via `test/faultinj/smoke_build.sh
  50000`: exit 0; delayed-flush overflows 0 vs 351; stale-read wrong-seq 0/2600 vs
  missed ~1850/2600; torn-window torn 0/50000 vs ~25k/50000. arm64's weaker memory
  model makes this a stronger synchronization check than x86.
- **Adversarial review applied.** A 7-dimension review→verify pass surfaced real
  findings; fixed: `acquire_load` `static_assert` + corrected over-claiming
  docstrings; replaced the stale mutex+unordered_map with a fixed lock-free table
  (no hot-path lock, no unbounded growth); nanosleep→busy-spin gap (fixes
  10–100× oversleep + EINVAL); getenv errno/range checks; `g_log` closed at exit;
  RNG hoist + division-free coin; SetParams release/acquire + documented single-
  threaded-config contract; torn-window structural anti-lap throttle (safety no
  longer timing-dependent) + retry; RUN_SERIAL CTest; layout `static_assert`s;
  dropped dead `width` param; per-adversary summary honesty.
- **Not yet verified — needs `broker` (x86 + full build):** the x86 intrinsic paths
  (`_mm_clflushopt`/`_mm_clwb`/`_mm_prefetch`, compiled out on arm64); that Track-01's
  CMake integration builds; that `embarlet` links with the flag across all TUs; the
  in-vivo pipeline run.

## Blocker (needs human)

`rsync ./ broker:~/Embarcadero-sessions/04-fault-injection/` was denied by the
Claude Code sandbox (cross-host transfer). Run it yourself (`! rsync …`) or grant
the permission, then:
```bash
# standalone smoke on broker (x86, seconds, no CMake integration needed)
ssh broker 'cd ~/Embarcadero-sessions/04-fault-injection && test/faultinj/smoke_build.sh 200000'
```
After Track-01 applies the CMake hunks below, also:
```bash
ssh broker 'cd ~/Embarcadero-sessions/04-fault-injection && \
  cmake -S . -B build -DCXL_FAULT_INJECTION=ON \
    -DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero-main-ab/build/_deps/grpc-src && \
  cmake --build build --target faultinj_harness -j 64 && ctest --test-dir build -R faultinj_harness && \
  cmake --build build --target embarlet -j 64'
```
Then fill in `docs/experiments/fault_injection.md` §8 (in-vivo).

## Coordination — hunks for the CMake integrator (Track 01 / human)

1. Root `CMakeLists.txt`, after the `COLLECT_LATENCY_STATS` block:
```cmake
option(CXL_FAULT_INJECTION "Build the CXL non-coherence fault-injection layer (Track 04/W6)" OFF)
```
2. `test/CMakeLists.txt` (add the subdir):
```cmake
if(CXL_FAULT_INJECTION)
    add_subdirectory(faultinj)
endif()
```
3. `src/CMakeLists.txt`, after the `embarlet` `target_link_libraries(...)` block
   (propagates the PUBLIC `CXL_FAULT_INJECTION` def + include dirs to embarlet):
```cmake
if(CXL_FAULT_INJECTION)
    target_link_libraries(embarlet cxl_faultinj)
endif()
```
The def is NOT global on purpose — only targets that link `cxl_faultinj` get the
hooks, so other seam-using targets (throughput_test, sequencers) keep production
accessors and don't need to link it.

## Coordination — proposed Track-01 read-seam adoption (topic.cc)

To let **stale-read** reach the live readiness polls, route them through
`CXL::acquire_load` (identical to `__atomic_load_n(...,ACQUIRE)` when OFF, so safe
to adopt unconditionally). I did **not** edit `topic.cc` (Track-01-owned). Exact
one-liners — but see the SEAM NOTE: this may instead land on `RegionAccessor`:
- `topic.cc:610`  `... __atomic_load_n(&current_batch->batch_complete, __ATOMIC_ACQUIRE)`
  → `... CXL::acquire_load(&current_batch->batch_complete)`
- `topic.cc:832`  `__atomic_load_n(reinterpret_cast<volatile uint64_t*>(&tinode_->offsets[broker_id].written_addr), __ATOMIC_ACQUIRE)`
  → `CXL::acquire_load(reinterpret_cast<volatile uint64_t*>(&tinode_->offsets[broker_id].written_addr))`
- Optional `flags` polls (~6282/6406/6434/6763): `__atomic_load_n(&…->flags, __ATOMIC_ACQUIRE)` → `CXL::acquire_load(&…->flags)`

## Notes for other tracks

- **Track 01:** owns the CMake integration (hunks above) and — per BACKGROUND —
  the single `RegionAccessor` seam that this layer should migrate onto.
- **Track 03:** `src/cxl_transport/` is yours; I've vacated it (now `test/faultinj/`).
- **Track 05 (RDMA):** we share the accessor-interpose point — converge on one
  `RegionAccessor`, not two seams. These results supplement your in-silicon evidence.
- **Track 02 (TLA⁺):** stale-read is the runtime analogue of item (11) stale-CV ACK-relay.
- **Track 07:** verdict matrix in `fault_injection.md` §6 feeds the D9 self-audit table.

## Open questions for the human

1. Grant the `broker` rsync/ssh so I can confirm the x86 build + fill §8 results?
2. Adopt the `CXL::acquire_load` interim seam now, or wait for Track 01's
   `RegionAccessor` and migrate directly onto it?
3. In-vivo run: wrap the existing throughput/e2e launcher, or a dedicated
   `scripts/` fault-injection launcher?
