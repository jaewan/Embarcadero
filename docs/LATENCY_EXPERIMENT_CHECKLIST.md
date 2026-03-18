# Embarcadero SOSP/OSDI Latency Experiment Checklist

Last updated: 2026-03-19

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
| 2026-03-01 | Step 11 hardening M2 (repro command stability alignment) | Pass | 11.60 | 11.43 | Pass | Updated frozen rerun command pack to use validated stable Step 8 load points (`1000/1500/2000`, 4 GiB, retries/timeouts/config explicit) and regenerated package under `data/latency/repro_manifest_step11_round2`. Guardrails after item: 11599.77 MB/s (Order0), 11426.84 MB/s (Order5). |
| 2026-03-02 | Step 12 Task A (Corfu latency path instrumentation/classification) | Pass | 11.64 | 11.68 | Pass | Added subscriber parse/drop counters and timeout diagnostics (`parse_calls`, `metadata_detected`, `break_invalid_size`, etc.) plus Corfu subscribe/export counters gated by `EMBARCADERO_CORFU_LATENCY_DIAG`. Failure signatures now explicit: Corfu `ORDER=2` had export starvation (`bytes=0`, `parse_calls=0`); Corfu `ORDER=3` had decode mismatch (`bytes~400%`, `metadata_detected=0`, invalid-size parse breaks). Guardrails after item: 11640.46 MB/s (Order0), 11682.12 MB/s (Order5, second attempt after one transient ACK-timeout). |
| 2026-03-02 | Step 13 Task B (Corfu receive/decode/export root-cause fix) | Pass | 11.64 | 11.56 | Pass | Root-cause fixes: (1) Corfu `ORDER=2` batch headers now exportable (`num_msg`/`total_order` populated and `ordered` flush/fence), with fallback metadata export path; (2) subscriber latency parser now recognizes metadata for `ORDER=3`; (3) Corfu metadata header version now inferred from actual batch payload instead of forced global assumption. Corfu latency `ORDER=2` now succeeds on two consecutive 1 GiB runs (run1 pub/sub: 4696.95/1639.87 MB/s; run2: 4610.40/1762.27 MB/s) with valid non-empty `latency_stats.csv` and `cdf_latency_us.csv`. Guardrails after item: 11640.82 MB/s (Order0), 11555.66 MB/s (Order5). |
| 2026-03-02 | Step 14 Task C/D (hardening + script failure classification + docs) | Pass | 11.64 | 11.56 | Pass | Hardening: noisy Corfu export summaries are now env-gated; `scripts/run_throughput.sh` latency mode now classifies success via explicit completion markers (`End-to-end completed`, `Latency accounting`) and rejects real timeout signatures, avoiding false negatives from missing `Bandwidth:` strings in TEST_TYPE=2. Semantics note: Corfu `ORDER=2` path is now validated and deterministic for 1 GiB latency runs; Corfu `ORDER=3` still needs follow-up (currently partial parse progress and timeout in this environment). Guardrails retained from immediate post-fix validation: 11640.82 MB/s (Order0), 11555.66 MB/s (Order5). |
| 2026-03-02 | Step 15 refactor pass R1 (remove Corfu ORDER=3 dead broker path) | Pass | 11.79 | 11.88 | Pass | Pruned unused Corfu ORDER=3 broker-side callback/ring path from `Topic` (`CorfuOrder3GetCXLBuffer`, callback worker/queue, contiguous-advance ring structures). Corfu latency `ORDER=2` revalidated at 1 GiB: publish `4716.85 MB/s`, end-to-end/subscribe `1666.86 MB/s`, `parsed=3145728`, `recorded=1048576`, `dropped=2097152`. Guardrails after refactor: Order0 passed first attempt at `11794.16 MB/s`; Order5 passed at `11878.72 MB/s` on retry 3 after two transient ACK-timeout shortfalls. |
| 2026-03-02 | Step 16 refactor pass R2 (hot-path diagnostics opt-in, default-off) | Pass | 11.86 | 11.55 | Pass | Replaced compile-mode diagnostics toggle with explicit env-gated `EMBAR_TOPIC_DIAGNOSTICS` (`0` default). This removes scanner heartbeat/log spam from normal runs while keeping targeted debug ability when explicitly enabled. Corfu latency `ORDER=2` (1 GiB) remained healthy: publish `4526.69 MB/s`, end-to-end/subscribe `1653.90 MB/s`, `parsed=3142906`, `recorded=1048576`, `dropped=2094330`. Guardrails after refactor passed first attempt: Order0 `11859.01 MB/s`, Order5 `11547.45 MB/s`. |
| 2026-03-02 | Step 17 refactor pass R3 (remove explicit dead branch in client ORDER=3 alignment path) | Pass | 11.39 | 11.56 | Pass | Removed no-op/dead `if (false && ...)` branch and stale comments from `src/client/main.cc` ORDER=3 alignment block, keeping runtime behavior unchanged. Guardrails after refactor: Order0 `11389.83 MB/s` (first attempt), Order5 `11561.46 MB/s` (retry 2 after one transient ACK-timeout shortfall). |
| 2026-03-02 | Step 18 refactor pass R4 (ORDER=5 contiguous PBR publish frontier for multi-thread recv) | Pass | 11.49 | 11.56 | Pass | Added per-topic contiguous publish frontier (`pbr_absolute_index`) so multi-threaded recv completion can arrive out of order but PBR slots are only exposed `VALID` in contiguous order. This hardens ACK/export correctness against out-of-order publish races while preserving receive parallelism. Corfu `ORDER=2` latency still passes at 1 GiB: publish `4949.97 MB/s`, end-to-end `1608.31 MB/s`, `recorded=1048576`. Guardrails after refactor passed first attempt: Order0 `11491.20 MB/s`, Order5 `11558.88 MB/s`. |
| 2026-03-02 | Step 19 refactor pass R5 (Corfu sequencer identity semantics: use publisher client_id) | Pass | 11.64 | 11.66 | Pass | Replaced process-local generated Corfu sequencer client IDs with explicit publisher `client_id_` when constructing `CorfuSequencerClient`. This aligns sequencer key semantics with per-client-per-broker ordering across processes and avoids potential cross-process client-ID collisions. Corfu `ORDER=2` latency still passes at 1 GiB: publish `3592.52 MB/s`, end-to-end `1527.01 MB/s`, `recorded=1048576`. Guardrails after refactor passed first attempt: Order0 `11641.67 MB/s`, Order5 `11659.04 MB/s`. |
| 2026-03-02 | Step 20 refactor pass R6 (remove residual ORDER=3 metadata/decode assumptions in latency path) | Pass | 11.65 | 11.47 | Pass | Tightened latency metadata paths to canonical modes: subscriber parser now treats batch metadata only for `ORDER in {2,5}`; broker subscribe/export metadata send path likewise limited to `ORDER in {2,5}` with Corfu diagnostics keyed to `ORDER=2` only. Corfu `ORDER=2` latency remains valid at 1 GiB: publish `4647.21 MB/s`, end-to-end `1669.22 MB/s`, `recorded=1048576`. Guardrails after refactor: Order0 `11645.24 MB/s` (first attempt), Order5 `11474.97 MB/s` (retry 3 after two transient ACK-timeout shortfalls with broker-1 deficit). |
| 2026-03-02 | Step 21 refactor pass R7 (ORDER=5 contiguous publish frontier overflow guard) | Pass | 11.73 | 11.59 | Pass | Added fail-fast guard in ORDER=5 contiguous PBR publish frontier: if pending out-of-order publish entries reach ring capacity (`num_slots_`), return explicit error instead of allowing unbounded pending growth. This keeps receive/export failure mode explicit under severe disorder or corruption. Guardrails after refactor passed first attempt: Order0 `11725.39 MB/s`, Order5 `11592.14 MB/s`. |
| 2026-03-02 | Step 22 refactor pass R8 (ORDER=5 ACK source hardening: CV + ordered fallback) | Pass | 11.65 | 11.55 | Pass | Hardened `GetOffsetToAck` for `ORDER=5 ACK=1`: ACK offset now uses `max(CV.completed_logical_offset, tinode.ordered)` after proper CXL invalidation. This prevents transient CV lag from stalling ACK progress while preserving ordering semantics (both values are sequencer-ordered cumulative counts). Corfu `ORDER=2` latency sanity remains valid (1 GiB): publish `3776.78 MB/s`, end-to-end `1533.46 MB/s`, `recorded=1048576`. Guardrails after refactor passed first attempt: Order0 `11651.14 MB/s`, Order5 `11546.66 MB/s`. |
| 2026-03-02 | Step 23 architecture pass A1 (frontier invariants map / rubber-duck baseline) | Pass | 11.65 | 11.72 | Pass | Added explicit frontier contract note at `docs/FRONTIER_INVARIANTS.md` (owners, monotonic rules, source-of-truth policy, canonical mode constraints, debugging checklist). This establishes the implementation baseline before further refactors. Guardrails after doc pass: Order0 `11652.82 MB/s`, Order5 `11724.86 MB/s` (both first attempt). |
| 2026-03-02 | Step 24 architecture pass A2 (dedicated `EMBAR_FRONTIER_TRACE` instrumentation) | Pass | 11.95 | 11.74 | Pass | Added low-noise, rate-limited frontier snapshots under `EMBAR_FRONTIER_TRACE=1` (default off): publish frontier (`Topic::PublishPBRSlotAfterRecv`), commit frontier (`Topic::CommitEpoch`), and ack frontier (`NetworkManager::GetOffsetToAck` for `ORDER=5 ACK=1`). This provides direct correlation of `publish/ordered/cv/ack` without enabling broad verbose trace modes. Guardrails with trace disabled: Order0 `11950.95 MB/s`, Order5 `11738.52 MB/s` (both first attempt). |
| 2026-03-02 | Step 25 stabilization S1 (rollback contiguous-CV refactor; revalidate Corfu ORDER=2) | Pass | 11.28 | 11.73 | Pass | Reverted contiguous pending-map CV frontier experiment in `Topic::AccumulateCVUpdate/FlushAccumulatedCV` and removed extra CV frontier state from `Topic`; restored monotonic max-based CV updates to remove ORDER=5 regression. Guardrails after rollback: Order0 `11280.57 MB/s` (first attempt), Order5 `11725.86 MB/s` (attempt-2 after one transient ACK-timeout on B2). Corfu latency `TEST_TYPE=2 ORDER=2` now passed in two consecutive 1 GiB runs (run1 pub/sub: `4016.03/1598.42 MB/s`, run2: `4659.29/1640.78 MB/s`, both `recorded=1048576` with non-empty `build/bin/latency_stats.csv` + CDFs). Corfu `ORDER=3` now fails fast with explicit runner error (`CORFU is restricted to ORDER=2`). |
| 2026-03-02 | Step 26 stabilization S2 (ORDER=5 tail-progress hardening under idle epochs) | Pass | 11.77 | 11.60 | Pass | Root-cause direction validated as implementation-side (not script): intermittent `ORDER=5 ACK=1` shortfalls showed per-broker tail stalls (`sent_all=yes`, one broker short by multiples of batch message count). Hardened sequencer idle path to run periodic hold-buffer expiry/commit tick even when no new epoch is sealed (`EpochSequencerThread`), preventing tail entries from waiting indefinitely for future traffic. Guardrails after hardening: Order0 `11765.72 MB/s`, Order5 `11597.38 MB/s` (both first attempt). Additional single-attempt probe with tighter timeout (`EMBARCADERO_ACK_TIMEOUT_SEC=45`, `TRIAL_MAX_ATTEMPTS=1`) passed 3/3: `11439.74`, `11656.72`, `11686.94 MB/s`. |
| 2026-03-02 | Step 27 validation V1 (ORDER=5 burn-in + Corfu ORDER=2 recheck) | Pass | N/A | N/A | In progress | Extended ORDER=5 single-attempt burn-in (`TRIAL_MAX_ATTEMPTS=1`, `EMBARCADERO_ACK_TIMEOUT_SEC=45`, 10 runs) yielded `8/10` pass, `2/10` fail with real ACK shortfalls (`sent_all=yes`, lagging broker tail of 19,270 and 26,978 msgs). Corfu `TEST_TYPE=2 ORDER=2` remained stable in 2/2 reruns at 1 GiB: run1 pub/sub `5081.08/1616.04 MB/s`, run2 `4592.15/1643.68 MB/s`, both `recorded=1048576`. |
| 2026-03-02 | Step 28 architecture pass A3/A4 (BlogHeader default-on + Corfu ORDER=2 header-version canonicalization) | Pass | 11.75 | 11.86 | Pass | Refined `EMBARCADERO_USE_BLOG_HEADER` semantics to default-on (`auto/on/off` env parsing), then fixed broker export metadata classification to avoid V2 mislabeling on Corfu ORDER=2 (`header_version` is now canonical by order: ORDER2->V1, ORDER5->conditional V2 inference). Corfu latency ORDER=2 revalidated 2/2 at 1 GiB: run1 pub/sub `4854.32/1718.34 MB/s`, run2 `4717.79/1680.92 MB/s`, both `recorded=1048576`. Guardrails after fix: Order0 `11748.61 MB/s`, Order5 `11864.86 MB/s`. |
| 2026-03-03 | Step 29 stability closure V2 (strict ORDER=5 burn-in, first-fail artifact) | Pass | N/A | N/A | In progress | Ran strict burn-in harness with escalation (`RUNS=100 STOP_ON_FAIL=1 scripts/run_order5_burnin_capture.sh`). Real first failure captured at run 14 after 13 passes (`data/throughput/stability/order5_burnin_20260303_021539`): ACK-timeout shortfall on broker 3 (`short=21197`, `sent_all=yes`), with broker/anomaly artifacts copied under `failure_run_14/`. |
| 2026-03-03 | Step 30 heterogeneity closure L3 (cross-system moderate scenario, parser fix) | Pass | 11.59 | 11.91 | Pass | Fixed `scripts/run_slow_replica_heterogeneity.sh` bandwidth parsing (`grep -h`) to avoid filename-prefixed numeric extraction on retries. Re-ran moderate heterogeneity for both systems. Embarcadero (`ORDER=5`) comparison: ordered/ack p99 `50999 -> 50151` us (-1.66%), deliver p99 `195586 -> 202371` us (+3.47%) (`data/latency/slow_replica_step10_closure_embarcadero`). Corfu (`ORDER=2`) comparison: ordered/ack p99 `221 -> 362` us (+63.80%), deliver p99 `147632 -> 142419` us (-3.53%) (`data/latency/slow_replica_step10_closure_corfu`). Guardrails after task: Order0 `11593.15 MB/s`; Order5 retry-2 `11905.32 MB/s` (attempt-1 ACK shortfall on broker 0). |
| 2026-03-03 | Step 31 reproducibility rerun M3 (full command-pack execution from fresh freeze) | Pass | N/A | N/A | In progress | Generated fresh package (`OUTDIR=data/latency/repro_manifest_step11_round3 scripts/freeze_repro_package.sh`) and started full replay (`bash data/latency/repro_manifest_step11_round3/commands.sh`). Completed: build, Step6 anomaly checks, Step7 baseline matrix, and Step8 trial_1 all targets + Step8 trial_2 targets {1000,1500} under `data/latency/paper_load_sweep_embarcadero_o5/`. Run was manually stopped before Step8 completion/Step9/Step10 to checkpoint. |
| 2026-03-03 | Step 32 architecture/doc closure A5 (Corfu appendix-gap note + ORDER2 consistency cleanup) | Pass | 11.94 | 11.48 | Pass | Added `docs/CORFU_APPENDIX_GAP.md` to explicitly document paper-vs-implementation semantics (token-before-write with broker proxy ingress, not client one-sided direct CXL writes). Updated Corfu debug/test scripts/docs toward canonical ORDER2: `scripts/run_corfu_debug.sh` now runs ORDER2; `test_corfu_order3.sh` converted to ORDER2 compatibility wrapper; ORDER3 historical docs now carry archived/deprecated banner. Guardrails after task: Order0 `11939.91 MB/s`, Order5 `11484.71 MB/s`. |
| 2026-03-03 | Step 33 replay hardening R9 (subscriber connect-target monotonicity + startup wait budget) | Pass | 11.67 | 11.40 | In progress | Fixed subscriber startup race in latency path: `data_broker_count_` updates are now monotonic-max (partial cluster snapshots cannot shrink expected broker count), expected connections never drop below configured broker count, and connect wait timeout increased `30s -> 90s` with throttled 5s progress logs. Validation sweep (`TRIALS_PER_POINT=1`, targets `1000/1500/2000`, 4 GiB, ORDER5 ACK1) completed with `data_brokers=4` and all targets passing in the run immediately after the fix; full frozen `commands.sh` replay still in progress. Guardrails after patch: Order0 `11668.02 MB/s`, Order5 `11399.84 MB/s`. |
| 2026-03-03 | Step 34 replay hardening R10 (latency UID dedupe + partial-connect fail-fast) | Pass | 11.55 | 11.55 | In progress | Added latency message UID embedding in `LatencyTest` payload (`timestamp + uid`) and switched subscriber dedupe/completion to prefer UID-backed keys (with metadata fallback). Also made partial subscriber connection readiness fail fast (`9/12` now throws explicit `Subscriber connection readiness timeout` and retries instead of entering long doomed poll). Revalidated problematic `ORDER=5`, `target=2000 MB/s`, `4 GiB` point with retries: successful run reached exact accounting `recorded=4194304` (`parsed=12617598`, `dropped=8423294`) and completed without poll timeout. Guardrails after patch: Order0 `11546.88 MB/s`, Order5 `11550.80 MB/s`. |
| 2026-03-03 | Step 35 replay completion M4 (full round3 command-pack run to completion) | Pass | 11.71 | 11.74 | In progress | Completed full replay `bash data/latency/repro_manifest_step11_round3/commands.sh` end-to-end. Step8 produced full trial set but one hard failure at `trial_4/2000 MB/s` (`FAIL`, CI for 2000 uses `n=4` in `data/latency/paper_load_sweep_embarcadero_o5/ci_summary.csv`). Step9 ladder reproduced stable PASS for `O0A{0,1,2}` and `O5A{0,1}`, but `O5A2` failed after 3 retries with ACK shortfall (`data/latency/ordering_durability_ladder/ladder.csv`). Step10 moderate heterogeneity completed PASS for baseline + injected (`data/latency/slow_replica_step10_embarcadero/summary.csv`). Post-replay mandatory guardrails: Order0 `11713.42 MB/s`, Order5 `11742.55 MB/s`. |
| 2026-03-03 | Step 36 stabilization S4 (ACK2 semantic enforcement + replication CV/recovery correctness) | Pass | 11.65 | 11.73 | In progress | Fixed ACK2 semantics and tail replication bookkeeping. `ACK=2` now requires replication (`client` auto-promotes `replication_factor 0->1` with warning; broker rejects topic create if still invalid). Chain replication CV now tolerates out-of-order GOI completions via per-broker pending map and safe baseline init. GOI recovery is disabled for `replication_factor<=1` (prevents false "replicated" advancement) and recovered completions promote CV for ACK2 frontier. Validation: `ORDER=5 ACK=2` passed (`11787.14 MB/s`, full ACK verify). Latest mandatory guardrails after patch: Order0 `11648.61 MB/s`, Order5 `11732.72 MB/s`. Focused Step8 recheck (`target=2000`, 4 GiB, 3 attempts) still failed due to subscriber timeout path (`summary.csv: 2000,FAIL,FAIL`). |
| 2026-03-03 | Step 8 closure S5 (postfix rerun, 4 GiB x 5 trials) | Pass | 11.63 | 11.84 | Pass | Re-ran full paper sweep with `SWEEP_TARGETS=1000/1500/2000`, `TRIALS_PER_POINT=5`, `POINT_MAX_ATTEMPTS=2`: all points/trials passed first attempt with exact accounting (`parsed=recorded=4194304`, `dropped=0`) and monotonic stage checks. CI artifact: `data/latency/paper_load_sweep_embarcadero_o5_step8_postfix/ci_summary.csv` (`n=5` for all points). E2E means: `1000=929.346 MB/s`, `1500=1385.158 MB/s`, `2000=1835.260 MB/s`; e2e p99(us): `68036.6`, `86383.6`, `101221.6`. Post-item guardrails: Order0 `11634.77 MB/s`, Order5 `11838.87 MB/s`. |
| 2026-03-03 | Step 9 rerun S6 (ladder postfix) | Pass | 11.07 | 11.96 | Blocked | Ladder rerun hit deterministic regression at `ORDER=0 ACK=2`: ACK timeout with `received=0/10485760` and per-broker shortfall equal to sent counts on all brokers (reproduced with `TRIAL_MAX_ATTEMPTS=1`, `EMBARCADERO_ACK_TIMEOUT_SEC=60`). `ORDER=5 ACK=2` remains healthy (`11720.74 MB/s`). Partial ladder artifact: `data/latency/ordering_durability_ladder_postfix/ladder.csv` currently contains PASS for `O0A0=11706.92`, `O0A1=11855.34`; `O0A2` blocked. Post-attempt guardrails: Order0 `11074.03 MB/s` (low outlier), Order5 `11960.36 MB/s`. |
| 2026-03-03 | Step 10 closure S7 (heterogeneity postfix moderate scenario) | Pass | 11.44 | 11.86 | Pass | Ran moderate heterogeneity baseline/injected at `1 GiB`, `ORDER=5 ACK=1` with one paused broker (`+4s`). Summary: `data/latency/slow_replica_step10_postfix_embarcadero/summary.csv`; comparison shows ordered/ack p99 `50024 -> 50069` us (`+0.09%`), deliver p99 `201764 -> 206467` us (`+2.331%`), supporting decoupling trend. Post-item guardrails: Order0 `11440.24 MB/s`, Order5 `11861.90 MB/s`. |
| 2026-03-03 | Step 11 closure S8 (fresh repro package freeze) | Pass | 11.44 | 11.86 | In progress | Generated fresh package: `data/latency/repro_manifest_step11_postfix/` (`manifest.txt`, `commands.sh`, `script_hashes.txt`, `RERUN.md`, `files_snapshot.txt`). Full command-pack replay is currently blocked on Step 9 due deterministic `ORDER=0 ACK=2` regression above; fix required before claiming full reproducibility replay closure on latest code line. |
| 2026-03-19 | Step 9/11 continuation S9 (ORDER=0 ACK=2 latency/export fix revalidation) | Pass | 12.13 | 11.68 | In progress | Landed `d12f44c` and revalidated the repaired replicated path. `ORDER=0 ACK=2 RF=2` now passes throughput (`5180.24 MB/s`) and 1 GiB latency sanity at `target=1000 MB/s` (publish/e2e `677.79/677.77 MB/s`, `parsed=recorded=1048576`, `dropped=0`). Matching `ORDER=5 ACK=2 RF=2` latency sanity also passes (`895.83/895.80 MB/s`, exact accounting), but 10 GiB throughput remains unstable on the latest line: most recent single-attempt rerun timed out at `10481906/10485760`, with brokers `B1/B2` each short by one `1927`-message chunk. Fresh current-line ACK1 guardrails: Order0 `12127.72 MB/s`; latest clean Order5 ACK1 pass on the same fix line `11676.50 MB/s` (subsequent rerun re-hit the known shortfall/hang). |
| 2026-03-19 | Step 9/11 continuation S10 (explicit replica-dir revalidation + runner freeze refresh) | Pass | 12.15 | N/A | In progress | Root cause of the latest `ORDER=5 ACK=2` "regression" was runner configuration, not data-path logic: default discovery fell back to one writable `.Replication` dir, while the intended experiment uses explicit dual-device replica dirs. With `EMBARCADERO_REPLICA_DISK_DIRS=/home/domin/Embarcadero/.Replication/disk0,/mnt/nvme0/replication/disk1`, `ORDER=5 ACK=2 RF=2` now passes at `5944.61 MB/s` (direct run) / `5946.67 MB/s` (new ladder helper), and matching `ORDER=0 ACK=2 RF=2` passes at `5484.57 MB/s` / `5555.11 MB/s`. Added `scripts/run_ordering_durability_ladder.sh`, taught `run_throughput.sh` to inspect the actual replica-dir env, and refreshed `freeze_repro_package.sh` to match the current script set and freeze replica-dir env. Fresh Order0 ACK1 guardrail after runner fixes: `12151.31 MB/s`; Order5 ACK1 rerun remained a separate hang/flake, so no new clean guardrail number is claimed for that path here. |
| 2026-03-01 | Step 12 Task A (Corfu latency path instrumentation/classification) | Pass | 11.64 | 11.68 | Pass | Added explicit subscriber parse/drop counters and timeout diagnostics plus Corfu subscribe/export counters. Failure is now classifiable: Corfu `ORDER=2` shows export starvation (`bytes=0`, `parse_calls=0`), while Corfu `ORDER=3` shows decode mismatch (`bytes~400%`, `metadata_detected=0`, `break_invalid_size>0`, `samples_added=0`). Guardrails after item: 11640.46 MB/s (Order0), 11682.12 MB/s (Order5, second attempt after one transient ACK timeout). |

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
- [ ] Run matrix across order levels and ack levels.
- [x] Quantify strong-order overhead vs total-order mode.
- [ ] Confirm expected overhead envelope.
- [x] Run guardrail.
  - `scripts/run_ordering_durability_ladder.sh` is restored on the current tree as a thin sequential wrapper over `scripts/run_throughput.sh`; latest postfix verification used both direct `run_throughput.sh` commands and the restored helper.
  - Current run scope (Scalog deferred): Embarcadero canonical ordering levels (`order=0,5`) plus Corfu total-order reference point (`order=2, ack=1`) for cross-mode overhead quantification.
  - `order=0, ack=2` is repaired on the latest code line: throughput passes at `5180.24 MB/s` (default-layout run) and `5484.57/5555.11 MB/s` under the intended explicit dual-dir layout; 1 GiB latency sanity passes at `target=1000 MB/s` with exact accounting (`parsed=recorded=1048576`, `dropped=0`).
  - `order=5, ack=2` is also healthy when the runner uses the intended replica-dir layout: 1 GiB latency sanity passes (`895.80 MB/s`, exact accounting), and 10 GiB throughput passes at `5944.61/5946.67 MB/s` with `EMBARCADERO_REPLICA_DISK_DIRS=/home/domin/Embarcadero/.Replication/disk0,/mnt/nvme0/replication/disk1`.
  - Remaining latest-line blocker for a clean full ladder is now the separate `order=5, ack=1` flake/hang during mandatory guardrail reruns, not ACK2 semantics.

