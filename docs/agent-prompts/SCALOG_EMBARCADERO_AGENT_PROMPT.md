# Embarcadero: CXL CLFLUSHOPT Store-Ordering Bug — Agent Task Prompt

## Task

Fix a critical CXL memory visibility bug that causes intermittent ACK stalls in Embarcadero's ORDER=5 publish path. The bug is **fully diagnosed** — this prompt contains the root cause, the exact code sites, and the fix pattern. Your job is to implement the fix across all affected sites, build, and validate with a 10-trial stress test.

---

## 1. What Is Embarcadero?

Embarcadero is a **distributed publish/subscribe system** that uses **CXL (Compute Express Link) shared memory** for inter-process communication between brokers. It is a research system designed for high-throughput, totally-ordered message delivery.

### Architecture Overview

```
                    ┌─────────────────────────────────────────────┐
                    │              CXL Shared Memory               │
                    │  ┌──────────┬──────────┬────────┬─────────┐ │
                    │  │ControlBlk│    CV    │  GOI   │PBR+BLog │ │
                    │  │  128B    │   4KB    │  16GB  │ ~495GB  │ │
                    │  └──────────┴──────────┴────────┴─────────┘ │
                    └───────┬──────────┬──────────┬───────────────┘
                            │          │          │
              ┌─────────────┤    ┌─────┤    ┌─────┤
              ▼             ▼    ▼     ▼    ▼     ▼
         ┌─────────┐  ┌─────────┐  ┌─────────┐  ┌─────────┐
         │Broker 0 │  │Broker 1 │  │Broker 2 │  │Broker 3 │
         │  (HEAD) │  │(follower│  │(follower│  │(follower│
         │Sequencer│  │         │  │         │  │         │
         └────┬────┘  └────┬────┘  └────┬────┘  └────┬────┘
              │            │            │            │
         TCP connections from publishers/subscribers
```

- **Head Broker (Broker 0)**: Runs the `EpochSequencerThread` that globally orders batches, and `BrokerScannerWorker5` threads that scan each broker's PBR.
- **Follower Brokers (1-3)**: Accept publisher connections, receive data into CXL BLog, write batch metadata into their PBR.
- **CXL Memory**: Non-coherent shared memory visible to all brokers. Requires explicit cache flushes for visibility.
- **PBR (Producer Batch Ring)**: Per-broker ring buffer of `BatchHeader` entries (128B each, 2 cache lines). Metadata for each batch.
- **BLog**: Per-broker append log where raw message payloads are stored.
- **CompletionVector (CV)**: Per-broker progress tracking (sequencer_logical_offset, completed_pbr_head) used by the ACK path.

### ORDER=5 Publish Pipeline (5 stages)

```
Publisher → TCP → NetworkManager (ingestion broker)
    Stage 1: recv() payload into BLog (CXL), then:
             ReservePBRSlotAfterRecv() — writes CLAIMED flag
             PublishPBRSlotDirect()    — memcpy full header, set VALID + batch_complete=1
             flush + fence to CXL

    Stage 2: BrokerScannerWorker5 (runs on head broker B0)
             Scans each broker's PBR for VALID batches
             Pushes to EpochBuffer for sequencing

    Stage 3: EpochSequencerThread (runs on head broker B0)
             Globally orders batches, writes total_order
             Updates CV (sequencer_logical_offset)

    Stage 4: Replication (if RF>1)

    Stage 5: AckThread reads CV, sends ACKs to publishers
```

### Key Data Structure: BatchHeader (128 bytes, 2 cache lines)

```
Cache Line 0 (bytes 0-63):
  offset 0:  size_t   batch_seq           (8B)
  offset 8:  uint32_t client_id           (4B)
  offset 12: uint32_t num_msg             (4B)
  offset 16: uint32_t batch_complete      (4B) — volatile
  offset 20: uint32_t broker_id           (4B)
  offset 24: uint32_t ordered             (4B)
  offset 28: uint32_t flags               (4B) — CLAIMED=0x1, VALID=0x2
  offset 32: uint16_t epoch_created       (2B)
  offset 34: uint16_t _pad0              (2B)
  offset 36: uint64_t batch_id            (8B)
  offset 44: uint64_t pbr_absolute_index  (8B)
  offset 52: (end of first CL payload, padded to 64B)

Cache Line 1 (bytes 64-127):
  offset 64: size_t total_size             (8B)
  offset 72: size_t start_logical_offset   (8B)
  offset 80: size_t log_idx                (8B)
  offset 88: size_t total_order            (8B)
  offset 96: size_t batch_off_to_export    (8B)
```

