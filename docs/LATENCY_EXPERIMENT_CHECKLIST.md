# Embarcadero SOSP/OSDI Latency Experiment Checklist

Last updated: 2026-03-01

## Guardrail (run after every checklist item)

- [x] Build succeeds:
  - `cmake -S . -B build -DCOLLECT_LATENCY_STATS=ON`
  - `cmake --build build -j`
- [x] Throughput sanity (Order 0) stays in expected range (11-12 GB/s):
  - `NUM_BROKERS=4 TEST_TYPE=5 ORDER=0 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 SEQUENCER=EMBARCADERO scripts/run_throughput.sh`
- [x] Throughput sanity (Order 5) stays in expected range (11-12 GB/s):
  - `NUM_BROKERS=4 TEST_TYPE=5 ORDER=5 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 SEQUENCER=EMBARCADERO scripts/run_throughput.sh`
- [x] Record measured bandwidths for both runs in this file.

## Baseline recording table

| Date | Step | Build | Order0 GB/s | Order5 GB/s | Pass/Fail | Notes |
|---|---|---|---:|---:|---|---|
| 2026-03-01 | Step 0 (plan file created) | Pass | 11.59 | 11.81 | Pass | Measured from `run_throughput.sh` (11589.04 MB/s, 11805.25 MB/s) |
| 2026-03-01 | Step 1 (latency harness correctness) | Pass | 11.92 | 11.75 | Pass | Order 5 had one transient ACK-timeout attempt, then passed on retry |
| 2026-03-01 | Step 2 (integrity gate script, in progress) | Pass | 11.74 | 11.57 | In progress | New validator script added; smoke hit latency-path timeout (subscriber stalled on one broker) |
| 2026-03-01 | Step 2 debug pass A (subscriber connect + hotpath guard) | Pass | 11.89 | 11.33 | In progress | Added `s.WaitUntilAllConnected()` in latency test and gated receiver-side batch-header rewrite behind `EMBAR_VALIDATE_ORDER`; latency smoke still times out with 44.1% bytes and B0-only receive accounting |
| 2026-03-01 | Step 2 debug pass B (validator float fix) | Pass | 11.72 | 11.41 | In progress | Fixed float parsing in negative-latency check; integrity still fails: Order5 timeout/B0-only receive, Order0 produces zero latency samples |
| 2026-03-01 | Step 2/3 debug pass C (subscriber path root-cause fix + online latency accounting) | Pass | 11.60 | 11.34 | Pass | Root cause fixed in ORDER=5 export gating (`pbr_absolute_index` readiness, not `ordered` bit); subscriber latency moved to streaming parse + unique-message poll criterion. Validation passed at 64 MiB and 4 GiB for ORDER=0/5 (plus 8 MiB smoke for ORDER=0). Throughput guardrails: 11598.58 MB/s (Order0), 11344.58 MB/s (Order5). |
| 2026-03-01 | Step 3 debug pass D (strict parsed/recorded/dropped accounting) | Pass | 11.60 | 11.46 | Pass | Added explicit latency accounting counters and CSV fields (`Parsed,Recorded,Dropped`) in `latency_stats.csv`; revalidated 64 MiB + 4 GiB integrity (Order0/5) and reran throughput guardrails (11598.42 MB/s, 11462.24 MB/s). |
| 2026-03-01 | Step 4 pass E (offered-load control + pacing) | Pass | 11.56 | 11.85 | Pass | Added `--target_mbps` CLI option, deterministic open-loop pacing in `LatencyTest`, offered-load summary logging (`target`, `achieved_offered`, `achieved_goodput`), and `TARGET_MBPS` passthrough in `scripts/run_latency.sh`. Baseline guardrails with pacing disabled remain in range (11557.34 MB/s, 11845.24 MB/s). |
| 2026-03-01 | Step 5 pass F (stage decomposition + monotonic check) | Pass | 11.84 | 11.66 | Pass | Added stage metric `append_send_to_ordered_batch_latency` (derived from ACK path for `ack_level=1`), generated `stage_latency_summary.csv` with `append_send_to_ordered`, `append_send_to_ack`, `append_send_to_deliver`, and added monotonicity check (`ordered <= ack <= deliver` on p50/p99/p999/max). Revalidated latency integrity (64 MiB Order0/5) and guardrails (11838.70 MB/s, 11656.87 MB/s). |
| 2026-03-01 | Step 6 pass G (ordering-anomaly instrumentation) | Pass | 11.51 | 11.44 | Pass | Added ORDER=5 anomaly counters and CSV export (`order5_anomaly_counters_broker*.csv`): `FifoViolations`, `AckOrderViolations`, and skipped/timeout subcounters (`ScannerTimeoutSkips`, `HoldTimeoutSkips`, `HoldBufferForcedSkips`, `StaleEpochSkips`). Added `scripts/run_order5_anomaly_checks.sh` with explicit cross-broker single-client stress mode; summary at `data/latency/order5_anomaly_checks/summary.csv` shows zero anomalies for normal + stress runs. Latest guardrail rerun after cleanup: Order0=11508.03 MB/s, Order5=11438.51 MB/s (earlier pass: 11178.77 MB/s / 11689.65 MB/s). |
| 2026-03-01 | Step 6 hardening H1 (main stale-skip counter completeness) | Pass | 11.37 | 11.74 | Pass | Added missing increments for main-loop stale skips (`order5_skipped_batches`, `order5_stale_epoch_skips`) in `EpochSequencerThread` stale-batch fencing path. Guardrails after patch: 11369.98 MB/s (Order0), 11735.94 MB/s (Order5). |
| 2026-03-01 | Step 6 hardening H2 (remove hot-path anomaly CSV writes) | Pass | 11.23 | 11.49 | Pass | Removed periodic anomaly CSV emission from `EpochSequencerThread` hot path; keep end-of-run snapshot in `Topic` teardown. Guardrails after patch: 11233.38 MB/s (Order0), 11485.70 MB/s (Order5; first attempt timed out once, retry passed). |
| 2026-03-01 | Step 6/7 hardening H3 (anomaly runner robustness + baseline schema unification fields) | Pass | 11.60 | 11.95 | Pass | Hardened `scripts/run_order5_anomaly_checks.sh` with graceful-stop timeout fallback and robust multi-file CSV parsing (`FNR==1`). Expanded `scripts/run_baseline_matrix.sh` output schema to include latency/stage/anomaly fields (explicit `NA` when not measured). Revalidated anomaly stress summary (all zero) and guardrails: 11599.65 MB/s (Order0), 11948.39 MB/s (Order5). |
| 2026-03-01 | Step 7 pass I (baseline matrix execution: Embarcadero + Corfu scope) | Pass | 11.20 | 11.24 | Pass | Executed unified baseline matrix runner with reduced matrix (`EMBARCADERO O0/O5`, `CORFU O2`) and explicit semantics/schema fields. Throughputs: 11196.50 MB/s (E O0), 11237.86 MB/s (E O5), 11221.80 MB/s (Corfu O2). LazyLog status documented as `deferred_no_local_impl` (no local implementation baseline). |
| 2026-03-01 | Step 7 guardrail closure I1 | Pass | 11.81 | 11.78 | Pass | Mandatory Step 7 post-item guardrails completed: Order0=11811.02 MB/s, Order5=11776.20 MB/s. |
| 2026-03-01 | Step 8 hardening J1 (sweep timeout+retry robustness) | Pass | 11.63 | 11.34 | Pass | Hardened `scripts/run_throughput_latency_sweep.sh`: ACK timeout now aligned with E2E timeout budget (`EMBARCADERO_ACK_TIMEOUT_SEC`), per-target retries added (`POINT_MAX_ATTEMPTS`), and optional broker-count validation (`STRICT_BROKER_COUNT`). Guardrails after patch: 11625.63 MB/s (Order0), 11340.87 MB/s (Order5). |
| 2026-03-01 | Step 8 pass J2 (paper latency-vs-load 4 GiB x 5 trials) | Pass | 11.59 | 11.59 | Pass | Completed offered-load sweep with fixed 4 GiB payload for targets `1000/1500/2000 MB/s`, 5 trials each: artifacts in `data/latency/paper_load_sweep_embarcadero_o5_step8/` (`trial_*/`, `ci_summary.csv`, `frontier.png/.pdf`). CI means (e2e p50/p99/p99.9 us): 1000 -> 6007.8 / 82888.8 / 127539.8; 1500 -> 6307.0 / 97924.0 / 138782.0; 2000 -> 6474.8 / 104410.4 / 150343.6. Guardrails after item: 11588.37 MB/s (Order0), 11590.38 MB/s (Order5). |
| 2026-03-01 | Step 9 pass K1 (ordering/durability ladder, Embarcadero canonical levels) | Pass | 11.63 | 11.64 | Pass | Ran ladder matrix with `order in {0,5}`, `ack in {0,1,2}` at 10 GiB payload via `scripts/run_ordering_durability_ladder.sh`. PASS points: O0A0=11649.87, O0A1=11482.48, O0A2=11476.69, O5A0=11595.52, O5A1=11450.01 MB/s. O5A2 failed after 3 retries (ACK timeout shortfall on B0/B3). Overhead estimates from PASS points: strong-order vs unordered at ACK1 = 0.283%; ACK1 vs ACK0 overhead = 1.437% (Order0), 1.255% (Order5). Guardrails after item: Order0=11634.81 MB/s, Order5=11635.62 MB/s. |
| 2026-03-01 | Step 9 closure K2 (strong-order vs total-order quantification) | Pass | 11.38 | 11.57 | Pass | Added explicit total-order comparison point via Corfu `O2A1` throughput (`10663.57 MB/s`, command: `NUM_BROKERS=4 TEST_TYPE=5 ORDER=2 ACK=1 SEQUENCER=CORFU scripts/run_throughput.sh`). Embarcadero strong-order `O5A1` (11450.01 MB/s from ladder) is +7.37% vs Corfu total-order point in this environment. Guardrails after item: 11383.40 MB/s (Order0), 11571.80 MB/s (Order5). |
| 2026-03-01 | Step 10 hardening L1 (heterogeneity runner correctness + stress runs) | Pass | 11.76 | 11.71 | Pass | Refactored `scripts/run_slow_replica_heterogeneity.sh` to run baseline+slow modes, avoid stale CSV reuse, classify timeout failures correctly, emit `summary.csv`/`comparison.csv`, and normalize OUTDIR to project-relative paths. Moderate scenario (`1 GiB`, pause 4s) completed with small stage shifts: ordered/ack p99 -0.52%, deliver p99 +0.49% (`data/latency/slow_replica_step10_embarcadero`). Strong scenario (`4 GiB`, pause 8s) failed both baseline and injected runs (subscriber/ACK timeout), recorded as stress-failure artifacts (`data/latency/slow_replica_step10_embarcadero_strong`). Guardrails after item: 11759.85 MB/s (Order0), 11708.03 MB/s (Order5). |
| 2026-03-01 | Step 10 hardening L2 (Corfu startup + timeout determinism) | Pass | 11.69 | 11.48 | Pass | Fixed `scripts/run_slow_replica_heterogeneity.sh` for cross-system robustness: add `corfu_global_sequencer` startup/cleanup in Corfu mode, add hard `timeout` wrapper (`RUN_TIMEOUT_SEC`), and parameterize config paths. Corfu 1 GiB moderate run now exits deterministically with explicit failure artifacts instead of hanging (`data/latency/slow_replica_step10_corfu/summary.csv`: baseline=FAIL, slow_injected=FAIL, both subscriber-poll timeout). Guardrails after item: 11692.43 MB/s (Order0), 11484.01 MB/s (Order5). |
| 2026-03-01 | Step 11 pass M1 (repro package hardening) | Pass | 11.78 | 11.49 | Pass | Upgraded `scripts/freeze_repro_package.sh` to emit full manifest, rerun guide (`RERUN.md`), script/config snapshot, file list, and expanded SHA256 coverage. Validated output under `data/latency/repro_manifest_step11`. Guardrails after item: 11777.57 MB/s (Order0), 11485.48 MB/s (Order5). |

