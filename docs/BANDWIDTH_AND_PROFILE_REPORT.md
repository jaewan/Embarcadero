# Bandwidth and Publish Pipeline Profile Report

**Date:** 2026-02-01  
**Config:** 4 brokers (1 sequencer-only + 3 data), Order 0, ACK 1, publish-only, 1KB message size.

---

## 1. Bandwidth

### This run (1 GB total)

- **Total size:** 1,073,741,824 bytes (1 GB)
- **Duration:** ~2.27 s
- **Bandwidth:** **450.12 MB/s** (from client log and `data/throughput/pub/result.csv`)

### Historical CSV (10 GB total)

From `data/throughput/pub/result.csv` (10 GB, same config):

| Run   | pub_bandwidth_mbps |
|-------|--------------------|
| 1     | 2738.86            |
| 2     | 2730.86            |
| 3     | 2740.19            |

- **Average (10 GB):** **~2735 MB/s** (~2.67 GB/s)

So:

- **1 GB run:** ~450 MB/s (short run, cold caches, less amortization).
- **10 GB runs:** ~2735 MB/s (steady state, better for comparing changes).

Use 10 GB (or multiple trials) for steady-state bandwidth comparison.

---

## 1b. Investigation: Why Is Bandwidth "Lower"? (Variance and Bottlenecks)

### Variance in the CSV

`result.csv` shows a wide range for 10 GB runs under the same config:

- **~1954–2015 MB/s** (older runs, rows 2–6): likely different config (e.g. staged recv) or machine state.
- **~3218 MB/s** (one run, row 7): outlier; may be no profile, different load, or thermal.
- **~2723–2780 MB/s** (recent runs, direct recv): **current baseline**; in line with ~2735 MB/s reported above.

So "lower" usually means comparison to the 3218 outlier or to expectations; **current direct-recv baseline is ~2735–2755 MB/s**.

### What the profile says

Broker logs print **`[PublishPipelineProfile]`** every 10 s. A typical snapshot:

| Component                  | % of pipeline | Notes |
|---------------------------|---------------|--------|
| **DrainPayloadToBuffer**  | **97–98%**    | Socket `recv()` loop into batch/CXL buffer. |
| DrainHeader               | &lt;1%        | Read batch header. |
| ReserveBLogSpace          | ~1%           | BLog allocation. |
| ReservePBRSlotAndWriteEntry | &lt;1%     | PBR slot + write. |
| CompleteBatchInCXL        | &lt;0.1%      | Flush + fence. |
| GetCXLBuffer / CopyAndFlushPayload | 0% | Not used on direct-to-CXL path. |

**Conclusion:** The **dominant bottleneck is DrainPayloadToBuffer** — i.e. **reading payload from the network** (kernel/TCP + `recv()` syscalls). All other pipeline stages are minor.

### Profile overhead (addressable)

Pipeline profiling is **always on** by default: every batch pays 2× `steady_clock::now()` and 2× `RecordProfile()` (atomic `fetch_add`) in the hot path. Senior review estimated **~50–130 ns per batch** of measurement overhead plus cache-line contention. **Disabling profiling for benchmark runs can recover a few percent.**

- **Note:** Profiling has been **restored to always-on**: timing and `RecordProfile()` run on every batch regardless of config. The config option `enable_publish_pipeline_profile` (if present) is no longer used to gate the hot path.

### Recv bottleneck (partially addressable)

Time in **DrainPayloadToBuffer** is mostly in the **kernel and TCP stack**, not in our loop. The recv path already uses a **loop until EAGAIN** to reduce syscalls per batch.

**Addressable levers:** (1) Turn off pipeline profile when measuring (see above). (2) **Kernel/stack:** Larger `SO_RCVBUF`, interrupt coalescing, NIC offload (e.g. GRO). (3) **Batch size:** Larger batches amortize per-batch recv overhead; current 2 MB is already sizable. (4) **io_uring:** Batched recv could reduce syscall count; requires a dedicated change. (5) **NUMA/CPU:** Pinning and NUMA already used in scripts.

**Not addressable in app logic:** The core cost is **kernel recv + TCP**. Beyond the above, gains require kernel/stack or hardware changes.

### Effect of disabling pipeline profile (measured)

Same 3×10 GB test, **with** vs **without** pipeline profile (`enable_publish_pipeline_profile: true` vs `false`):

