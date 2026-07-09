# Subscriber Delivery SLO Measurement

Date: 2026-07-08
Branch: `delivery-measure`
Base: `origin/subscriber-tail` (`a23ff6c1`)

## Scope

This branch wires a true delivery-stamped `publish_to_deliver_latency` metric through `Subscriber::Consume()`/`ConsumeOrdered()`. The old receive-side sample is relabeled to `publish_to_receive_latency` and remains in `latency_stats.csv`.

This document now contains the 2026-07-09 ORDER=5 delivery hardening validation. The full 4 GiB six-load curve passes ordering and export-overrun gates in both flush policies, but the tail at 4 GiB is seconds at and above 1000 MB/s. Do not cite the earlier 64 MiB smoke rows as the headline result.

## 2026-07-09 Export/Flush Hardening

The smoke rows below are superseded for any headline SLO claim. They remain diagnostic only.

Implemented in this pass:

- Flush policy is intent-driven. `cxl_type==Real` now defaults to explicit payload flushes on every mapping path, including DAX, mbind-backed real CXL, and real-CXL fallbacks. `EMBARCADERO_CXL_COHERENT=1` is the explicit operator opt-in for the coherent single-domain optimization. `cxl_type==Emul` remains coherent by default.
- The non-coherent writer path flushes the full received batch payload range before `batch_complete`/`publish_commit` publication, not just the first per-message header cacheline.
- Chain replication invalidates the full payload range and fences before reading payload bytes unless `EMBARCADERO_CXL_COHERENT=1` is set.
- ORDER=5 export ring lapping is no longer a silent permanent stall. The subscriber cursor resyncs to the oldest live descriptor, increments `ExportOverruns` and `ExportSkippedBatches` in `order5_anomaly_counters_broker*.csv`, and `scripts/run_latency.sh` fails the trial if either counter is nonzero. `EMBARCADERO_ORDER5_EXPORT_OVERRUN_FATAL=1` makes this path fatal for CI/diagnostic runs.
- Sequencer restart recovery rebuilds compact ORDER=5 export descriptors from recovered GOI entries before live sequencing resumes, preserving per-broker compact export sequence numbers for the recovered committed prefix. If recovered history exceeds the compact ring window, the overrun counters above make the gap visible.
- Latency-vs-load aggregation carries `order5_export_overruns` and `order5_export_skipped_batches` from per-trial metadata.

Validation completed in this pass:

```text
cmake --build build -j --target embarlet throughput_test
ctest --test-dir build -R 'unit_order5_publisher_rollover|unit_order5_session_fencing|unit_scalog_ack_invariant' --output-on-failure
bash -n scripts/run_latency.sh scripts/run_latency_vs_load.sh scripts/run_throughput_latency_sweep.sh scripts/lib/broker_lifecycle.sh
python3 -m py_compile scripts/aggregate_latency_vs_load.py
```

The focused unit tests passed. The first attempted CTest filter used the historical names from the task brief and found no tests; the build registers them as `unit_*`.

Topology and runtime:

- Host: single moscxl host, all brokers, sequencer, publisher, and subscriber on one machine.
- CXL: real CXL exposed as CPU-less NUMA node 2. `numactl -H` reported `node 2 cpus:` empty and `node 2 size: 255975 MB`.
- Placement: scripts bound brokers with node 2 in the memory policy; broker logs show `CXL region bound to NUMA node 2`.
- HugeTLB: not available for the 4 GiB latency runs. Logs show 5248 MB free huge pages but 8194 MB needed, `/dev/hugepages` not writable, then THP/madvise fallback.
- Flush-ON run: default real-CXL policy, no `EMBARCADERO_CXL_COHERENT`; broker logs show `CXL payload flush policy: explicit_flush_required`.
- Flush-OFF run: `EMBARCADERO_CXL_COHERENT=1`; broker logs show `cache_coherent_mapping` and `NetworkManager: skipping full payload-range flush on cache-coherent CXL mapping`.

Validation caveat: recovery descriptor rebuild was implemented, but a true restart-mid-stream plus reconnecting-subscriber E2E test was not run. The available validation is build/unit plus the lapping delivery curves below.

## 4 GiB Headline Curves

Each row is 3 trials, ORDER=5, ACK=1, RF=0, 4 brokers, 1 KiB messages, 4 GiB total, steady pacing, `EMBARCADERO_DEDUPE_LATENCY=1`, `EMBARCADERO_SUBSCRIBER_DIAG=1`, and `EMBARCADERO_ORDER5_EXPORT_OVERRUN_FATAL=1`. Values are averages with min-max ranges in parentheses. Latencies are publish-to-deliver at `ConsumeOrdered`.

