# Bandwidth Diagnostics Runbook

Quick reference for collecting and interpreting diagnostics related to the 10 GB/s → 1 GB/s bandwidth regression.

## 0. Network socket config summary (blocking recv-to-blog path)

**Broker accepted publish sockets** (where blocking `recv()` direct-to-blog runs) — `SetAcceptedSocketBuffers()` in `network_manager.cc`:

| Option       | Value   | Purpose |
|-------------|---------|--------|
| TCP_NODELAY | 1       | Disable Nagle's algorithm (no batching of small segments; low latency). |
| TCP_QUICKACK | 1       | Kernel sends ACKs immediately (reduces client RTT / backoff). |
| SO_RCVBUF   | 32 MB   | Large receive buffer so `rcv_space` is not kernel default (~43 KB). |
| SO_SNDBUF   | 32 MB   | Large send buffer for ACKs. |

**Client data sockets** (publisher → broker) — `GetNonblockingSock()` in `common.cc`:

- TCP_NODELAY, TCP_QUICKACK, SO_SNDBUF (128 MB), SO_RCVBUF, SO_ZEROCOPY (when used).

**Kernel limits:** `SO_RCVBUF`/`SO_SNDBUF` are capped by `net.core.rmem_max`/`wmem_max`. Run `scripts/tune_kernel_buffers.sh` (e.g. 128 MB) so these options take effect.

## 1. Client-side: EAGAIN counters

When the client's `send()` returns `EAGAIN`/`EWOULDBLOCK`/`ENOBUFS`, the kernel TCP send buffer is full (broker not draining fast enough). The client backs off with `epoll_wait(1ms)` and increments `g_send_eagain_count`.

**Where to look:**

- **Pipeline profile (end of run):**  
  `[PublisherPipelineProfile] Send EAGAIN/ENOBUFS count: <N>`  
  If `N` is large (e.g. thousands) relative to send batches, client is frequently backing off → likely contributor to low throughput.

- **Send diagnostic (during run):**  
  `[Send diagnostic] EAGAIN/ENOBUFS count: <N>`  
  Emitted when EAGAIN is first seen.

**How to collect:** Run publisher with `GLOG_minloglevel=0` (INFO) so these lines appear. Example:

```bash
GLOG_minloglevel=0 ./build/src/embarlet --role publisher ...
```

## 2. Broker-side: Ring full and related metrics

CXL ring full events and batch drops indicate backpressure on the broker (e.g. sequencer or consumers not keeping up).

**Where to look:**

- **During run:**  
  `CXLAllocationWorker: Ring full, retry attempt ...` (first 10 and every 1000th).  
  `blocking_ring_full` path: similar log with `total_ring_full=...`.

- **Shutdown summary:**  
  `[BrokerMetrics] ring_full=<N> batches_dropped=<M> cxl_retries=<R> staging_exhausted=<S>`  
  Emitted in `LogPublishPipelineProfile()` when any of these are > 0.

**How to collect:** Run broker with `GLOG_minloglevel=0`. After shutdown, check broker log for `[BrokerMetrics]`.

## 3. Kernel socket buffer tuning

Explicit `SO_RCVBUF`/`SO_SNDBUF` (e.g. 32 MB broker, 128 MB client) are capped by kernel limits. If `net.core.rmem_max`/`wmem_max` are too low, buffers stay small and throughput can drop.

**Script:** `scripts/tune_kernel_buffers.sh`

- Sets `net.core.rmem_max` and `net.core.wmem_max` (default 128 MB).
- Sets `net.ipv4.tcp_rmem` / `tcp_wmem` so new sockets can use larger buffers.
- Run as root (e.g. `sudo ./scripts/tune_kernel_buffers.sh`).
- To use a different size: `EMBARCADERO_KERNEL_BUF_MB=32 sudo ./scripts/tune_kernel_buffers.sh`.

**Verify:** After tuning, `sysctl net.core.rmem_max net.core.wmem_max` should show the intended bytes (e.g. 134217728 for 128 MB).

## 4. EAGAIN backoff experiment (optional)

Current behavior on EAGAIN: `epoll_wait(efd, events, 64, 1)` (1 ms). If EAGAIN count is very high, trying a shorter wait can test whether client backoff is the bottleneck:

- **1 ms (current):** Yields to broker; avoids client busy-loop while broker blocks in `recv()`.
- **0 ms:** Non-blocking poll; no sleep. May improve throughput if broker is fast enough, at the cost of higher CPU. Can worsen ACK progress if broker was CPU-starved.

Change only in controlled experiments; keep 1 ms as default for stability.

## 5. Focus configuration for debugging

Use **ORDER=0, ACK=1, publish-only (TEST_TYPE=5)** to isolate network/buffer behavior from sequencer and ordering. Example:

```bash
# 10GB publish-only, ORDER=0, ACK=1
ORDER=0 ACK=1 TEST_TYPE=5 TEST_SIZE_GB=10 ./test/e2e/test_order0_ack1.sh
```

Then inspect:

- Client log: `[PublisherPipelineProfile]` and EAGAIN count.
- Broker log: `[BrokerMetrics]` and `[PublishPipelineProfile]`.
- Kernel: `scripts/tune_kernel_buffers.sh` applied and `sysctl` values confirmed.

## References

- Expert panel assessment (socket config comparison, root-cause hypotheses): see conversation summary / bandwidth regression notes.
- Recv diagnostics: `docs/DIAGNOSTIC_RUNBOOK_RECV.md`.
- Kernel/send investigation: `docs/SEND_PAYLOAD_EPOLL_AND_RECV_INVESTIGATION.md`.
