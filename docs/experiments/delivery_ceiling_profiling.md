# ORDER=5 delivery ceiling profiling

Date: 2026-07-09
Branch: `delivery-measure`
Starting tip: `c150fd1`

Goal: confirm whether the ORDER=5 single-connection delivery ceiling is the broker export path (`SubscribeNetworkThread` doing serial CXL-read -> copy-to-socket), classify the sub-cause, and stop before implementing any export-path fix.

## Measurement-only harness change

The retained `delivery_latency_stats.csv` is summary-only, so total delivery goodput is biased by fill/drain time. This pass added a measurement-only CSV, `delivery_steady_throughput.csv`, computed from delivered bytes between the 25th and 75th percentile delivery timestamps.

No export pipeline, sharding, zerocopy, or delivery-path fix was implemented in this pass.

## Workload

Profiled workload:

```bash
env DATA_DIR=data/latency/delivery_ceiling_profiling \
  RUN_ID=profile_8g_perf_20260709T063921Z \
  TARGET_MBPS=1000 \
  THREADS_PER_BROKER=1 \
  ORDERS=5 \
  MODES=steady \
  SEQUENCER=EMBARCADERO \
  ACK_LEVEL=1 \
  REPLICATION_FACTOR=0 \
  MSG_SIZE=1024 \
  TOTAL_MESSAGE_SIZE=8589934592 \
  NUM_TRIALS=1 \
  NUM_BROKERS=4 \
  SCENARIO=local \
  EMBARCADERO_DEDUPE_LATENCY=1 \
  EMBARCADERO_SUBSCRIBER_DIAG=1 \
  EMBARCADERO_ORDER5_EXPORT_OVERRUN_FATAL=1 \
  EMBARCADERO_DELIVERY_DRAIN_BATCH=4096 \
  bash scripts/run_latency.sh
```

Artifact directory:

`data/latency/delivery_ceiling_profiling/steady/profile_8g_perf_20260709T063921Z/EMBARCADERO_order5_ack1_msg1024_bytes8589934592_trial1`

Profiler scratch directory:

`/tmp/delivery_profile_auto2`

The profiled run delivered all 8,388,608 messages and passed the ORDER=5 assertion with zero overruns, skips, or duplicates.

## Throughput and latency

| run | total e2e goodput | windowed delivery goodput | publish->receive p99 | publish->deliver p99 |
| --- | ---: | ---: | ---: | ---: |
| `profile_8g_20260709T063425Z` | 279.07 MB/s | 288.213 MiB/s | 20.150 s | 20.189 s |
| `profile_8g_auto_20260709T063636Z` | 254.08 MB/s | 269.706 MiB/s | 22.389 s | 23.109 s |
| `profile_8g_perf_20260709T063921Z` | 274.962 MB/s | 287.078 MiB/s | 20.559 s | 20.627 s |

Profiled run `delivery_steady_throughput.csv`:

```csv
Method,StartSample,EndSample,WindowMessages,WindowSeconds,PayloadBytes,PayloadMiBPerSec
delivery_timestamp_p25_p75,2097152,6291456,4194304,14.267895,4294967296,287.078083
```

## Thread CPU

Process ids in the profiled run:

```text
brokers=2996620 2996621 2996622 2996623
client=2997313
```

The expected F1 signature was a follower broker `SubscribeNetworkThread` pegged near 100% CPU. That was not observed.

Observed steady-state CPU:

| process | tid | role | observed CPU |
| --- | ---: | --- | ---: |
| subscriber `2997313` | `2997364` | `ReceiveWorkerThread` | 100.00% |
| subscriber `2997313` | `2997365` | `ReceiveWorkerThread` | 100.00% |
| subscriber `2997313` | `2997366` | `ReceiveWorkerThread` | 99.90% |
| subscriber `2997313` | `2997367` | `ReceiveWorkerThread` | 100.00% |
| follower broker `2996621` | `2997380` | export/send-ish worker | 7.29% |
| follower broker `2996622` | `2997382` | export/send-ish worker | 9.39% |
| follower broker `2996623` | `2997381` | export/send-ish worker | 7.19% |

The head broker had expected head/sequencer/scanner CPU, including `grpcpp_sync_ser` threads around one core. That is not the follower export ceiling being tested here.

Conclusion from `pidstat`: the live limiter is not a single broker export thread consuming a full core. The subscriber receive side is saturated instead.

## Broker perf

`perf record -g --call-graph dwarf -p 2996622 -- sleep 15` was captured during steady state. Kernel symbols were masked by `/proc/sys/kernel/kptr_restrict=1`, so the exact kernel names such as `tcp_sendmsg` and `copy_user_enhanced_fast_string` could not be resolved without sudo/vmlinux access.