Flush-ON, default real-CXL/non-coherent policy:

| Target MB/s | Achieved offered MB/s | E2E goodput MB/s | p50 ms | p99 ms | p99.9 ms | max ms |
|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100.0 (100.0-100.0) | 94.1 (94.1-94.1) | 20.1 (20.0-20.2) | 93.3 (69.0-117.2) | 132.9 (95.3-183.2) | 152.7 (105.5-206.2) |
| 1000 | 1000.0 (1000.0-1000.0) | 451.6 (434.9-475.9) | 1503.1 (1028.8-1891.0) | 4644.3 (4160.4-4979.7) | 4722.8 (4248.7-5048.6) | 4732.5 (4257.1-5067.0) |
| 2000 | 2000.0 (2000.0-2000.0) | 546.2 (543.7-550.1) | 1014.7 (961.6-1075.5) | 5222.2 (5161.9-5260.4) | 5314.7 (5268.9-5346.6) | 5322.5 (5269.5-5358.0) |
| 4000 | 2234.5 (2225.5-2239.7) | 508.5 (496.6-520.5) | 1521.5 (1170.2-1927.8) | 6014.7 (5820.5-6203.1) | 6102.7 (5901.7-6303.4) | 6110.8 (5913.4-6304.3) |
| 6000 | 2222.0 (2211.5-2229.7) | 525.0 (510.2-545.7) | 1323.9 (1155.9-1558.5) | 5732.0 (5426.7-5961.8) | 5837.4 (5540.5-6070.6) | 5849.2 (5554.6-6071.2) |
| 8000 | 2226.2 (2220.0-2236.1) | 515.1 (452.8-548.8) | 1525.2 (1072.4-2398.6) | 5954.0 (5393.6-7009.3) | 6054.8 (5502.9-7087.9) | 6059.2 (5503.6-7099.7) |

Flush-OFF, explicit single-host coherent CXL-NUMA optimization:

| Target MB/s | Achieved offered MB/s | E2E goodput MB/s | p50 ms | p99 ms | p99.9 ms | max ms |
|---:|---:|---:|---:|---:|---:|---:|
| 100 | 100.0 (100.0-100.0) | 94.1 (94.1-94.1) | 20.1 (20.1-20.2) | 89.6 (78.4-102.7) | 129.4 (108.0-159.3) | 158.4 (137.2-183.4) |
| 1000 | 1000.0 (1000.0-1000.0) | 478.1 (440.6-509.8) | 1076.0 (428.1-1877.0) | 4158.9 (3607.2-4851.6) | 4234.6 (3675.9-4936.7) | 4246.7 (3682.2-4947.3) |
| 2000 | 2000.0 (2000.0-2000.0) | 528.4 (508.1-549.6) | 1136.1 (970.9-1219.6) | 5452.4 (5148.1-5752.2) | 5569.5 (5265.1-5876.2) | 5583.6 (5276.9-5885.1) |
| 4000 | 2214.7 (2210.5-2218.7) | 538.8 (513.4-553.9) | 1118.4 (1044.7-1220.1) | 5531.2 (5302.9-5897.7) | 5638.1 (5408.5-6009.1) | 5645.8 (5430.0-6009.9) |
| 6000 | 2211.5 (2206.5-2216.6) | 528.1 (516.4-551.3) | 1296.7 (1037.4-1593.6) | 5685.9 (5354.0-5862.1) | 5786.8 (5465.4-5952.7) | 5794.9 (5466.2-5964.5) |
| 8000 | 2210.6 (2206.9-2213.5) | 507.6 (466.3-549.8) | 1531.2 (1022.0-2148.8) | 6028.2 (5366.6-6728.0) | 6129.4 (5480.5-6806.6) | 6136.7 (5481.3-6818.4) |

Artifacts:

- Flush-ON: `data/latency_vs_load/delivery_hardening_flush_on/EMBARCADERO_order5_ack1_rf0_flush_on/run_20260709T040000Z_flush_on`
- Flush-OFF: `data/latency_vs_load/delivery_hardening_flush_off/EMBARCADERO_order5_ack1_rf0_flush_off/run_20260709T041839Z_flush_off`
- Parent export-off A/B: `/home/domin/Embarcadero-sessions/delivery-ab-parent/data/latency_vs_load/delivery_ab_parent_export_off/EMBARCADERO_order5_ack1_rf0_parent_export_off/run_20260709T044521Z_parent_export_off`

