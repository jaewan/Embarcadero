# Embarcadero SOSP/OSDI Latency Experiment Checklist

Last updated: 2026-03-01

## Guardrail (run after every checklist item)

- [ ] Build succeeds:
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
- [ ] Add counters for FIFO violations and ack-order violations.
- [ ] Add skipped/timeout batch counters for strong ordering mode.
- [ ] Add stress workload mode to intentionally cross brokers for a single client.
- [ ] Run guardrail.

### Step 7: Baseline matrix automation
- [ ] Unify one script to run Embarcadero / Corfu / Scalog with same metric schema.
- [ ] Keep semantic labels explicit per run (order, ack, replication factor).
- [ ] Decide LazyLog status (implemented baseline vs deferred claim).
- [ ] Run guardrail.

### Step 8: Paper-critical latency-vs-load experiment
- [ ] Run offered-load sweep at fixed 4 GiB payload per point.
- [ ] Use >=5 trials per point; collect confidence intervals.
- [ ] Produce CDFs and latency-goodput frontiers for p50/p99/p99.9.
- [ ] Run guardrail.

### Step 9: Ordering/durability ladder experiment
- [ ] Run matrix across order levels and ack levels.
- [ ] Quantify strong-order overhead vs total-order mode.
- [ ] Confirm expected overhead envelope.
- [ ] Run guardrail.

### Step 10: Slow-replica heterogeneity experiment
- [ ] Inject one slow replica/disk.
- [ ] Show effect on `append->ordered` vs `append->ack` for each system.
- [ ] Validate Embarcadero decoupling claim under heterogeneity.
- [ ] Run guardrail.

### Step 11: Final reproducibility package
- [ ] Freeze scripts + config + command lines.
- [ ] Produce final CSV manifest + plotting scripts.
- [ ] Run full rerun from clean state.
- [ ] Run guardrail.
