
You are working on **Embarcadero**, a CXL-based pub/sub message broker. Your task is to optimize the ORDER=5 sequencer by eliminating per-message CXL writes.
First create a new git branch per-msg-order and work from the branch.
### System Architecture (What You Need to Know)

Embarcadero is a distributed message broker that uses **CXL (Compute Express Link) memory** as shared storage between broker threads. The hot path is:

1. **Publisher** batches messages (e.g., 500 × 1KB = 512KB batch), sends over TCP to broker.
2. **NetworkManager (receiver thread)** writes batch payload into CXL BLog (Broker Log) and writes a `BatchHeader` entry into the PBR (Per-Broker Ring — a circular buffer of 128-byte `BatchHeader` slots in CXL).
3. **Sequencer thread** (`BrokerScannerWorker5` in `topic.cc`) polls PBR for new batches, enforces per-client FIFO via `batch_seq` tracking, assigns a global `total_order` via atomic `fetch_add`, then calls `AssignOrder()` to stamp metadata.
4. **Subscribe thread** (`network_manager.cc:1368`) reads ordered batches from CXL via `GetBatchToExportWithMetadata()`, prepends a 16-byte `BatchMetadata` wire header, and sends batch payload over TCP to subscribers.
5. **Subscriber** (`subscriber.cc`) parses `BatchMetadata` to know `batch_total_order` and `num_messages`, then derives each message's `total_order` incrementally.

**CXL is non-coherent** — writes require explicit `CXL::flush_cacheline()` + `CXL::store_fence()` to be visible to other cores/threads.

### The Problem

`Topic::AssignOrder()` in `src/embarlet/topic.cc:681-749` currently performs **3 CXL writes + 1 flush + 1 fence PER MESSAGE** in the batch:

```cpp
// src/embarlet/topic.cc:706-737 (CURRENT CODE — THE HOT PATH TO OPTIMIZE)
for (size_t i = 0; i < num_messages; ++i) {
    // Wait for each message to arrive in CXL (paddedSize written by receiver)
    while (msg_header->paddedSize == 0) {
        if (stop_threads_) return;
        std::this_thread::yield();
    }
    size_t current_padded_size = msg_header->paddedSize;

    // Per-message CXL writes (THIS IS THE WASTE):
    msg_header->logical_offset = logical_offset++;   // CXL write #1
    msg_header->total_order = seq++;                 // CXL write #2
    msg_header->next_msg_diff = current_padded_size; // CXL write #3
    CXL::flush_cacheline(msg_header);                // CXL flush
    CXL::store_fence();                              // store fence

    tinode_->offsets[broker].ordered++;
    tinode_->offsets[broker].written++;

    msg_header = reinterpret_cast<MessageHeader*>(
        reinterpret_cast<uint8_t*>(msg_header) + current_padded_size);
}
// After loop:
header_for_sub->batch_off_to_export = (uint8_t*)batch_to_order - (uint8_t*)header_for_sub;
header_for_sub->ordered = 1;
```

For a 512KB batch with 500 messages: **500 writes + 500 flushes + 500 fences** in the sequencer hot path.

### Why These Per-Message Writes Are Redundant

The subscriber **already derives per-message `total_order` from the batch header**, not from per-message CXL fields:

**Broker subscribe thread** (`src/network_manager/network_manager.cc:1387-1398`):
```cpp
// Prepends BatchMetadata to each batch on the wire:
batch_meta.batch_total_order = batch_total_order;  // from BatchHeader.total_order
batch_meta.num_messages = num_messages;
batch_meta.header_version = header_version;
// Then sends batch payload bytes (messages) after this 16-byte prefix
```

**Subscriber** (`src/client/subscriber.cc:1571, 1595-1596`):
```cpp
// Reads batch_total_order from wire:
batch_state.next_message_order_in_batch = potential_metadata->batch_total_order;
// Derives per-message total_order incrementally:
if (v2_hdr->total_order == 0) {
    v2_hdr->total_order = batch_state.next_message_order_in_batch++;
}
```