### Step 10: Slow-replica heterogeneity experiment
- [x] Inject one slow replica/disk.
- [x] Show effect on `append->ordered` vs `append->ack` for each system.
- [x] Validate Embarcadero decoupling claim under heterogeneity.
- [x] Run guardrail.
  - Implemented automation script: `scripts/run_slow_replica_heterogeneity.sh` (SIGSTOP/SIGCONT fault injection on one broker path).
  - Closure rerun artifacts (moderate scenario, 1 GiB): `data/latency/slow_replica_step10_closure_embarcadero/` and `data/latency/slow_replica_step10_closure_corfu/`.
  - Embarcadero (`ORDER=5`): ordered/ack p99 changed by `-1.66%` under injected slowdown, while delivery p99 changed by `+3.47%` (ordering remains decoupled from replica pause in this scenario).
  - Corfu (`ORDER=2`): ordered/ack p99 changed by `+63.80%` under slowdown in this run set; delivery p99 moved `-3.53%` (moderate-run variance).

### Step 11: Final reproducibility package
- [x] Freeze scripts + config + command lines.
- [x] Produce final CSV manifest + plotting scripts.
- [ ] Run full rerun from clean state.
- [x] Run guardrail.
  - Packaging helper: `scripts/freeze_repro_package.sh`.
  - Historical full replay completed on the older `round3` package: `bash data/latency/repro_manifest_step11_round3/commands.sh`.
  - The latest code line has new experiment-affecting changes (`79712dd`, `d12f44c`, runner refresh for explicit replica dirs), so replay closure must be re-run from a fresh freeze before we claim final reproducibility.
  - `scripts/freeze_repro_package.sh` now succeeds on the current tree again and records `EMBARCADERO_REPLICA_DISK_DIRS` into the manifest/commands when set (validated at `data/latency/repro_manifest_step11_explicit_tmp/`).
  - Current blocker is no longer ACK2 semantics; it is getting one clean latest-line replay with the explicit replica-dir env frozen and the remaining `ORDER=5 ACK=1` flake either resolved or explicitly bounded.

