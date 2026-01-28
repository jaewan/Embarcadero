# Embarcadero Publish Pipeline: Complete Walkthrough

**Date:** 2026-01-28
**Purpose:** Explain the end-to-end publish pipeline for developers unfamiliar with the codebase.

---

## Overview

Embarcadero is a distributed shared log that leverages disaggregated CXL memory. When a publisher sends messages, they flow through a multi-stage pipeline from the client to shared memory, get globally ordered by a sequencer, and are acknowledged back to the client.

```
Publisher → Network → Broker → Sequencer → Acknowledgment
  (Client)   (TCP)   (Receiver) (Order5)    (AckThread)
```

---

## Stage 1: Publisher (Client Side)

**File:** `src/client/publisher.cc`

### 1.1 Buffering

Publishers don't send messages individually. Instead, they batch them:

- **Buffer Management:** Each publisher thread maintains a `Buffer` that accumulates messages
- **Batch Formation:** When a buffer reaches ~2MB (configurable `batch_size`), it's sealed and queued for transmission
- **Message Metadata:** Each message gets a `BlogMessageHeader` with:
  - `client_id`: Unique publisher identifier
  - `batch_seq`: Sequence number within this client's batches (0, 1, 2, ...)
  - `msg_seq_in_batch`: Position within the batch
  - `payload_size`: Size of the message payload

**Key Point:** The `batch_seq` is critical for ORDER=5 FIFO semantics. The sequencer must process batches 0, 1, 2, ... in order for each client.

### 1.2 Network Transmission

**Files:** `src/client/publisher.cc` (PublishThread)

- **Connection Pool:** Each publisher maintains TCP connections to all brokers
- **Load Balancing:** Batches are distributed round-robin across brokers
- **Transmission Protocol:**
  1. Send batch metadata (client_id, batch_seq, num_messages, total_size)
  2. Send serialized message payloads
  3. Keep socket open for receiving acknowledgments

**Parallelism:** Multiple PublishThreads (default: 4 per broker × 4 brokers = 16 threads) send batches concurrently.

---

## Stage 2: Broker Reception (NetworkManager)

**File:** `src/network_manager/network_manager.cc`

### 2.1 Receiving Data

Each broker runs a `NetworkManager` with:
- **I/O Threads:** 12 threads handle incoming TCP connections
- **Zero-Copy Path:** Data flows directly from socket → CXL memory (no intermediate copy)

### 2.2 Batch Header Ring Allocation

**Critical Data Structure:** Each broker has a **batch header ring** in CXL memory.

```
Broker's Batch Header Ring (1MB = 8,192 slots by default)
┌────────┬────────┬────────┬───┬────────┐
│ Slot 0 │ Slot 1 │ Slot 2 │...│ Slot N │ (wraps around)
└────────┴────────┴────────┴───┴────────┘
   128B     128B     128B         128B
```

Each slot holds a `BatchHeader` (128 bytes):
```cpp
struct BatchHeader {
    uint32_t num_msg;           // Number of messages in batch
    uint32_t batch_complete;    // 1 = ready for sequencer
    uint64_t log_idx;           // Offset to payload in CXL log
    uint64_t batch_seq;         // Client's batch sequence number
    uint64_t client_id;         // Publisher identifier
    // ... other metadata
};
```

**Allocation Process (GetCXLBuffer):**

1. **Allocate Batch Header Slot:**
   - Read `batch_headers_` pointer (current position in ring)
   - Assign slot, advance pointer by sizeof(BatchHeader)
   - **Ring Wrap Check:** If pointer >= ring end, check if it's safe to wrap:
     ```cpp
     if (batch_headers_ >= batch_headers_end) {
         // Read consumed_through to see if slot 0 is free
         if (consumed >= sizeof(BatchHeader)) {
             batch_headers_ = ring_start;  // Safe to wrap
         } else {
             // Ring full: spin-wait for sequencer to consume slot 0
         }
     }
     ```

2. **Allocate Payload Space:**
   - Allocate space in the CXL log region for message payloads
   - Store offset in `BatchHeader.log_idx`

3. **Receive Payload:**
   - Read message data from socket directly into CXL log
   - Parse message headers to populate batch metadata

4. **Mark Ready:**
   - Set `batch_complete = 1`
   - Flush cache line to CXL (ensures visibility to sequencer)
   - **This signals the sequencer that the batch is ready to process**

**Key Insight:** The batch header ring is the synchronization point between receiver and sequencer. Once `batch_complete=1` and flushed, the batch is visible to the sequencer.

---

