# Senior Engineer: Methods to Optimize recv() Time

**Context:** DrainPayloadToBuffer (the recv loop on the broker) accounts for **97–98%** of broker pipeline time. Time is spent in **kernel/TCP** (copy from kernel socket buffer to user/CXL memory) and in **syscall overhead** (each `recv()` is a syscall). Below are methods ordered by impact and feasibility.

---

## 1. Already in place

- **Loop until EAGAIN:** We drain in a loop instead of one recv per epoll wakeup (~20 recv/batch → fewer epoll round-trips). Keep this.
- **Direct-to-CXL:** recv into CXL buffer; no extra copy to staging then to CXL.
- **SO_RCVBUF 32 MB + kernel tune:** `scripts/tune_kernel_buffers.sh` (rmem_max/wmem_max) so the requested 32 MB takes effect. Ensure it is applied on broker (and client) hosts.
- **TCP_NODELAY, TCP_QUICKACK:** Set on accepted sockets.
- **NUMA:** Scripts use `numactl` for broker; keep recv threads and CXL on nodes that minimize NIC ↔ CPU ↔ CXL latency.

---

## 2. High impact, reasonable effort

### 2.1 io_uring for batched recv

**Idea:** Replace many `recv()` syscalls per batch with one or a few io_uring submissions; reap completions in batch. Cuts syscall count and can improve cache behavior.

**How:** Use `IORING_OP_RECV` (or `IORING_OP_RECV_MULTISHOT` on newer kernels). For each connection/batch you still need to recv into one contiguous buffer (our CXL region); io_uring doesn’t change that. The win is **batched submission + completion**, so instead of 20+ `recv()` syscalls per 2 MB batch you do e.g. 1–2 `io_uring_enter()` calls that drive multiple recvs internally.

**Caveats:**  
- Need a recv path that works with io_uring (non-blocking, buffer ownership, connection state).  
- Provided buffers (`IORING_REGISTER_PBUF_RING`) can reduce allocator pressure; optional.  
- Kernel 5.15+ for solid multishot recv.

**Effort:** Medium–high (new code path, testing, fallback to current recv on older kernels or on error).

**Impact:** Can cut syscall count by 10–20× per batch; often 10–30% throughput gain in recv-heavy workloads.

---

### 2.2 Kernel / NIC: GRO and interrupt coalescing

**GRO (Generic Receive Offload):** Kernel coalesces small TCP segments into fewer, larger “super-packets” before they hit the socket. Fewer segments → fewer wakeups and sometimes larger chunks per recv.

**How:** Usually enabled by default. Check `ethtool -k <iface> | grep generic-receive-offload`. If disabled, enable (often not needed). For virtual/loopback, GRO may not apply.

**Interrupt coalescing:** Reduce number of interrupts per second so each interrupt delivers more data; fewer context switches.

**How:** `ethtool -C <iface>` (e.g. increase `rx-usecs` or use adaptive). Tune so latency is still acceptable; aggressive coalescing can add latency.

**Effort:** Low (tuning, no code change).  
**Impact:** Low–medium; helps when the bottleneck is “too many small segments” or “too many interrupts.”

---

### 2.3 Measure recv calls per batch

**Idea:** Add a lightweight counter: how many `recv()` calls per completed batch (per connection or global). Log or expose as a metric.

**Why:** If you still see 15–25 recv calls per 2 MB batch, the kernel is returning small chunks (e.g. ~84–200 KB). That confirms SO_RCVBUF/kernel limits or TCP window as the cause and justifies io_uring and kernel tuning. If recv count is already low (e.g. 1–3 per batch), the cost is more in copy/CPU than in syscall count.

**Effort:** Low (one counter in `DrainPayloadToBuffer`, log or export).  
**Impact:** Informs where to optimize next.

---

## 3. Medium impact, medium effort

### 3.1 SO_BUSY_POLL / busy-polling

**Idea:** Reduce wakeup latency when data arrives by busy-polling the device queue for a short time instead of sleeping in epoll.