## Delivery Correctness Gate

The 4 GiB load laps the 512-slot export ring many times. Both headline curves completed all 36 trials with:

```text
delivery_ordering_assertion.csv files: 36
bad assertion files: 0
Delivered == Target for every trial: 4,194,304/4,194,304
InvalidMessages = 0
DuplicateTotalOrder = 0
OutOfOrderTotalOrder = 0
DuplicateUid = 0
MissingUid = 0
Pass = 1
```

The exported anomaly counters were also zero in both curves:

```text
flush-ON:  order5_export_overruns=0, order5_export_skipped_batches=0
flush-OFF: order5_export_overruns=0, order5_export_skipped_batches=0
```

Thus the lapping run did not silently stall and did not require skip/resync. If a future overrun occurs, `run_latency.sh` treats nonzero counters as a failed trial.

## A/B Attribution

Export-rewrite OFF uses a temporary worktree at `6a791bc` plus the harness metadata commit only. This is not a perfect same-binary feature toggle because `6a791bc` predates the later explicit coherent opt-in work; interpret it as old export behavior versus current export behavior. Flush contribution is measured within the current branch by comparing default flush-ON with explicit flush-OFF.

Parent export-OFF results:

| Target MB/s | Achieved offered MB/s | E2E goodput MB/s | p50 ms | p99 ms | p99.9 ms | max ms |
|---:|---:|---:|---:|---:|---:|---:|
| 1000 | 1000.0 (1000.0-1000.0) | 400.3 (398.2-403.3) | 1614.8 (1541.8-1653.0) | 5763.5 (5685.2-5814.4) | 5870.4 (5793.3-5922.6) | 5882.9 (5805.9-5935.1) |
| 4000 | 2239.6 (2197.9-2262.2) | 449.0 (439.4-457.1) | 2004.6 (1951.5-2107.9) | 7044.3 (6902.7-7254.7) | 7170.5 (7027.3-7382.0) | 7181.7 (7037.3-7395.5) |

Using p99 publish-to-deliver latency:

| Load | Parent export-OFF p99 | Current flush-ON p99 | Current flush-OFF p99 | Export rewrite improvement vs parent | Flush-OFF delta vs flush-ON |
|---:|---:|---:|---:|---:|---:|
| 1000 MB/s | 5763.5 ms | 4644.3 ms | 4158.9 ms | 1604.6 ms | 485.5 ms, about 30.3% of the export-improvement magnitude |
| 4000 MB/s | 7044.3 ms | 6014.7 ms | 5531.2 ms | 1513.1 ms | 483.5 ms, about 32.0% of the export-improvement magnitude |

Conclusion: the export rewrite is the larger tail reducer at these two points. The coherent flush skip helps, but it is not the main source of the tail reduction.

## Real-CXL RF Smoke

Single-host RF=2 smoke, flush-ON, ORDER=5, ACK=2, 4 brokers, 64 MiB total:

```text
DATA_DIR=data/latency/delivery_hardening_rf2_smoke
Delivered=65536/65536
Pass=1
Publish->Deliver p50=3.885 ms p99=4.746 ms p99.9=4.855 ms max=4.912 ms
```

This exercises the RF/ACK2 path on the same host and with the default full-body flush policy. It is not a multi-host cross-coherence-domain stress test; no multi-host RF>1 deployment was run.

## Code Changes

- Fixed the write-offset race by capturing the recv target buffer index with the write pointer and advancing that exact buffer. `EMBARCADERO_SUBSCRIBER_DIAG=1` asserts the advanced offset does not exceed capacity.
- Pinned the write role during an in-flight `recv()` so consumer release cannot swap a buffer while the receiver is writing into it.
- Scoped ORDER=0 out of partial-flush delivery SLO: partial flush is disabled for ORDER=0 because the legacy parser has no residual carry-over across buffer swaps.
- Enabled the ordered delivery stream during latency measurement and added a concurrent drain loop that timestamps `Consume()` return.
- Added `delivery_latency_stats.csv`, `cdf_delivery_latency_us.csv`, `delivery_ordering_assertion.csv`, and `delivery_stage_breakdown.csv`.
- Added an ordered-consumer fast path: if `pending_messages_` already contains the next total order, `ConsumeOrdered()` returns it before relocking and reparsing connection streams.

## Build

Passed:

```text
cmake --build build -j64 --target throughput_test embarlet
```

Warnings were existing third-party/sign-compare warnings.