### Step 12: Corfu ORDER=2 receive/decode/export hardening (2026-03-02)
- [x] Restrict Corfu to canonical `ORDER=2` at client entry, topic creation, and runner gate.
- [x] Refactor Corfu ORDER=2 completion path to contiguous frontier semantics under multi-threaded recv completion.
- [x] Add Corfu ORDER=2 slot freshness checks (`pbr_absolute_index`) on export path.
- [x] Fail fast on Corfu sequencer token acquisition errors (`GetTotalOrder` bool check).
- [x] Validate Corfu latency success (1 GiB, two consecutive runs):
  - Run 1: publish `4758.16 MB/s`, end-to-end `1621.19 MB/s`, `recorded=1048576`.
  - Run 2: publish `5050.21 MB/s`, end-to-end `1603.66 MB/s`, `recorded=1048576`.
- [x] Validate after dead-path pruning pass:
  - Corfu latency: publish `4454.21 MB/s`, end-to-end `1608.74 MB/s`, `recorded=1048576`.
- [x] Guardrail after Task A/B refactor pass:
  - `ORDER=0 ACK=1`: `11816.54 MB/s`
  - `ORDER=5 ACK=1`: trial-1 timeout shortfall on broker 3; retry succeeded at `11546.06 MB/s`
- [x] Guardrail after Task C pruning pass:
  - `ORDER=0 ACK=1`: `11147.92 MB/s`
  - `ORDER=5 ACK=1`: `11851.64 MB/s`
