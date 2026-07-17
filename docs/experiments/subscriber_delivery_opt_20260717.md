# Subscriber delivery retained-chunk experiment

Starting tip: `4ae8bc3f1aae4de781cd3b6d5a7c8b5e96df8ca8` (with pre-existing unrelated worktree changes).

## Change

`ParseAndStageOrderedBytes` now retains one copy of each receive chunk and makes complete ordered frames views into that chunk. `OwnedMessage::data` remains the fallback for a frame assembled across receive boundaries. The view is retained by the existing `OwnedMessagePtr` until the next `ConsumeOrdered` or `ConsumeOrderedBatch` release, so no receive buffer is recycled while a consumer view is live.

## Results

The default 64 GiB / 8 GiB-segment configuration has only three usable segments after the fixed 32 GiB GOI and metadata regions, so it cannot start the four-broker workload. Both capacity runs therefore used the same explicit, capacity-only override: `EMBARCADERO_SEGMENT_SIZE=4294967296` and `EMBARCADERO_REQUIRED_CXL_SEGMENTS=4`.

| run | windowed MiB/s | e2e MB/s | delivered | ordering assertion | artifact directory |
| --- | ---: | ---: | ---: | --- | --- |
| baseline | 262.339 | 259.642 | 8,388,608 / 8,388,608 | Pass=1; no timeout, invalid, duplicate, out-of-order, or missing entries | `data/latency/subscriber_delivery_opt/steady/baseline_4gseg_20260717T000000Z/EMBARCADERO_order5_ack1_msg1024_bytes8589934592_trial1` |
| retained chunk | 281.605 | 280.465 | 8,388,608 / 8,388,608 | Pass=1; no timeout, invalid, duplicate, out-of-order, or missing entries | `data/latency/subscriber_delivery_opt/steady/after_retained_chunk_4gseg_20260717T000000Z/EMBARCADERO_order5_ack1_msg1024_bytes8589934592_trial1` |
| ratio | 1.073x | 1.080x | n/a | pass | n/a |

The patch is correct under the gate but does not meet the 1.5x target. The retained chunk removes per-message payload allocation/copy for complete receive frames, but the receive worker still performs parsing, order staging, and a single receive-chunk copy before returning to `recv`.

The one-broker 1 GiB smoke also passed at 489.532 MiB/s, with 1,048,576 / 1,048,576 messages and a clean assertion. Artifact directory:

`data/latency/subscriber_delivery_opt/steady/smoke1g_20260717T000000Z/EMBARCADERO_order5_ack1_msg1024_bytes1073741824_trial1`

No 1.5x claim is made. The one next gated experiment is approach B: make each receive worker enqueue retained chunks to a bounded parser/stager queue, so it returns to `recv` without parsing/staging each message. Repeat the same four-broker 8 GiB recipe and require both the ordering assertion and a 1.5x ratio.