## Delivery Correctness

All retained delivery runs passed the machine-checked assertion:

```text
Delivered == Target
InvalidMessages = 0
DuplicateTotalOrder = 0
OutOfOrderTotalOrder = 0
DuplicateUid = 0
MissingUid = 0
Pass = 1
```

## Smoke Results

All latency rows used `EMBARCADERO_DEDUPE_LATENCY=1` and `EMBARCADERO_SUBSCRIBER_DIAG=1`.

| Topology | Total | Target | Change State | Deliver p50 | Deliver p99 | Deliver p99.9 | Deliver max | Receive p99 | Ordering |
|---|---:|---:|---|---:|---:|---:|---:|---:|---|
| 1 broker ORDER=5 | 8 MiB | 1000 MB/s | delivery drain + event wait | 17.385 ms | 22.631 ms | 22.666 ms | 22.667 ms | 22.038 ms | pass, 8192/8192 |
| 4 brokers ORDER=5 | 64 MiB | 1000 MB/s | before consumer fast path | 115.724 ms | 128.941 ms | 129.163 ms | 129.211 ms | 10.013 ms | pass, 65536/65536 |
| 4 brokers ORDER=5 | 64 MiB | 1000 MB/s | after consumer fast path | 67.380 ms | 89.184 ms | 89.568 ms | 89.619 ms | 23.501 ms | pass, 65536/65536 |

An attempted parser-offset/direct-return optimization was discarded because it regressed the same 4-broker point to 170.800 ms p99.

## Stage Breakdown

Retained 4-broker after-fast-path tail (`>= p99`) was still dominated by post-receive consumer/backlog delay:

```text
Tail threshold:             89184 us
Tail matched samples:       656
Dominant stage:             consumer_poll_or_backlog
BufferAge p99:              25958 us
ReceiveToDeliver p99:       78089 us
RecvChunkBytes p99:         ~1 MiB
```

Interpretation: the true delivery path is now being measured and is lossless, but the single-message `ConsumeOrdered()` drain remains the limiting stage on the 4-broker 1 KiB-message load. The receive side is no longer the whole tail.

## Throughput Guard

4-broker ORDER=5 publish-only, 4 GiB, 1 KiB messages:

```text
Publish bandwidth: 12076.87 MB/s
Send-done bandwidth: 12107.27 MB/s
HugeTLB: fallback to THP/madvise path
```

This meets the approximate 12 GB/s guard in the same fallback regime noted by prior runs.

## SLO Status

- 4 GiB correctness gate: passed for both flush-ON and flush-OFF, 36/36 trials, no ordering failures, no export skips/overruns.
- 100 MB/s point: under 100 ms average p99 in both flush policies.
- 1000 MB/s and above: delivery p99 is seconds, not tens of milliseconds. The branch should not claim a sub-50 ms or sub-100 ms headline delivery SLO at 4 GiB.
- 4000/6000/8000 MB/s targets did not achieve requested offered load; the publisher saturated around 2.2 GB/s and the end-to-end drain around 0.5 GB/s.

## Hard-Gate Status

- Item 1 flush policy: passed. Default is explicit flush for real CXL; coherent mode requires `EMBARCADERO_CXL_COHERENT=1`.
- Item 2 full-body flush and reader invalidation: implemented. RF=2 single-host smoke passed, but no multi-host cross-coherence-domain stress was run.
- Item 3 export ring lapping: passed. The 4 GiB curve lapped the 512-slot export ring and completed with zero skips/overruns.
- Item 4 recovery contract: partially complete. Recovery descriptor rebuild is implemented, but the requested restart-mid-stream plus reconnecting-subscriber E2E test was not run.
- Item 5 headline both ways and attribution: passed for flush-ON/OFF curves and two-point parent A/B attribution.
- Item 6 delivery/ordering curve: passed for RF=0 on real CXL NUMA node 2 with both flush policies. Real-CXL RF smoke is single-host only.
- Commit-message correction: the relevant existing messages do not claim payload-size recompute removal was the root cause; no published-history rewrite was performed.
- Build and focused unit tests: passed.

## Caveats

- The true restart/reconnect recovery acceptance test remains missing.
- Multi-host RF>1 correctness was not exercised.
- ORDER=0 partial-flush delivery is explicitly out of scope in this branch.
- HugeTLB was unavailable for the throughput guard and 4 GiB latency curves; results used THP/madvise fallback.
- `latency_stats.csv` now means receive-side wire arrival only: `publish_to_receive_latency`.
