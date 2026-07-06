# CXL non-coherence fault injection (Track 04 / W6)

A harness that interposes at the **CXL accessor seam** to drive the Embarcadero
pipeline under adversarial non-coherence schedules, plus a standalone in-vitro
harness that constructs the guarantee-critical access patterns directly.

> **Circularity caveat (read first, and repeat it in the paper — panel #7).**
> Injection validates the design **against the assumptions it was built on**. It
> **cannot, by construction, surface an unmodeled hardware behavior.** It is a
> *supplement*, never a *substitute*, for silicon. A fresh reviewer will
> (correctly) read a green result here as *"validated in model and injection."*
> Therefore every result below must be reported **alongside**:
> - the **Track 05** in-silicon RDMA evidence (one-sided RDMA to remote memory is
>   itself cross-host non-coherent — that is the real-hardware exercise of the
>   coherence-free discipline), and
> - the **Track 02** TLA⁺ model (same failure modes, formally checked).
>
> The harness answers a narrower question: *given our fault model, does the
> implementation's flush/fence/publish-commit discipline actually hold?*

Plan references: `Paper/improvement_plan.md` → **W6**, **G4** (process- vs
host-level), **D8** (flush discipline), **I.4 claim 2** (coherence-free
discipline). Complements Track 02 item (11) (stale-CV ACK-relay).

---

## 1. The seam

All fault-relevant CXL primitives are already funnelled through one place —
`Embarcadero::CXL::` in [`src/common/performance_utils.h`](../../src/common/performance_utils.h):

| Primitive | Role | Adversary hook |
|---|---|---|
| `flush_cacheline(addr)` | `clflushopt` a line to CXL | delayed-flush, torn-window |
| `store_fence()` / `full_fence()` | `sfence` / `mfence` after flush | delayed-flush drain point |
| `invalidate_cacheline_for_read(addr)` | `clflush` before a fresh read | stale-read |
| `acquire_load(ptr)` **(new read seam)** | acquire-load of a polled field | stale-read |

The interposition layer lives in
[`test/faultinj/`](../../test/faultinj/):
`cxl_fault_inject.{h,cc}` (controller + adversaries) and `faultinj_harness.cc`
(in-vitro driver). It is compiled **only** when `-DCXL_FAULT_INJECTION=ON`;
otherwise the seam is byte-for-byte the production code and this layer is not
built. No protocol logic is edited — the adversaries substitute the accessor
implementation behind the existing seam.

### The read seam (`CXL::acquire_load`)

Stale reads **cannot be produced on a single coherent host** by suppressing a
`clflush` — the CPU cache is coherent within the box. To reach the live
sequencer readiness loops we add `CXL::acquire_load<T>(ptr)`:

- **Production build:** literally `__atomic_load_n(ptr, __ATOMIC_ACQUIRE)` —
  adopting it at a call site is a semantic no-op.
- **Injection build:** routed through the stale-read adversary, which may return a
  previously-observed value within a bounded window.

Routing the `topic.cc` polling loads through it is a **Track-01 change**
(ownership), proposed as exact one-liners in
[`sessions/handoff-04.md`](../../sessions/handoff-04.md). Until Track 01 adopts
it, in-vivo stale-read reaches only the paths already using the seam; the
**in-vitro harness exercises stale-read fully today**.

---

## 2. The three adversaries

### Delayed-flush
Models `clflushopt`'s weak ordering: on `flush_cacheline`, with probability
`CXL_FI_FLUSH_DEFER_PPM` the physical flush is **deferred** into a per-thread set
and **drained (reordered) at the next `store_fence`/`full_fence`**. Honouring the
fence keeps a *correct* design correct (an `sfence` orders prior `clflushopt`).
What it catches: a **missing fence** — the deferred set then grows without
draining; overflow of the buffer is flagged as a missing-fence discipline bug.

### Stale-read
Models bounded cross-host staleness: after a value change becomes visible, the
adversary may keep serving the **previous** value for up to `CXL_FI_STALE_WINDOW`
reads (probability `CXL_FI_STALE_PPM`), via `acquire_load`; it may also **suppress
an invalidation** so a reader keeps its stale line. A correct re-polling reader
only *delays*; a reader that reads once and assumes coherence **misses the
update**.

### Torn-window
Widens the gap between the two cache-line flushes of a 128-B record
(`CXL_FI_TORN_GAP_NS`, probability `CXL_FI_TORN_PPM`) so a concurrent reader can
observe a **half-updated** record. The design's **publish-commit-last** +
spatial guard (reviewer C praised these) must make such a torn read unobservable
to a reader that gates on the commit token.

---

## 3. Configuration (environment)

| Variable | Meaning | Default when its mode is on |
|---|---|---|
| `CXL_FI_MODE` | `off` \| `delayed_flush` \| `stale_read` \| `torn_window` \| `all` (comma-separable) | `off` |
| `CXL_FI_SEED` | deterministic RNG seed | `0x5eed` |
| `CXL_FI_FLUSH_DEFER_PPM` | P(defer a flush), ppm | `500000` |
| `CXL_FI_STALE_PPM` | P(serve stale), ppm | `300000` |
| `CXL_FI_STALE_WINDOW` | max stale reads after a change | `8` |
| `CXL_FI_TORN_PPM` | P(inject gap on a flush), ppm | `200000` |
| `CXL_FI_TORN_GAP_NS` | gap width, ns | `2000` |
| `CXL_FI_LOG` | append event log to this path (else stderr) | — |
| `CXL_FI_ABORT_ON_VIOLATION` | `1` = `abort()` on an asserted violation | `0` |

The RNG is per-thread and seeded from `CXL_FI_SEED`, so each thread's *injection
decisions* are deterministic. Results are **not** bit-for-bit reproducible: the
harness is multi-threaded and the observable outcomes depend on real thread
interleavings (and, in-vivo, on the pipeline's scheduling). Treat the seed as
controlling the fault stream, not the whole run.

---

## 4. Building

**Fastest loop — standalone smoke (seconds, no CMake/gRPC):**
```bash
test/faultinj/smoke_build.sh [iters]   # builds+runs the in-vitro harness
```
Compiles only the two faultinj `.cc` + the header seam with a plain compiler
call. Auto-detects the glog header (pkg-config / default path / a local `LOG()`
stub — no glog symbol is linked) and passes the right per-arch flags (x86:
`-march=native` for the flush intrinsics; arm64+clang: neutralizes the one
non-constant `__builtin_prefetch` in `performance_utils.h`). Use it to verify the
controller, adversaries, seam hooks, and `acquire_load`. This is the **standalone
build of record today** — it needs no CMake wiring.

**Full CMake build (broker) — requires Track-01 integration first.**
Per `sessions/00-BACKGROUND.md`, root `CMakeLists.txt` / `src/CMakeLists.txt`
additions are the CMake integrator's (Track 01 / human-at-merge) job, so this
layer is **not** self-wired. The exact hunks Track 01 needs (the
`option(CXL_FAULT_INJECTION …)`, `add_subdirectory(faultinj)` in `test/`, and
`target_link_libraries(embarlet cxl_faultinj)`) are in `sessions/handoff-04.md`.
Once applied:
```bash
cmake -S . -B build -DCXL_FAULT_INJECTION=ON \
  -DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero-main-ab/build/_deps/grpc-src
cmake --build build --target faultinj_harness -j 64   # in-vitro (fast)
cmake --build build --target embarlet       -j 64     # in-vivo (full pipeline)
```
`OFF` (default) builds exactly as before — this layer is not compiled.

> SEAM NOTE: the interposition currently hooks the `Embarcadero::CXL::*` inline
> functions in `common/performance_utils.h` (guarded, zero-footprint when OFF).
> Per BACKGROUND's "one shared accessor seam" rule this is an **interim**: Tracks
> 04 and 05 must converge on the single `RegionAccessor` interface Track 01
> exposes at design-freeze. Migrate then; coordinate the one interpose point via
> the human.

---

## 5. Running

### In-vitro (no cluster, no lock)
```bash
./build/bin/faultinj_harness            # default 200000 iterations
./build/bin/faultinj_harness 1000000    # stress
ctest -R faultinj_harness               # CTest wrapper (50k iters)
```
Each adversary runs a **matched pair** — a correct protocol that must survive
(*confirmed-safe*) and a buggy one the adversary must expose (*caught*). The
binary exits non-zero if either expectation is unmet, so it self-tests that the
harness actually bites.

### In-vivo (live pipeline — **take the testbed `flock`**)
```bash
ssh broker 'flock -w 2400 ~/Embarcadero-sessions/testbed.lock -c "
  cd ~/Embarcadero-sessions/04-fault-injection &&
  echo \"[$(date -u +%FT%TZ)] 04: START faultinj in-vivo\" >> ~/Embarcadero-sessions/activity.log &&
  CXL_FI_MODE=all CXL_FI_LOG=/tmp/cxl_fi.log \
    ./scripts/<the e2e / throughput launcher> ;
  echo \"[$(date -u +%FT%TZ)] 04: END\" >> ~/Embarcadero-sessions/activity.log
"'
```
Drive load through the existing throughput/e2e harness and assert the ordering /
per-session-prefix invariants still hold (existing client validation), then check
`/tmp/cxl_fi.log` for the `SUMMARY` and any `VIOLATION` lines.

---

## 6. What each adversary catches (verdict matrix)

| Adversary | Correct protocol (expect *confirmed-safe*) | Buggy protocol (expect *caught*) |
|---|---|---|
| delayed-flush | flush + `store_fence` each publish → deferred set always drained (`overflows==0`) | flush but never fence → deferred set overflows (missing-fence flagged) |
| stale-read | re-poll `batch_complete` via `acquire_load` until ready → staleness only delays; `seq` always correct | read the flag once → misses a committed update within the stale window |
| torn-window | publish-commit-last + guard → reader gating on the commit token never sees a torn payload | commit-before-payload → reader observes commit with stale payload (checksum mismatch) |

Results are recorded via `RecordCheck(...)` and summarised by `DumpSummary(...)`.

---

## 7. Coverage & limitations (read before trusting a green run)

Beyond the top-level circularity caveat, the harness has concrete blind spots:

- **Seam coverage is not total.** One raw flush sits *outside* the `CXL::` seam —
  `src/network_manager/network_manager.cc` (~L1285) issues `_mm_clflushopt` /
  `_mm_clwb` directly in KAFKA mode. Delayed-flush / torn-window do **not**
  intercept it. Routing it through `CXL::flush_cacheline` is a proposed one-line
  coordination item (see handoff); until then that path is uninstrumented.
- **Stale-read only reaches loads that adopt `acquire_load`.** Most reads in the
  hot path are plain `struct->field` loads, which stay fresh (coherent host) and
  are not perturbed. Coverage is deliberately scoped to the *readiness/polling*
  loads the protocol's correctness hinges on (`batch_complete`, `written_addr`,
  `ordered`, `flags`) — and only after Track 01 adopts the seam there.
- **Delayed-flush is weak in-vivo on a single host.** Because the host cache is
  coherent, deferring a physical flush does not itself make a reader observe
  stale data. Its in-vivo signal is a *missing fence*, detected only when a
  thread issues >256 flushes with **no** intervening fence (buffer overflow) — a
  coarse detector that a nearby fence will mask. The crisp delayed-flush result
  is the **in-vitro** synthetic buggy-writer, which tests the discipline, not the
  live code path directly.
- **The harness models non-coherence with data races on `volatile`.** That is
  technically C++ UB (it is exactly the hazard we emulate) — the same
  `volatile`+fence discipline the product code uses. Consequence:
  `-fsanitize=thread` is not meaningful here, and results are x86-specific.
- **`DumpSummary("harness-final")` reflects only the last sub-run** (stats are
  reset per scenario). Read the per-adversary printed lines for the full picture.

## 8. Results

### In-vitro, local smoke (arm64 macOS, 2026-07-05, branch `session/04-fault-injection`)

First validation via `smoke_build.sh` (50000 iters). Exit 0; all three adversaries
produced the expected matched pair. arm64's weaker memory model makes this a
stronger check of the harness synchronization than x86 would be.

| Adversary | Correct (confirmed-safe) | Buggy (caught) |
|---|---|---|
| delayed-flush | `overflows=0` | `overflows=351` |
| stale-read | `wrong-seq=0 / 2600` | `missed=1817 / 2600` (≈70% = `stale_ppm`) |
| torn-window | `torn=0 / 50000` | `torn=27119 / 50000` |

**Still to verify on `broker` (x86 + full build):** the x86 flush/prefetch
intrinsic paths (compiled out on arm64), the CMake `CXL_FAULT_INJECTION` wiring,
that `embarlet` links with the flag across all TUs, and the in-vivo run. See
handoff.

### x86 harness (broker, 2026-07-06)

`test/faultinj/smoke_build.sh 50000` on x86_64 (`-march=native`, exercising the
real `_mm_clflushopt`/`_mm_clwb`/`_mm_prefetch` paths compiled out on arm64):
**exit 0, all three adversaries pass** (delayed-flush overflows 0 vs 351;
stale-read wrong-seq 0/2600 vs missed 1850/2600; torn-window 0/50000 vs 24676).
Closes the platform-coverage gap.

### Zero-footprint perf regression (broker, 2026-07-06, flag OFF)

The Track-04 integration is present in the tree (root option, `add_subdirectory`,
embarlet link, seam hooks) but built with `CXL_FAULT_INJECTION=OFF`. Verification
that our work does not disturb production performance:

- **Binary identity:** the OFF embarlet is `md5 = f68e5e35…`, **byte-identical to
  the session-1 canonical binary**, and contains **0 faultinj symbols**. So by
  construction our code cannot change runtime behavior when OFF. (My latest
  `performance_utils.h` differs from what produced that binary only inside the
  `acquire_load` `#ifdef` branch + a compile-time `static_assert` — both
  uninstantiated in production — so it too yields identical machine code.)
- **Loopback O0** (control, ×3): 12.31 / 12.41 GB/s, ACK 100% (ref ~12.46).
- **Loopback O5** (×5, no retries): 12.48 / 12.64 / 12.17 / 12.15 / 11.97 GB/s,
  ACK 100% every trial; anomaly sidecars all zero (`FifoViolations=0`,
  `ScannerTimeoutSkips=0`, `SkippedBatches=0`) on all brokers. (ref 11.46–12.11.)
- **Real-wire c3/100G O0** (×3): 7.01 / 7.74 / 7.26 GB/s (ref 7.38/7.43/7.65).
- **Real-wire c3/100G O5:** intermittent `incomplete_or_missing_bandwidth`
  (hold-buffer timeouts; `FifoViolations=0`, `ScannerTimeoutSkips=0` — *not* the
  scanner-freeze signature). **Confirmed pre-existing, not Track 04:** the binary
  is md5-identical to the canonical, config identical, and session 1's own
  canonical O5 real-wire runs show the same intermittent failures. Root cause is
  environmental (hugepages under-provisioned → THP fallback on a timing-sensitive
  path), shared by all sessions.

**Conclusion:** Track 04, flag OFF, is byte-identical to the canonical build and
does not disrupt performance on any path we control.

### In-vivo fault injection (flag ON) — still pending

Not yet run: an embarlet built with `-DCXL_FAULT_INJECTION=ON` (confirms it links
across all TUs = gate 2) and a live pipeline run under injection (gate 3). Both
need a full ON build + the exclusive testbed lock; the CMake integration hunks are
Track 01's to apply (see handoff). Pair results with Track 05 silicon + Track 02 TLA⁺.