### CXL Memory Primitives (in `src/common/performance_utils.h`)

```cpp
namespace CXL {
    // CLFLUSHOPT — flushes cache line to CXL memory (NON-BLOCKING, WEAKLY ORDERED)
    inline void flush_cacheline(const void* addr) { _mm_clflushopt(addr); }

    // SFENCE — store fence, orders all prior stores and flushes
    inline void store_fence() { _mm_sfence(); }

    // LFENCE — load fence
    inline void load_fence() { _mm_lfence(); }

    // MFENCE — full memory fence
    inline void full_fence() { _mm_mfence(); }
}
```

---

## 2. The Bug: CLFLUSHOPT Reordering Before Stores

### Intel SDM Specification

From the Intel Software Developer's Manual (Volume 3, §11.12):

> **CLFLUSHOPT is weakly ordered with respect to other CLFLUSHOPT instructions, loads, and stores.**
> To ensure ordering of CLFLUSHOPT after a store, software can insert an SFENCE instruction between the store and the CLFLUSHOPT.

This means on x86-64, the CPU can reorder CLFLUSHOPT to execute **before** preceding stores. Without an SFENCE between stores and CLFLUSHOPT, the flush may evict the **old** cache line contents (before the stores reached the cache), leaving CXL memory with stale data.

### The Correct CXL Write+Flush Pattern

```cpp
// WRONG (current code):
memcpy(cxl_location, &data, sizeof(data));     // stores go to store buffer/cache
CXL::flush_cacheline(cxl_location);             // CLFLUSHOPT — may reorder before memcpy!
CXL::store_fence();                             // SFENCE — too late, flush already happened

// CORRECT:
memcpy(cxl_location, &data, sizeof(data));     // stores go to store buffer/cache
CXL::store_fence();                             // SFENCE — drain stores to cache ← NEW
CXL::flush_cacheline(cxl_location);             // CLFLUSHOPT — now guaranteed to flush new data
CXL::store_fence();                             // SFENCE — ensure flush completes
```

### Smoking Gun Evidence

During a stress test (Trial 8 of 10), Broker 2 (B2) had a massive ACK shortfall: only 52,910 out of 3,932,596 messages acknowledged. A diagnostic probe in `BrokerScannerWorker5` showed:

```
[SCANNER_EMPTY_STUCK] B2 stuck at slot_off=13952 slot_seq=109 for 5003ms
  probe[+0] flags=0x0  num_msg=481 batch_complete=0 batch_seq=441 broker_id=2
  probe[+1] flags=0x3  num_msg=481 batch_complete=1 batch_seq=442 broker_id=2
  probe[+2] flags=0x3  num_msg=481 batch_complete=1 batch_seq=443 broker_id=2
  ...
```

**Slot 109 (stuck)**: `flags=0x0` (EMPTY), but `num_msg=481`, `batch_seq=441` — data fields from `memcpy` are visible, but `flags` is still zero. The scanner sees `flags=0x0` and treats it as an empty slot, stalling forever.

**Slot 110+ (after)**: `flags=0x3` (CLAIMED|VALID), all fields correct — the timing race didn't hit these slots.

This is a textbook CLFLUSHOPT reordering bug: the `memcpy` wrote `flags=0x3` into the cache, but the `CLFLUSHOPT` executed before that store reached the cache, flushing the old `flags=0x0` to CXL memory.

---

## 3. Affected Code Sites (ALL in src/embarlet/topic.cc unless noted)

### CRITICAL WRITER SITES (must fix — store then flush without intervening SFENCE)

#### Site 1: `PublishPBRSlotDirect` (line ~1941-1949) — **PRIMARY BUG SITE**
```cpp
memcpy(batch_header_location, &published, sizeof(BatchHeader));
__atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE);
// BUG: missing CXL::store_fence() here
CXL::flush_cacheline(batch_header_location);
CXL::flush_cacheline(batch_header_next_line);  // second cache line
CXL::store_fence();
```

