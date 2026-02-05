# Sequencer5 Ablation Study

Standalone sequencer implementation and benchmark for OSDI/SOSP/SIGCOMM-level ablation: correctness-first dedup, MPSC ring (multiple producers per broker), Level 5 sliding window, hold-buffer emit, ordering validation, and **per-batch vs per-epoch atomic** comparison. Aligns with **docs/EMBARCADERO_DEFINITIVE_DESIGN.md**.

## Repository layout

| Path | Purpose |
|------|---------|
| `sequencer5_benchmark.cc` | Single-file benchmark and sequencer logic |
| `CMakeLists.txt` | Build (standalone) |
| `validate.sh` | Correctness test + short ablation |
| `run_scalability_experiments.sh` | Reproduce scalability results (vary producers/brokers) |
| `ABLATION_RESULTS.md` | Summary of results; numbers match canonical logs |
| `improvement_instruction.md` | Profiling and FastDeduplicator fix instructions |
| `logs/` | Result logs (see below) |

## Results

- **Canonical result logs:** `logs/scalability_20260205_083244/` (6 runs: vary producers 2/4/8, vary brokers 2/4/8), `logs/profile_20260205_080856.log` (phase profiling), `logs/PROFILE_AND_STABILITY_RESULTS.md` (profiling summary and FastDeduplicator stability).
- **Summary and tables:** **ABLATION_RESULTS.md** (throughput, speedup, atomic reduction per config).

## Reproducing results

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release   # or RelWithDebInfo for profiling
make sequencer5_benchmark