| Config              | Trial 1 (MB/s) | Trial 2 (MB/s) | Trial 3 (MB/s) | Mean (MB/s) |
|---------------------|----------------|----------------|----------------|-------------|
| Profile **on** (baseline) | 2780.27       | 2750.30        | 2723.65        | ~2751       |
| Profile **off**     | 2767.00        | 2773.56        | 2738.69        | **2759.75**  |

Difference is **~+9 MB/s mean** (~+0.3%), within normal run-to-run variance. So pipeline profiling overhead is **small** at current throughput; disabling it is optional for peak benchmarks.

---

## 2. Publish Pipeline Profile (Bottlenecks)

Broker logs print an aggregated pipeline profile every **10 s** (`[PublishPipelineProfile]`).  
Typical snapshot (one data broker, ~2.9k batches in 10 s):

| Component                  | total (us) | count | avg (ns) | % of pipeline |
|---------------------------|-----------|-------|----------|----------------|
| DrainHeader               | 14–26k    | ~2.9k | ~5–8k    | **0.47–0.96%** |
| ReserveBLogSpace          | 29–30k    | ~2.9k | ~10k     | **0.93–1.02%** |
| **DrainPayloadToBuffer**  | **2.66–3.07M** | **71–88k** | **30–35k** | **97.7–98.0%** |
| ReservePBRSlotAndWriteEntry | 15–19k  | ~2.9k | ~5–6k    | **0.51–0.71%** |
| CompleteBatchInCXL        | 2.2–2.5k  | ~2.9k | ~750–850 | **0.07–0.09%** |
| GetCXLBuffer              | 0         | 0     | 0        | 0% (happy path) |
| CopyAndFlushPayload       | 0         | 0     | 0        | 0% (happy path) |

- **TOTAL (example):** ~2.72–3.13 s of pipeline time in that 10 s window (multiple threads).
- GetCXLBuffer / CopyAndFlushPayload are 0 on the **direct-to-CXL path** (recv into CXL buffer); they matter only on the staging-buffer path.

### Main bottleneck

- **DrainPayloadToBuffer** accounts for **~97–98%** of profiled pipeline time.
- It is the **socket recv** loop that fills the current batch buffer (or CXL buffer).
- Per-call average ~30–35 µs; call count is large (many recv batches per 10 s).

So the **dominant cost** is **reading payload from the network into the batch/CXL buffer**, not BLog reservation, PBR, or batch completion.

---

## 3. Recommendations

1. **Bandwidth**
   - Use **10 GB** total size and **3+ trials** for stable bandwidth numbers (e.g. ~2735 MB/s for current config).
   - Keep 1 KB message size and Order 0 for publish-only if matching this setup.

2. **Profile**
   - Bottleneck is **DrainPayloadToBuffer** (network recv). Already optimized with **loop drain** (drain until EAGAIN) to reduce syscalls per batch.
   - Further gains likely from:
     - **Kernel/stack:** TCP tuning, NIC offload, interrupt coalescing.
     - **Batch size:** Larger batches (if protocol allows) to amortize recv overhead.
     - **CPU:** Pinning and NUMA (e.g. `numactl`) already used in `run_throughput.sh`.

3. **Other components**
   - DrainHeader, ReserveBLogSpace, ReservePBRSlotAndWriteEntry, CompleteBatchInCXL are all **&lt;1–1%** each; no need to optimize further before tackling recv.

---

## 4. How to reproduce

### Bandwidth: 3 trials × 10 GB from project root (throughput_test directly)

From project root, run the dedicated script. It starts brokers from `build/bin`, runs **throughput_test** from project root (not `run_throughput.sh`), 3 trials × 10 GB each, restarting brokers before each trial:

```bash
cd /home/domin/Embarcadero
bash scripts/run_bandwidth_10gb_3trials.sh
```

- Brokers: started in `build/bin` (head + 3 followers), with readiness wait.
- Client: `build/bin/throughput_test --config config/client.yaml -m 1024 -s 10737418240 --record_results -t 5 -o 0 -a 1 --sequencer EMBARCADERO` from project root.
- Results: printed at end (Trial 1–3 MB/s and mean); also last rows of `data/throughput/pub/result.csv` (column **12** = pub_bandwidth_mbps).

Expect ~2735 MB/s mean for 10 GB trials (see §1).

### One-off run (run_throughput.sh)

