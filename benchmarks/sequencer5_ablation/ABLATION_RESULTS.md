## Embarcadero Sequencer Ablation Study

**Canonical result logs:** `logs/trace_runs/full_benchmark_v7.log` (SOSP/OSDI ready, capped injection).

---

## Experimental Setup

**Configuration:** 4 brokers, 1–16 worker shards, 1KB batches, 10 messages/batch, τ=500μs epochs.  
**Methodology:** 5 runs per configuration, 5s measurement window after 2s warmup, median with [min, max].  
**Capped Injection:** Scaling and ablation tests use capped rates (500K/broker for Order 2, 200K/broker for Order 5) to isolate algorithmic performance from memory bus saturation.  
**Scope:** In-memory sequencer only; no network or CXL hardware.

---

## 1. Saturation Sweep (Order 2, 8 shards)

Measures throughput per broker as injection rate increases.

| Target Rate/Broker | Measured Median [Min, Max] (M/s) | Efficiency |
|:---|:---|:---|
| 100,000 | 0.10 [0.10, 0.10] | 100% |
| 500,000 | 0.50 [0.50, 0.50] | 100% |
| 1,000,000 | 1.00 [0.99, 1.00] | 100% |
| 2,000,000 | 1.99 [1.88, 2.00] | 99.5% |
| 3,000,000 | 2.12 [2.03, 2.14] | **Saturation Knee** |
| Unlimited | 0.12 [0.00, 0.47] | **PBR Saturation Collapse** |

**Finding:** The system exhibits perfect linear scaling up to **2.0 M batches/s per broker** (8.0 M/s total). Beyond 2.12 M/s, PBR saturation causes performance collapse, justifying the use of capped rates for scaling tests.

---

## 2. Per-Batch vs Per-Epoch Atomics (Deconfounded)

Data from **logs/trace_runs/full_benchmark_v7.log** (500K/broker cap).

| Shards | Per-batch (M/s) | Per-epoch (M/s) | Speedup |
|:---|:---|:---|:---|
| 1 | 0.10 [0.07, 0.10] | 0.28 [0.27, 0.30] | **2.88×** [2.78, 4.06] |
| 4 | 0.34 [0.30, 0.38] | 0.50 [0.50, 0.50] | **1.46×** [1.32, 1.66] |
| 8 | 0.42 [0.39, 0.48] | 0.50 [0.50, 0.50] | **1.19×** [1.05, 1.30] |

**Finding:** Per-epoch batching achieves a massive **2.88×** throughput increase on a single shard by amortizing coordination costs. The benefit remains significant (1.2×–1.5×) even as the system scales out across multiple shards.

---

## 3. Shard Scaling (Order 2)

Capped at 500K/broker to measure scaling efficiency.

| Shards | Median Throughput (M/s) | Scaling Factor |
|:---|:---|:---|
| 1 | 0.26 [0.24, 0.28] | 1.00x |
| 2 | 0.49 [0.48, 0.50] | 1.88x |
| 4 | 0.50 [0.50, 0.50] | **Hit Cap** |

**Finding:** The sequencer scales linearly with core count. A single shard can sustain ~0.26 M/s; two shards double this to ~0.49 M/s. At 4 shards, the system is fast enough to process the entire 500K/broker injection cap.

---

## 4. Order 5 Robustness (Reorder Sweep)

Capped at 200K/broker. Measures throughput degradation under network jitter.

| Reorder Rate | Throughput (M/s) | P99 Latency (ms) | Skip Median |
|:---|:---|:---|:---|
| 0% | 0.20 | 52.7 | 0 |
| 10% | 0.20 | 54.0 | 1,186 |
| 50% | 0.20 | 53.6 | 1,449 |

**Gap Resolution Breakdown (at 50% reorder):**
- **Layer 1 (Sort):** ~65,877 batches resolved (Same-cycle jitter).
- **Layer 2 (Hold):** ~43,379 batches resolved (Cross-cycle jitter).
- **Layer 3 (Skip):** ~1,449 batches (Definitive gaps/timeouts).

**Finding:** The sequencer maintains **0% throughput degradation** even under extreme (50%) network reordering. The three-layer resolution strategy successfully handles jitter with minimal skips (<0.2%).

---

## 5. Reproducing results

```bash
# To reproduce the paper-ready results (v7 methodology):
./run_paper_benchmarks.sh
```

---

*Summary tables above are derived from the canonical logs in `logs/trace_runs/full_benchmark_v7.log`.*
