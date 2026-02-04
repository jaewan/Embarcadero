# ACK Timeout Investigation: Why Publish-Only Test Does Not Finish

**Date:** 2026-01-30  
**Symptom:** Publish-only test (10 GB, 1 KB, order 0, ack 1, test 5) often hits ACK timeout: received ~8.88M out of 10.48576M ACKs, reported 0 MB/s. `total_batches_sent=4608` but we need ~5436 batches to send all messages.

---

## 1. When Did It Last Succeed?

From our conversation history (no git):

- In an earlier bandwidth run with `run_bandwidth_10gb_3trials.sh`, **Trial 2** completed successfully (**2629.90 MB/s**). Trial 1 and Trial 3 hit ACK timeout (0 MB/s).
- So success is timing-dependent: sometimes all ACKs arrive, sometimes we time out with ~8.88M ACKs.

---

## 2. Root Cause: Early `publish_finished` Before Poll()

**Test flow (test type 5, publish-only):**

1. `for (i = 0; i < n; i++) p.Publish(message);`  // main thread writes all messages
2. `p.DEBUG_check_send_finish();`  // **SealAll()**, then **publish_finished_ = true**, then ReturnReads()
3. `p.Poll(n);`  // WriteFinishedOrPuased() again, DumpBufferStats(), ReturnReads(), publish_finished_ = true, wait client_order_ >= n, **join threads**, wait ACKs

**Bug:** `DEBUG_check_send_finish()` sets **`publish_finished_ = true`** before `Poll()` runs. So as soon as we’ve sealed and signaled “finished”, any PublishThread that gets `nullptr` from `Buffer::Read()` will exit (because it sees `publish_finished_` and breaks).

**When does `Buffer::Read()` return `nullptr`?**

1. **No data in buffer:** `writer_head == tail` → return `nullptr` immediately.
2. **Batch not sealed yet:** reader waits in a loop (spin/yield/sleep) for `total_size`/`num_msg`; if it waits longer than the **2 s timeout** in `Buffer::Read()`, it returns `nullptr`.

So a PublishThread can get `nullptr` because:

- It is waiting for the **next** batch to be sealed (main thread is still in `SealAll()` sealing other buffers), and the **2 s timeout** in `Read()` fires → `nullptr`; then it sees `publish_finished_` and exits without reading the remaining batches, or
- It sees `writer_head == tail` for its buffer (e.g. empty slot) and gets `nullptr`, then exits because `publish_finished_` is already true.

**Effect:** We send only **4608** batches (~8.88M messages) instead of **~5436** (~10.48576M). The missing ~828 batches (~1.6M messages) are still in the buffers but were never read/sent because some PublishThreads exited early when they got `nullptr` and `publish_finished_` was already set.

**Why it sometimes works:** If the main thread finishes `SealAll()` and the PublishThreads haven’t yet hit the 2 s timeout or the “no data” path, they keep reading and send all batches. So success depends on scheduling and timing.

---

## 3. What Changed After It Last Succeeded?

We did not change the test flow or the order of `DEBUG_check_send_finish()` vs `Poll()`. The race has been there; recent runs (e.g. 25% backoff test, single-trial run_throughput.sh) simply hit the bad timing more often. No single “commit” to point to; the design allows `publish_finished_` to be set before threads have drained all sealed batches.

---

## 4. Fix: Do Not Set `publish_finished_` in DEBUG_check_send_finish()

**Change:** In `DEBUG_check_send_finish()`, only call `WriteFinishedOrPuased()` and `pubQue_.ReturnReads()`. **Do not** set `publish_finished_ = true` there.

**Rationale:** `Poll()` already does:

1. `WriteFinishedOrPuased()` (SealAll)
2. ReturnReads()
3. **Then** `publish_finished_.store(true)`
4. Wait for `client_order_ >= n`
5. Join PublishThreads
6. Wait for ACKs

So `publish_finished_` should be set only in `Poll()`, **after** SealAll() has run. That way, when PublishThreads see `publish_finished_`, all batches have been sealed and they can drain the rest before exiting on `nullptr` + `publish_finished_`.

**Result:** PublishThreads only treat “finished” as “exit when no more batches” after the main thread has sealed everything. We avoid early exit and send all ~5436 batches, so ACKs can complete and the test can finish.

---

## 5. Follow-Up: Buffer::Read() 2 s Timeout and Memory Ordering

**Attempted fixes:**
1. **Do not set `publish_finished_` in DEBUG_check_send_finish()** — only Poll() sets it after SealAll(). Applied; run still showed 4608 batches sent.
2. **Acquire fence in Read()** before the wait loop — applied. **Release fence in SealAll()** — was documented as "applied" but was **not** present in code; Seal() has it, SealAll() did not. Without it, readers can spin on stale total_size/num_msg and hit the 10s timeout. **Now added** in `Buffer::SealAll()` after writing batch_header fields so PublishThreads see sealed batches.

**Observation from log:** `Buffer::SealAll sealed_buffers=1 sealed_messages=164` — only one buffer has a partial batch at Poll() time; the rest were already sealed by BATCH_SIZE. So ~5436 batches exist; readers still exit after 4608 (384 per thread). So some readers are getting `nullptr` (either `writer_head == tail` or the **2 s timeout** in the wait loop) and then seeing `publish_finished_` and exiting.

