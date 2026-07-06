# ORDER=5 Optimization: Batch-Level Ordering (Eliminate Per-Message CXL Stamping)

**Status:** Proposed — implement after merging `latency-work` to `main`, on a new branch.

## Problem

The ORDER=5 (Embarcadero epoch) sequencer thread in `AssignOrder()` performs **3 CXL operations per message**:

```cpp
// src/embarlet/topic.cc:706-737 (current code)
for (size_t i = 0; i < num_messages; ++i) {
    while (msg_header->paddedSize == 0) { std::this_thread::yield(); }
    size_t current_padded_size = msg_header->paddedSize;

    msg_header->logical_offset = logical_offset++;   // CXL write
    msg_header->total_order = seq++;                 // CXL write
    msg_header->next_msg_diff = current_padded_size; // CXL write
    CXL::flush_cacheline(msg_header);                // CXL flush
    CXL::store_fence();                              // store fence

    tinode_->offsets[broker].ordered++;
    tinode_->offsets[broker].written++;
    msg_header = reinterpret_cast<MessageHeader*>(
        reinterpret_cast<uint8_t*>(msg_header) + current_padded_size);
}
```

For a 512 KB batch with 1 KB messages (~500 messages), this is:
- **500 CXL writes** (total_order)
- **500 CXL writes** (logical_offset)
- **500 CXL writes** (next_msg_diff)
- **500 CXL flush_cacheline calls**
- **500 store fences**

These per-message CXL operations are the dominant cost in the sequencer hot path.

## Key Insight: Subscribers Already Derive Per-Message Order from Batch Metadata

The broker's subscribe thread (`network_manager.cc:1387`) prepends a `BatchMetadata` header to each batch on the wire:

```cpp
// src/network_manager/network_manager.cc:1387-1398
batch_meta.batch_total_order = batch_total_order;  // from BatchHeader
batch_meta.num_messages = num_messages;
batch_meta.header_version = header_version;
```

The subscriber (`subscriber.cc:1571, 1762`) reconstructs per-message `total_order` from this batch metadata — it never reads the per-message CXL fields:

```cpp
// src/client/subscriber.cc:1571
batch_state.next_message_order_in_batch = potential_metadata->batch_total_order;

// src/client/subscriber.cc:1595-1596
if (v2_hdr->total_order == 0) {
    v2_hdr->total_order = batch_state.next_message_order_in_batch++;
}

// src/client/subscriber.cc:1762 (ConsumeBatchAware)
size_t current_total_order = batch_state.next_message_order_in_batch;
```

**The per-message `total_order`, `logical_offset`, and `next_msg_diff` written to CXL are never read by the ORDER=5 subscribe path.** They are redundant.

## Proposed Optimization

Replace the per-message loop with batch-level stamping:

```cpp
// PROPOSED: src/embarlet/topic.cc AssignOrder() for ORDER=5
void Topic::AssignOrder(BatchHeader *batch_to_order, size_t start_total_order,
                        BatchHeader* &header_for_sub) {
    size_t num_messages = batch_to_order->num_msg;
    if (num_messages == 0) return;

    // Stamp batch header only (already done on line 702)
    batch_to_order->total_order = start_total_order;

    // Update tinode watermarks in bulk
    tinode_->offsets[batch_to_order->broker_id].ordered += num_messages;
    tinode_->offsets[batch_to_order->broker_id].written += num_messages;

    // Mark batch ready for subscribe thread
    header_for_sub->batch_off_to_export = ...;
    header_for_sub->ordered = 1;
}
```

This reduces the sequencer work from **O(N) CXL ops** to **O(1)** per batch.

## Expected Performance Impact

| Metric | Current | Optimized | Improvement |
|--------|---------|-----------|-------------|
| CXL writes per batch (500 msgs) | ~1500 | ~2 | **750x fewer** |
| CXL flushes per batch | ~500 | ~1 | **500x fewer** |
| Store fences per batch | ~500 | ~1 | **500x fewer** |
| Sequencer latency per batch | High | Near-zero | Significant |