## Step-by-step implementation plan

### Step 1: Fix latency harness correctness
- [x] Replace non-canonical order usage in latency scripts (`order=4` -> `order=5` for strong ordering).
- [x] Make total payload explicit and fixed at 4 GiB for latency experiments (`-s 4294967296`).
- [x] Make output paths trial-safe (no overwrite).
- [x] Add clear run metadata (sequencer/order/ack/msg-size/total-bytes/trial) into output folder names.
- [x] Run guardrail.

### Step 2: 4 GiB sample-integrity gate
- [x] Add a strict latency validation script for 4 GiB runs:
  - expected samples = `total_bytes / message_size`
  - fail on sample mismatch, timeout, negative latency, missing output files.
- [x] Run for Order 0 + Order 5 (Embarcadero).
- [x] Run guardrail.

### Step 3: Make latency accounting robust for long runs
- [x] Move to streaming/online message-latency accounting in subscriber (avoid post-hoc reconstruction bias).
- [x] Keep strict counters (`parsed`, `recorded`, `dropped`) and emit them in CSV.
- [x] Verify exact sample count on 4 GiB.
- [x] Run guardrail.

### Step 4: Add offered-load control for latency-vs-throughput curves
- [x] Add `--target_mbps` CLI arg in `throughput_test`.
- [x] Implement open-loop pacing in latency test path (token bucket / deterministic pacer).
- [x] Log achieved offered rate and achieved goodput per run.
- [x] Run guardrail.