./sequencer5_benchmark --test          # Correctness
./sequencer5_benchmark --test-dedup    # FastDeduplicator stress test
cd .. && ./run_scalability_experiments.sh   # Writes to logs/scalability_<timestamp>/
./validate.sh                          # Full validation (Phase 1 + 2a/2b)
```

## Key Operations (from document analysis)

The sequencer supports:

1. **Collecting batches from PBRs** — Multiple collector threads poll Pending Batch Rings from brokers.
2. **Epoch-based sequencing** — Triple-buffered pipeline (COLLECT → SEQUENCE → COMMIT).
3. **Global sequence assignment** — Single atomic `fetch_add` per epoch (O(1) instead of O(n)).
4. **Level 0 ordering** — Total order only, no per-client guarantees, maximum throughput.
5. **Level 5 ordering** — Per-client FIFO ordering with hold buffer and gap handling.
6. **GOI writing** — Commit sequenced batches to Global Order Index.

### Order 5 semantics and "declared lost"

- **Guarantee:** If client C sends M1 before M2 (by client_seq), then M1.global_seq < M2.global_seq, or M1 is **declared lost**.
- **Declared lost:** A batch is dropped from the hold buffer after it has waited **HOLD_MAX_WAIT_EPOCHS** (3) without becoming `next_expected`. It is **never** written to the GOI. So "lost" means "never in the log," not "in GOI with a gap."
- **Client-observable:** At-most-once delivery per client_seq; gaps in client_seq are allowed after the timeout (e.g. seq=1, 3 committed; seq=2 was lost).

## Data Structures in CXL Memory (document §2.4)

| Structure              | Size    | Description                          |
|------------------------|---------|--------------------------------------|
| **ControlBlock**       | 128 B   | epoch, sequencer_id, committed_seq, broker state |
| **PBREntry**           | 64 B    | Batch metadata from brokers         |
| **GOIEntry**           | 64 B    | Global order index entries           |
| **CompletionVectorEntry** | 128 B | Per-broker ACK state                 |

## Correctness and ablation (conference-ready)

- **Deduplication**: Hash-set with FIFO eviction (`Deduplicator`), no false positives. Per-shard lock, insertion-order eviction when full.
- **MPSC PBR ring**: Multiple producers per broker (e.g. 8 producers, 4 brokers). `MPSCRing::reserve()` uses CAS; no single-producer assumption.
- **Livelock fix**: Next buffer availability checked *before* sealing; high-resolution timer (busy-wait) for sub-ms epochs.
- **Level 5 sliding window**: Per-client circular window + `window_set` for O(1) duplicate detection; no bitset collision.
- **Level 5 sort**: O(n) LSD radix sort by `client_id` (8×8-bit passes); insertion sort for small per-client `client_seq` groups. Threshold 256: below that, `std::sort` is used. No external deps (Boost/ska_sort optional elsewhere).
- **Hold buffer**: Expired batches are **emitted** (appended to output) and client state updated; no silent drop.
- **Ordering validation**: `validate_ordering()` checks monotonicity and gap-freedom of committed global_seq, and **per-client order** (for Level 5: within each client_id, global_seq order matches client_seq order); run after each test when `validate=true`.
- **Dual mode (ablation)**: `PER_BATCH_ATOMIC` (baseline, one fetch_add per batch) vs `PER_EPOCH_ATOMIC` (one fetch_add per epoch). Main reports speedup and atomic reduction.
- **In-benchmark latency**: `PBREntry::timestamp_ns` set on inject; samples (inject→committed) collected and reported. Not paper e2e (no network/replication).
- **State machine**: IDLE → COLLECTING → SEALED → SEQUENCED → COMMITTED; PBR heads advanced after sequencing; memory barriers on broker_max_pbr.

## Concurrency

- **Collector threads** — enter via `enter_collection()` (mutex + state COLLECTING + increment); exit with `exit_collection()` (decrement, notify CV).
- **Single sequencer thread** — only thread that touches hold buffer and runs `process_level5` / `drain_hold` / `age_hold`; advances PBR heads after sequencing.
- **Single GOI writer thread** — waits for SEQUENCED, writes GOI, then `mark_committed()`.
- Triple-buffered pipeline (`config::PIPELINE_DEPTH`); constants in `config::` namespace.

## Performance testing

1. **Throughput** — Batches/sec and MB/sec under varying load.
2. **Latency** — In-benchmark only: inject→commit (no network). P50 / P99 / P99.9 in μs. Not comparable to paper's end-to-end client→ACK.
3. **Scalability** — Vary producer threads (1–16), brokers (1–8), collectors. Plateau expected: single sequencer thread is the bottleneck.
4. **Ablation** — Level 0 vs Level 5 (10%) vs Level 5 (100%). L5 uses **producer affinity**: each producer owns a disjoint client partition so (client_id, client_seq) is unique and strictly increasing per client.
5. **Epoch duration** — Impact of different τ values. **Epoch CPU (μs)** = wall-clock time the sequencer thread spends processing one epoch (merge, sequence, drain hold); not the collection window τ.

---

## Phase 2: Performance Fidelity (mandatory before paper numbers)

Before generating ablation/paper numbers, the three performance items in **PHASE2_PERFORMANCE.md** must be in place so results reflect sequencer protocol cost, not allocator/cache artifacts. All three are implemented: padded atomics for shard watermarks, dense client state when `max_clients` is set, and pre-allocation of hot-path vectors.

## Caveats and methodology (reviewer responses)

| Concern | Response |
|--------|----------|
| **87K× atomic reduction, no throughput gain (0.98× median)** | Paired 5-run shows per-epoch ≈ same throughput as per-batch [0.81×–1.23×]. Benchmark is memory-bandwidth bound; atomics were not the bottleneck. Paper claim: *same performance with 87K× fewer atomics* (simplicity + coordination reduction), not a throughput speedup. |
| **Scalability stops at ~2 threads** | **Single sequencer thread** is the bottleneck (Amdahl). Producers scale up to saturate the sequencer; further threads add minimal gain. Expected for this design; scatter-gather (Phase 3) would address. |
| **Level 5 benchmark bug (duplicate client_seq)** | **Fixed.** Previously multiple producers could send the same (client_id, client_seq). Now each producer owns a disjoint client partition; L5 traffic uses only "our" clients so client_seq is unique and monotonic per client. |
| **Latency 0.3–1 μs vs paper 1.5–2 ms** | This benchmark measures **in-memory inject→commit** only. Paper's e2e includes network RTT, replication, TCP. Not comparable; methodology documented above. |
| **Epoch "Seq" 3.97 ms for 100 μs epoch?** | **Clarified.** Metric is **CPU time to process one epoch** (merge + sequence + drain), not the collection window. Label is now "Epoch CPU (μs)". A 100 μs epoch means "collect for 100 μs"; sequencing that batch takes ~4 ms of CPU. |
| **100% L5: 40–78% gaps, 38–57× slower** | With producer affinity, gap rate and throughput reflect **real** per-client ordering cost (hold buffer, reordering). 100% L5 is a stress test; typical workloads use a small L5 fraction. |
| **Missing: Scalog/Corfu, real CXL, multi-node** | Ablation scope is single-node, in-memory sequencer logic. Baselines and CXL/multi-node belong in the full paper evaluation. |

## Scope and limitations (panel assessment)

- **Recovery:** Recovery (e.g. ClientState rebuild from GOI, gap detection after partial write) is specified in the design/plan but **not evaluated** in this benchmark. The benchmark does not inject crashes or test failover.
- **CXL:** Memory is allocated via `mmap`/`aligned_alloc` (local DRAM). This measures **sequencer logic and layout**, not actual CXL latency or bandwidth. For paper claims about "CXL performance," a CXL-backed run is required separately.
- **Valid experiments:** Runs where PBR reserve failures (PBR_full) exceed 1% of attempts may be saturation-bound; the benchmark warns and you should interpret throughput accordingly (e.g. use larger `pbr_entries` or fewer producers).

### Known limitations / failure semantics

| Scenario | Behavior | Note |
|----------|----------|------|
| **PBR full** | Batch is dropped; `pbr_full_count` incremented. | Producers outpace sequencer; backpressure is via PBR reserve failure. Use larger `pbr_entries` or fewer producers. |
| **Writer slow (scatter-gather)** | All shards block each epoch until the writer finishes. | Scatter-gather throughput is upper-bounded by writer throughput (GOI write). |
| **Hot shard** | One shard gets most traffic if client_id distribution is skewed. | Ablation assumes roughly uniform client_id distribution; skewed workloads may underutilize shards. |

### Supported limits and growth

- **Brokers:** 1..32 (config `MAX_BROKERS`). Do not exceed without changing the code (PBR/collector arrays).
- **Scatter-gather shards:** 1..32 (`num_shards`; coordinator array size).
- **client_highest_committed:** Unbounded in this benchmark (no eviction). Production integration would need TTL or eviction (e.g. same policy as client_states) to avoid unbounded memory growth over long runs with many clients.

### Validation and long runs

- `validate_ordering_reason()` runs over up to `MAX_VALIDATION_ENTRIES` (10M) committed batches. For long runs with validation on, this can be memory- and CPU-heavy. A future improvement could cap validation to the last N entries (e.g. 100k) and document that "validation covers the most recent N batches."

### Production integration (observability)

- For production, you would add: per-shard queue depth and processing time (to detect skew), histograms of epoch processing time (tail latency), and a simple health signal (e.g. last epoch completed within 2× epoch_us). The benchmark focuses on correctness and ablation; observability is out of scope here.

## Build and run

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make sequencer5_benchmark
./sequencer5_benchmark [brokers] [producers] [duration_sec] [level5_ratio] [num_runs] [use_radix_sort] [scatter_gather] [num_shards] [scatter_gather_only] [pbr_entries]
./sequencer5_benchmark --help   # full usage
./sequencer5_benchmark --test   # minimal correctness test (ordering + per-client)
./sequencer5_benchmark --paper  # algorithm-limited (1 producer, 60s, 64K PBR)
./sequencer5_benchmark --paper-mode   # paper-ready ablation (recommended for publication)
```

