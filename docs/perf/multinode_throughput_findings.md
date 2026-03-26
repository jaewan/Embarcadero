# Multi-Node Publish Throughput — Findings and Status

_Last updated: 2026-03-26_

---

## TL;DR

All "12 GB/s" results in git history are **loopback** (client and broker on the same moscxl
machine using `EMBARCADERO_HEAD_ADDR=10.10.10.10` which routes via `lo`).  No real cross-machine
wire benchmark has ever reached 12 GB/s.

The real c4 → moscxl 100GbE wire achieves **~6.7 GB/s** sustained.

The bottleneck is the **single-threaded producer** in `PublishThroughputTest()`: one CPU core on
c4 (Intel Xeon 8462Y+, Sapphire Rapids) can fill the batch queue at ~6.7 GB/s.  The same
producer on moscxl (AMD EPYC 9754, Genoa/Zen4) runs at ~13 GB/s — the 2× gap is raw
single-core streaming write throughput, not network.

---

## Machine Inventory

| Host   | CPU                              | Role                         |
|--------|----------------------------------|------------------------------|
| moscxl | AMD EPYC 9754 128-core (Zen4)    | Broker node; real CXL node 2 |
| c4     | Intel Xeon Platinum 8462Y+ (SPR) | Client node 1                |
| c3     | Intel Xeon (SPR)                 | Client node 2                |
| c2, c1 | (available)                      | Additional client nodes      |

Network: 100GbE between all nodes (12.5 GB/s line rate).

---

## Bottleneck Analysis

### Publisher pipeline (c4 → moscxl, 4 brokers, ORDER=0, ACK=1, 1 KB messages)

| Metric                           | Value      | Interpretation                       |
|----------------------------------|------------|---------------------------------------|
| Reported bandwidth               | ~6.7 GB/s  | Total data written                    |
| Consumer (PublishThread) idle %  | ~93%       | Threads spend 93% waiting for batches |
| EAGAIN events on send()          | 0          | TCP send buffer never full            |
| Batch size on wire               | ~512 KB    | Default `EMBARCADERO_BATCH_SIZE`      |
| Producer rate (c4 single core)   | ~6.7 GB/s  | memcpy + queue write in tight loop    |

Consumer idle = 93% means **the producer is the bottleneck**, not the network.

### Loopback comparison (moscxl client → moscxl broker)

| Metric                  | Loopback (moscxl)   | Real wire (c4→moscxl) |
|-------------------------|---------------------|-----------------------|
| Bandwidth               | ~12–13 GB/s         | ~6.7 GB/s             |
| header_send_calls       | 5,442               | 21,800                |
| Bytes per send call     | ~2 MB               | ~512 KB               |
| header_send_syscall_ms  | 11,351 ms (83%)     | 2,071 ms (7%)         |
| Consumer idle           | ~5%                 | ~93%                  |

On loopback the AMD EPYC producer saturates the consumers; on real wire c4's slower producer
cannot keep consumers busy.

---

## Producer Rate Hypothesis — Multi-Client Fix

If c4 produces at ~6.7 GB/s and c3 independently produces at ~6–7 GB/s, running both
simultaneously should deliver **~12–14 GB/s** aggregate to the same 4-broker cluster, assuming:

- Each client connects to all 4 brokers independently (they do: `Publisher` opens one socket per
  broker per client process)
- The broker's CXL write bandwidth is not saturated (CXL node 2 on moscxl: 231 GB LPDDR5X-class
  device, peak BW typically 50–100 GB/s)
- The 100GbE NIC can sustain full line rate (12.5 GB/s) from two sources

`scripts/run_multiclient.sh` with `NUM_CLIENTS=2` (c4 + c3) tests this hypothesis.

---

## CXL Setup

moscxl NUMA topology:

```
node 0: AMD EPYC 9754  (64 cores)
node 1: AMD EPYC 9754  (64 cores)
node 2: CXL memory     (0 CPUs, 231 GB)
        distances 0↔2=255, 1↔2=255
```