Top no-children samples on follower broker `2996622`:

| sample share | resolved user stack |
| ---: | --- |
| 7.14% | unresolved kernel address under `__libc_send` -> `NetworkManager::SendMessageData` -> `SubscribeNetworkThread` |
| 6.88% | unresolved kernel address under `__libc_recv` -> `HandlePublishRequest` |
| 5.23% | `NetworkManager::HandlePublishRequest` |
| 3.83% | unresolved kernel address under `__libc_send` -> `NetworkManager::SendMessageData` |
| 2.27% | `TopicManager::GetTopic` |

There was no dominant broker-side `memcpy`/`memmove`, CXL mapping read, `flush_cacheline`, or `full_fence` path. Because the broker export worker was not CPU-pegged, this perf profile does not support either a broker CPU copy ceiling or a broker CXL-read ceiling.

## Socket backlog

`ss -tmi` during the profiled run showed the broker delivery sockets with large `Send-Q`/`notsent` queues and the subscriber sockets with receive queues near the configured receive buffer.

| broker port | broker Send-Q | broker `notsent` | broker `rwnd_limited` | subscriber Recv-Q |
| ---: | ---: | ---: | ---: | ---: |
| 1214 | 98,944,813 B | 98,944,813 B | 73.4% | 32,833,362 B |
| 1215 | 112,041,413 B | 112,041,413 B | 74.8% | 31,414,824 B |
| 1216 | 89,923,147 B | 89,923,147 B | 75.1% | 31,788,530 B |
| 1217 | 89,711,710 B | 89,711,710 B | 73.9% | 32,372,973 B |

This is the backpressure signature: the brokers have bytes ready to send, but the subscriber receive side is not draining the loopback sockets fast enough.

## Node-2 read bandwidth

`numastat -p 2996622` was captured before and after the profiled window:

| point | node 2 private | total |
| --- | ---: | ---: |
| before | 65,536.00 MB | 65,564.84 MB |
| after | 0.00 MB | 28.35 MB |

This confirms the broker mapped the expected node-2/CXL-backed allocation before the run ended, but `numastat -p` reports memory placement, not read bandwidth. The process unmapped/exited before the after snapshot, so this is not a bandwidth delta.

`perf list` exposed AMD UMC memory bandwidth events such as `dram_read_data_for_local_processor`, `dram_read_data_for_remote_processor`, and `umc_mem_read_bandwidth`, but this pass did not find an unprivileged way to scope those counters specifically to node 2. Node-2 read bandwidth therefore remains unavailable from this profiling pass.

Given the broker export threads were not CPU-pegged and socket state was receiver-window limited, the missing node-2 bandwidth counter does not change the classification.

## Send diagnostics

The broker source contains the `SubscribeNetworkThread` exit send diagnostic around `src/network_manager/network_manager.cc:2091-2102`, but the profiled run still did not emit `send-diag`, `avg_send_us`, `epoll_pct`, or `throughput_mbs` lines in the copied broker logs. The run shut down cleanly from the harness perspective, but this diagnostic was not available.

## Classification

The expected F1 claim was not confirmed.

Observed limiter: subscriber-side receive/backpressure structure. Four subscriber receive worker threads were pinned at about one full core each, follower broker export/send workers were not pegged, broker send queues were deep, broker `notsent` bytes were high, and subscriber receive queues were near full.

Rejected classifications:

| hypothesis | result | reason |
| --- | --- | --- |
| broker export CPU copy/send bound | not confirmed | follower broker export/send workers were about 7-9% CPU, not near 100% |
| broker CXL-read bound | not confirmed | no dominant CXL read/flush/fence symbols, and export workers were not saturated |
| raw node-2 bandwidth plateau | not measured directly | node-2 bandwidth counters were unavailable, but socket/CPU evidence points elsewhere |
| backpressure/receiver structure | confirmed | broker Send-Q/notsent pinned, subscriber Recv-Q deep, subscriber receive workers pegged |

## Recommendation

Do not implement the broker export pipeline/sharding fix yet. The cheapest next fix is subscriber-side batch receive/parse/stage handoff: remove per-message receive-side allocation/copy/metadata work from the hot `ReceiveWorkerThread` path and hand ordered batches through in spans or pooled buffers. Expected gain: at least recover the prior roughly 0.45-0.5 GB/s single-connection ceiling, with a plausible 2-4x gain if the pinned receive-worker path stops doing per-message work.

Revisit broker `writev`/`sendmmsg`, `MSG_ZEROCOPY`, or double-buffered export only after subscriber receive workers are no longer pegged and the loopback receive queues stay shallow.