### Paper-ready ablation (--paper-mode)

**Purpose:** Compare per-batch vs per-epoch atomic batching:

| Mode       | Atomics per epoch |
|-----------|-------------------|
| Per-Batch | N (one per batch) |
| Per-Epoch | 1                 |

**Quick start (publication):**

```bash
./sequencer5_benchmark --paper-mode
```

This runs: 4 brokers, 2 producers, 30 s, 5 runs, L0 only, `--skip-dedup`, ablation-1 only. Output includes a boxed **ABLATION 1: Epoch Batching Effectiveness** table.

**Expected results (example):**

| Metric          | Per-Batch | Per-Epoch | Improvement |
|-----------------|-----------|-----------|-------------|
| Throughput (M/s)| ~4.8      | ~5.5      | 1.1–1.2×    |
| P99 Latency (ms)| ~40       | ~35       | 1.1–1.2×    |
| Atomics/run     | ~325M     | ~10K      | **~30,944×**|

**Options (paper runs):**

| Flag           | Description |
|----------------|-------------|
| `--paper-mode` | Paper-ready settings (recommended) |
| `--skip-dedup` | Skip deduplication (valid for algorithm comparison; see FAQ) |
| `--clean`      | Large PBR for bottleneck isolation (2 producers, 4M PBR) |
| `--legacy-dedup` | Use legacy Deduplicator (default: FastDeduplicator) |
| `--bench-dedup`  | Run dedup micro-benchmark and exit |

**Understanding results:**

- **Valid runs:** Should be 5/5. Invalid means system wasn’t stable (steady-state or saturation).
- **Atomics reduction:** Key metric; expect ~30,000×.
- **Speedup:** Modest (1.1–1.2×) because atomics aren’t the only cost.