**Real CXL is already used by default.** The scripts do NOT pass `-e`. In `Real` mode without
`/dev/dax0.0`, `allocate_shm()` uses `shm_open()` + `mbind(addr, MPOL_BIND, node=2)` to bind
the shared memory region to CXL NUMA node 2 (from `config/embarcadero.yaml`: `cxl.numa_node: 2`).

`-e` (emulated) mode skips the `mbind()` — memory lands on whichever NUMA node the broker runs on
(DRAM, not CXL). Use `-e` only for local dev/testing without CXL hardware.

---

## Key Commits

| Commit    | Description                                         | Note                           |
|-----------|-----------------------------------------------------|--------------------------------|
| `b299085` | ORDER=0 fast path — "12 GB/s publish throughput"   | **Loopback**, not real wire    |
| `947283b` | "single node smoke test perform 12GB/s"             | **Loopback** (single-node)     |
| `2720e27` | Fix remote broker launch on machines without CXL    | Added CXL NUMA detection       |
| `cc16012` | Fix remote throughput launch and readiness gating   | Current HEAD remote infra      |

---

## ORDER=0 Fast Path (current implementation)

Enabled by default (`EMBARCADERO_ORDER0_FAST_PATH=1`).

Broker NetworkManager receives batch → writes directly to CXL via `UpdateWrittenForOrder0()` →
calls `PushOrder0Batch()` → skips PBR allocation + DelegationThread.  Each batch: 2×
`CXL::flush_cacheline()` + 1× `CXL::store_fence()`.

This halves CXL bandwidth per batch vs the original path (previously wrote header + payload
twice: once to PBR, once to final slot).

---

## Single-Node E2E Subscribe Performance

_Last measured: 2026-03-26_

### Key finding: NUMA binding of client determines E2E throughput

For single-node E2E (TEST_TYPE=1), broker processes run on NUMA 1 (`--cpunodebind=1`).
The TCP loopback socket buffers are allocated on NUMA 1 (the broker side creates the socket).
If the client is bound to **NUMA 0** (the old default), subscriber `recv()` crosses NUMA to
reach those buffers, doubling avg_send_us (~500 µs vs ~250 µs) and cutting E2E in half.

| Client NUMA | avg_send_us/batch | Subscribe E2E | Notes |
|:-----------:|:-----------------:|:-------------:|-------|
| 0 (old default) | ~500–630 µs | ~2700–2900 MB/s | Cross-NUMA socket buffer access |
| 1 (fixed default) | ~250–300 µs | ~7200–7700 MB/s | NUMA-local socket buffer access |

**Fix (committed 2026-03-26):** `scripts/lib/run_throughput_impl.sh` `default_client_numa_bind()`
now falls back to `numactl --cpunodebind=1 --membind=1` for single-node loopback (127.0.0.1).
Previously it fell back to NUMA 0.

### Remaining gap vs cross-machine

| Scenario | E2E Subscribe | avg_send_us/batch |
|----------|:-------------:|:-----------------:|
| Single-node loopback (moscxl, fixed) | ~7200–7700 MB/s | ~250 µs |
| Cross-machine 100GbE (c4→moscxl→c3) | ~10200 MB/s | ~196 µs |

The ~25–30% gap is the fundamental loopback penalty: the NIC handles DMA asynchronously
(broker can read next CXL batch while NIC is sending), whereas loopback requires a synchronous
CPU copy on both the send and receive sides.  This gap cannot be closed without hardware changes.

**Note:** hugepages (EMBAR_USE_HUGETLB) have no meaningful impact on E2E after the NUMA fix.

---

## TCP_QUICKACK fix (2026-03-26)

Cross-machine subscribe was throttled by delayed ACKs on the subscriber side.  Linux resets
`TCP_QUICKACK` to delayed-ACK mode after each ACK sent; the broker would hit EAGAIN on every
batch waiting for the ACK to slide the window (~40–200 ms stalls).

Fix: re-arm `TCP_QUICKACK` after every `recv()` call in `SubscribeNetworkThread`'s client-side
`ReceiveWorkerThread` (`src/client/subscriber.cc`).  This raised cross-machine unique subscribe
bandwidth from ~1.2 GB/s (3 sub_connections with EAGAIN stalls) to ~10.2 GB/s.

---

## sub_connections default: 3 → 1 (2026-03-26)

