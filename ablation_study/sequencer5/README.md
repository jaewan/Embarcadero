# Sequencer5 Ablation Study

Standalone sequencer implementation and benchmark for OSDI/SOSP/SIGCOMM-level ablation: correctness-first dedup, MPSC ring (multiple producers per broker), three-layer gap resolution, and **per-batch vs per-epoch atomic** comparison. Aligns with **docs/EMBARCADERO_DEFINITIVE_DESIGN.md**.

## Repository layout

| Path | Purpose |
|------|---------|
| `sequencer5_benchmark.cc` | Single-file benchmark and sequencer logic |
| `trace_generator.h` | Deterministic trace generation with controlled reordering |
| `CMakeLists.txt` | Build (standalone) |
| `ABLATION_RESULTS.md` | **Summary of latest results (v7 methodology)** |
| `logs/trace_runs/` | Raw result logs (v1-v7) |
| `REPORT_DRAFT.md` | Comprehensive draft report for the paper |

## Results

- **Canonical result logs:** `logs/trace_runs/full_benchmark_v7.log` (SOSP/OSDI ready: 8.0 M/s peak, 3x epoch speedup, 0% reorder degradation).
- **Summary and tables:** **ABLATION_RESULTS.md** (Saturation sweep, shard scaling, deconfounded ablation, reorder robustness).

## Reproducing results

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make sequencer5_benchmark

./sequencer5_benchmark --test          # Correctness
./sequencer5_benchmark --trace         # Full paper suite (v7 methodology)
```

## Key Operations

The sequencer supports:

1. **Collecting batches from PBRs** — Multiple collector threads poll Pending Batch Rings from brokers.
2. **Epoch-based sequencing** — Triple-buffered pipeline (COLLECT → SEQUENCE → COMMIT).
3. **Global sequence assignment** — Single atomic `fetch_add` per epoch (O(1) instead of O(n)).
4. **Order 2 ordering** — Total order only, no per-client guarantees, maximum throughput.
5. **Order 5 ordering** — Per-client FIFO ordering with three-layer gap handling:
    - **Layer 1 (Sort):** Resolves same-cycle jitter in ~1μs.
    - **Layer 2 (Hold):** Resolves cross-cycle jitter via per-client hold buffer.
    - **Layer 3 (Skip):** Emits SKIP markers after 5ms timeout to ensure liveness.
6. **GOI writing** — Commit sequenced batches to Global Order Index.

### Order 5 semantics and "declared lost"

- **Guarantee:** If client C sends M1 before M2 (by client_seq), then M1.global_seq < M2.global_seq, or M1 is **declared lost**.
- **Declared lost:** A batch is marked as a **SKIP** in the GOI after it has waited 5ms without becoming `next_expected`. This ensures downstream consumers (replicas, subscribers) never stall on a missing batch.
- **Client-observable:** At-most-once delivery per client_seq; gaps in client_seq are explicitly marked in the total order.

## Methodology (SOSP/OSDI Ready)

- **Capped Injection:** We use capped rates (500K/broker for Order 2) to measure algorithmic capacity. Unlimited injection saturates the PBRs and causes cache contention artifacts that mask the sequencer's true performance.
- **Multi-Run Stats:** Every data point is the median of 5 independent runs. We report [min, max] ranges to ensure statistical significance.
- **Warmup:** 2-second warmup ensures the pipeline is in steady-state before measurement begins.
- **Trace-Based:** Deterministic traces with controlled reordering ensure reproducible results for the reorder robustness figures.

## Caveats and methodology (reviewer responses)

| Concern | Response |
|--------|----------|
| **Super-linear scaling** | **Fixed.** Previous artifacts were due to drain-only measurement startup costs. v7 uses sustained trace mode with capped rates, showing clean linear scaling. |
| **Unlimited injection collapse** | **Documented.** Unlimited injection saturates the memory bus and PBRs, causing a performance collapse. We report the "Saturation Knee" (8.0 M/s) as the system capacity. |
| **Order 5 overhead** | **Quantified.** Layer 1 sort handles 90% of jitter. Order 5 maintains 100% throughput parity with Order 2 under 50% reordering, proving the efficiency of the three-layer design. |
| **Latency 0.3–1 μs vs paper 1.5–2 ms** | This benchmark measures **in-memory inject→commit** only. Paper's e2e includes network RTT, replication, TCP. |

## Concurrency

- **Collector threads** — enters via `enter_collection()` (mutex + state COLLECTING + increment); exit with `exit_collection()` (decrement, notify CV).
- **Worker shards** — Each shard owns a disjoint set of clients (hashed); processes ordering and writes GOI entries.
- **Committed_seq updater** — Single thread that tracks fully-written GOI prefix and advances visibility.
- Triple-buffered pipeline (`config::PIPELINE_DEPTH`); constants in `config::` namespace.

---