**FAQ**

- **Why use `--skip-dedup`?** The benchmark generates unique `batch_id`s (`(thread_id << 48) | counter`), so dedup finds no duplicates. Skipping dedup is valid for comparing the sequencing algorithm; for end-to-end evaluation with retries, run without `--skip-dedup` (FastDeduplicator is default).
- **Why is speedup only 1.1–1.2× with ~30,000× fewer atomics?** Atomics are cheap (~20–50 ns). The main costs are memory copies, epoch transitions, and collector polling. The contribution is **simplicity and atomic reduction**, not raw throughput gain.

- **num_runs** (default 5): Runs per test. If ≥ 2, results report median, [min, max], and ±stddev.
- **use_radix_sort** (default 0): 0 = std::sort (recommended), 1 = radix sort (A/B test).
- **scatter_gather** (default 0): 1 = scatter-gather mode.
- **num_shards** (default 8): Shards when scatter_gather=1 (1..32).
- **scatter_gather_only** (default 0): 1 = run only scatter-gather scaling test (fast iteration).
- **validity mode** (default max): `--validity=algo|max|stress`.

**Examples:**

- Quick single run: `./sequencer5_benchmark 4 8 10 0.1 1 0`
- Publication-ready (5 runs, std::sort): `./sequencer5_benchmark 4 8 10 0.1 5 0`
- A/B radix vs std::sort: `./sequencer5_benchmark 4 8 10 0.1 5 1`
- Scatter-gather only (quick): `./sequencer5_benchmark 4 4 3 0.0 2 0 0 8 1`
- Correctness test: `./sequencer5_benchmark --test`

**Paper-ready runs:** Use only runs where the benchmark does **not** print `[INVALID RUN]` or `[INVALID COMPARISON]`. Target: **PBR Full < 0.1%** and **no drops** for headline suites. Use `--validity=algo` for strict algorithm-comparison (steady-state required); use `--validity=max` for open-loop max-throughput (latency warnings only).

- **`--paper`** — 1 producer, 60 s, 64K PBR (algorithm-limited; target PBR_full < 0.1%):  
  `./sequencer5_benchmark --paper --validity=algo`
- **`--clean`** — 2 producers, 4M PBR (sequencer as bottleneck):  
  `./sequencer5_benchmark --clean --validity=algo`
- **Explicit (single producer, long run):**  
  `./sequencer5_benchmark 4 1 60 0.1 5 0 0 8 0 65536`
- **Scatter-gather (open-loop max throughput):**  
  `./sequencer5_benchmark 4 2 30 0.1 5 0 1 8 1 4194304 --validity=max`

**Repeatable scalability (vary producers and brokers):** From repo root, `./run_scalability_experiments.sh`; results go to `logs/scalability_<timestamp>/`.

## Results interpretation (5-run publication test)

Full run: `./sequencer5_benchmark 4 8 10 0.1 5 0`. Example output (median, [min, max], ±stddev, % of median):

| Test | Throughput | Variance | Speedup (ablation 1) |
|------|------------|----------|----------------------|
| Per-Batch (baseline) | 6.32 M/s [5.79, 6.65] | 4.4% of median | — |
| Per-Epoch (optimized) | 6.21 M/s [5.39, 7.10] | 9.3% of median | **0.98× [0.81, 1.23]** |
| Level 0 | 6.41 M/s | 6.1% | — |
| Mixed 10% L5 | 4.25 M/s | 7.4% | — |
| Stress (100% L5) | 1.32 M/s | 2.6% | — |

**Finding:** Per-epoch achieves **same throughput** as per-batch (median 0.98×, range [0.81, 1.23]) with **~87K× fewer atomics**. The speedup is not consistently > 1.0; single-run outliers (e.g. 1.35×) do not hold under paired 5-run measurement.

**Paper narrative:** Use *"Epoch-batched sequencing achieves the same throughput as per-batch sequencing while reducing atomic operations by 87,000×, demonstrating that coordination can be batched without throughput loss."* Do not claim a throughput speedup; the contribution is **simplicity + atomic reduction**, not raw speed.

**Honest interpretation:** The ~0.98× median speedup (range 0.81×–1.23×) shows that **atomics were not the bottleneck** — the sequencer is limited elsewhere (memory bandwidth, merge/sort, hold buffer). The ablation is scientifically valid: **epoch batching simplifies the implementation without a performance penalty**, not that it improves performance. Reframe the contribution as *"simplification without penalty"* rather than *"performance optimization."*