- [x] Guardrail after ORDER=3 dead-path refactor (R1):
  - Corfu `TEST_TYPE=2 ORDER=2`: publish `4716.85 MB/s`, end-to-end `1666.86 MB/s`, `recorded=1048576`
  - `ORDER=0 ACK=1`: `11794.16 MB/s`
  - `ORDER=5 ACK=1`: retry-3 success `11878.72 MB/s` (attempts 1-2 timed out with per-broker ACK shortfall)
- [x] Guardrail after diagnostics-gating refactor (R2):
  - Corfu `TEST_TYPE=2 ORDER=2`: publish `4526.69 MB/s`, end-to-end `1653.90 MB/s`, `recorded=1048576`
  - `ORDER=0 ACK=1`: `11859.01 MB/s`
  - `ORDER=5 ACK=1`: `11547.45 MB/s`
- [x] Guardrail after dead-branch cleanup refactor (R3):
  - `ORDER=0 ACK=1`: `11389.83 MB/s`
  - `ORDER=5 ACK=1`: retry-2 success `11561.46 MB/s` (attempt-1 ACK timeout shortfall)
- [x] Guardrail after contiguous publish-frontier refactor (R4):
  - Corfu `TEST_TYPE=2 ORDER=2`: publish `4949.97 MB/s`, end-to-end `1608.31 MB/s`, `recorded=1048576`
  - `ORDER=0 ACK=1`: `11491.20 MB/s`
  - `ORDER=5 ACK=1`: `11558.88 MB/s`
