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

## 7. References

- `src/cxl_manager/cxl_datastructure.h` — offset_entry layout
- `src/embarlet/topic.cc` — EmbarcaderoGetCXLBuffer, BrokerScannerWorker5
- `docs/memory-bank/BANDWIDTH_10GB_ASSESSMENT.md` — ring wrap issue and 1MB mitigation
