# Lock-Free Batch Header Ring: Consumed-Through Protocol

**Date:** 2026-01-27  
**Goal:** Allow a small batch header ring (e.g. 64KB) without overwriting unconsumed slots. No locks; minimal shared state; correct on non-coherent CXL.

---

## 1. First-Principles Problem

**Resource:** A ring of N batch-header slots in CXL. Each slot holds a BatchHeader; payload lives elsewhere.

**Writers:** NetworkManager on **broker B** (receiving publishes). Allocates slot, receives payload, sets `batch_complete=1`, flushes.

**Readers:** BrokerScannerWorker5 on **head broker**. Scans ring, sees `batch_complete=1`, processes, clears `batch_complete=0`, flushes.

**Non-coherent:** Writer (broker B) and reader (head) are on different CPUs. Visibility is via flush (writer) and invalidate (reader).

**Bug:** When the writer has produced N batches it wraps (resets cursor to slot 0). If the reader has not yet consumed slot 0, the writer overwrites it → that batch is lost → no ACK → client stalls. Current mitigation: make N huge (1MB) so we don’t wrap.

---

## 2. Invariant and Protocol

**Invariant:** The writer may reuse a slot only after the reader has consumed it.

**“Consumed”:** The reader has processed the batch (AssignOrder5, etc.) and cleared `batch_complete` for that slot.

**Protocol:**

- **Shared variable:** `batch_headers_consumed_through` (byte offset, relative to ring start). Meaning: “I have consumed all slots with start offset < this value.”  
  Stored in CXL in `offset_entry[broker_id]` (sequencer region).  
  **Writer:** broker B. **Reader of this variable:** broker B (in GetCXLBuffer before wrap).  
  **Writer of this variable:** head broker’s BrokerScannerWorker5.

- **Consumer (BrokerScannerWorker5):** After processing the slot at byte offset `off` from ring start, set  
  `batch_headers_consumed_through = off + sizeof(BatchHeader)`,  
  then flush that cache line, store_fence.

- **Producer (EmbarcaderoGetCXLBuffer):** When allocation would wrap (next slot is slot 0), **before** reusing slot 0:
  1. Invalidate cache line containing `batch_headers_consumed_through`.
  2. Load `consumed = batch_headers_consumed_through`.
  3. Slot 0 is consumable when `consumed >= sizeof(BatchHeader)`. If `consumed < sizeof(BatchHeader)`, the ring is full: spin (invalidate, load, pause) until `consumed >= sizeof(BatchHeader)`.
  4. Then set allocation cursor to ring start and use slot 0.

No lock. One shared variable per broker, written by sequencer and read by that broker. Cache-line isolation: variable lives in sequencer region; sequencer writes it, broker only reads it (with invalidate).

---

## 3. Why This Is Lock-Free and Correct

- **Single producer, single consumer per broker:** One writer (broker B’s NetworkManager) and one reader (head’s BrokerScannerWorker5 for broker B) for that broker’s ring. So we have SPSC per ring.

- **No RMW on the shared variable:** Sequencer only writes `batch_headers_consumed_through`; broker only reads it. So we don’t need atomics or compare-and-swap; we need visibility (flush on writer, invalidate on reader). On CXL that’s exactly flush_cacheline / load_fence.

- **Bounded wait:** The sequencer advances `batch_headers_consumed_through` after every consumed batch. So if the producer spins, the consumer will eventually advance it and the producer will proceed. No lock, no deadlock.

- **Small ring:** With this protocol we can safely use a small ring (e.g. 64KB = 512 slots). When the producer would wrap, it waits until the consumer has freed slot 0. Throughput is limited only by consumer speed, not by ring size.

---

## 4. Layout and Placement

- **`batch_headers_consumed_through`** in `offset_entry` (CXL), sequencer region (bytes 512–767).  
  At offset 528 (after `ordered` 512, `ordered_offset` 520). Padding reduced by 8 bytes.  
  **Writer:** Sequencer (head). **Reader:** Broker B. Same cache line as `ordered`/`ordered_offset` is acceptable: sequencer writes that line (ordered, ordered_offset, batch_headers_consumed_through); broker only reads batch_headers_consumed_through and must invalidate that line before reading.

- **Producer read:** Broker B, in GetCXLBuffer, uses `tinode_->offsets[broker_id_].batch_headers_consumed_through`. Invalidate the sequencer cache line for that offset_entry, then load.

- **Consumer write:** Head’s BrokerScannerWorker5, after processing a slot, sets  
  `tinode_->offsets[broker_id].batch_headers_consumed_through = (slot_ptr - ring_start) + sizeof(BatchHeader)`,  
  then flushes the sequencer cache line for that offset_entry, store_fence.

---

## 5. Performance

- **Hot path (no wrap):** Producer does one extra comparison when `batch_headers_ >= end`. No shared-memory read when not wrapping. Zero cost when ring is not full.

- **Wrap path:** One invalidate + one load; if full, spin (invalidate, load, pause) until consumer advances. No mutex, no lock. Spin is bounded by consumer progress.

- **Consumer:** One extra store per batch plus existing flush of sequencer region. No extra cache line if we keep it in the same sequencer 64B line we already flush.