This should substantially improve ORDER=5 throughput, bringing it closer to ORDER=0 (which has no sequencer overhead).

## Dependencies to Verify Before Implementing

### 1. `next_msg_diff` — CXL-Local Message Traversal

`next_msg_diff` is used by several CXL-local readers to walk the message linked list:

- **`cxl_manager.cc:800,822,936,991`** — Other sequencer implementations (ORDER=1,2,3) use `next_msg_diff` to traverse messages in CXL. ORDER=5 does NOT use these code paths.
- **`disk_manager.cc:1095,1104`** — `GetMessageAddr()` uses `next_msg_diff` to find the next message boundary for disk persistence. **Must verify:** Does ORDER=5 disk persistence use this path? If yes, `next_msg_diff` must still be written (possibly by the network receive thread instead of the sequencer).
- **`network_manager.cc:1070`** — ORDER=0 subscribe path writes `next_msg_diff` inline. ORDER=5 subscribe path uses batch-level export (different code path).

**Action:** Verify that ORDER=5 disk persistence and replication do NOT depend on per-message `next_msg_diff` in CXL. If they do, move the `next_msg_diff` write to the network receive thread (which already knows `paddedSize`) instead of the sequencer thread.

### 2. `logical_offset` — Message Position Tracking

`logical_offset` is used by:
- CXL sequencers (ORDER=1,2,3) for message position tracking
- `tinode_->offsets[broker].written` watermark advancement

**Action:** For ORDER=5, verify that `logical_offset` is only consumed by the sequencer itself (for tinode watermark tracking). If so, the sequencer can maintain a local counter without writing to each message header in CXL.

### 3. `paddedSize` Wait Loop — Message Completion Check

The current loop (`while (msg_header->paddedSize == 0)`) ensures each message has been fully written to CXL before the sequencer stamps it. With batch-level stamping, this per-message wait is eliminated.

**Action:** Ensure the sequencer only proceeds after the entire batch is written. The `batch_complete` flag (already used by other orders) or the network thread's completion signal can serve this purpose. For ORDER=5 the current code (Sequencer 4 comment on line 691) explicitly does NOT use `batch_complete` — this needs to change.

### 4. Subscriber Wire Protocol Compatibility

The subscriber already handles `total_order == 0` in message headers by deriving order from `BatchMetadata` (`subscriber.cc:1595`). No subscriber changes needed.

### 5. `tinode_->offsets[broker].ordered` / `.written` Semantics

Currently incremented per-message. After optimization, increment by `num_messages` in bulk. Verify that no reader expects these to advance one-at-a-time (they are used as watermarks, so bulk advancement should be fine).

## Implementation Plan

1. **Add `batch_complete` support for ORDER=5**: Have the network receive thread set `batch_complete` on the batch header after all message data is written to CXL (same as other orders).

2. **Simplify `AssignOrder()`**: Replace per-message loop with batch-level stamping. Wait on `batch_complete` instead of per-message `paddedSize`.

3. **Move `next_msg_diff` write (if needed)**: If disk persistence requires `next_msg_diff`, write it in the network receive thread when message data is copied to CXL (the `paddedSize` is already known at that point).

4. **Update tinode watermarks**: Increment `ordered` and `written` by `num_messages` in one step.

5. **Test**:
   - ORDER=5 single-broker throughput (compare before/after)
   - ORDER=5 multi-broker throughput
   - ORDER=5 subscriber correctness (verify total_order sequence)
   - ORDER=5 latency (1-msg-per-batch, if epoch sequencer can handle it)
   - Disk persistence with ORDER=5 (if applicable)

6. **Benchmark**: Compare ORDER=0 vs ORDER=5 (optimized) — target: ORDER=5 within 5% of ORDER=0.