## Stage 3: Sequencer (BrokerScannerWorker5)

**File:** `src/embarlet/topic.cc` (BrokerScannerWorker5)

The head broker runs sequencer threads that scan all brokers' batch header rings and assign global order.

### 3.1 Polling for Ready Batches

Each `BrokerScannerWorker5` thread (one per broker) continuously scans that broker's ring:

```cpp
while (true) {
    // 1. Check if batch is ready
    if (current_batch_header->num_msg == 0 ||
        current_batch_header->batch_complete == 0) {
        // Not ready, try ProcessSkipped5(), then advance
        ProcessSkipped5();
        current_batch_header++;  // Move to next slot
        if (current_batch_header >= ring_end)
            current_batch_header = ring_start;  // Wrap
        continue;
    }

    // 2. Batch is ready! Extract metadata
    client_id = current_batch_header->client_id;
    batch_seq = current_batch_header->batch_seq;
    num_msg = current_batch_header->num_msg;

    // 3. FIFO Validation: Check if this is the expected batch_seq
    //    for this client...
}
```

### 3.2 FIFO Ordering (ORDER=5)

**Critical for Correctness:** ORDER=5 guarantees that batches from the same client appear in the global log in the order they were sent (batch_seq order).

**Implementation:**

1. **Per-Client Tracking:** Maintain `next_expected_batch_seq_[client_id]`
   - For new client: expect batch_seq = 0
   - After processing batch N: expect batch_seq = N+1

2. **In-Order Processing:**
   ```cpp
   if (batch_seq == expected_seq) {
       // Process immediately
       start_total_order = global_seq_.fetch_add(num_msg);
       next_expected_batch_seq_[client_id]++;
       AssignOrder5(batch_header, start_total_order);
   }
   ```

3. **Out-of-Order Deferral:**
   ```cpp
   else if (batch_seq > expected_seq) {
       // Batch arrived too early, defer it
       skipped_batches_5_[client_id][batch_seq] = batch_header;
   }
   else {
       // Duplicate or old batch, ignore
   }
   ```

4. **Deferred Batch Processing (ProcessSkipped5):**
   - Called periodically (every 10 batches or when idle)
   - Scans `skipped_batches_5_` for batches that are now ready
   - If batch N arrived and we've processed 0..N-1, process batch N

**Example Scenario:**

```
Time 1: Batches arrive: 0, 1, 2, 3, 18, 17, 16
  Process: 0, 1, 2, 3
  Defer: 16, 17, 18 (waiting for 4-15)

Time 2: Batches arrive: 4, 5, 6, ...
  Process: 4 → triggers ProcessSkipped5 → checks if 5 is ready → ...
  Eventually processes deferred 16, 17, 18 when 4-15 arrive
```

### 3.3 Global Order Assignment (AssignOrder5)

Once a batch passes FIFO validation:

```cpp
void AssignOrder5(BatchHeader* batch, size_t start_total_order) {
    int broker_id = batch->broker_id;

    // 1. Assign global sequence numbers
    //    Batch with N messages gets: [start_total_order, start_total_order+N)

    // 2. Update BatchMetadata in CXL
    for (int i = 0; i < batch->num_msg; i++) {
        size_t msg_offset = batch->log_idx + i * message_size;
        BatchMetadata* meta = GetBatchMetadata(msg_offset);
        meta->batch_total_order = start_total_order + i;
    }

    // 3. Update broker's ordered count (for ACK tracking)
    tinode_->offsets[broker_id].ordered += batch->num_msg;

    // 4. Flush to CXL (ensure AckThread sees updates)
    CXL::flush_cacheline(&tinode_->offsets[broker_id].ordered);
    CXL::store_fence();

    // 5. Mark batch header as consumed
    batch->batch_complete = 0;
    batch->num_msg = 0;  // Signals receiver can reuse this slot
    CXL::flush_cacheline(batch);

    // 6. Update consumed_through (see Stage 5)
}
```

**Concurrency Control:**

- `global_seq_` is an atomic counter (lock-free `fetch_add`)
- `next_expected_batch_seq_` is protected by striped mutexes (32 stripes by client_id)
- This allows multiple sequencer threads to process different clients in parallel

---

## Stage 4: Acknowledgment (AckThread)

**File:** `src/network_manager/network_manager.cc` (AckThread)

Each broker runs an `AckThread` that monitors ordered progress and sends ACKs to publishers.

### 4.1 Monitoring Progress

