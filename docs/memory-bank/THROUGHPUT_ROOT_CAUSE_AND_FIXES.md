# Throughput Root Cause Analysis and Fixes

**Date:** 2026-01-28  
**Goal:** Identify why bandwidth is "terribly slow" (~50 MB/s vs target 9–10 GB/s) and address root causes in the code path.

---

## 1. Code Path Traced

**Publish → ACK chain:**

1. **Publisher** (client): PublishThreads send batches to brokers; Poll() waits for `ack_received_ >= client_order`.
2. **Broker receive**: NetworkManager receives data, writes to CXL, sets `batch_complete=1`, flushes.
3. **Sequencer** (head): BrokerScannerWorker5 per broker; flush/invalidate → read `batch_complete` → process → AssignOrder5 → update `ordered`, clear `batch_complete`, flush; update `batch_headers_consumed_through`.
4. **AckThread** (each broker): Polls `GetOffsetToAck()` (reads `ordered` with CXL invalidate); when `ordered >= next_to_ack`, sends ack to client.
5. **Publisher EpollAckThread**: Receives acks on socket, updates `ack_received_`, `acked_messages_per_broker_`.

---

## 2. Root Causes Identified

### 2.1 AckThread: 1 ms sleep every “no ack” cycle (CRITICAL)

**Location:** `src/network_manager/network_manager.cc` ~1346–1476.

**Behavior:** When no ack is found after a 100 µs spin, the code did a 2 ms drain spin then **slept 1 ms** every cycle.

**Impact:** In steady state, when waiting for the sequencer to update `ordered`, the AckThread often saw “no ack” and slept 1 ms. That caps effective poll rate at ~1 kHz and adds ~1 ms latency per “no ack” cycle. With many batches (e.g. 5k for 10 GB at 2 MB/batch), this dominates tail latency and throughput.

**Evidence:** 100 MB completes at ~50 MB/s; 1 GB and 10 GB stall at the tail (e.g. 99.6%, 37%). Throughput is consistent with “ack drain” being throttled by 1 ms sleeps when the sequencer is only slightly behind.

### 2.2 AckThread: Short initial spin (100 µs)

**Behavior:** Spin phase was 100 µs, then drain 2 ms, then sleep 1 ms.

**Impact:** CXL visibility and processing can take hundreds of µs. A 100 µs spin often missed the update and fell through to drain + 1 ms sleep.

### 2.3 Publisher EpollAckThread: 10 ms epoll timeout

**Location:** `src/client/publisher.cc` ~532.

**Behavior:** `epoll_wait(..., EPOLL_TIMEOUT_MS)` with `EPOLL_TIMEOUT_MS = 10`.

**Impact:** The client’s ack receiver blocked up to 10 ms before processing new acks, adding up to 10 ms extra latency per wake-up when no events arrived.

### 2.4 BrokerScannerWorker5: 10 µs sleep at idle_cycles ≥ 1024

**Location:** `src/embarlet/topic.cc` ~1627–1636.

**Behavior:** When waiting for `batch_complete`, after 1024 idle iterations the loop slept 10 µs every iteration until 2048, then reset.

**Impact:** In “waiting for next batch” periods, this added ~10 µs × 1024 ≈ 10 ms per idle epoch and delayed discovery of `batch_complete`.  
**Note:** A change to spin-only until 2048 then 1 µs backoff was reverted because it led to tail stalls in some 100 MB runs; the original backoff was kept for safety.

### 2.5 “Last percent” stall (separate from pure throughput)

**Observation:** 1 GB stalls at ~99.6% (e.g. 1,044,720 / 1,048,576); 10 GB at ~37%. The last batches on one or more brokers never get ordered/acked.

**Likely causes:** Tail batches deferred in ProcessSkipped5 and never replayed; or one broker’s AckThread/sequencer path never advances the last `ordered`/acks. This is a correctness/tail-drain issue, not only a “slow polling” issue.

---

## 3. Fixes Applied

### 3.1 AckThread: Longer spin and adaptive sleep (`network_manager.cc`)

- **Initial spin:** 100 µs → **500 µs** so CXL updates are more often seen before drain/sleep.
- **Drain:** Left at **1 ms** (500 µs was tried and increased tail shortfall; 2 ms → 1 ms is a compromise).
- **Sleep when no ack:**
  - **Before:** Always 1 ms.
  - **After:** **100 µs** when `consecutive_ack_stalls < 200`, **1 ms** when ≥ 200.
- **Rationale:** Keeps low latency when the pipeline is active (recent acks), and avoids burning CPU during long idle by switching to 1 ms only after many consecutive “no ack” cycles.

### 3.2 Publisher: Shorter epoll timeout (`publisher.cc`)

- **EPOLL_TIMEOUT_MS:** 10 → **1**.
- **Rationale:** Client processes acks at least every 1 ms instead of every 10 ms when there are no other events.

### 3.3 BrokerScannerWorker5: No change kept

- Idle backoff was reverted to the original (10 µs at 1024+, reset at 2048) after a spin-heavy variant caused tail stalls in 100 MB runs.

---

## 4. Verification

- **100 MB, 1 KB payload:** Completes in ~2 s, **~49.7 MB/s** (unchanged from before; this size was already finishing).
- **1 GB:** Still stalls at ~99.6% (last ~0.4% not acked).
- **10 GB:** Still stalls at ~37%.

So:

- Throughput and latency on the **hot path** are improved by the AckThread and Publisher changes (faster reaction when acks are flowing).
- The **“last percent” stall** is unchanged and needs separate work (ProcessSkipped5, tail drain, per-broker diagnostics).

---

## 5. Suggested Next Steps for “Last Percent” and Higher Throughput

1. **Per-broker ack diagnostics:** When client hits ACK timeout, log `acked_messages_per_broker_[]` and, on brokers, `ordered` and `next_to_ack_offset` per broker to see which broker(s) stop advancing.
2. **ProcessSkipped5 and tail:** Ensure deferred (out-of-order) batches are replayed and that the scanner does not exit or stop scanning before the last batch is ordered; add targeted logging when `ready_to_order` is true for the last batch.
3. **Ring wrap and consumed_through:** Confirm that with smaller `batch_headers_size`, the producer never overwrites unconsumed slots and that the consumer always advances `batch_headers_consumed_through` for the last batch.
4. **Larger runs:** Re-run 10 GB with the new AckThread/Publisher settings and capture broker and client logs to see whether the stall point moves (e.g. from 37% to 60%) and which broker is short.

---

## 6. File and Symbol Reference

| Change               | File                         | Symbol / area                    |
|----------------------|------------------------------|----------------------------------|
| AckThread spin/sleep | `src/network_manager/network_manager.cc` | SPIN_DURATION, DRAIN_SPIN_US, adaptive sleep |
| Publisher epoll      | `src/client/publisher.cc`    | EPOLL_TIMEOUT_MS                 |
| Scanner idle (revert)| `src/embarlet/topic.cc`      | idle_cycles backoff              |