### Step 5: Stage-level latency decomposition
- [x] Add metrics: `append->ordered`, `append->ack`, `append->deliver`.
- [x] Emit p50/p95/p99/p99.9/max per metric.
- [x] Verify monotonic relation (`ordered <= ack <= deliver`) for sampled messages.
- [x] Run guardrail.

### Step 6: Ordering-anomaly instrumentation
- [x] Add counters for FIFO violations and ack-order violations.
- [x] Add skipped/timeout batch counters for strong ordering mode.
- [x] Add stress workload mode to intentionally cross brokers for a single client.
- [x] Run guardrail.

### Step 7: Baseline matrix automation
- [x] Unify one script to run Embarcadero / Corfu / Scalog with same metric schema.
- [x] Keep semantic labels explicit per run (order, ack, replication factor).
- [x] Decide LazyLog status (implemented baseline vs deferred claim).
- [x] Run guardrail.
  - Completed in current scope (Scalog deferred by user): `scripts/run_baseline_matrix.sh` executed with matrix {Embarcadero O0/O5, Corfu O2}. Output: `data/latency/baseline_matrix/baseline_matrix.csv`. LazyLog status fixed as `deferred_no_local_impl` in schema field.

### Step 8: Paper-critical latency-vs-load experiment
- [x] Run offered-load sweep at fixed 4 GiB payload per point.
- [x] Use >=5 trials per point; collect confidence intervals.
- [x] Produce CDFs and latency-goodput frontiers for p50/p99/p99.9.
- [x] Run guardrail.
  - Completed artifacts: `data/latency/paper_load_sweep_embarcadero_o5_step8/trial_{1..5}/`, `ci_summary.csv`, `frontier.png`, `frontier.pdf`.
  - Executed stable paper point set in this environment: `SWEEP_TARGETS='1000 1500 2000'`, `TOTAL_BYTES=4294967296`, `ORDER=5`, `ACK=1`, `SEQUENCER=EMBARCADERO`.
  - Aggregated CI file confirms `n=5` per point for e2e throughput and latency quantiles.