```cpp
size_t GetOffsetToAck(const char* topic, uint32_t ack_level) {
    if (ack_level == 0) return -1;  // No ACKs

    if (ack_level == 1) {  // ACK after ordering
        if (order > 0) {
            // Invalidate cache to see sequencer's updates
            CXL::flush_cacheline(&tinode->offsets[broker_id].ordered);
            CXL::load_fence();
            return tinode->offsets[broker_id].ordered;
        }
    }

    if (ack_level == 2) {  // ACK after replication
        // Check all replicas' replication_done counters
        return min(replication_done[replica_i]);
    }
}
```

**Non-Coherent CXL:** The sequencer (on head broker) writes `ordered`. The AckThread (on each broker) must **invalidate its cache** to see the latest value. Otherwise, it reads stale data!

### 4.2 Sending ACKs

```cpp
while (true) {
    size_t ack_offset = GetOffsetToAck(topic, ack_level);

    if (ack_offset > last_acked_offset) {
        // Send ACK to publisher
        uint64_t ack_count = ack_offset;
        send(publisher_socket, &ack_count, sizeof(ack_count));
        last_acked_offset = ack_offset;
    }

    // Configurable spin/sleep pattern
    spin(500µs);
    if (no_progress) sleep(1ms);
}
```

**ACK Flow:**
1. Sequencer assigns order → updates `tinode->offsets[broker].ordered`
2. AckThread reads (with cache invalidation) → sees new count
3. AckThread sends count to publisher via TCP
4. Publisher's EpollAckThread receives → updates `ack_received_`
5. Publisher's Poll() sees `ack_received_ >= messages_sent` → returns

---

## Stage 5: Critical Issue - Batch Header Ring Wrap

### The Problem

**Current State (as of 2026-01-28):**

When batches arrive out of order (e.g., 0,1,2,3,18,17,16,19,32...), many batches are deferred in `skipped_batches_5_`. The sequencer processes in-order batches (0-3) then waits for missing batches (4-17).

**The Ring Wrap Bug:**

1. **Receiver keeps allocating:** Broker continues receiving batches 32, 33, 34, ... and allocating slots
2. **Sequencer stalls:** Can't process batches 18+ because 4-17 are missing
3. **consumed_through doesn't advance:** Sequencer only updates it for in-order batches
4. **Ring fills up:** Eventually, receiver allocates all 8,192 slots
5. **Receiver wraps:** Checks `consumed_through >= sizeof(BatchHeader)`, sees slot 0 is consumed, wraps
6. **Overwrites deferred batches:** Receiver now allocates slots 1, 2, 3, ... **overwriting batch headers that haven't been processed yet!**
7. **Deferred batches destroyed:** ProcessSkipped5 can't process batches 4-17 because their headers are gone
8. **System stalls:** Sequencer never processes remaining batches → no ACKs → publisher times out at ~37% completion

### consumed_through Protocol

**Design Intent (from LOCKFREE_BATCH_HEADER_RING_DESIGN.md):**

```cpp
// INVARIANT: All slots with offset < consumed_through are consumed
//
// Producer (GetCXLBuffer):
//   if (wrapping && consumed_through >= sizeof(BatchHeader))
//       wrap();  // Safe, slot 0 is consumed
//
// Consumer (BrokerScannerWorker5):
//   after processing slot at offset O:
//       consumed_through = O + sizeof(BatchHeader);
```

**The Bug:**

When batch at slot 1000 is processed **before** slot 999, setting `consumed_through = 1001 * sizeof(BatchHeader)` is **wrong** because slot 999 is still deferred. The producer sees slot 999 is "consumed" and overwrites it!

**Attempted Fix (incomplete):**

Track minimum unconsumed offset and only update `consumed_through` when processing slots in order. But this fix is incomplete—it only prevents wrapping to slot 0, not overwriting subsequent slots.

**Workaround (to be tested):** Increase ring size from 1MB (8,192 slots) to 10MB (81,920 slots) so the ring never wraps during normal 10GB runs (~1,359 batches/broker).

---

## Key Data Structures

### TInode.offset_entry (per broker)

**File:** `src/cxl_manager/cxl_datastructure.h`

```cpp
struct alignas(64) offset_entry {
    // BROKER REGION (bytes 0-255): Written by broker, read by sequencer
    volatile size_t log_offset;              // Next write position in CXL log
    volatile size_t batch_headers_offset;    // Start of batch header ring
    volatile size_t written_addr;            // Last written address

    // REPLICATION REGION (bytes 256-511): Written by replication threads
    volatile uint64_t replication_done[NUM_MAX_BROKERS];

    // SEQUENCER REGION (bytes 512-767): Written by sequencer, read by AckThread
    volatile uint64_t ordered;               // Count of ordered messages
    volatile size_t ordered_offset;          // Last ordered log offset
    volatile size_t batch_headers_consumed_through;  // Ring wrap protocol
};
```

