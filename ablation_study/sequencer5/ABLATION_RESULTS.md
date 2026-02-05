## Embarcadero Sequencer Ablation Study

**Canonical result logs:** `logs/scalability_20260205_083244/` (6 runs), `logs/profile_20260205_080856.log`, `logs/PROFILE_AND_STABILITY_RESULTS.md`.

---

## Experimental Setup

**Configuration:** 4 brokers (unless varied), 2–8 producer threads, 1KB batches, 10 messages/batch, τ=500μs epochs, `--skip-dedup`, `--suite=ablation`.  
**Methodology:** 5 runs per configuration, 30s measurement, median with [min, max] and ±stddev. Ordering validation enabled.  
**Scope:** In-memory sequencer only; no network or CXL hardware.

---

## 1. Per-Batch vs Per-Epoch Atomics (current results)

Data from **logs/scalability_20260205_083244/** (30s, 5 runs each).

### Vary producers (4 brokers fixed)

| Producers | Per-batch (M/s) | Per-epoch (M/s) | Speedup | Atomic reduction |
|-----------|-----------------|-----------------|---------|-------------------|
| 2 | 4.20 [3.48, 4.72] | 4.69 [3.63, 5.12] | **1.14×** [0.86, 1.27] | 22,403× |
| 4 | 5.61 [4.61, 7.69] | 5.90 [5.43, 6.95] | **1.09×** [0.71, 1.25] | 35,336× |
| 8 | 6.27 [6.20, 6.39] | 6.10 [5.72, 6.41] | **0.98×** [0.89, 1.03] | 35,444× |

### Vary brokers (4 producers fixed)

| Brokers | Per-batch (M/s) | Per-epoch (M/s) | Speedup | Atomic reduction |
|---------|-----------------|-----------------|---------|-------------------|
| 2 | 5.13 [4.79, 5.82] | 4.87 [4.74, 6.90] | **0.92×** [0.83, 1.44] | 34,030× |
| 4 | 5.05 [4.68, 5.10] | 5.45 [5.30, 6.10] | **1.10×** [1.04, 1.20] | 27,434× |
| 8 | 5.20 [4.86, 5.84] | 5.64 [4.73, 6.06] | **1.04×** [0.83, 1.18] | 30,566× |

**Finding:** Per-epoch batching achieves **0.92×–1.14×** throughput vs per-batch (median ~1.0×) with **~22k–35k× fewer atomics**. Throughput parity holds across producer and broker counts; atomic reduction is consistently four orders of magnitude.

---

## 2. Phase profiling and stability

- **Phase breakdown:** See `logs/PROFILE_AND_STABILITY_RESULTS.md` and `logs/profile_20260205_080856.log`. Per-epoch cost is dominated by merge, partition, and L0 (dedup when enabled); assign (single fetch_add + loop) is a small fraction.
- **FastDeduplicator:** Stability verified via `--test-dedup` and multi-run ablation without `--skip-dedup`; no crashes. See same doc.

---

## 3. Level 5 and scalability (design narrative)

- **Level 5 overhead:** Per-client ordering adds per-client state and hold-buffer cost; phase analysis shows L5 logic dominates when enabled. Quantified in earlier runs (see README); current scalability suite is L0-only (`0% L5`).
- **Single-thread bottleneck:** Throughput plateaus as producers increase; sequencer thread is the ceiling. Scatter-gather (§3.3 in design doc) addresses scaling.
- **Efficiency:** Sequencer achieves a fraction of theoretical CXL bandwidth; single-threaded design is a deliberate tradeoff for simplicity and correctness.

---

## 4. Reproducing results

```bash
cd build
./sequencer5_benchmark --test-dedup                    # FastDeduplicator stability
./run_scalability_experiments.sh                      # Writes to ../logs/scalability_<timestamp>/
# Or single config, e.g.:
./sequencer5_benchmark 4 4 30 0.0 5 --skip-dedup --suite=ablation
```

---

*Summary table above is derived from the canonical logs in `logs/`. For raw output and phase lines, use those files.*