```bash
cd /home/domin/Embarcadero
NUM_BROKERS=4 NUM_TRIALS=1 TEST_TYPE=5 ORDER=0 TOTAL_MESSAGE_SIZE=10737418240 \
  timeout 300 bash scripts/run_throughput.sh 2>&1 | tee /tmp/throughput_run.log
```

- Bandwidth: client log "Bandwidth: X.XX MB/s" or last row of `data/throughput/pub/result.csv` (pub_bandwidth_mbps).
- Profile: `grep "[PublishPipelineProfile]" build/bin/broker_*_trial1.log` (after run; logs may be under `data/throughput/logs/`).

---

**Summary:** Steady-state publish bandwidth is **~2735–2755 MB/s** (10 GB, direct recv). Pipeline profile shows **DrainPayloadToBuffer (recv)** at **~97–98%** of pipeline time; other stages are minor. The main bottleneck is **kernel/TCP recv**; it is partially addressable (disable profile for benchmarks, kernel tuning, io_uring, batch size). Further gains beyond that depend on network/stack or hardware.

---

## 5. Profile Summary, Component Details, and Recv Implementation

### 5.1 What profile we ran and the results

**Profile:** The **publish pipeline profile** (`[PublishPipelineProfile]`) runs inside the broker’s **PublishReceiveThread** and **CXLAllocationWorker**. It measures wall-clock time (nanoseconds) per pipeline component per batch and aggregates totals and counts. Every **10 seconds** one thread logs the aggregated snapshot; on shutdown the destructor logs once more.

**What we ran:** 3 trials × 10 GB publish-only (Order 0, ACK 1, 1 KB messages, 4 brokers, direct recv). Bandwidth was measured with profile **on** and **off**:

| Config        | Trial 1 (MB/s) | Trial 2 (MB/s) | Trial 3 (MB/s) | Mean (MB/s) |
|---------------|----------------|----------------|----------------|-------------|
| Profile **on**  | 2780.27        | 2750.30        | 2723.65        | ~2751       |
| Profile **off** | 2767.00        | 2773.56        | 2738.69        | 2759.75     |

Difference with profile off is ~+9 MB/s mean (~0.3%), within run-to-run variance.

**Typical profile snapshot (one data broker, ~10 s window):**

| Component                  | total (µs)   | count   | avg (ns) | % of pipeline |
|---------------------------|-------------|--------|----------|----------------|
| DrainHeader               | 14–26k      | ~2.9k  | ~5–8k    | 0.47–0.96%    |
| ReserveBLogSpace          | 29–30k      | ~2.9k  | ~10k     | 0.93–1.02%    |
| **DrainPayloadToBuffer**  | **2.66–3.07M** | **71–88k** | **30–35k** | **97.7–98.0%** |
| ReservePBRSlotAndWriteEntry | 15–19k   | ~2.9k  | ~5–6k    | 0.51–0.71%    |
| CompleteBatchInCXL        | 2.2–2.5k    | ~2.9k  | ~750–850 | 0.07–0.09%    |
| GetCXLBuffer              | 0           | 0      | 0        | 0% (direct path) |
| CopyAndFlushPayload       | 0           | 0      | 0        | 0% (direct path) |

---

### 5.2 What each component is (in detail)

- **DrainHeader**  
  Reads the **batch header** (64-byte `BatchHeader`: batch_seq, total_size, num_msg, etc.) from the TCP socket into `ConnectionState::batch_header`. One or more non-blocking `recv(..., MSG_DONTWAIT)` calls until the full header is received or `EAGAIN`. Typically completes in one or two recvs. **~0.5–1%** of pipeline time.

- **ReserveBLogSpace**  
  Reserves space in the **BLog** (broker log in CXL) for this batch’s payload. On the direct-to-CXL path this is a `log_addr_.fetch_add(total_size)` (or equivalent) plus segment-boundary check; no CXL read on the happy path. **~1%** of pipeline time.

- **DrainPayloadToBuffer**  
  The **socket recv** loop that fills the batch payload. Destination is either a staging buffer (staging path) or the **CXL buffer** (direct path). Calls `recv(fd, dest + payload_offset, remaining, MSG_DONTWAIT)` in a **loop until EAGAIN or complete** so that one epoll wakeup can drain as much as the kernel has ready (~20 recvs per batch in earlier profiles). This is where almost all pipeline time is spent; the cost is **kernel/TCP** (copy from kernel socket buffer into user/CXL memory). **~97–98%** of pipeline time.