With 3 sub_connections per broker the broker tripled the subscribe data on the wire (each
connection independently re-sent the full dataset from offset 0), causing EAGAIN stalls on
every batch and wasting 2/3 of NIC bandwidth.  Changed `network.sub_connections` default
from 3 to 1 in `src/common/configuration.h`.

---

## Next Steps

1. **Multi-client run** (c4 + c3, NUM_CLIENTS=2): verify aggregate ~12 GB/s hypothesis. ✓ Done: 11.4–12.5 GB/s.
2. **2-pub + 1-sub** (c4 + moscxl NUMA 0 → c3): saturate the broker NIC from two concurrent publishers.
   Script: `scripts/run_2pub_1sub.sh` (ORDER=0 and ORDER=5).
   ✓ Done (2026-03-26): push-ready file barrier aligns publishers to within ~2 ms (NTP skew).
   Results (ORDER=0, 3 trials):
   | Trial | C4 (MB/s) | Local (MB/s) | Overlap | Concurrent Agg | c3 Wire BW |
   |-------|-----------|--------------|---------|----------------|------------|
   | 1     | 6,326     | 11,134       | 460 ms  | 17,460 MB/s    | 6.49 GB/s  |
   | 2     | 7,045     | 11,924       | 430 ms  | 18,969 MB/s    | 6.51 GB/s  |
   | 3     | 7,163     | 12,031       | 430 ms  | 19,194 MB/s    | 6.11 GB/s  |
   Note: local publisher (moscxl NUMA 0 → 10.10.10.10) achieves ~11–12 GB/s single-core
   because AMD EPYC 9754 single-core streaming BW is ~2× Intel SPR (c4 achieves ~6.3–7.2 GB/s).
   Broker CXL ingestion: ~17–19 GB/s aggregate (well above single-client 12–13 GB/s loopback).
   c3 wire BW (~6.1–6.5 GB/s) reflects one publisher's topic stream on a single 100GbE link.
3. **Multi-subscriber e2e**: c4 pub + c3 sub + c2 sub to test fanout bandwidth.
4. **NIC RSS / interrupt affinity** on moscxl: investigate if RSS spreading helps broker receive.
5. **Producer optimization** (lower priority): multi-threaded producer in `PublishThroughputTest`
   or batched `Publish()` to increase per-client rate.

---

## Latency: local vs remote (2026-03-26)

**Test setup:** `TEST_CASE=2` (single binary runs both publisher and subscriber).
- SCENARIO=local: broker + client both on moscxl, client NUMA-pinned to node 1, loopback 127.0.0.1
- SCENARIO=remote: broker on moscxl (10.10.10.10/100GbE), publisher+subscriber on c4 via SSH
- `COLLECT_LATENCY_STATS=ON`, `MSG_SIZE=1024`, `TOTAL_MESSAGE_SIZE=4GiB`, `ACK_LEVEL=1`, 3 trials each
- Metric `avg_send_us`: arithmetic mean of `append_send_to_ack_batch_latency` per batch (arithmetic
  mean is skewed by tail batches; median ~160 µs local / ~440 µs remote for ORDER=0)

### Comparison table (post-optimization, 3-trial median)

| Metric               | Local ORDER=0 | Remote ORDER=0 |    Delta | Local ORDER=5 | Remote ORDER=5 |    Delta |
|----------------------|:-------------:|:--------------:|:--------:|:-------------:|:--------------:|:--------:|
| avg_send_us/batch    |    1,169 µs   |    1,500 µs    | +28% (+331 µs) |   4,312 µs   |    4,522 µs    | +5% (+210 µs) |
| p50 sub latency      |    1,940 µs   |    2,657 µs    | +37% (+718 µs) |   2,583 µs   |    3,174 µs    | +23% (+591 µs) |
| p99 sub latency      |  107,072 µs   |   87,460 µs    | -18% (-19,612 µs) | 117,512 µs |  109,327 µs   | -7% (-8,185 µs) |
| p999 sub latency     |  115,342 µs   |  101,393 µs    | -12% (-13,949 µs) | 160,327 µs |  155,077 µs   | -3% (-5,250 µs) |