#### Site 2: `ReservePBRSlotAfterRecv` (line ~1917-1921)
```cpp
__atomic_store_n(&slot->flags, kBatchHeaderFlagClaimed, __ATOMIC_RELEASE);
__atomic_store_n(&slot->batch_complete, 0, __ATOMIC_RELEASE);
__atomic_store_n(&slot->num_msg, 0, __ATOMIC_RELEASE);
// BUG: missing CXL::store_fence() here
CXL::flush_cacheline(batch_headers_log);
CXL::store_fence();
```

#### Site 3: `InvalidateOrder5HeldSlot` (line ~96-102)
```cpp
hdr->batch_complete = 0;
__atomic_store_n(&hdr->flags, 0u, __ATOMIC_RELEASE);
// BUG: missing CXL::store_fence() here
CXL::flush_cacheline(hdr);
CXL::flush_cacheline(reinterpret_cast<const uint8_t*>(hdr) + 64);
CXL::store_fence();
```

#### Site 4: `FlushAccumulatedCVLogicalOnly` (line ~2514)
```cpp
// After compare_exchange_strong writes to CXL memory (CV entry):
CXL::flush_cacheline(entry);   // at end of per-broker loop
// BUG: no store_fence() between CAS writes and this flush
// The final CXL::store_fence() at line 2516 is AFTER all flushes
```

#### Site 5: `FlushAccumulatedCV` (line ~2596)
Same pattern as Site 4 — CAS writes to CV entry, then flush, with SFENCE only after all broker loops complete.

#### Site 6: `CommitEpoch` / `AdvanceConsumedThroughForProcessedSlots` consumed_through writes (line ~3936-3939, ~6296-6298)
```cpp
tinode_->offsets[b].batch_headers_consumed_through = val;
// BUG: missing CXL::store_fence() here
CXL::flush_cacheline(CXL::ToFlushable(&tinode_->offsets[b].batch_headers_consumed_through));
// SFENCE is after the loop
```

#### Site 7: Various TInode ordered/offset writes
- `tinode_->offsets[broker_id_].validated_written_byte_offset = ...` then flush (line ~458)
- `tinode_->offsets[broker_id_].ordered += ...` then flush (line ~1203)
- `slot_header->ordered = 1` then flush (line ~1191)
- Sequencer `msg_header->total_order = seq` then flush (lines ~837, ~908, ~1013 in `cxl_manager.cc`)

#### Site 8: `chain_replication.cc` — CV tail writes (line ~591, ~673, ~789)
```cpp
entry->num_replicated.compare_exchange_strong(...);
CXL::flush_cacheline(entry);   // BUG: no SFENCE before flush
CXL::store_fence();
```

#### Site 9: `cxl_manager.cc` — initialization flushes (lines ~332, ~362, ~404, ~647)
```cpp
control_block_->epoch.store(1, std::memory_order_release);
CXL::flush_cacheline(control_block_);   // BUG: no SFENCE before flush
CXL::store_fence();
```

#### Site 10: `disk_manager.cc` — replication_done writes (line ~644-652)
```cpp
// After updating replication_done value:
CXL::flush_cacheline(rep_done_addr);   // BUG: no SFENCE before flush
// SFENCE after both flushes
```

### READER SITES (CORRECT — no fix needed)

Reader flushes use `flush_cacheline` to *invalidate* the local cache before reading. These are followed by `load_fence()` or `full_fence()`, not `store_fence()`. The pattern is:
```cpp
CXL::flush_cacheline(addr);   // invalidate stale cache
CXL::load_fence();            // ensure invalidation completes before load
value = *addr;                // read fresh from CXL
```
These are NOT affected by the bug because there are no preceding stores that need ordering.

---

## 4. The Fix

### Approach: Add SFENCE Before Every Writer CLFLUSHOPT

For every site where stores precede `CXL::flush_cacheline()`, insert `CXL::store_fence()` between the last store and the first flush. The trailing `CXL::store_fence()` after the flush(es) remains.

