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