**Cache Line Alignment:** Each region is 256B (4 cache lines). Ensures no false sharing between broker/sequencer/replication threads.

### BatchHeader (in ring)

```cpp
struct alignas(128) BatchHeader {
    uint32_t num_msg;           // 0 = slot empty, N = N messages in batch
    uint32_t batch_complete;    // 0 = processing/consumed, 1 = ready for sequencer
    uint64_t log_idx;           // CXL offset to first message
    uint64_t batch_seq;         // Client's sequence number (FIFO ordering)
    uint64_t client_id;         // Publisher ID
    int broker_id;              // Which broker received this
    // ... padding to 128B
};
```

**Lifecycle:**
1. Receiver allocates slot → sets all fields
2. Receiver sets `batch_complete=1`, flushes → visible to sequencer
3. Sequencer processes → sets `batch_complete=0`, `num_msg=0`, flushes → slot reusable
4. Receiver can reuse slot after seeing `num_msg=0`

---

## Performance Characteristics

### Design Goals
- **10 GB/s bandwidth:** Target throughput for 10GB publish
- **Low latency:** Sub-millisecond message ordering
- **Strong consistency:** ORDER=5 FIFO + total order

### Current Bottlenecks (as of 2026-01-28)

1. **Batch Header Ring Wrap:** Causes 37% stall at 10GB (root cause identified above)
2. **ProcessSkipped5 Locking:** Locks all 32 stripes, processes one batch, unlocks → N lock cycles for N deferred batches (fixed: now processes all in one lock)
3. **Out-of-Order Arrival:** High variance in batch arrival order causes many deferrals

### Optimizations Applied

1. **Striped Mutex:** 32 stripes keyed by client_id reduce contention
2. **Batch Processing:** Receiver/sequencer work at batch granularity (~2000 messages/batch)
3. **Zero-Copy:** Data flows socket → CXL without intermediate copy
4. **Lock-Free Sequencing:** `global_seq_` uses atomic fetch_add
5. **Configurable Backoff:** AckThread and BrokerScannerWorker5 use adaptive spin/sleep

---

## Testing the Pipeline

### Bandwidth Measurement

**Script:** `scripts/measure_bandwidth_proper.sh`

```bash
# 100MB test (quick sanity check)
TOTAL_MESSAGE_SIZE=104857600 NUM_ITERATIONS=1 bash scripts/measure_bandwidth_proper.sh

# 10GB test (target: 9-10 GB/s)
bash scripts/measure_bandwidth_proper.sh
```

**Result Location:** `data/throughput/pub/result.csv` (column 12 = pub_bandwidth_mbps)

### Configuration

**File:** `config/embarcadero.yaml`

```yaml
storage:
  batch_size: 2097152              # 2MB batch size
  batch_headers_size: 1048576      # 1MB ring (8,192 slots) - TO BE INCREASED
  segment_size: 4294967296         # 4GB segments

network:
  io_threads: 12                   # Receiver threads per broker
```

### Environment Variables

```bash
# ACK timeout (for testing)
export EMBARCADERO_ACK_TIMEOUT_SEC=90

# AckThread tuning
export EMBARCADERO_ACK_SPIN_US=500
export EMBARCADERO_ACK_DRAIN_US=1000
```

---

## Known Issues & Limitations

1. **Batch Header Ring Wrap Bug** (critical): Causes 37% stall at 10GB scale
   - **Status:** Root cause identified
   - **Workaround:** Increase ring size to 10MB
   - **Proper Fix:** Modify GetCXLBuffer to check consumed_through before EACH slot allocation, not just at wrap

2. **ORDER=4 Not Supported:** May hang indefinitely (see `docs/memory-bank/known_limitations.md`)

3. **ORDER=1 Not Implemented:** Sequencer not ported

4. **Sequencer Recovery:** `next_expected_batch_seq_` is in-memory only (lost on crash)

---

## References

- **Paper:** `Paper/Text/Introduction.tex` - Original design
- **Ring Protocol:** `docs/memory-bank/LOCKFREE_BATCH_HEADER_RING_DESIGN.md`
- **Deviations:** `docs/memory-bank/spec_deviation.md`
- **Data Structures:** `src/cxl_manager/cxl_datastructure.h`
- **Handoff Context:** `docs/HANDOFF_CLAUDE_CODE.md`

---

**End of Document**