- **ReservePBRSlotAndWriteEntry**  
  Allocates a slot in the **PBR** (per-broker batch header ring in CXL), writes the batch metadata (e.g. pointer to payload, num_msg) into that slot, and returns the `BatchHeader*` for completion. On the happy path uses lock-free 128-bit CAS; no CXL read. **~0.5–0.7%** of pipeline time.

- **CompleteBatchInCXL**  
  Marks the batch visible to the sequencer: sets `num_msg` and `batch_complete` in the PBR slot, flushes both cache lines of the 128-byte `BatchHeader`, optional Order0 `written` update, then `store_fence()`. **~0.07–0.09%** of pipeline time.

- **GetCXLBuffer**  
  Used only on the **staging path**: CXLAllocationWorker calls it to get a CXL buffer and optional segment/logical-offset for a batch that was drained into a staging buffer. On the **direct-to-CXL path** the recv thread already has the CXL buffer from ReserveBLogSpace; this component is **not** used, so **0%** in the profile.

- **CopyAndFlushPayload**  
  Used only on the **staging path**: copies payload from staging buffer to CXL in 256 KB chunks and flushes each chunk’s cache lines. On the **direct-to-CXL path** payload is received directly into CXL, so this component is **not** used, **0%** in the profile.

---

### 5.3 How kernel/TCP recv is implemented in the codebase

Recv is implemented as **non-blocking TCP** plus **epoll** and a **drain-until-EAGAIN** loop. No custom kernel code; it uses the standard Linux socket API.

**1. Socket and epoll setup**

- **MainThread** creates the server socket, sets `SO_REUSEADDR`, `TCP_NODELAY`, and (in `ConfigureNonBlockingSocket`) **non-blocking** via `fcntl(O_NONBLOCK)`, **TCP_QUICKACK**, and **SO_SNDBUF/SO_RCVBUF** (e.g. 32 MB). Accepts connections and pushes them to `publish_connection_queue_`.
- **PublishReceiveThread** creates an **epoll** instance (`epoll_create1(0)`), adds each new publish socket with **EPOLLIN | EPOLLET** (edge-triggered). Main loop calls **`epoll_wait(epoll_fd, events, 64, 0)`** (timeout 0 = non-blocking; returns immediately with up to 64 ready fds).

**2. Per-connection state machine**

Each connection has a **phase**: `WAIT_HEADER` → (if direct CXL) reserve BLog → `WAIT_PAYLOAD` → drain payload → reserve PBR slot → complete batch → back to `WAIT_HEADER`. Header and payload are received in separate steps.

**3. DrainHeader (header recv)**

- **Location:** `NetworkManager::DrainHeader()` in `network_manager.cc`.
- **API:** `recv(fd, reinterpret_cast<uint8_t*>(&state->batch_header) + state->header_offset, remaining, MSG_DONTWAIT)`.
- **Semantics:** One or more recvs until `sizeof(BatchHeader)` bytes are in `state->batch_header`, or `EAGAIN`/error. `remaining = sizeof(BatchHeader) - state->header_offset`; partial reads advance `header_offset`. No loop here; one call per epoll wakeup is enough for the small header.

**4. DrainPayloadToBuffer (payload recv — hot path)**

- **Location:** `NetworkManager::DrainPayloadToBuffer()` in `network_manager.cc`.
- **API:** `recv(fd, dest + state->payload_offset, remaining, MSG_DONTWAIT)` inside a **while (payload_offset < total_size)** loop.
- **Semantics:**  
  - `dest` is either staging buffer or **CXL buffer** (direct path).  
  - Each call requests up to `remaining = total_size - payload_offset` (can be multi-MB).  
  - Kernel returns as many bytes as are currently available (often a full TCP segment or several); if nothing is ready, returns -1 with `errno == EAGAIN`.  
  - Loop continues until either `payload_offset == total_size` (batch complete) or `EAGAIN` (return false; same socket will be woken again by epoll when more data arrives).  
  - So **kernel/TCP recv** = copy from kernel socket receive buffer into user (or CXL-mapped) memory; the profile measures this whole loop (many recv syscalls per batch).

**5. Staging path (DrainPayload)**

