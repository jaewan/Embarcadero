# Subscriber Delivery SLO Measurement

Date: 2026-07-08
Branch: `delivery-measure`
Base: `origin/subscriber-tail` (`a23ff6c1`)

## Scope

This branch wires a true delivery-stamped `publish_to_deliver_latency` metric through `Subscriber::Consume()`/`ConsumeOrdered()`. The old receive-side sample is relabeled to `publish_to_receive_latency` and remains in `latency_stats.csv`.

This is not a completed headline SLO package. The full 4 GiB, six-load, three-trial curve and the A/B copy-elimination study were not completed in this pass. The results below are smoke/diagnostic runs used to validate correctness and localize the remaining tail source.

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

## A/B Status

Not completed. No valid fractions are reported for delivery-bounding vs copy-elimination. The branch has the instrumentation needed to run the A/B correctly, but the controlled arms and three-trial load points still need to be executed.

## SLO Status

- One-broker smoke: under 50 ms p99 at the tested point.
- Four-broker smoke: under 100 ms p99 after the consumer fast path, but not under 50 ms p99.
- Full sustainable-load curve: not established.
- The old ~733 ms tail is reduced on these smoke points, but the headline SLO cannot be claimed until the full curve and A/B gate are run.

## Caveats

- Smoke totals were 8 MiB and 64 MiB, not the requested 4 GiB headline runs.
- Results are one trial per point, not three.
- ORDER=0 partial-flush delivery is explicitly out of scope in this branch.
- HugeTLB was unavailable for the throughput guard; results used THP/madvise fallback.
- `latency_stats.csv` now means receive-side wire arrival only: `publish_to_receive_latency`.