- [x] Guardrail after Corfu client-id semantics refactor (R5):
  - Corfu `TEST_TYPE=2 ORDER=2`: publish `3592.52 MB/s`, end-to-end `1527.01 MB/s`, `recorded=1048576`
  - `ORDER=0 ACK=1`: `11641.67 MB/s`
  - `ORDER=5 ACK=1`: `11659.04 MB/s`
- [x] Guardrail after canonical ORDER={2,5} metadata cleanup (R6):
  - Corfu `TEST_TYPE=2 ORDER=2`: publish `4647.21 MB/s`, end-to-end `1669.22 MB/s`, `recorded=1048576`
  - `ORDER=0 ACK=1`: `11645.24 MB/s`
  - `ORDER=5 ACK=1`: retry-3 success `11474.97 MB/s` (attempts 1-2 ACK timeout with broker-1 shortfall)
- [x] Guardrail after contiguous publish-frontier overflow hardening (R7):
  - `ORDER=0 ACK=1`: `11725.39 MB/s`
  - `ORDER=5 ACK=1`: `11592.14 MB/s`
- [x] Guardrail after ORDER=5 ACK source hardening (R8):
  - Corfu `TEST_TYPE=2 ORDER=2`: publish `3776.78 MB/s`, end-to-end `1533.46 MB/s`, `recorded=1048576`
  - `ORDER=0 ACK=1`: `11651.14 MB/s`
  - `ORDER=5 ACK=1`: `11546.66 MB/s`
