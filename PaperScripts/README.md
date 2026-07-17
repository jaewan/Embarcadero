# PaperScripts — paper-replication entry points

Forked from `scripts/` with **publication-honest defaults**. Shared machinery
(`run_multiclient.sh`, `broker_lifecycle.sh`, cluster setup) stays under
`scripts/`; this directory owns the paper cell matrix and knobs.

## Prerequisites

1. Commit **≥ `9c6aea99`** (ORDER=5 join-watchdog fix — removes ~100 ms artificial Poll tax).
2. Brokers on **moscxl**; publishers on **c4** (N=1) / **c4,c3,c1** (scaling).
3. Deploy client: `scp build/bin/throughput_test c4:~/Embarcadero/build/bin/`
4. Client libs (Ubuntu 22.04 clients):
   `CLIENT_LD_LIBRARY_PATH=…/third_party/glog-0.6/lib:…/third_party/yaml-cpp-0.8/lib`
5. One-time: `bash scripts/cluster_setup.sh` (or `SKIP_CLUSTER_SETUP=1` if already synced).

## What changed vs `scripts/`

| Knob | Old (`scripts/`) | PaperScripts |
|------|------------------|--------------|
| E2/E9 `RUNTIME_MODE` | often labeled `latency` (overridden to TP) | **`throughput`** |
| `CLIENT_PUB_BATCH_KB` | harness default **512** | **2048** (yaml design point) |
| Output root | `data/overnight_eval/` | **`data/paper_eval/`** |
| Metrics | Bandwidth only in summaries | Bandwidth + **Send-done** + join/ack_wait (via updated `run_multiclient.sh`) |

Latency cells (E3 / E9 latency) still use `RUNTIME_MODE=latency` and do **not** force 2048 KB batches.

## Entry points

| Script | Purpose |
|--------|---------|
| `run_fig1_throughput_scaling.sh` | **Fig 1** RF2/ACK2 scaling N=1..4 (3R+1L); appendable CSV. Scalog RF2 sinks matched (`--replicate_to_disk` / mem-copy + amortized sync). LazyLog excluded by default (`SKIP_LAZYLOG=1`; metadata-bound). |
| `plot_fig1_throughput_scaling.py` | Plot Fig 1 from campaign `results.csv` |
| `FIG1.md` | Fig 1 draft + **caveats** (sink mismatch, metrics, CXL) |
| `run_fig2_latency_vs_load.sh` | **Fig 2** latency vs load: primary Embar **O5 ACK2 RF2**; matched-load mechanism ablation (O0/O5/ACK2); optional 1–2 baseline points. |
| `plot_fig2_latency_vs_load.py` | Plot Fig 2 load sweep + mechanism table/bar |
| `FIG2.md` | Fig 2 draft + caveats (RF2 primary, mechanism table, pacing) |
| `run_fig3_failure.sh` | **Fig 3** broker-kill ACK throughput: prefix-safe hold vs arrival-order sensitivity |
| `FIG3.md` | Fig 3 contract + knobs |
| `run_e2_throughput_matrix.sh` | Wait for idle cluster → E2 N=1 Embar+baselines matrix |
| `run_overnight_eval.sh` | Full paper overnight (E2 + E3 + E9 + E8) |
| `run_order5_latency_package.sh` | ORDER=5 latency package (wraps `scripts/publication/`) |
| `run_post_sweep_queue.sh` | Post-sweep E4/E7/E10/delivery (wraps `scripts/`) |

## Quick start

```bash
# Fig 1 (1 trial first; re-run appends more trials to same CSV)
NUM_TRIALS=1 bash PaperScripts/run_fig1_throughput_scaling.sh
# Add trials later:
NUM_TRIALS=1 bash PaperScripts/run_fig1_throughput_scaling.sh

# Fig 2 latency vs load (primary Embar O5 ACK2 RF2 + mechanism ablation)
NUM_TRIALS=3 WARMUP_TRIALS=1 bash PaperScripts/run_fig2_latency_vs_load.sh
# Smoke:
# FIG2_FAST_CXL=1 NUM_TRIALS=1 LOAD_POINTS_MBPS="100 500" MECHANISM_LOAD_MBPS=100 \
#   TOTAL_BYTES=$((512<<20)) bash PaperScripts/run_fig2_latency_vs_load.sh
# Requires: cmake -DCOLLECT_LATENCY_STATS=ON (local + remote throughput_test)

# Smoke overnight (fast)
SMOKE=1 bash PaperScripts/run_overnight_eval.sh

# Paper E2 N=1 matrix (recommended after idle)
bash PaperScripts/run_e2_throughput_matrix.sh

# Full overnight (detached)
nohup bash PaperScripts/run_overnight_eval.sh \
  > /tmp/paper_eval_$(date -u +%Y%m%dT%H%M%SZ).log 2>&1 &
```

## Reading results

- Cell logs: `data/paper_eval/<RUN_TAG>/logs/`
- Multiclient trials: `data/paper_eval/<RUN_TAG>/multiclient/logs/...`
- Prefer **Send-done** (and check `publisher_join_ms` in `[POLL_BREAKDOWN]`) if Bandwidth and Send-done diverge on short runs.

## Do not

- Nest `flock` around overnight (it already locks inside `run_multiclient`).
- Claim CXL-Scalog/LazyLog are Embar — see `docs/baselines/porting_rule.md`.
- Edit only `scripts/run_overnight_eval.sh` for paper runs — evolve **this** tree.
