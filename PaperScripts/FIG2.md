# Fig 2 — Append latency vs offered load (coordination claim)

## Claim

Under **ORDER=5 ACK=2 RF=2 DRAM replica completion**, Embarcadero
**append→ACK** latency stays low / epoch-bounded as offered load increases
(paper `tab:latency-sweep` / “RF2 nearly free on the CXL poll path”).

A **disk ablation** at one matched load shows media-durable RF2 cost (Fig1 disk
contract). A **mechanism table** decomposes O0 → O5 → DRAM RF2 → disk RF2.
Matched **RF2 ACK2 DRAM** Corfu/Scalog points share the primary sink.

Publish→deliver remains a **scoped inset** (ordered-consume ceiling ~270 MB/s).

## Fixed knobs (publication)

| Knob | Value | Why |
|------|-------|-----|
| Brokers | 4 | Paper topology |
| **Primary RF / ACK / sink** | **2 / 2 / DRAM replica** | Coordination claim |
| Disk ablation | RF2 ACK2 disk-durable @ matched load | Media cost vs primary |
| Publisher | 1 remote (`c4`) | Offered-load axis |
| Message size | 1024 B | Paper table |
| Bytes / trial | 4 GiB | Steady percentiles |
| Pacing | **`steady`** | Paper table |
| Epoch µs | 500 | ORDER=5 design point |
| Runtime | `latency` (linger ON) | Headline Embar |
| CXL size | 72 GiB (script default) | Four segments + GOI |
| CXL zero | **`metadata`** | Practical restart |
| Build | `-DCOLLECT_LATENCY_STATS=ON` | Preflighted |

## Series

| Cell | Role |
|------|------|
| `fig2_embar_o5_ack2_rf2_mem` | **Primary** full load sweep |
| `fig2_embar_o5_ack2_rf2_disk` | Disk ablation (matched load) |
| `fig2_mech_embar_o0_ack1_rf0` | Mechanism: unordered floor |
| `fig2_mech_embar_o5_ack1_rf0` | Mechanism: + ordering |
| `fig2_mech_embar_o5_ack2_rf2_mem` | Mechanism: + mem RF2 |
| `fig2_mech_embar_o5_ack2_rf2_disk` | Mechanism: + disk RF2 |
| `fig2_corfu_o2_ack2_rf2_mem` | Baseline (mem) |
| `fig2_scalog_o1_ack2_rf2_mem` | Baseline (mem) |

## Metric

- **Primary:** append→ack (`pub_ack_*`, batch)
- **Deliver inset:** Embar mem primary, loads ≤300 MB/s
- Notes: `saturated_offered_lt_50pct_target`, `saturated_e2e_lt_50pct_target`

## How to run

```bash
NUM_TRIALS=1 bash PaperScripts/run_fig2_latency_vs_load.sh

# Mem-only (skip disk ablation)
SKIP_DISK_ABLATION=1 NUM_TRIALS=1 bash PaperScripts/run_fig2_latency_vs_load.sh

# Multi-trial publication
NUM_TRIALS=3 WARMUP_TRIALS=1 bash PaperScripts/run_fig2_latency_vs_load.sh
```

Default `CAMPAIGN_ID=fig2_append_latency`.

## Caveats

- **DRAM replica ≠ media-durable.** ACK2 waits for a full DRAM replica copy
  (coordination + memory-class durability assumption). True NVMe durability is
  the disk ablation / Fig1 disk panel.
- **Do not** plot mem-Embar against disk-baselines as peers.
- **Batch** `pub_ack_*` — report as batch append→ack.
- Do not mix `steady` and `open_loop` in one plot.
- **ACK-primary soft-fail:** `EMBARCADERO_LATENCY_ACK_PRIMARY=1` (Fig2 default)
  treats deliver-drain timeout as a note when pub ACK artifacts exist; true
  reorder/dup faults still hard-fail. Mutual exclusion via
  `/tmp/embarcadero_paper_fig2.lock` — never run two Fig2 campaigns in parallel.
