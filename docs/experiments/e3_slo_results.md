# E3 SLO Results: Embarcadero ORDER=5

**Status:** Phase 1 result.
**Run tag:** `20260708_e3_order5_slo_curve`
**Run commit:** `59f15b3690fe2ef3f9e87b2f17eb058f97e7146d`
**Raw tree:** `docs/experiments/e3_slo_raw/20260708_e3_order5_slo_curve/`

## Configuration

- System: Embarcadero-on-CXL
- Ordering: `ORDER=5`
- Sequencer: `EMBARCADERO`
- ACK/RF: `ACK=1`, `REPLICATION_FACTOR=1`
- Brokers: 4 on `moscxl`
- Publisher: one remote client, `c4`, NUMA 1
- Broker address used by client: `192.168.60.8`
- Message size: 1 KiB
- Total bytes: 1 GiB per trial
- Trials: 5 per offered-load point
- Peak input: 96.841 MB/s from E1 leg 4
- Offered-load points: 10, 48, 68, and 87 MB/s
- Pattern-kill cleanup disabled with `EMBARCADERO_DISABLE_PATTERN_KILL=1`

The remote client repeatedly reported capped socket buffers and insufficient HugeTLB free pages,
falling back to the THP/madvise path. Those warnings are preserved in the raw logs; the run completed
successfully and all trial rows passed the harness monotonicity checks.

## Trial Results

Medians and ranges are across five successful trials per load point.

| Target MB/s | Median goodput MB/s | Goodput range MB/s | Median append ACK p99 ms | Append ACK p99 range ms | Median E2E p50 ms | Median E2E p99 ms | E2E p99 range ms |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 10 | 9.404 | 9.404-9.411 | 4.444 | 4.345-4.683 | 133.347 | 262.206 | 233.750-287.336 |
| 48 | 45.145 | 45.029-45.146 | 5.426 | 5.059-5.519 | 53.502 | 432.832 | 357.966-710.912 |
| 68 | 63.924 | 63.351-63.930 | 5.651 | 5.406-5.992 | 52.106 | 593.554 | 383.142-812.091 |
| 87 | 81.745 | 81.004-81.751 | 6.503 | 5.662-7.840 | 67.481 | 1198.750 | 705.790-1432.598 |

## SLO Threshold Curve

The derived threshold curve is in
`docs/experiments/e3_slo_raw/20260708_e3_order5_slo_curve/e3_slo_threshold_curve.csv`.
It uses mean summary rows and reports both registered latency surfaces:

- `append_ack_p99`: no point satisfies 1 ms or 2 ms; 10 MB/s satisfies 5 ms; all tested points up to
  81.536 MB/s mean goodput satisfy 10 ms and looser thresholds.
- `publish_to_deliver_p99`: no tested point satisfies 1, 2, 5, 10, 20, 50, or 100 ms. The lowest
  measured mean p99 is 256.124 ms at the 10 MB/s target.

## Pre-Registered Comparison

This E3 leg supports the split predicted in the registration: append-side ORDER=5 latency remains
single-digit milliseconds at the highest tested load, but end-to-end publish-to-deliver tail latency
turns up well before the 90% load point. Because this run only measures Embarcadero-on-CXL, it does
not by itself decide the baseline comparison. It does, however, establish the Embarcadero SLO curve
for the D1-unblocked ORDER=5 path and preserves the contradiction boundary for future baseline
legs: any baseline that sustains comparable publish-to-deliver p99 at these offered loads must be
reported as narrowing the architecture claim.