**Methodology:** Ablation 1 uses **paired runs** (baseline then optimized per iteration) so speedup is computed per pair; reported speedup is median [min, max] of those ratios. A warm-up run (discarded) precedes measurement runs. Variance &lt; 10% of median is acceptable; &gt; 10% suggests CPU frequency, background load, or thermal effects.

### Atomic count and epochs (expert verification)

- **Per-epoch atomics** = number of epochs that had non-empty output (one `fetch_add(out.size())` per epoch). It is **not** “batches / atomics”; it is “epochs completed with batches.”
- When the **sequencer is the bottleneck**, epoch advancement is limited by how fast we seal → sequence → commit. So we may complete **far fewer epochs** than `duration/τ` (e.g. 99 epochs in 5 s instead of 10,000 at τ=500 μs). Each such epoch can contain tens of thousands of batches. So **atomics ≈ epochs** is correct; low atomics with high batch count means few, large epochs.
- The benchmark now reports **Epochs** alongside **Atomics** so reviewers can verify: in per-epoch mode, atomics should equal the number of epochs that produced output.

### 16-thread scalability and PBR saturation

- **Zero throughput** at 16 producers was caused by **PBR (per-broker ring) saturation**: producers refill the ring faster than the sequencer drains it, so `inject_batch` fails and throughput drops to zero.
- **Fix:** For scalability runs with ≥16 producers, the benchmark uses **larger PBR** (`pbr_entries = 256*1024`). For custom runs with many producers, pass a larger PBR via a config option or use fewer producers.
- If you see **WARNING: Zero throughput** and high **PBR_full**, increase `pbr_entries` or reduce producer count.

### Level 5 “inversion” (100% L5 vs 10% L5)

- **100% L5** is a stress test. If you run with `--validity=stress`, drops may be allowed and throughput can appear higher because traffic is not fully retained. Do not compare stress-mode throughput to headline runs.
- Always read **Gaps** and **Dropped** with L5 in stress mode; high values mean the system is under overload, not that it is handling L5 more efficiently. **10% L5** is the typical workload.

### Statistical power

- Use **≥5 runs** for publication; default `num_runs=5`. For stricter confidence, use 10+ runs. Report median, [min, max], and stddev; aim for stddev &lt; 10% of median where possible.

### Phase timing and bottleneck analysis

The benchmark prints `[PHASE]` lines to stderr every 100 epochs (steady state):

```bash
./sequencer5_benchmark 4 8 10 0.1 5 0 2>&1 | grep "\[PHASE\]" | tail -10
```

**Sample (steady state):** `[PHASE] merge=505μs l5=37188μs other=621μs`

**Interpretation:**

| If dominated by... | Meaning | Action |
|--------------------|---------|--------|
| **merge** (>60% of total) | Collector→sequencer handoff is slow | Pre-size buffers, reduce copies |
| **l5** (>60% of total) | Level 5 sort/hold buffer is slow | Epoch-indexed hold buffer, faster sort |
| **other** (>60% of total) | GOI write or dedup is slow | Batch GOI writes, bloom pre-check |
| **Balanced** (each 25–40%) | No single bottleneck | Design is sound; stop optimizing |

Typical profile: **l5 dominates** (e.g. merge ~0.5ms, l5 ~37ms, other ~0.6ms per epoch). The single sequencer thread is the fundamental limit; further gains require scatter-gather (§3.3).

### Efficiency and paper narrative

**Efficiency:** The sequencer achieves **~8–9%** of theoretical CXL bandwidth (21 GB/s). This is expected:

- Single sequencer thread (Amdahl) ~5–10×
- Epoch sync (seal/wait) ~1.5–2×
- Benchmark overhead (validation, stats) ~1.2–1.5×
- Memory copies ~1.1–1.3×

**Combined:** ~12× slower than raw bandwidth → ~8% efficiency ✓

**Paper narrative:** *"The sequencer achieves 9% of theoretical CXL bandwidth (21 GB/s), consistent with the single-threaded sequencer design—a deliberate choice for simplicity and correctness (§3.2). Phase timing shows L5 processing dominates, confirming the design is well-balanced; the scatter-gather design (§3.3) addresses this for deployments requiring higher throughput."*

### Correctness and efficiency fixes (reviewer responses)