---

## 6. Producer Spin and Mutex

When the ring is full, the producer (GetCXLBuffer) spins (invalidate, read `consumed_through`, pause) until `consumed_through >= sizeof(BatchHeader)`. The spin is done **inside** the Topic mutex, so no other allocation can proceed on that topic until the producer wraps. This is acceptable because (a) ring-full is rare when the sequencer keeps up, and (b) it avoids release-retry and extra complexity. The wait is bounded: the consumer advances `consumed_through` after every processed batch, so the producer eventually proceeds.

---

## 7. Gap and min-contiguous (2026-01)

**Problem:** The sequencer drains an epoch buffer that can have **gaps**: the scanner only pushes when `batch_complete=1`, so if slot 701 had `batch_complete=0` when we passed, the buffer may contain 700 and 702 but not 701. If we set `batch_headers_consumed_through = max(slot_offset + sizeof(BatchHeader))` across the epoch, we would advance past 702 and allow the producer to overwrite slot 701 (never processed) → batch loss and ACK stall.

**Fix:** Use **min-contiguous** per broker. For each broker, initialise from the current CXL `batch_headers_consumed_through`; then when processing batches (already sorted by `(broker_id, slot_offset)`), advance only when `p.slot_offset == next_expected`, else leave unchanged so the producer cannot overwrite the gap. Implemented in `src/embarlet/topic.cc` EpochSequencerThread (`[[CONSUMED_THROUGH_FIX]]`).

**Sentinel handling:** Initial value in CXL is `BATCHHEADERS_SIZE` (“all slots free”, see topic_manager.cc). For min-contiguous, that must be treated as `0` (next expected = slot 0), otherwise `slot_offset` (0, 64, …) never equals `BATCHHEADERS_SIZE` and `consumed_through` would never advance → producer would always see “all slots free” and overwrite unconsumed batches. Code: when reading from CXL, if `initial == BATCHHEADERS_SIZE` then set `initial = 0` before use (`[[SENTINEL]]`).

**Bounded spin:** Scanner can see `batch_complete=0` for in-flight slots (receiver not done yet). If we advance immediately we create gaps → min-contiguous holds `consumed_through` → producer hits ring full. So when `!batch_ready && num_msg > 0` (likely in-flight), we spin briefly (e.g. 50 × cpu_pause, ~200ns) and re-check; if `batch_complete==1` we retry at top of loop (full invalidation) and process. Reduces gaps so `consumed_through` advances smoothly (`[[BOUNDED_SPIN]]` in BrokerScannerWorker5).

---

## 8. References

- `src/cxl_manager/cxl_datastructure.h` — offset_entry layout
- `src/embarlet/topic.cc` — EmbarcaderoGetCXLBuffer, BrokerScannerWorker5
- `docs/memory-bank/BANDWIDTH_10GB_ASSESSMENT.md` — ring wrap issue and 1MB mitigation
- §7 above — gap / min-contiguous semantics

---

## 9. B0 Local-Ring Cache Coherency (2026-01)

**Problem:** §1 assumes writer (broker B) and reader (head’s BrokerScannerWorker5) are on **different** CPUs. For **B0’s ring only**, the writer (B0’s NetworkManager or CXLAllocationWorker) and the reader (B0’s BrokerScannerWorker5 for broker 0) run in the **same process** on the head broker. That creates a local cache-aliasing / visibility race: the scanner can read stale `num_msg` (and sometimes `batch_complete`) from the BatchHeader even after `CXL::flush_cacheline()` + `CXL::load_fence()` on the reader side.

**Dev setup note:** In a single-server dev setup, each “broker” may be a separate process on the same machine; those processes are typically **cache coherent** to each other (same machine). The design still treats brokers as non-coherent (flush/invalidate) so it is correct for a future multi-node deployment. The B0 issue is **same-process** writer/reader for B0’s ring (one process runs both B0’s writer and all four scanners), so B0’s ring has tighter coupling than “different process on same machine.”

**Evidence:** B0’s scanner reports ~1.88 msg/batch (e.g. 11,568 ordered / 6,161 batches) while B1/B2/B3 report ~700–1,355 msg/batch (expected ~1928). So B0’s scanner is effectively reading low or stale `num_msg` values; `ordered` is incremented by that value, so the deficit is in the read path, not in ordering logic.

**Root cause (likely):** On the same node, the writer’s flush (clflushopt) and the scanner’s invalidate (same instruction) both target the same physical line. Visibility can still be wrong if (1) the writer’s flush is in flight when the scanner’s read is satisfied from a buffer that hasn’t applied the write yet, or (2) the scanner and writer share or alternate on the same core, so cache/ordering edges are tighter than for remote rings.

**Fix attempts and side effects:**