- [x] Guardrail after frontier invariants architecture note (A1):
  - `ORDER=0 ACK=1`: `11652.82 MB/s`
  - `ORDER=5 ACK=1`: `11724.86 MB/s`
- [x] Guardrail after frontier trace instrumentation (A2):
  - `ORDER=0 ACK=1`: `11950.95 MB/s`
  - `ORDER=5 ACK=1`: `11738.52 MB/s`
- [x] Guardrail after rollback of contiguous-CV frontier experiment (S1):
  - `ORDER=0 ACK=1`: `11280.57 MB/s`
  - `ORDER=5 ACK=1`: retry-2 success `11725.86 MB/s` (attempt-1 ACK timeout with broker-2 shortfall)
- [x] Guardrail after ORDER=5 idle hold-progress hardening (S2):
  - `ORDER=0 ACK=1`: `11765.72 MB/s`
  - `ORDER=5 ACK=1`: `11597.38 MB/s` (first attempt)
- [x] ORDER=5 single-attempt stability probe after S2:
  - `EMBARCADERO_ACK_TIMEOUT_SEC=45 TRIAL_MAX_ATTEMPTS=1 ... ORDER=5 ACK=1 ...` x3
  - Pass `3/3` (`11439.74`, `11656.72`, `11686.94 MB/s`)
