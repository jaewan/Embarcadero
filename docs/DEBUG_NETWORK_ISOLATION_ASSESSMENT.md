# Assessment: Debug Mode to Isolate Network Throughput

**Goal:** Isolate network path by removing publisher and queue-buffer effects. Feed network threads at theoretical maximum: single pre-allocated buffer per thread, only update batch header each send, reuse same payload; all threads start sending atomically when a "go" flag is set.

**Verdict: Feasible.** The design is implementable with a dedicated debug path in the publisher and a small test hook. Below is a concise design and what to watch for.

---

## 1. Summary of the idea

- **Single DEBUG buffer per send thread:** One buffer of size `sizeof(BatchHeader) + batch_payload_size` (e.g. 2 MB) per PublishThread, allocated at init and reused for every send.
- **Header-only update:** Each send, the thread only overwrites the `BatchHeader` at the start of its buffer (client_id, broker_id, batch_seq, total_size, num_msg, etc.); payload bytes are unchanged (zeros or dummy).
- **Remove queue and producer:** PublishThreads do not read from QueueBuffer; they loop: update header → send header → send payload, for a fixed number of batches (or until a time limit).
- **Atomic start:** All PublishThreads spin on an atomic `go` flag. When the test (or first publish) sets `go`, every thread starts sending in the same window so the measured period reflects “all threads sending at once” (theoretical max load on the network).

This gives a **network-only** micro-benchmark: no queue contention, no buffer allocation in the loop, no real payload preparation—only header update + send.

---

## 2. Feasibility

**BatchHeader and wire format:**  
`BatchHeader` is 128 B (`cxl_datastructure.h`): batch_seq, client_id, num_msg, batch_complete, broker_id, ordered, total_size, start_logical_offset, log_idx, total_order, etc. The broker expects `recv(header)` then `recv(payload)` (or a single recv of header+payload). So:

- Each thread can maintain its own buffer: `[BatchHeader][payload]`.
- Each send: write only the `BatchHeader` at the start of that buffer, then send the whole buffer (or send header then payload in two steps to match current code).  
**Conclusion:** Header-only update + reuse of one buffer per thread is valid.

**Uniqueness of batch_seq:**  
Brokers use client_id and batch_seq (and broker_id) for ordering/ACK. Each PublishThread must use a monotonic batch_seq that does not collide with other threads. Current logic uses a per-thread notion (e.g. `batch_seq = pubQuesIdx` then increment by number of threads). In debug mode we can do:

- Thread `i` (0 ≤ i < num_publish_threads):  
  `batch_seq = thread_base_seq[i] + num_publish_threads * batch_count`  
  with `thread_base_seq[i] = i` (or a client-wide base + i). So each thread has a unique, monotonic sequence.  
**Conclusion:** Batch header update logic can be made correct so that “everyone works as buffer queue version” from the broker’s perspective.

**Atomic “go” and timer:**  
- Option A: Test/main sets `go = true` and `start_time = now()` when ready; all PublishThreads `while (!go.load()) {}` then enter the send loop.  
- Option B: First thread to enter the send path does `if (!go.exchange(true)) start_time = now();` and all threads spin on `go` then send.  

Either way we need a **barrier** so all threads are already spinning on `go` before it is set; otherwise the first thread might finish before others start. So:

1. All PublishThreads reach a “ready” point (e.g. barrier or atomic count).
2. Then test sets `go` (and optionally `start_time`), or first thread sets them.
3. All threads exit the spin and run the send loop.

**Conclusion:** Atomic go + barrier is standard and feasible.

**ORDER=0 vs ORDER=5:**  
For ORDER=0 (EMBARCADERO, no Corfu), the publisher does not call `GetTotalOrder`; it only sets client_id, broker_id, total_size, num_msg, batch_seq (and any other required header fields). So the debug path can be ORDER=0-only initially: same header fields as today, no sequencer RPC in the loop. For ORDER=5 we’d need to either skip this mode or add sequencer interaction (weaker isolation).  
**Conclusion:** Implement for ORDER=0 first; network isolation is clean.

---

## 3. Suggested design (minimal)

**Config / flag:**  
- Runtime: e.g. `EMBARCADERO_DEBUG_NETWORK_ISOLATION=1` or a config key `client.debug_network_isolation`.  
- When set, the publish test (e.g. test type 5) and the publisher use the isolated path below.

**Publisher side:**

1. **Per-thread DEBUG buffer**  
   - One buffer per PublishThread: size = `sizeof(BatchHeader) + batch_payload_size` (e.g. 2 MB from config).  
   - Allocated at thread start (or when entering the isolation path), never freed in the loop.  
   - Payload region can be zeroed once or left as-is; only the first `sizeof(BatchHeader)` bytes are updated each send.

2. **Barrier + go flag**  
   - Shared: `std::atomic<bool> go_{false};` and optionally `std::atomic<uint64_t> start_time_ns_{0};` (or a shared `std::chrono::time_point`).  
   - Barrier: e.g. `std::atomic<size_t> ready_count_{0};` — each thread does `ready_count_.fetch_add(1)` then `while (ready_count_.load() != num_threads) {}` (or use a proper barrier).  
   - Then each thread spins: `while (!go_.load()) {}` and optionally records `start_time` if using “first publish” semantics.