**Example fix for PublishPBRSlotDirect:**
```cpp
memcpy(batch_header_location, &published, sizeof(BatchHeader));
__atomic_store_n(&batch_header_location->batch_complete, 1, __ATOMIC_RELEASE);

CXL::store_fence();  // ← ADD: drain stores to cache before flushing
CXL::flush_cacheline(batch_header_location);
const void* batch_header_next_line = reinterpret_cast<const void*>(
    reinterpret_cast<const uint8_t*>(batch_header_location) + 64);
CXL::flush_cacheline(batch_header_next_line);
CXL::store_fence();  // ensure flushes complete (already exists)
```

### Performance Impact

`_mm_sfence()` is a lightweight instruction on modern x86 (~10-30 cycles). On the hot publish path, this adds at most ~30ns per batch — negligible compared to the TCP recv, memcpy, and CLFLUSHOPT costs. The alternative (random intermittent data loss) is catastrophic.

### For CV Flush Loops (Sites 4 & 5)

The CV flush functions iterate over brokers, doing CAS writes then a single flush per broker. The SFENCE should go before each per-broker flush, not just at the end:

```cpp
for (int broker_id = 0; broker_id < NUM_MAX_BROKERS; ++broker_id) {
    // ... CAS writes to entry->sequencer_logical_offset, etc. ...
    if (advanced_logical || advanced_pbr) {
        CXL::store_fence();          // ← ADD: drain CAS results before flush
        CXL::flush_cacheline(entry);
    }
}
CXL::store_fence();  // final fence (already exists)
```

---

## 5. Clean-up: Remove Diagnostic Instrumentation

After the fix is validated, remove these diagnostics that were added during investigation:

1. **PBR counters** in `topic.h` (~line 809): `pbr_slots_reserved_`, `pbr_slots_published_` and their getters (~line 463)
2. **PBR counter increments** in `topic.cc` (~line 1930, ~1950)
3. **`[PUB_CLOSE]` diagnostic** in `network_manager.cc` (~line 1204)
4. **`[SCANNER_EMPTY_STUCK]` diagnostic block** in `BrokerScannerWorker5` (~lines 5718-5759 in topic.cc)
5. **`[B0_PBR_WRITE]` diagnostic** in `ReservePBRSlotAfterRecv` (~lines 1922-1928)

Keep the `[SEQ5_IDLE_DIAG]` logging as it provides useful operational visibility.

---

## 6. How to Build

```bash
cd /home/domin/Embarcadero
mkdir -p build && cd build
cmake ..
# Build just the embarlet binary (faster than full build):
cmake --build . --target embarlet -j$(nproc)
# The binary lands at: build/bin/embarlet
```

Full rebuild takes ~3-5 minutes. Incremental rebuild after editing topic.cc takes ~30-60 seconds.

---

## 7. How to Run Experiments

### Quick Single-Trial Test

```bash
cd /home/domin/Embarcadero
export SYSTEM=EMBARCADERO ORDER=5 SEQUENCER=EMBARCADERO \
       NUM_CLIENTS=3 REPLICATION_FACTOR=1 ACK_LEVEL=1 \
       NUM_TRIALS=1 NUM_BROKERS=4 \
       PUBLICATION_BROKER_CONFIG=config/embarcadero.yaml \
       PUBLICATION_CLIENT_CONFIG=config/client.yaml
bash scripts/publication/run_throughput_cell.sh
```

### Full 10-Trial Stress Test (validation)

```bash
cd /home/domin/Embarcadero
export SYSTEM=EMBARCADERO ORDER=5 SEQUENCER=EMBARCADERO \
       NUM_CLIENTS=3 REPLICATION_FACTOR=1 ACK_LEVEL=1 \
       NUM_TRIALS=10 NUM_BROKERS=4 \
       PUBLICATION_BROKER_CONFIG=config/embarcadero.yaml \
       PUBLICATION_CLIENT_CONFIG=config/client.yaml
bash scripts/publication/run_throughput_cell.sh
```

### Cluster Topology

The test cluster is a multi-node setup:
- **moscxl** (this machine, 10.10.10.10): Runs all 4 brokers (B0=head, B1-B3=followers) on NUMA node 1, with CXL memory on NUMA node 2
- **c4**: Remote client machine (SSH)
- **c3**: Remote client machine (SSH)
- **local**: Client on this machine (NUMA node 0)

