# Where Did the ~2999 MB/s Bandwidth Go?

**Context:** Before the ACK timeout / buffer fixes we used to see ~2735–2999 MB/s (10 GB, order 0, ack 1, test 5). After the fixes we see ~888 MB/s. This doc summarizes what we changed in our chat (no git history) and the most likely causes of the drop.

---

## 1. What We Changed (From Our Chat)

### 1.1 Publisher

- **`publish_finished_`:** Stopped setting it in `DEBUG_check_send_finish()`. It is now set only in `Poll()` **after** `SealAll()`. So PublishThreads only see “finished” after all batches are sealed.
- **Logging:** Many `LOG(INFO)` → `VLOG(1)`/`VLOG(2)` (connection, SubscribeToCluster, Exiting, Read batch, Pipeline profile, etc.). Kept “Publisher finished sending” and bandwidth result as INFO.
- **Backoff:** Code already uses **25%** backoff (`zero_copy_send_limit * 3 / 4`) with comment “maintain higher throughput (old behavior)”. The assessment doc had described 50% (halve) as “current” and recommended reverting to 25%; that revert is already in place.

### 1.2 Buffer

- **Wrap handling:** In `Buffer::Read()` wait loop we now **re-load `reader_head` every iteration** with `memory_order_acquire` and recompute the batch pointer. So when the writer wraps and sets `reader_head = 0`, the reader follows and waits on the batch at 0 instead of timing out at the old offset.
- **Memory ordering:**  
  - **SealAll():** Release fence after writing batch header fields (and per-buffer inside the loop) so readers see `total_size`/`num_msg`.  
  - **Read():** Acquire fence **before** the wait loop; inside the loop, **every** iteration does `reader_head.load(std::memory_order_acquire)`.
- **Timeout:** Wait loop timeout increased from 2 s to 10 s.
- **Buffer size:** `buffer_size_mb` 768 → 1024 so 12 buffers hold 10 GB without wrap.
- **Logging:** SealAll per-buffer/summary and DumpBufferStats → VLOG(2)/VLOG(3); “Buffer::Read entering wait” → VLOG(3).

### 1.3 Scripts / Config

- **run_bandwidth_10gb_3trials.sh:** Added `-l 0` so default run is quiet.
- **client.yaml:** `buffer_size_mb: 1024`, `threads_per_broker: 3` (unchanged by us in this chat).

---

## 2. Where the Bandwidth Went: Likely Causes

### 2.1 Thread count (partial)

- **Config:** `config/client.yaml` has `threads_per_broker: 3` → **9** publish threads (3 data brokers × 3).
- **main.cc:** If `-n` is not passed (or equals 4), we use **config** value (3). So we run with 9 threads.
- **If the ~2735 MB/s baseline was with 4 threads/broker** (12 threads), then going to 3 (9 threads) is 25% fewer threads → you’d expect on the order of ~2050 MB/s, not 888. So thread count alone does **not** explain a 3× drop but can explain a chunk of it.

### 2.2 Memory ordering and Read() wait loop (main suspect)

- We added **release fences in SealAll()** and **acquire in Read()** (once before the loop and **on every wait-loop iteration** via `reader_head.load(std::memory_order_acquire)`).
- Effects:
  - **Correctness:** Readers reliably see sealed batches; no more ACK timeouts from stale reads.
  - **Cost:** Every wait-loop iteration now has an **acquire load** of `reader_head` plus pointer computation. That can:
    - Increase cache-coherence traffic and delay the moment the reader “sees” the sealed batch.
    - Slow down the handoff between writer (main thread) and reader (PublishThread) on every batch.
- If the reader now effectively waits longer or pays more per handoff, **throughput can drop**. A ~3× drop is consistent with the handoff becoming much more expensive (e.g. more time in the wait loop or more expensive iterations).

### 2.3 Logging

- **run_throughput.sh** does **not** pass `-l 0`, so default `log_level=1` → VLOG(1) and some INFO still print (e.g. test params, SubscribeToCluster, PublishThread connection/Read batch every 100, Buffer::Read entering wait, SealAll, etc.). That can add non-trivial I/O and lock contention.
- **run_bandwidth_10gb_3trials.sh** uses `-l 0` and we still see ~888 MB/s, so **logging is not the primary cause** of the drop, but it can still matter for run_throughput.sh.

### 2.4 Backoff

- **Current code:** 25% backoff on EAGAIN. Assessment doc had recommended this for “higher throughput”; it’s already in place, so backoff is unlikely to explain the 2735 → 888 drop.

---

## 3. What to Try

1. **Restore threads if baseline was 4:**  
   In `config/client.yaml` set `threads_per_broker: 4` (or run with `-n 4`). Remeasure. If you were comparing to a 12-thread run, this should recover a meaningful fraction (not 3×).

2. **Reduce cost of the Read() wait loop (keep correctness):**  
   - Keep the **re-load of `reader_head`** so wrap handling stays correct.  
   - Option A: Use `memory_order_relaxed` for the **per-iteration** `reader_head` load, and keep a single **acquire fence** (or one acquire load) only when we’re about to check `total_size`/`num_msg` for the **current** slot (so we still see the writer’s writes). This needs a careful concurrency review.  
   - Option B: Re-load `reader_head` only every N iterations (e.g. every 10 or 100) instead of every iteration, and document that wrap detection may be delayed by N iterations. Only safe if wrap is rare (e.g. with 1024 MB buffers and 10 GB test).

3. **Quiet run for throughput script:**  
   In `scripts/run_throughput.sh`, pass `-l 0` to `throughput_test` so it matches the bandwidth script and reduces log overhead when comparing numbers.

4. **Instrument:**  
   Add a counter for “number of times Read() entered the wait loop” and “total iterations in wait loop” per run. If those numbers are much higher than before the fixes, that supports “handoff cost” as the main cause of the drop.

---

## 4. Summary

- We changed: **publish_finished_** placement, **buffer wrap** handling (re-load `reader_head` in the wait loop), **memory ordering** (release in SealAll, acquire before and inside Read() wait loop), timeout, buffer size, and logging.
- The **~2999 / ~2735 → ~888 MB/s** drop is most plausibly from:
  - **Handoff cost:** Acquire on every wait-loop iteration (and extra release/acquire in SealAll/Read) making each batch handoff more expensive and/or visible later.
  - **Thread count:** If baseline was 4 threads/broker and we now run with 3, that explains part of the drop but not all.
- Next steps: try **threads_per_broker=4** (or `-n 4`), then consider **relaxing or batching the acquire in the Read() wait loop** while keeping wrap correctness, and use **`-l 0`** in run_throughput.sh for fair comparison.