**Conclusion:** The subscriber never reads `msg_header->total_order`, `msg_header->logical_offset`, or `msg_header->next_msg_diff` from CXL. It reconstructs everything from the batch-level `BatchMetadata` wire header.

### Key Data Structures

**CXL BatchHeader** (`src/cxl_manager/cxl_datastructure.h:388-422`, 128 bytes, 2 cache lines):
```cpp
struct alignas(64) BatchHeader {
    size_t batch_seq;              // Per-client sequence number
    uint32_t client_id;
    uint32_t num_msg;
    volatile uint32_t batch_complete;  // Set by NetworkManager when all payload received
    uint32_t broker_id;
    uint32_t ordered;              // Set to 1 by sequencer when globally ordered
    uint32_t flags;                // CLAIMED/VALID
    uint16_t epoch_created;
    uint64_t batch_id;
    uint64_t pbr_absolute_index;
    size_t total_size;             // Total payload bytes
    size_t start_logical_offset;
    size_t log_idx;                // Offset into BLog where payload lives
    size_t total_order;            // Global sequence number (set by sequencer)
    size_t batch_off_to_export;    // Export chain pointer (set by sequencer)
};
```

**CXL MessageHeader** (`src/cxl_manager/cxl_datastructure.h:434-443`, 64 bytes):
```cpp
struct alignas(64) MessageHeader {
    volatile size_t paddedSize;            // Set by publisher (pre-filled before send)
    void* segment_header;
    size_t logical_offset;                 // Set by sequencer (REDUNDANT for ORDER=5)
    volatile unsigned long long next_msg_diff;  // Set by sequencer (REDUNDANT for ORDER=5)
    volatile size_t total_order;           // Set by sequencer (REDUNDANT for ORDER=5)
    size_t client_order;
    size_t client_id;
    size_t size;
};
```

**Wire BatchMetadata** (`src/common/wire_formats.h:100-108`, 16 bytes):
```cpp
struct BatchMetadata {
    size_t batch_total_order;      // Starting total_order for this batch
    uint32_t num_messages;
    uint16_t header_version;       // 1=MessageHeader, 2=BlogMessageHeader
    uint16_t flags;
};
```

### What To Change

**Goal:** Replace the O(N) per-message loop in `AssignOrder()` with O(1) batch-level stamping.

**Step 1 — Use `batch_complete` for ORDER=5 readiness**

Currently ORDER=5's `AssignOrder()` waits per-message (`while (msg_header->paddedSize == 0)`). The NetworkManager already sets `batch_complete = 1` on the BatchHeader after all payload is received (`network_manager.cc:1096`). Change the sequencer to wait on `batch_complete` instead of per-message polling.

The scanner loop (`BrokerScannerWorker5`, `topic.cc:526`) already checks `batch_complete` for sequencer 5 flow — verify this and ensure `AssignOrder()` is only called after batch_complete is set. If so, the per-message `paddedSize` wait can be removed entirely.

**Step 2 — Simplify `AssignOrder()`**

Replace the per-message loop with:
```cpp
void Topic::AssignOrder(BatchHeader *batch_to_order, size_t start_total_order,
                        BatchHeader* &header_for_sub) {
    size_t num_messages = batch_to_order->num_msg;
    if (num_messages == 0) return;

    // Batch-level total_order (already exists on line 702)
    batch_to_order->total_order = start_total_order;

    // Bulk tinode watermark update
    int broker = batch_to_order->broker_id;
    tinode_->offsets[broker].ordered += num_messages;
    tinode_->offsets[broker].written += num_messages;

    // Mark batch ready for subscribe export
    header_for_sub->batch_off_to_export =
        (uint8_t*)batch_to_order - (uint8_t*)header_for_sub;
    header_for_sub->ordered = 1;
    header_for_sub = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(header_for_sub) + sizeof(BatchHeader));

    // Update ordered_offset for replication stage
    size_t ordered_offset = static_cast<size_t>(
        reinterpret_cast<uint8_t*>(batch_to_order) - reinterpret_cast<uint8_t*>(cxl_addr_));
    tinode_->offsets[broker].ordered_offset = ordered_offset;
}
```

**Step 3 — Verify `next_msg_diff` consumers don't need it for ORDER=5**