The `scripts/run_multiclient.sh` orchestrates:
1. Start brokers locally (head + followers)
2. Wait for all brokers to be ready
3. Launch clients on c4, local, c3 with a synchronized barrier start
4. Wait for all clients to complete (ACK timeout = 120s)
5. Collect results

### Interpreting Results

- **Success**: All trials print `bandwidth:` lines and exit 0
- **Failure**: A client reports `ACK timeout` or `waitForAcks timed out` — this means the scanner stalled and the CV never advanced far enough, so the publisher's ACK never arrived
- **Logs**: Broker logs are in `/tmp/broker_*.log` during a run, and copied to `multiclient_logs/` on failure. The head broker (broker_0.log) contains scanner and sequencer logs.

### What Success Looks Like

```
=== Trial 1 (EMBARCADERO Order 5, 4 brokers, msg=1024) ===
Trial 1 attempt 1/3
...
bandwidth: 3847.25 MB/s
...
=== Trial 10 (EMBARCADERO Order 5, 4 brokers, msg=1024) ===
Trial 10 attempt 1/3
...
bandwidth: 3912.11 MB/s
Done.
```

All 10 trials passing on the first attempt with zero ACK timeouts = fix validated.

---

## 8. Files You Will Edit

| File | What to change |
|---|---|
| `src/embarlet/topic.cc` | Fix Sites 1-6: add `CXL::store_fence()` before writer flushes. Remove diagnostic instrumentation. |
| `src/embarlet/topic.h` | Remove PBR counter members and getters (diagnostic cleanup). |
| `src/network_manager/network_manager.cc` | Remove `[PUB_CLOSE]` diagnostic. |
| `src/cxl_manager/cxl_manager.cc` | Fix Site 9: init flushes + sequencer flushes. |
| `src/disk_manager/disk_manager.cc` | Fix Site 10: replication_done flushes. |
| `src/disk_manager/chain_replication.cc` | Fix Site 8: CV tail flushes. |
| `src/cxl_manager/cxl_manager.h` | Fix the TInode flush in `UpdateBrokerOrderedOffset`. |

### Important: Also fix the documented usage pattern

In `src/common/performance_utils.h`, the doc comment for `flush_cacheline()` (around line 167-170) shows the wrong pattern:
```cpp
// Usage pattern (Paper spec):
//   msg_header->field = value;
//   CXL::flush_cacheline(msg_header);   ← WRONG, missing SFENCE before
//   CXL::store_fence();
```
Fix this to:
```cpp
// Usage pattern (Paper spec):
//   msg_header->field = value;
//   CXL::store_fence();                 // drain stores to cache
//   CXL::flush_cacheline(msg_header);   // flush cache line to CXL
//   CXL::store_fence();                 // ensure flush completes
```

---

## 9. Verification Checklist

1. [ ] Build succeeds with zero warnings related to your changes
2. [ ] All writer flush sites have SFENCE before CLFLUSHOPT
3. [ ] All reader flush sites are unchanged (flush → load_fence pattern)
4. [ ] Diagnostic instrumentation is removed
5. [ ] Documentation in performance_utils.h updated
6. [ ] 10-trial stress test passes with zero failures

---

## 10. Context: Previous Fixes Already Applied (do NOT revert)

These fixes are already in the codebase and are correct:

1. **Batch expiry in hold buffer** (`ProcessLevel5BatchesShard`): Fixed stale batches blocking the hold buffer drain, causing the original B2 last-batch stall.

2. **CLAIMED slot skip livelock** (`BrokerScannerWorker5`): Fixed a livelock where the scanner would repeatedly attempt to push a SKIP marker for a timed-out CLAIMED slot, fail, and reset its timer — preventing forward progress.

3. **Log preservation on failure** (`scripts/run_multiclient.sh`): Failed attempts now copy broker logs to `multiclient_logs/trial${trial}_attempt${attempt}_broker${b}.log`.

Do not modify these fixes. They address real bugs orthogonal to the CLFLUSHOPT ordering issue.