**Attempted fix 3:** Increased the wait timeout in `Buffer::Read()` from 2 s to **10 s**. Run still showed 4608 batches. **Observation:** PublishThreads exit almost exactly **10 s** after SealAll() (e.g. SealAll at 08:04:31.97, threads exit at 08:04:41.82). So readers are hitting the **10 s timeout** in the wait loop and then returning `nullptr` and exiting. So they are **not** seeing `total_size`/`num_msg` for the next batch (the 385th onward) within 10 s — i.e. that batch is either never sealed for that buffer, or there is a visibility/synchronization bug so the reader never sees the write. SealAll() only seals **one** buffer (the current write buffer’s partial batch); the other buffers’ “current” batches were already sealed by BATCH_SIZE in `Buffer::Seal()`. So the batch the reader is waiting for (e.g. 385th) should already be sealed. So the remaining suspect is **per-buffer indexing or reader_head vs batch layout**: e.g. reader_head advancing to the wrong slot, or the 385th batch in that buffer not being the one the reader thinks. Further debugging would need per-buffer dumps (reader_head, writer_head, tail) and which batch slot has total_size/num_msg set when the reader waits.

---

## 6. Further visibility fixes (release fence, volatile, per-buffer fence)

**Attempted fix 4:**  
- **Release fence in SealAll()** after writing batch headers (and **per-buffer** inside the loop) so the one reader waiting on the partial batch sees `total_size`/`num_msg`.  
- **Volatile loads in Read()** wait loop for `total_size`/`num_msg` so the compiler does not cache the load.  

**Result:** Re-runs still show 4608 batches, 8881500 ACKs, 0 MB/s. The single thread that should see the sealed partial batch still times out after 10 s.

**Conclusion so far:** 11 threads get `nullptr` from `writer_head == tail` (no partial batch in their buffer) and exit when `publish_finished_` is set. One thread is in the wait loop for the buffer that has the partial batch; that thread never sees the sealed values within 10 s. Either (1) the buffer index we seal is not the one that reader is reading (mapping bug), or (2) the reader is waiting on a different batch slot than the one SealAll() seals (reader_head vs writer_head). **Next step:** Add diagnostic logging in SealAll() (log sealed buffer index) and in Read() when entering the wait loop (log bufIdx, reader_head); or dump reader_head/writer_head/tail per buffer at SealAll() time.

---

## 7. ROOT CAUSE: Buffer wrap — reader and writer at different positions

**Diagnostic run (with logging in SealAll and Read):**

- **SealAll:** `sealing buffer idx=4 head=144747648 tail=144926208 reader_head=805305152 match=NO`
- We seal the batch at **head=144747648** (~141 MB into the buffer).
- The reader for buffer 4 has **reader_head=805305152** (~768 MB, near end of buffer).

So we are sealing the batch at **144747648**, but the reader is waiting on the batch at **805305152**. They are different positions in the same buffer.

**What this means:**

1. **Buffer wrap occurred.** The writer filled buffer 4 (768 MB), hit the “buffer full” path, and **wrapped**: it sealed the current batch, reset `writer_head` and `tail` to the start (and in the “reader consumed” branch it also reset `reader_head` to 0). The writer then continued writing at the **beginning** of the buffer (hence the partial batch at head=144747648).
2. **Reader did not wrap with writer.** The reader had already advanced to **805305152** (waiting on the “next” empty batch at the end of the buffer). When the writer wrapped, it reset `reader_head` to 0 in the wrap path — but the reader was **already inside `Read()`** with the old `reader_head` (805305152) and is stuck in the wait loop. So the reader keeps waiting on the batch at **805305152**, which is never written to or sealed (writer wrapped and writes from 0).
3. **SealAll() seals the wrong slot.** SealAll() correctly seals the **current** partial batch at **144747648** (after the wrap). The reader never sees that, because it is waiting on **805305152**. So the one thread that should drain the partial batch times out after 10 s and exits; the rest get `nullptr` from `writer_head == tail` and exit when `publish_finished_` is set.

**Root cause:** With **768 MB** per buffer and **12** buffers, total capacity is 9 GB. The 10 GB test writes **~10.7 GB** of data, so some buffer(s) **must** wrap. When the writer wraps, it resets `reader_head` to 0; a reader that is already in the wait loop at the end of the buffer never sees the reset and keeps waiting on an empty slot that is never sealed. SealAll() then seals the partial batch **after** the wrap (at ~141 MB), which is a different slot than the one the reader is waiting on.

**Attempted fix (insufficient):** Increase buffer size to **1024 MB** so the 10 GB test does not wrap (12 × 1024 MB = 12 GB > 10.7 GB). **Result:** Did not fix the issue; ACK timeouts persisted. Reverted config to **768 MB**.

---

## 8. Real fix: Buffer::Read() must follow writer after wrap

**Root cause (buffer code):** The reader’s wait loop used a **single** `batch_header` computed once from the initial `reader_head`. When the writer wraps, it sets `reader_head = 0` in the buffer, but the reader thread is already in the loop waiting on the **old** slot (e.g. 805305152). The reader never re-loads `reader_head`, so it never sees the batch sealed at 0 (or 144747648) and times out.

**Fix (in code):** In `Buffer::Read()`, inside the wait loop, **re-load `reader_head` each iteration** and recompute the batch pointer from the current `reader_head`. Then wait on `total_size`/`num_msg` for that slot (with volatile loads). When the writer wraps and sets `reader_head = 0`, the next iteration waits on the batch at 0, so the reader sees the sealed batch after the wrap instead of timing out.

**Config:** Reverted `buffer_size_mb` from 1024 to **768**. The correct fix is wrap handling in the buffer, not avoiding wrap by sizing.

**Status:** Implemented in `src/client/buffer.cc` (Read() wait loop). Re-run 10 GB test to confirm.