3. **Send loop (instead of Read from queue)**  
   - Loop for `N` batches (or until elapsed time ≥ T):  
     - Set `BatchHeader* h = reinterpret_cast<BatchHeader*>(my_debug_buffer);`  
     - `h->client_id = client_id_;`  
     - `h->broker_id = broker_id;`  
     - `h->batch_seq = thread_base_seq + num_publish_threads * batch_index;`  
     - `h->total_size = batch_payload_size;`  
     - `h->num_msg = batch_payload_size / message_size;`  
     - (Plus any other required fields: ordered=0, batch_complete=0, start_logical_offset, log_idx, etc. — match current ORDER=0 header.)  
     - Send header: same `send(sock, h, sizeof(BatchHeader), 0)` loop as today.  
     - Send payload: `send(sock, my_debug_buffer + sizeof(BatchHeader), batch_payload_size, 0)` (with same EAGAIN/backoff as today).  
   - No `Read()` from queue, no `ReleaseBatch()`.

4. **End of run**  
   - Either fixed N (total batches across all threads or per thread) or time-based. When done, each thread exits; test records end_time and computes bandwidth = total_bytes_sent / (end_time - start_time).

**Test side (e.g. test_utils / main):**

- For test type 5 (publish-only), when debug_network_isolation is set:  
  - Do not call `Publish()` in a loop.  
  - Call something like `Publisher::RunNetworkIsolationTest(total_batches_per_thread_or_total, duration_sec)`.  
  - That method: ensures threads are started and reach the barrier; then sets `go_ = true` and `start_time`; waits for all threads to finish (join or a “done” counter); sets `end_time`; returns or logs total bytes and bandwidth.

**Batch count / duration:**  
- Either: each thread sends exactly `K` batches (then total bytes = num_threads * K * (sizeof(BatchHeader) + batch_payload_size)), or run for a fixed duration and count batches sent.  
- For “theoretical max” it’s enough to run for a fixed duration (e.g. 5–10 s) and sum bytes sent by all threads.

---

## 4. What this isolates

- **Removes:** QueueBuffer producer/consumer, queue contention, batch allocation from pool, real payload preparation (BufferWrite, message formatting).  
- **Keeps:** Same sockets, same send path (header then payload, same EAGAIN/backoff), same broker recv path. So the number you get is “network + broker recv + CXL write” under maximum send load from the client, without publisher or queue being the bottleneck.

If this debug run reaches much higher MB/s than the normal publish-only test, the bottleneck is elsewhere (queue or producer). If it stays in the same range, the bottleneck is in the network/broker path.

---

## 5. Caveats and options

- **ORDER=5:** To run this with ORDER=5 we’d need to either (a) not use sequencer in the loop (send with a dummy/constant total_order and accept that ACK/ordering may be wrong) or (b) call the sequencer per batch (adds RPC cost, less isolation). So keep the first version ORDER=0-only.
- **ACK handling:** Brokers will still send ACKs. For a pure “send as fast as possible” test we can ignore ACKs in the debug path (or still run AckThread and ignore timeouts). Alternatively, run for a fixed duration and only measure send-side bytes and time.
- **One buffer per thread:** Using one shared buffer would require serializing header updates and sends, which would not stress the network. So “one buffer per thread” is the right design.
- **start_time:** If “first publish() atomically turn on a flag” means “first thread to send sets start_time”, then use Option B (first thread does CAS on `go` and sets start_time). Then all threads must have already passed the barrier so that when the first sets go, everyone else is in the spin loop and will start sending in the same window.

---

## 6. Implementation checklist (high level)

1. Add config/flag: `debug_network_isolation` (config or env).  
2. In Publisher: allocate per-thread DEBUG buffer (batch_size) when starting PublishThreads (only if flag set).  
3. In PublishThread main loop: if flag set, do barrier → spin on `go` → send loop (update header, send header, send payload) for N batches or duration; then exit.  
4. Add `Publisher::RunNetworkIsolationTest(...)` (or equivalent) that: sets up N/duration, starts threads (or reuses existing thread start), signals go and start_time, waits for completion, computes and logs bandwidth.  
5. In test_utils (test type 5): when flag set, call RunNetworkIsolationTest instead of the Publish() loop.  
6. Ensure batch_header fields match what broker expects for ORDER=0 (client_id, broker_id, batch_seq, total_size, num_msg, ordered=0, etc.).

---

## 7. Conclusion

The suggestion is **possible and useful** to isolate network issues. The main constraints are: (1) one DEBUG buffer per thread, (2) correct BatchHeader update so each thread uses a unique, monotonic batch_seq (and correct client_id/broker_id/total_size/num_msg), (3) barrier + atomic go so all threads start sending together, (4) ORDER=0 for clean isolation. Implementing this as a dedicated debug path (gated by config/env) gives a “network at theoretical max” number to compare against the full publish path and confirms whether the bottleneck is in the network/broker or in the publisher/queue.
