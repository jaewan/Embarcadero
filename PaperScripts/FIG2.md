# Fig 2 — Latency vs offered load (shared-log SLO)

## Claim

Under the paper **shared-log contract** (ORDER=5, ACK=2, RF=2, disk-durable),
Embarcadero publish→deliver latency stays low as offered load increases until
the knee. A small **matched-load mechanism table** shows what each Embar knob
costs (O0 ACK1 → O5 ACK1 → O5 ACK2 RF2).

Fig2 is the latency companion to Fig1 throughput (same RF2 durability claim).

## Fixed knobs (publication)

| Knob | Value | Why |
|------|-------|-----|
| Brokers | 4 | Paper topology |
| **Primary RF / ACK** | **2 / 2** | Matches Fig1 / shared-log guarantee |
| Sink | disk-durable (dual NVMe) | Media-durable ACK2 |
| Publisher | 1 remote (`c4`) | Isolates offered-load axis |
| Message size | 1024 B | Latency checklist |
| Bytes / trial | 4 GiB | Steady percentiles |
| Pacing | **`open_loop`** | Paper contract |
| Epoch µs | 500 | ORDER=5 design point |
| Runtime | `latency` (linger ON) | Headline Embar |
| CXL size | 256 GiB | Match Fig1 |
| CXL zero | **`full`** (default) | `FIG2_FAST_CXL=1` → metadata smoke |
| Build | `-DCOLLECT_LATENCY_STATS=ON` | Preflighted |

## Independent variable = offered load (MB/s)

Default:

```
100 250 500 750 1000 1500 2000
```

## Series

### Primary panel (full load sweep)

| Cell | Config |
|------|--------|
| `fig2_embar_o5_ack2_rf2` | Embar O5 ACK2 RF2 disk-durable |

### Mechanism ablation (one matched load, default 500 MB/s)

| Cell | Isolates | Metric |
|------|----------|--------|
| `fig2_mech_embar_o0_ack1_rf0` | Unordered floor | **append→ack** |
| `fig2_mech_embar_o5_ack1_rf0` | + ordering | append→ack (+ deliver) |
| `fig2_mech_embar_o5_ack2_rf2` | + durable RF2 | append→ack (+ deliver) |

Use append→ack so O0 is comparable (O0 has no `ConsumeOrdered` deliver stamp).

### Optional (off by default)

| Knob | Effect |
|------|--------|
| `SKIP_RF0_COMPANION=0` | Full-sweep Embar O5 ACK1 RF0 companion |
| `SKIP_NOLINGER=0` | RF2 nolinger companion |
| `INCLUDE_BASELINES=1` | Corfu/Scalog at `BASELINE_LOAD_MBPS` only (default `500 1000`) — **not** a full baseline sweep |
| `SKIP_MECHANISM=1` | Skip ablation table |

## Metric

- **Primary curve:** publish→deliver p50/p99 from `delivery_latency_stats.csv`
- **Mechanism table:** append→ack p50/p99; deliver shown when present
- Rows with `achieved < 50% of target` get `saturated_offered_lt_50pct_target`

## Appendable results

```
data/paper_eval/fig2/<CAMPAIGN_ID>/results.csv
data/paper_eval/fig2/<CAMPAIGN_ID>/mechanism_summary.csv
data/paper_eval/fig2/<CAMPAIGN_ID>/campaign_contract.md
```

Schema upgrades rotate the old CSV aside. `TARGET_TRIALS=K` skips
`(cell, target_mbps)` pairs with ≥ K `ok` rows.

## How to run

```bash
# Smoke
FIG2_FAST_CXL=1 NUM_TRIALS=1 LOAD_POINTS_MBPS="100 500" \
  MECHANISM_LOAD_MBPS=100 TOTAL_BYTES=$((512<<20)) \
  bash PaperScripts/run_fig2_latency_vs_load.sh

# Publication pass
NUM_TRIALS=3 WARMUP_TRIALS=1 \
  bash PaperScripts/run_fig2_latency_vs_load.sh

# Primary only
ONLY_CELLS=fig2_embar_o5_ack2_rf2 \
  SKIP_MECHANISM=1 NUM_TRIALS=1 bash PaperScripts/run_fig2_latency_vs_load.sh

# + matched-load baselines
INCLUDE_BASELINES=1 BASELINE_LOAD_MBPS="500 1000" \
  NUM_TRIALS=1 bash PaperScripts/run_fig2_latency_vs_load.sh

# Replot
python3 PaperScripts/plot_fig2_latency_vs_load.py \
  --csv data/paper_eval/fig2/fig2_latency_vs_load/results.csv \
  --pdf data/paper_eval/fig2/fig2_latency_vs_load/fig2_latency_vs_load.pdf
```

## Prerequisites

1. Rebuild with **`-DCOLLECT_LATENCY_STATS=ON`** (local + `c4`).
2. Dual NVMe replica dirs writable (same as Fig1).
3. Cluster idle (`WAIT_FOR_IDLE=1`).

## Caveats

### A. Why RF2 (not RF0) for the primary curve

The paper claims a durable shared log. Fig1 already uses RF2/ACK2. Showing only
RF0 latency would understate the guarantee clients wait on.

### B. Mechanism table vs full sweep

Ablation is **one load** so the figure stays readable. Do not expand it into
three full load sweeps unless space allows.

### C. open_loop vs steady

Default `open_loop`. Overnight E3 historically used `steady` — do not mix.

### D. Baselines

Only 1–2 matched loads if paced harnesses match. Prefer RF0 ACK1 baselines
labeled as such; do not plot RF0 baseline against RF2 Embar as “same claim.”

### E. E2E / delivery inset

Keep deliver-path commentary to one sentence + optional tiny inset — do not let
per-connection delivery goodput steal the figure.

### F. CXL zero mode

Publication default `full`. `FIG2_FAST_CXL=1` is smoke-only.