- **DrainPayload** is used when **not** direct-to-CXL: one recv per call (no inner loop), into `state->staging_buf`. When complete, the batch is handed to **CXLAllocationWorker**, which calls **GetCXLBuffer** and **CopyAndFlushPayload** (those show up in the profile only on the staging path).

**6. Summary**

- **Kernel/TCP recv** in this codebase = **`recv(..., MSG_DONTWAIT)`** on non-blocking TCP sockets, with **epoll** for readiness and a **drain-until-EAGAIN** loop in `DrainPayloadToBuffer` to minimize epoll round-trips per batch. No custom kernel or TCP code; all recv cost is in the kernel’s TCP stack and copy into the destination buffer (user or CXL).

---

## 6. Run and parsed profile (Order 0, ACK 1, test 5, 10 GB, 1 KB)

### 6.1 Run config and bandwidth (this run)

**Config:** Order 0, ACK 1, test type 5 (publish-only), total message size **10 GB**, payload **1 KB**, 4 brokers (1 sequencer + 3 data), direct recv, profiling on.

**Command (from project root):**
```bash
./scripts/run_bandwidth_10gb_3trials.sh
```
- Brokers: `build/bin/embarlet` with `config/embarcadero.yaml` (followers) and `embarcadero_sequencer_only.yaml` (head).
- Client: `throughput_test --config config/client.yaml -m 1024 -s 10737418240 --record_results -t 5 -o 0 -a 1 --sequencer EMBARCADERO`.

**Bandwidth results (this run):**

| Trial | pub_bandwidth_mbps |
|-------|--------------------|
| 1     | 2721.04            |
| 2     | 2725.55            |
| 3     | 2735.09            |
| **Mean** | **2727.23**     |

### 6.2 Profile snapshot timing

The pipeline profile logs **every 10 s** (`[PublishPipelineProfile]`). Each trial is ~3.7 s, so the 10 s snapshot does **not** fire during a single trial. Parsed profile below is from **existing** broker logs (`build/bin/broker_*_trial2.log`, `broker_*_trial3.log`) from the same config (Order 0, ACK 1, publish-only, 10 GB, 1 KB). To capture profile in the same run, use a single trial with total size ≥ ~30 GB so the test lasts >10 s, or inspect logs from a run that keeps brokers up >10 s.

### 6.3 Parsed profile results

Parsed from `[PublishPipelineProfile] === Aggregated time per component ===` and the following component lines (one 10 s window per broker). **total** = µs, **count** = number of calls, **avg** = ns per call, **%** = share of pipeline time.

**Broker 1 (trial 3, 10 s window):**

| Component                  | total (µs) | count  | avg (ns) | % of pipeline |
|---------------------------|------------|--------|----------|----------------|
| DrainHeader               | 14,622     | 2,886  | 5,066    | 0.47%          |
| ReserveBLogSpace          | 29,173     | 2,886  | 10,108   | 0.93%          |
| **DrainPayloadToBuffer**  | **3,063,500** | **86,698** | **35,335** | **98.01%** |
| ReservePBRSlotAndWriteEntry | 15,845   | 2,886  | 5,490    | 0.51%          |
| CompleteBatchInCXL        | 2,444      | 2,886  | 847      | 0.08%          |
| GetCXLBuffer              | 0          | 0      | 0        | 0%             |
| CopyAndFlushPayload        | 0          | 0      | 0        | 0%             |
| **TOTAL**                 | **3,125,680** | —   | —        | 100%           |

**Broker 2 (trial 3, 10 s window):**

| Component                  | total (µs) | count  | avg (ns) | % of pipeline |
|---------------------------|------------|--------|----------|----------------|
| DrainHeader               | 14,653     | 2,881  | 5,086    | 0.47%          |
| ReserveBLogSpace          | 28,824     | 2,881  | 10,004   | 0.92%          |
| **DrainPayloadToBuffer**  | **3,070,436** | **87,132** | **35,238** | **98.02%** |
| ReservePBRSlotAndWriteEntry | 16,191   | 2,881  | 5,620    | 0.52%          |
| CompleteBatchInCXL        | 2,210      | 2,881  | 767      | 0.07%          |
| GetCXLBuffer              | 0          | 0      | 0        | 0%             |
| CopyAndFlushPayload        | 0          | 0      | 0        | 0%             |
| **TOTAL**                 | **3,132,397** | —   | —        | 100%           |

**Broker 3 (trial 3, 10 s window):**