`next_msg_diff` is used by these CXL-local readers — verify none apply to ORDER=5:

| Consumer | File:Line | Used for ORDER=5? |
|----------|-----------|-------------------|
| `DelegationThread` | `topic.cc:373` | **NO** — DelegationThread is for ORDER=0 only. ORDER=5 uses `BrokerScannerWorker5`. |
| `CorfuGetCXLBuffer` replication callback | `topic.cc:922` | **NO** — CORFU only (ORDER=2). |
| `SetOrder0Written` / `FinalizeOrder0WrittenIfNeeded` | `topic.cc:1345,1397` | **NO** — ORDER=0 only. |
| `GetMessageAddr` (ORDER=0 subscribe) | `message_export.cc:101,107` | **NO** — ORDER=0 subscribe path only. ORDER=5 uses `GetBatchToExportWithMetadata`. |
| `cxl_manager.cc` sequencers | `cxl_manager.cc:800,822,936,991` | **NO** — These are ORDER=1,2,3 sequencers in CXL manager. ORDER=5 sequencer is `BrokerScannerWorker5` in `topic.cc`. |
| `buffer_manager.cc:114` | `buffer_manager.cc:114` | **VERIFY** — Check if buffer_manager is used in ORDER=5 path. |
| `disk_manager.cc:1095,1104` | `disk_manager.cc:1095,1104` | **VERIFY** — Check if disk persistence for ORDER=5 walks messages via `next_msg_diff`. |

**Step 4 — Verify `logical_offset` is not needed**

For ORDER=5, `logical_offset` in per-message headers is used only by the sequencer's own tinode watermark tracking (`tinode_->offsets[broker].ordered++`). With bulk updates this field is no longer needed in the message header.

Check: does `GetBatchToExportWithMetadata` (subscribe export) or the replication path read per-message `logical_offset`? If not, it's safe to skip.

**Step 5 — Test Plan**

1. **Correctness:** Single-broker ORDER=5 throughput test — verify subscriber receives all messages with correct total_order sequence (no gaps, no duplicates).
   ```bash
   # From scripts/
   ORDER=5 NUM_BROKERS=1 ./run_throughput.sh
   ```

2. **Multi-broker correctness:** 4-broker ORDER=5 test.
   ```bash
   ORDER=5 NUM_BROKERS=4 ./run_throughput.sh
   ```

3. **Performance comparison:** ORDER=0 vs ORDER=5 (optimized). Target: ORDER=5 within 5% of ORDER=0 throughput.

4. **Subscriber ordering verification:** Add a check in subscriber that `total_order` values are monotonically increasing (no regression in order correctness).

### Files to Read First

Read these files to understand the full context before making changes:

1. `src/embarlet/topic.cc` — lines 520-749 (scanner + AssignOrder) — **THE MAIN CHANGE**
2. `src/cxl_manager/cxl_datastructure.h` — lines 388-443 (BatchHeader + MessageHeader structs)
3. `src/network_manager/network_manager.cc` — lines 1096-1155 (batch_complete + PBR publish) and lines 1368-1399 (subscribe export with BatchMetadata)
4. `src/client/subscriber.cc` — lines 1556-1610 (BatchMetadata parsing + total_order derivation)
5. `src/common/wire_formats.h` — lines 96-112 (BatchMetadata struct)
6. `docs/ORDER5_BATCH_LEVEL_ORDERING_OPTIMIZATION.md` — detailed design doc

### Constraints

- Do NOT change the subscriber wire protocol (BatchMetadata format must stay the same).
- Do NOT change ORDER=0, ORDER=2 (CORFU), or any other order's code paths.
- CXL writes MUST be followed by `CXL::flush_cacheline()` + `CXL::store_fence()` for visibility. But the point is we need far fewer of them.
- The `batch_to_order->total_order = start_total_order` write (line 702) must stay — the subscribe thread reads it via `GetBatchToExportWithMetadata`.
- Keep `header_for_sub->ordered = 1` and `batch_off_to_export` — the subscribe thread polls these.
- Preserve `tinode_->offsets[broker].ordered_offset` update — replication depends on it.