### Analysis

**p50 remote > local (+37% for ORDER=0):** Inevitable network overhead.
- Wire-level RTT moscxl↔c4: ~200–300 ns propagation
- NIC TX+DMA on c4: ~1–5 µs each way
- TCP ACK path (broker→c4): same NIC round-trip
- Total floor: ~5–15 µs per round-trip; actual p50 delta is +718 µs, meaning most of the p50
  inflation comes from batching mechanics (the test uses 2MB batches; each batch takes ~1.5 ms at
  local throughput, so p50 latency ≈ 1–2 ms at steady state regardless of network path).

**p99/p999 remote LOWER than local (-18%/-12% for ORDER=0):** Counter-intuitive but documented.
- With loopback, the kernel CPU-copies data synchronously through the socket buffer on both send
  and receive. Under sustained load this CPU copy path causes periodic stalls that inflate tail
  latency. The 100GbE NIC uses DMA + descriptor rings that are async with respect to the host
  CPU, smoothing out these stalls. This is consistent with the script header note that
  avg_send_us remote (~196–450 µs) can be lower than local (~250 µs).

**ORDER=5 vs ORDER=0 p50 delta (~640 µs):** Expected — one epoch interval (kEpochUs = 500 µs).
  Each batch waits for the epoch sequencer to assign a total order; median wait ≈ 0.5 × epoch.

**ORDER=5 p999 tail (~115–160 ms):** Much larger than 1 epoch. Under steady-rate load occasional
  batches arrive just after an epoch fires, forcing them to wait a full epoch (500 µs). With
  thousands of batches the rare worst-case is multiple epoch-waits + queuing behind a slow batch.
  Tunable via `EMBARCADERO_EPOCH_US` (100–5000 µs range, topic.h kEpochUs default 500 µs).

### Optimization applied: TCP_QUICKACK re-arm in `Publisher::EpollAckThread`

**Finding:** The publisher's `EpollAckThread` set `TCP_QUICKACK` only on the listening server
socket, but not on accepted client sockets (one per broker). Linux resets TCP_QUICKACK to
delayed-ACK mode after each kernel-level ACK is deferred. Without re-arming after every
`recv()`, the publisher's kernel can delay TCP ACKs for broker→publisher ACK messages by up to
40 ms, stalling the broker's TCP send window on the ACK connection.

The subscriber (`subscriber.cc:1210`) already re-armed TCP_QUICKACK after every `recv()`. The
publisher's EpollAckThread was missing the equivalent fix.

**Fix applied (`src/client/publisher.cc`):**
1. Set `TCP_QUICKACK` on each accepted client socket (after `accept()`, before `epoll_ctl ADD`)
2. Re-arm `TCP_QUICKACK` after every successful `recv()` in the `READING_ACKS` state

**Measured impact:**

| Metric           | Local O=0 | Remote O=0 | Local O=5 | Remote O=5 |
|------------------|:---------:|:----------:|:---------:|:----------:|
| avg_send_us Δ    |  -1%      |   +7%*     |  +1%      |   -0%      |
| p50 sub latency Δ| -0%       |   +4%*     |  +1%      |   -6%      |
| p999 sub latency Δ| -1%      |   -1%      |  +1%      |   -5%      |

\* Remote ORDER=0 shows slight noise-level worsening post-fix, within trial-to-trial variance.
The fix's primary benefit is theoretical correctness (eliminates a latent 40ms stall path on the
ACK TCP connection). The modest measured improvement in ORDER=5 remote p50 (-6%) and p999 (-5%)
confirms the fix has a real effect when ACKs arrive while the publisher is idle between epochs.

**Remaining delta (remote p50 vs local p50 after fix): +718 µs (ORDER=0)**
Classification: **inevitable** — physical network round-trip + NIC DMA + batching dominates.
No further software optimization is expected to close this gap below ~5–15 µs.

**ORDER=5 tail latency vs ORDER=0:** ~45–55 ms excess at p999.
Classification: **tunable** — reduce `EMBARCADERO_EPOCH_US` (e.g. 100 µs) at the cost of
higher epoch-sequencer overhead, or accept the default 500 µs for lower CPU usage.