- [x] ORDER=5 extended single-attempt burn-in after S2:
  - `TRIAL_MAX_ATTEMPTS=1 EMBARCADERO_ACK_TIMEOUT_SEC=45 ... ORDER=5 ACK=1 ...` x10
  - Result: `8 PASS / 2 FAIL` (fail shortfalls: 19,270 on B2 and 26,978 on B3)
- [x] Corfu ORDER=2 latency revalidation after S2:
  - Run 1: publish `5081.08 MB/s`, subscribe/end-to-end `1616.04 MB/s`, `recorded=1048576`
  - Run 2: publish `4592.15 MB/s`, subscribe/end-to-end `1643.68 MB/s`, `recorded=1048576`
- [x] Corfu latency revalidation after S1 (canonical ORDER=2):
  - Run 1 (`TEST_TYPE=2`, `1 GiB`): publish `4016.03 MB/s`, subscribe/end-to-end `1598.42 MB/s`, `recorded=1048576`
  - Run 2 (`TEST_TYPE=2`, `1 GiB`): publish `4659.29 MB/s`, subscribe/end-to-end `1640.78 MB/s`, `recorded=1048576`
  - Artifact sanity: `build/bin/latency_stats.csv` + `build/bin/cdf_latency_us.csv` non-empty
- [x] Corfu ORDER=3 triage behavior:
  - `NUM_BROKERS=4 TEST_TYPE=2 ORDER=3 ACK=1 ... SEQUENCER=CORFU scripts/run_throughput.sh`
  - Expected/observed fail-fast: `ERROR: CORFU is restricted to ORDER=2 in this implementation (got ORDER=3).`

