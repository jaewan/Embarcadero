# Paper Experiment Registry

Maps each paper figure/table to the script that produces it,
the data directory with results, and the plot script.

## Figures

### Fig 1 — ACK-paced throughput scaling (RF=2/ACK2)
- **Script:** `PaperScripts/run_fig1_throughput_scaling.sh`
- **Data:** fixed-commit campaigns
  `data/paper_eval/fig1/fig1_{embar,scalog}_official_3eaadffb/results.csv`
- **Plot:** `PaperScripts/plot_fig1_throughput_scaling.py`
- **Manifest:** `data/paper_eval/fig1/fig1_fixed_commit_3eaadffb_manifest.json`
- **Selected rows:** `data/paper_eval/fig1/fig1_fixed_commit_3eaadffb_selected.csv`
- **Generated figure:** `data/paper_eval/fig1/throughput_scaling.{pdf,png}`
- **Notes:** The plot accepts every successful row at commit `3eaadffb`; no
  performance-based outlier filtering. N<=3 uses overlap throughput and N=4
  uses ACK-drain aggregate bandwidth.

### Fig 2 — Append latency vs offered load (mechanism proof)
- **Script:** `PaperScripts/run_fig2_latency_vs_load.sh`
- **Primary validator:** `PaperScripts/summarize_fig2_primary.py` (requires
  exactly 3 successful steady-rate trials at every load and a clean campaign
  contract)
- **Primary data:** `data/paper_eval/fig2/fig2_append_latency_primary_806b1809/results.csv`
- **Primary summary:** `data/paper_eval/fig2/fig2_append_latency_primary_806b1809/primary_summary.csv`
- **Primary manifest:** `data/paper_eval/fig2/fig2_append_latency_primary_806b1809/primary_manifest.json`
- **Paper plot:** `PaperScripts/plot_append_latency_paper.py`
- **Full diagnostic plot:** `PaperScripts/plot_fig2_latency_vs_load.py`
- **Notes:** Also produces Tab. epoch-sweep and the mechanism ablation.
  Corfu/Scalog comparison: `data/paper_eval/fig2/fig2_corfu_official_3eaadffb/` and `fig2_scalog_official_3eaadffb/`.
  Validate each clean baseline campaign with
  `PaperScripts/summarize_fig2_baseline.py`.
  Each new pass stores the commit, complete working-tree patch, binary/config
  hashes, remote-client binary hash, and parameters under
  `<campaign>/provenance/<pass-id>/`.

### Fig 3 — Failure and per-session recovery
- **Script:** `PaperScripts/run_fig3_failure.sh`
- **Data:** `data/paper_eval/fig3/fig3_failure_official_8351459f/`
- **Validator:** `PaperScripts/summarize_fig3_failure.py`
- **Notes:** Q2 experiment. The validator derives detection, fence, resubmit,
  and ACK-frontier offsets from all three raw traces and records that this is a
  protocol-ACK audit, not a downstream apply-state audit.

### Fig. 1 path ablation
- **Script:** `PaperScripts/run_fig1_path_decomp.sh`
- **Validator:** `PaperScripts/summarize_fig1_path_ablation.py`
- **Data:** `data/paper_eval/fig1/fig1_path_decomp/results.csv`
- **Metric:** N=2 overlap throughput, matching the main N<=3 methodology.

### Q2 two-session gap isolation
- **Script:** `scripts/run_failure_suite.sh` (E4a session-gap mode)
- **Validator:** `PaperScripts/summarize_session_isolation.py`
- **Data:** `data/failure_suite/e4a_commit_official_806b1809/e4a_session_gap/`
- **Status:** the existing result passes semantic checks but records a dirty
  worktree, so it must be rerun from a clean commit before submission.

## Tables

### Tab. latency-sweep — Embar append latency across load
- **Script:** `PaperScripts/run_fig2_latency_vs_load.sh` (subset of Fig 2 campaign)
- **Data:** `data/paper_eval/fig2/fig2_append_latency_primary_806b1809/primary_summary.csv`

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
- **Aggregator:** `PaperScripts/build_slow_replica_summary.py`
- **Data (Embar):** `data/latency/slow_replica/trial_{1,2,3}/EMBARCADERO/`
  - Baseline: ACK1 P99 1.091ms, ACK2 P99 411ms
  - Slow-sync: ACK1 P99 1.126ms, ACK2 P99 10,476ms (+25.5×)
- **Data (Scalog):**
  `data/latency/slow_replica_scalog_ack1_official_806b1809/trial_{1,2,3}/SCALOG/`
- **LazyLog:** not reported; its paced fault workload failed the fail-closed
  delivery checker.
- **Notes:** See `PaperScripts/SLOW_REPLICA.md` for full config and analysis.
  The paper-facing canonical source is
  `data/latency/slow_replica/paper_stage_rows.csv` with hashes and
  `paper_summary.json`; the legacy root `slow_replica_comparison.csv` is not
  used because a later failed LazyLog attempt overwrote it.

### Tab. ablation-throughput — Throughput path ablation
- **Script:** `PaperScripts/run_fig1_path_decomp.sh`
- **Data:** `data/paper_eval/fig1/` (path decomp cells)

## Appendix / Supporting

### LazyLog faithful ACK measurement (FIG2_LAZYLOG)
- **Script:** `PaperScripts/run_fig1_throughput_scaling.sh` with `SKIP_LAZYLOG=0`
- **Plan:** `PaperScripts/FIG2_LAZYLOG.md`
- **Status:** Pending end-to-end hardware validation runs.