| Component                  | total (µs) | count  | avg (ns) | % of pipeline |
|---------------------------|------------|--------|----------|----------------|
| DrainHeader               | 15,512     | 2,905  | 5,340    | 0.53%          |
| ReserveBLogSpace          | 29,444     | 2,905  | 10,135   | 1.00%          |
| **DrainPayloadToBuffer**  | **2,873,003** | **88,116** | **32,604** | **97.80%** |
| ReservePBRSlotAndWriteEntry | 17,255   | 2,905  | 5,939    | 0.59%          |
| CompleteBatchInCXL        | 2,191      | 2,905  | 754      | 0.07%          |
| GetCXLBuffer              | 0          | 0      | 0        | 0%             |
| CopyAndFlushPayload        | 0          | 0      | 0        | 0%             |
| **TOTAL**                 | **2,937,489** | —   | —        | 100%           |

**Summary (averaged across brokers 1–3, trial 3):**

- **DrainPayloadToBuffer:** ~97.8–98.0% of pipeline time; ~86k–88k calls in 10 s; ~32–35 µs avg per call. Dominant cost is **kernel/TCP recv**.
- **ReserveBLogSpace:** ~0.9–1.0%; ~2.9k calls; ~10 µs avg.
- **DrainHeader:** ~0.47–0.53%; ~2.9k calls; ~5–5.3 µs avg.
- **ReservePBRSlotAndWriteEntry:** ~0.51–0.59%; ~2.9k calls; ~5.5–6 µs avg.
- **CompleteBatchInCXL:** ~0.07–0.08%; ~2.9k calls; ~750–850 ns avg.
- **GetCXLBuffer / CopyAndFlushPayload:** 0% on direct-to-CXL path.

---

## 7. Publisher-side profile (buffer write + send)

To measure client-side bottlenecks (buffer management and send), enable the **publisher pipeline profile**:

- **Config:** `client.performance.enable_publisher_pipeline_profile: true` in `config/client.yaml` (or `EMBARCADERO_ENABLE_PUBLISHER_PIPELINE_PROFILE=1`).
- **Default:** `false` so there is no overhead when disabled.

**Components:**

1. **BufferWrite** — Time in `Buffer::Write()`: writing one message (header + payload) into the current buffer, tail update, and optional `Seal()` when the batch reaches `BATCH_SIZE`. This is the **buffer management** cost on the writer thread(s) that call `Publish()`.
2. **SendPayload** — Time in `PublishThread` to send one batch’s payload (the `while (sent_bytes < len)` loop with `send()`). Measures **actual send** cost (kernel/TCP) from the client.

**Output:** At **shutdown** (when the `Publisher` is destroyed), the client calls `LogPublisherProfile()` and logs `[PublisherPipelineProfile]` with aggregated total µs, count, avg ns, and % per component. No periodic logging in the hot path.

**Overhead when enabled:** One `steady_clock::now()` at entry and exit of `Buffer::Write()` and around the send loop, plus two atomic `fetch_add` per recorded sample. Keep disabled for peak bandwidth runs; enable to compare buffer vs send cost on the client.

### 7.1 Parsed publisher profile (run with profile at shutdown)

From a run with `enable_publisher_pipeline_profile: true` and `LogPublisherProfile()` called in `~Publisher()`:

| Component   | total (µs) | count | avg (ns) | % of pipeline |
|------------|------------|-------|----------|----------------|
| BufferWrite | 0         | 0     | 0        | 0%             |
| **SendPayload** | **11,005,624–11,608,731** | **4991** | **2,205,094–2,325,933** | **100%** |
| **TOTAL**  | **~11.0–11.6 s** | — | — | 100% |

**Interpretation:** On the client, the only component with samples in that run was **SendPayload** (the `send()` loop in `PublishThread`). So in the measured path, **sending payload to the broker (kernel/TCP send)** accounts for 100% of the publisher pipeline time; buffer management (BufferWrite) had no samples in that run because BufferWrite profiling was not yet wired in `Buffer::Write()` at the time. As of the latest change, `Buffer::Write()` (BATCH_OPTIMIZATION path) now records `kPublisherBufferWrite`; a future run with the same config will show both BufferWrite and SendPayload.

**Summary:** Client-side bottleneck in the profiled path is **SendPayload** (TCP send). BufferWrite is now instrumented for comparison on the next run.
