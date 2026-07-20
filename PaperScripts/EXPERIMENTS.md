# Paper Experiment Registry

Maps each paper figure/table to the script that produces it,
the data directory with results, and the plot script.

## Figures

### Fig 1 — ACK-paced throughput scaling (RF=2/ACK2)
- **Script:** `PaperScripts/run_fig1_throughput_scaling.sh`
- **Data:** `data/paper_eval/fig1/fig1_rf2_ack2_scaling/results.csv`
- **Plot:** `PaperScripts/plot_fig1_throughput_scaling.py`
- **Notes:** Embar O5 + Corfu + Scalog × {disk, mem}. LazyLog excluded (metadata-bound; see FIG2_LAZYLOG.md).

### Fig 2 — Append latency vs offered load (mechanism proof)
- **Script:** `PaperScripts/run_fig2_latency_vs_load.sh`
- **Data:** `data/paper_eval/fig2/fig2_append_latency/results.csv`
- **Plot:** `PaperScripts/plot_fig2_latency_vs_load.py`
- **Notes:** Also produces Tab. epoch-sweep and the mechanism ablation.
  Corfu/Scalog comparison: `data/paper_eval/fig2/fig2_corfu_official_3eaadffb/` and `fig2_scalog_official_3eaadffb/`.

### Fig 3 — Failure and per-session recovery
- **Script:** `PaperScripts/run_fig3_failure.sh`
- **Data:** `data/paper_eval/fig3/`
- **Notes:** Q2 experiment. Uses `failure_combined.pdf` figure.

## Tables

### Tab. latency-sweep — Embar append latency across load
- **Script:** `PaperScripts/run_fig2_latency_vs_load.sh` (subset of Fig 2 campaign)
- **Data:** `data/paper_eval/fig2/fig2_append_latency/results.csv` (Embar rows)

### Tab. epoch-sweep — Epoch-period causal proof
- **Script:** `PaperScripts/run_fig2_latency_vs_load.sh` (epoch sweep cells)
- **Data:** `data/paper_eval/fig2/fig2_append_latency/results.csv` (epoch_us varied cells)

### Tab. latency — Cross-system append latency at 2 GB/s
- **Script:** `PaperScripts/run_fig2_latency_vs_load.sh`
- **Data:** `data/paper_eval/fig2/fig2_append_latency/results.csv` (Embar + Scalog)

### Tab. kv-pipelined — SMR FIFO end-to-end (Q3)
- **Script:** `benchmarks/kv_store/run_smr_fifo_eval.sh`
- **Data:** `data/smr_fifo_paperscale/`
- **Key results:** Embar 655K ops/s (Valid=YES), Corfu 649K (Valid=YES), Scalog 626K (Valid=NO, 13,719 reorders, 348× serialize cost)

### Tab. slow-replica — Ordering-replication independence
- **Script:** `scripts/run_slow_replica_heterogeneity.sh`
- **Data (Embar):** `data/latency/slow_replica/EMBARCADERO/`
  - Baseline: ACK1 P99 ~1.1ms, ACK2 P99 ~409ms
  - Slow-sync: ACK1 P99 ~1.1ms (unchanged), ACK2 P99 ~10,472ms (+25.6×)
- **Data (LazyLog):** `data/latency/slow_replica_lazylog_rf2_2b_v3/`
  - Baseline: ACK1 P99 ~1,006ms, ACK2 P99 ~976ms
  - Slow-stop: ACK1 P99 ~1,121ms (+11.5%), ACK2 P99 ~937ms (within noise)
- **Notes:** See `PaperScripts/SLOW_REPLICA.md` for full config and analysis.

### Tab. ablation-throughput — Throughput path ablation
- **Script:** `PaperScripts/run_fig1_path_decomp.sh`
- **Data:** `data/paper_eval/fig1/` (path decomp cells)

## Appendix / Supporting

### LazyLog faithful ACK measurement (FIG2_LAZYLOG)
- **Script:** `PaperScripts/run_fig1_throughput_scaling.sh` with `SKIP_LAZYLOG=0`
- **Plan:** `PaperScripts/FIG2_LAZYLOG.md`
- **Status:** Pending end-to-end hardware validation runs.