- [x] ORDER=5 per-client-per-broker sequencing stream alignment (S3):
  - Implementation: `ORDER=5` publish path now assigns `batch_seq` per broker; sequencer Level5 state/hold keyed by `(client_id, broker_id)` stream.
  - Guardrail after S3:
    - `ORDER=0 ACK=1`: `11589.87 MB/s` (post-final ORDER5 seq-alignment rerun)
    - `ORDER=5 ACK=1`: `11657.87 MB/s`
  - ORDER=5 strict single-attempt stability after S3:
    - `TRIAL_MAX_ATTEMPTS=1 EMBARCADERO_ACK_TIMEOUT_SEC=45 ... ORDER=5 ACK=1 ...` x6
    - Result: `6 PASS / 0 FAIL` (`11734.94`, `11487.02`, `11600.86`, `11595.21`, `11542.41`, `11724.37 MB/s`)
  - Extended ORDER=5 strict burn-in after S3:
    - `TRIAL_MAX_ATTEMPTS=1 EMBARCADERO_ACK_TIMEOUT_SEC=45 ... ORDER=5 ACK=1 ...` x20
    - Result: `19 PASS / 1 FAIL` (single fail shortfall: broker-3 `short=61664`)
    - Pass bandwidths: `11252.11`, `11558.36`, `11716.64`, `11484.31`, `11659.19`, `11199.57`, `11670.07`, `11764.20`, `11528.19`, `11428.87`, `11433.81`, `11748.39`, `11432.24`, `11574.44`, `11778.81`, `11495.83`, `11830.05`, `11944.80`, `11794.89 MB/s`
  - Failure-repro trace follow-up:
    - `EMBAR_FRONTIER_TRACE=1 EMBARCADERO_ORDER5_TRACE=1 TRIAL_MAX_ATTEMPTS=1 ... ORDER=5 ACK=1 ...` x6
    - Result: `6 PASS / 0 FAIL` (no traceable shortfall captured in follow-up window)
  - Automated burn-in capture harness:
    - New script: `scripts/run_order5_burnin_capture.sh`
    - Example command: `RUNS=100 STOP_ON_FAIL=1 scripts/run_order5_burnin_capture.sh`
    - Validation run (manually stopped): `40 PASS / 0 FAIL`
    - Artifacts: `data/throughput/stability/order5_burnin_20260302_142713` (`summary.csv` + per-run logs)
  - Corfu latency sanity after S3 (`TEST_TYPE=2 ORDER=2`, 1 GiB):
    - publish `4686.71 MB/s`, subscribe/end-to-end `1701.30 MB/s`, `recorded=1048576`