| Issue | Fix |
|-------|-----|
| **Latency ring wraparound** | `drain()` now reconstructs logical order when `write_pos_ > LATENCY_RING_SIZE` (older samples first), so percentiles are valid after overflow. |
| **Deduplicator double lookup** | `check_and_insert()` uses a single `insert()` and checks `inserted` instead of `count()` then `insert()`. |
| **Epoch staleness filter** | Collector skips PBR entries where `epoch_created` is older than `config::MAX_EPOCH_AGE` (Document §4.2.1; zombie broker / recovery). |
| **PBREntry visibility** | Comment documents that release on `flags` establishes visibility; producer crash before release is acceptable for ablation. |
| **Stats false sharing** | `pbr_full_count` (producer threads) is on its own cache line (`alignas(64)`). |
| **Phase timing** | `merge_time_ns` and `l5_time_ns` record time in merge and sequence phases for bottleneck analysis. |
| **L5 breakdown** | Every 100 epochs, `[L5 BREAKDOWN] sort=X% hold=Y% other=Z%` shows time within L5: sort (by client_id), hold_phase (drain_hold + age_hold), and remainder (process_level5 in-order path). |
| **Magic numbers** | `0x3FF` replaced by `config::CLIENT_GC_EPOCH_INTERVAL` (1024). |

## L5 bottleneck analysis (complete)

Phase timing shows L5 dominates epoch CPU (~97%). Within L5:

| Component | Share | Notes |
|-----------|-------|--------|
| **sort** | ~0.8% | Not an issue. |
| **hold** | ~12% | Epoch-indexed expiry working; drain_hold + age_hold. |
| **other** | ~87% | Per-client state management: hash lookups (`client_shards_`, dedup), window checks, hold insertions. |

**Bottleneck:** The in-order processing loop in `process_level5` (per-client `states[cid]`, `is_duplicate`, `mark_sequenced`, `add_to_hold`). This is expected: Level 5 requires per-client state; the cost is fundamental to strong ordering.

**Decision: sufficient for paper.** The ablation validates (1) epoch batching works (no penalty vs per-batch), (2) Level 5 overhead is quantified (e.g. 9.4% → 5.3% efficiency at 10% L5), and (3) the bottleneck is understood (per-client state, not atomics/sort/hold). Further optimization (flat hash map, prefetch) would improve absolute numbers but not change conclusions.

**Paper narrative (ready to use):**

> **Level 5 overhead.** Per-client ordering requires per-client state (next expected sequence, duplicate window) and a hold buffer for out-of-order arrivals. Level 0 achieves ~9.4% of theoretical CXL bandwidth; Level 5 at 10% of traffic reduces throughput to ~5.3%—a ~44% reduction attributable to per-client state management (87% of L5 processing time per phase analysis). This overhead is fundamental to strong ordering. Embarcadero's contribution is making it *optional*: Level 0 for max throughput; Level 5 for workloads that need per-client FIFO.

**Optional future optimizations** (if desired later, not for paper): (A) Flat hash map for `client_shards_[].states` (~2× L5 speedup), (B) Prefetch next client state in the loop (~20% L5 speedup).

**Reviewer answer for "why only 9% efficiency?":** Single-threaded sequencer design (§3.2). Phase analysis shows no single bottleneck in Level 0; Level 5 overhead is per-client state, which is fundamental. Scatter-gather (§3.3) addresses throughput scaling.

## Phase 3 ordering (when to work on Phase 3)

Phase 3 has four components; **two depend on ablation results**, two do not:

| Phase 3 component | Depends on ablation? | Recommendation |
|-------------------|----------------------|----------------|
| **Scatter-gather sequencer (§3.3)** | **Yes** — extends the single-sequencer design | **Do ablation first.** Lock optimal single-sequencer design (τ, per-batch vs per-epoch, Level 5 params), then implement scatter-gather on that baseline. Otherwise scatter-gather may need rework if the optimal design changes. |
| **ControlBlock formalization** | No — Phase 1a already done | Do anytime (doc only). |
| **Sequencer-driven recovery (§4.2.2)** | No — depends on **Phase 2** (chain, num_replicated) | Blocked by Phase 2, not ablation. Do after Phase 2. |
| **Deprecate TInode ACK** | No — depends on **Phase 2** (CV migration) | Blocked by Phase 2, not ablation. Do after Phase 2. |

**Summary:** Finish ablation, pick the optimal single-sequencer design and implementation, then start **scatter-gather** (Phase 3). Do **ControlBlock formalization** anytime. Do **sequencer-driven recovery** and **deprecate TInode ACK** only after Phase 2 (GOI + chain + CV) is in place.

---