**How:** `setsockopt(SO_BUSY_POLL, usec)` and optionally `SO_INCOMING_CPU`; tune `net.core.busy_poll`, `net.core.busy_read`. Wakes faster but uses more CPU.

**Use when:** You need lower latency and can afford extra CPU; less about raw throughput.

**Effort:** Low (socket option + sysctl).  
**Impact:** Latency; throughput gain possible but not guaranteed.

---

### 3.2 Recv thread and buffer NUMA affinity

**Idea:** Pin each PublishReceiveThread to a core (and optionally its CXL buffers) on the same NUMA node as the NIC. Reduces cross-node traffic and can improve memory bandwidth for the recv→CXL path.

**How:** Use `numactl` or `pthread_setaffinity_np` for recv threads; ensure CXL buffers are allocated from the right node (you may already do this). If the NIC is on node 0 and CXL on node 2, pin recv threads to a node that minimizes hop cost (e.g. node 0 for NIC, then copy or DMA to CXL; or measure which pinning is best).

**Effort:** Low–medium (affinity + measuring).  
**Impact:** Medium when cross-node traffic is significant.

---

### 3.3 More recv threads

**Idea:** You have `num_publish_receive_threads`; if recv is CPU-bound (many connections, many epoll events), more threads can drain more connections in parallel.

**Caveat:** If the bottleneck is kernel/TCP or a single shared resource (e.g. one NIC queue), more threads may not help and can add contention. Measure before/after.

**Effort:** Low (config + test).  
**Impact:** Low–medium; only if recv is actually CPU-bound and under-threaded.

---

## 4. Lower impact or higher effort

### 4.1 Larger batch size

**Idea:** Amortize per-batch overhead (header parse, BLog reserve, PBR, completion) over more bytes. Protocol and buffer layout must allow it (e.g. 4 MB instead of 2 MB per batch).

**Effort:** Protocol/config change; may increase latency and memory.  
**Impact:** A few percent if recv itself isn’t the limiter; small if the cost is per-byte copy in kernel.

---

### 4.2 Zero-copy recv (kernel support)

**Idea:** Avoid copying from kernel socket buffer to user space (e.g. DMA directly into CXL-backed memory). Linux has no mainstream `MSG_ZEROCOPY`-style recv; options are limited (e.g. io_uring with provided buffers + kernel support, or specialist NIC/driver features).

**Effort:** High; may require kernel/driver work or different NIC.  
**Impact:** Theoretically large; in practice only if you adopt a specific kernel/io_uring/NIC path.

---

### 4.3 recv into huge pages

**Idea:** CXL buffer (or staging buffer) in huge pages so TLB and possibly DMA are friendlier.

**How:** Allocate CXL/staging from huge-page pool if not already.  
**Effort:** Low if allocation path is under your control.  
**Impact:** Usually small for recv alone; can help overall memory throughput.

---

## 5. Recommended order

1. **Ensure kernel buffer tuning** (rmem_max, etc.) is applied and **verify** with getsockopt (already implemented). Re-measure throughput.
2. **Add recv-calls-per-batch metric**; log or export. Interpret: many calls → syscall/segment bottleneck (io_uring, GRO, coalescing); few calls → copy/bandwidth bottleneck (NUMA, huge pages, kernel).
3. **Try GRO and interrupt coalescing** (ethtool); low risk, quick to test.
4. **Evaluate io_uring recv path** for the broker publish path; highest potential gain for recv-heavy, multi-connection workload.
5. **Tune NUMA/affinity** for recv threads and CXL buffers; then consider SO_BUSY_POLL or more recv threads only if metrics show CPU or latency as the limiter.

---

## 6. Summary

- **Biggest levers:** io_uring (batched recv), kernel/NIC tuning (SO_RCVBUF applied, GRO, coalescing), and measuring recv calls per batch.
- **Already done:** Loop-until-EAGAIN, direct-to-CXL, 32 MB SO_RCVBUF + tune script, TCP options, NUMA in scripts.
- **Next steps:** Apply kernel tune everywhere, add recv-per-batch counter, then pilot io_uring recv and GRO/coalescing; use metrics to decide NUMA and thread count.