- **Extra invalidation for local ring only** (e.g. when `broker_id == 0`): B0 improved ~6× (e.g. 11K→69K ACKs) but **B3 regressed** (e.g. 254K→113K). Likely reason: **CPU contention**. All four BrokerScannerWorker5 threads run in the same process; adding extra flush+load_fence for B0’s scanner makes that thread heavier, so the scheduler gives less time to B1/B2/B3 scanners → B3’s progress drops. So the regression is indirect (resource contention), not a B3-specific bug.
- **Two-stage read** (check `batch_complete` then re-invalidate then read `num_msg`): Broke the bounded-spin logic and led to 0 ACKs; reverted.
- **Multi-cacheline flush** (e.g. flush both BatchHeader lines on every read): System hung or performed very poorly; reverted.

**Architecture note:** BatchHeader is 64B-aligned; `num_msg` and `batch_complete` are in the first cache line (static_asserts in `cxl_datastructure.h`). The writer already flushes **both** cache lines of the BatchHeader when setting `batch_complete=1` (NetworkManager and CXLAllocationWorker). So the issue is not “BatchHeader spans multiple lines and we only flush one”; the issue is visibility on the **local** ring when reader and writer share the same process/CPU.

**Recommended next steps (in order of preference):**

1. **Thread pinning:** Pin B0’s BrokerScannerWorker5 (broker_id 0) to a different CPU core than the writer threads (NetworkManager / CXLAllocationWorker). This avoids same-core cache aliasing and doesn’t add read-side overhead for B1/B2/B3. No change to flush/invalidate logic.

2. **Write-side ordering:** Ensure the writer’s view of the first cache line is committed before flush. After `__atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE)`, add `CXL::store_fence()` **before** the first `flush_cacheline(batch_header_location)`, so all prior stores to that line (including the memcpy’d `num_msg`) are ordered before the flush. Then keep the existing double flush + store_fence after. This may help same-node visibility without touching the scanner.

3. **Different sync for local vs remote:** Use a stronger read-side guarantee only when scanning the local ring (e.g. `broker_id == 0` on head): e.g. full seq_cst fence between invalidate and read, or a second invalidate+load_fence, but **combine with thread pinning** so the B0 scanner doesn’t starve B1/B2/B3 (e.g. pin B0 scanner to a dedicated core so extra work doesn’t steal CPU from other scanners).

4. **Do not:** Rely on “extra invalidation for local ring” alone without pinning; it fixes B0 but regresses B3 due to contention. Do not add a second full cache-line flush on every scanner iteration for all brokers; that caused hang/poor performance.

---

### §9.1 Post-mortem: Why “thread pinning (core 0) + write-side store_fence” gave 0 ACKs

When a fix yields **0 ACKs**, the pipeline is broken end-to-end (no batch is ever seen as ready, or the sequencer never runs, or deadlock). Likely causes for that specific combo:

- **Pinning to core 0:** Core 0 often runs the OS / main thread. If B0’s scanner (or all four scanners) were pinned to core 0, then (a) the writer (NetworkManager / CXLAllocationWorker) may also be scheduled on core 0 → same-core contention and possible starvation of the writer, so `batch_complete=1` is never set or is delayed; or (b) EpochSequencerThread (which drains the epoch buffer) may run on core 0 → scanner and sequencer fight for one core → pipeline stalls. **Safer:** Pin only B0’s scanner to a **non-zero** core (e.g. 1 or 2, or the last core) that is **not** used by the writer or EpochSequencerThread. Do **not** pin all four scanners to the same core.

- **Write-side store_fence:** Adding `CXL::store_fence()` **before** the first `flush_cacheline(batch_header_location)` is correct in principle (order all stores to the line before flush). If the fence was added **after** the flush, or in the wrong path (e.g. only in CXLAllocationWorker and not in NetworkManager receive path), then one writer path might not flush correctly and the scanner would never see `batch_complete=1` for that path. **Check:** store_fence must appear in **both** places that set `batch_complete=1` (NetworkManager and CXLAllocationWorker), and the sequence must be: store `batch_complete=1` → store_fence → flush line 1 → flush line 2 → store_fence.

- **Interaction:** Pinning + fence together can hide a bug (e.g. fence in only one path); when reverted to baseline, always revert **both** so the baseline is unchanged before trying one change at a time.

**Before trying pinning again:** (1) Confirm exactly which thread(s) were pinned and to which core(s). (2) Prefer pinning only the B0 scanner to a single **non-zero** core. (3) Try write-side store_fence **alone** (no pinning) first; if ACKs stay ~74%, the fence alone didn’t break the pipeline and the 0-ACK failure was likely pinning.

---

### §9.2 Conservative path (before more invasive fixes)

1. **Diagnostic-only:** Add logging when B0’s scanner sees `batch_complete==1` and `num_msg < 100` (or `num_msg == 0`). No change to flush/fence or pinning. Run E2E and inspect logs to confirm B0 is reading low `num_msg` and how often. This validates the §9 hypothesis without touching the sync protocol.

2. **One change at a time:** If trying fixes again: (a) add write-side store_fence only (both writer paths), run E2E; (b) if still ~74%, add pinning only for B0’s scanner to a **non-zero** core, run E2E; (c) never pin to core 0; never pin all scanners to one core.

3. **Review failed implementations:** For each reverted fix, document: exact code change (file + line), which thread was pinned to which core, and order of operations (store vs fence vs flush). That makes it possible to see whether 0 ACKs came from pinning, from fence placement, or from an interaction.