### Step 9: Ordering/durability ladder experiment
- [x] Run matrix across order levels and ack levels.
- [x] Quantify strong-order overhead vs total-order mode.
- [x] Confirm expected overhead envelope.
- [x] Run guardrail.
  - Implemented automation script: `scripts/run_ordering_durability_ladder.sh`.
  - Current run scope (Scalog deferred): Embarcadero canonical ordering levels (`order=0,5`) plus Corfu total-order reference point (`order=2, ack=1`) for cross-mode overhead quantification.
  - Known instability retained: `order=5, ack=2` failed repeatedly in this environment (ACK timeout shortfall), so durability-strongest point remains unresolved.

### Step 10: Slow-replica heterogeneity experiment
- [x] Inject one slow replica/disk.
- [ ] Show effect on `append->ordered` vs `append->ack` for each system.
- [ ] Validate Embarcadero decoupling claim under heterogeneity.
- [x] Run guardrail.
  - Implemented automation script: `scripts/run_slow_replica_heterogeneity.sh` (SIGSTOP/SIGCONT fault injection on one broker path).
  - Current scope execution: Embarcadero moderate + strong scenarios completed; Corfu heterogeneity latency now fails deterministically (no hang) with zero delivered bytes and subscriber-poll timeout in both baseline and slow-injected modes (`data/latency/slow_replica_step10_corfu/`), so cross-system comparison and claim-quality decoupling validation remain pending.

### Step 11: Final reproducibility package
- [x] Freeze scripts + config + command lines.
- [x] Produce final CSV manifest + plotting scripts.
- [ ] Run full rerun from clean state.
- [x] Run guardrail.
  - Implemented packaging helper (pending full validation run): `scripts/freeze_repro_package.sh`.
