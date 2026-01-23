# Data Structures: Memory Layout Reference

**Document Purpose:** Canonical reference for all shared memory structures in Embarcadero
**Status:** Current implementation + Paper spec target states
**Last Updated:** 2026-01-23

---

## Executive Summary

This document provides **byte-level precision** documentation of all CXL-resident data structures. Each structure is analyzed for:

1. **Exact Memory Layout** (field offsets, padding, alignment)
2. **Ownership Model** (who writes which cache lines)
3. **Concurrency Safety** (false sharing analysis)
4. **Migration Path** (current → target structure)

**Critical Insight:** Current structures use a **TInode-based model** (pre-paper) with mixed ownership within cache lines. Target architecture requires **strict cache-line ownership separation** per Single Writer Principle.

---

## 1. Core Metadata Structures

### 1.1 TInode (Current Implementation)

**Purpose:** Topic metadata + per-broker coordination structure
**Location:** `src/cxl_manager/cxl_datastructure.h:28730-28744`
**Alignment:** 64-byte cache line

```cpp
struct alignas(64) TInode {
    // --- Cache Line 0: Topic Configuration (Read-Only after creation) ---
    char topic[TOPIC_NAME_SIZE];        // Offset 0-31 (32 bytes)
    volatile bool replicate_tinode;     // Offset 32 (1 byte)
    volatile int order;                 // Offset 36 (4 bytes, padding +3)
    volatile int32_t replication_factor;// Offset 40 (4 bytes)
    volatile int32_t ack_level;         // Offset 44 (4 bytes)
    SequencerType seq_type;             // Offset 48 (4 bytes)
    // Padding to 64 bytes: 12 bytes

    // --- Cache Lines 1-N: Per-Broker State (NUM_MAX_BROKERS entries) ---
    volatile offset_entry offsets[NUM_MAX_BROKERS]; // 128 bytes each
};

static_assert(sizeof(TInode) >= 64, "TInode must be cache-line aligned");
```

**Size Calculation:**
```
TInode size = 64 (header) + (NUM_MAX_BROKERS * 128)
            = 64 + (20 * 128) = 2,624 bytes
            = 41 cache lines
```

**Issues:**
- ✗ `offsets[]` array has mixed broker/sequencer ownership (see offset_entry analysis)
- ✗ No separation between immutable config and mutable state
- ✓ Read-only fields are cache-line aligned (good)

---

### 1.2 offset_entry (Current Implementation)

**Purpose:** Per-broker coordination metadata
**Location:** `src/cxl_manager/cxl_datastructure.h:28717-28733`
**Size:** 128 bytes (2 cache lines)

```cpp
struct alignas(64) offset_entry {
    // --- Cache Line 0: Broker-Written Fields ---
    volatile size_t log_offset;              // Offset 0 (8 bytes)
    volatile size_t batch_headers_offset;    // Offset 8 (8 bytes)
    volatile size_t written;                 // Offset 16 (8 bytes)
    volatile unsigned long long written_addr;// Offset 24 (8 bytes)
    volatile int replication_done[NUM_MAX_BROKERS]; // Offset 32 (80 bytes)
    // Total: 112 bytes in cache line 0 (OVERFLOW: 48 bytes spill to line 1)

    // --- Cache Line 1: Sequencer-Written Fields ---
    volatile int ordered;                    // Offset 64 (4 bytes)
    volatile size_t ordered_offset;          // Offset 68 (8 bytes, misaligned)
    // Padding to 128 bytes: 52 bytes
};
```

**Byte Layout Visualization:**
```
Cache Line 0 (Bytes 0-63):
[log_offset][batch_headers_offset][written][written_addr][replication_done[0-7]]

Cache Line 1 (Bytes 64-127):
[replication_done[8-19]][ordered][ordered_offset][PADDING: 52 bytes]
```

**CRITICAL ISSUE: False Sharing**
```
Broker writes:  Bytes 0-111  (spans 2 cache lines!)
Sequencer reads: Bytes 64-76  (reads from SAME cache line broker is writing to!)
```

**Fix Required:** Split into separate structures on distinct cache lines.

---

### 1.3 BrokerMetadata (Target - Paper Spec)

**Purpose:** Replace offset_entry with proper cache-line separation
**Location:** NEW (to be added to cxl_datastructure.h)
**Size:** 128 bytes (2 cache lines, strictly separated)

```cpp
// Part 1: Broker Local Metadata (Cache Line 0)
struct alignas(64) BrokerLocalMeta {
    volatile uint64_t log_ptr;           // Offset 0 (8 bytes)
    volatile uint64_t processed_ptr;     // Offset 8 (8 bytes)
    volatile uint32_t replication_done;  // Offset 16 (4 bytes) - CHANGED: counter, not array
    volatile uint32_t _reserved1;        // Offset 20 (4 bytes)
    volatile uint64_t log_start;         // Offset 24 (8 bytes) - Starting address of Blog
    volatile uint64_t batch_headers_ptr; // Offset 32 (8 bytes)
    uint8_t padding[24];                 // Offset 40-63 (24 bytes)
};
static_assert(sizeof(BrokerLocalMeta) == 64, "Must fit in one cache line");

// Part 2: Sequencer Metadata (Cache Line 1)
struct alignas(64) BrokerSequencerMeta {
    volatile uint64_t ordered_seq;       // Offset 0 (8 bytes) - Global sequence number
    volatile uint64_t ordered_ptr;       // Offset 8 (8 bytes) - Pointer to last ordered msg
    volatile uint64_t epoch;             // Offset 16 (8 bytes) - Sequencer epoch
    volatile uint32_t status;            // Offset 24 (4 bytes) - 0=idle, 1=active, 2=failed
    volatile uint32_t _reserved2;        // Offset 28 (4 bytes)
    uint8_t padding[32];                 // Offset 32-63 (32 bytes)
};
static_assert(sizeof(BrokerSequencerMeta) == 64, "Must fit in one cache line");

// Combined structure (2 cache lines)
struct BrokerMetadata {
    BrokerLocalMeta local;      // Cache line 0 (broker writes)
    BrokerSequencerMeta seq;    // Cache line 1 (sequencer writes)
};
static_assert(sizeof(BrokerMetadata) == 128, "Must be exactly 2 cache lines");
```

**Ownership Model:**
```
BrokerLocalMeta (Cache Line 0):
  Writers:  Owner broker ONLY
  Readers:  Sequencer (polling processed_ptr), Replication threads

BrokerSequencerMeta (Cache Line 1):
  Writers:  Sequencer ONLY
  Readers:  Owner broker, Subscribers (polling ordered_ptr)
```

**Migration Strategy:**
```cpp
// Phase 1: Coexistence - Dual-write to both structures
void UpdateBrokerState(size_t broker_id, size_t new_written) {
    // Legacy path
    tinode->offsets[broker_id].written = new_written;

    // New path
    bmeta[broker_id].local.processed_ptr = new_written;
    CXL::flush_cacheline(&bmeta[broker_id].local);
    CXL::store_fence();
}

// Phase 2: Switch readers to new structure
uint64_t GetProcessedPtr(size_t broker_id) {
    if (use_new_bmeta) {
        return bmeta[broker_id].local.processed_ptr;
    } else {
        return tinode->offsets[broker_id].written;  // Fallback
    }
}

// Phase 3: Deprecate TInode (after validation period)
```

---

## 2. Message Structures

### 2.1 MessageHeader (Current Implementation)

**Purpose:** Per-message metadata in CXL log
**Location:** `src/cxl_manager/cxl_datastructure.h:28762-28775`
**Size:** 64 bytes (1 cache line)

```cpp
struct alignas(64) MessageHeader {
    volatile size_t paddedSize;               // Offset 0 (8 bytes) - Receiver writes
    void* segment_header;                     // Offset 8 (8 bytes) - Combiner writes
    size_t logical_offset;                    // Offset 16 (8 bytes) - Combiner writes
    volatile unsigned long long next_msg_diff;// Offset 24 (8 bytes) - Combiner writes
    volatile size_t total_order;              // Offset 32 (8 bytes) - Sequencer writes
    size_t client_order;                      // Offset 40 (8 bytes) - Receiver writes
    size_t client_id;                         // Offset 48 (8 bytes) - Receiver writes
    size_t size;                              // Offset 56 (8 bytes) - Receiver writes
};
static_assert(sizeof(MessageHeader) == 64, "Must fit in one cache line");
```

**Byte Layout:**
```
Bytes 0-7:   paddedSize       [Receiver writes]
Bytes 8-15:  segment_header   [Combiner writes]
Bytes 16-23: logical_offset   [Combiner writes]
Bytes 24-31: next_msg_diff    [Combiner writes]
Bytes 32-39: total_order      [Sequencer writes] ← FALSE SHARING!
Bytes 40-47: client_order     [Receiver writes]
Bytes 48-55: client_id        [Receiver writes]
Bytes 56-63: size             [Receiver writes]
```

**FALSE SHARING ANALYSIS:**
```
Receiver writes:  Bytes 0-7, 40-63 (24 bytes total, non-contiguous)
Combiner writes:  Bytes 8-31 (24 bytes)
Sequencer writes: Bytes 32-39 (8 bytes) ← IN THE SAME CACHE LINE!
```

**Problem:** Sequencer writing `total_order` invalidates cache line that Receiver/Combiner are actively using.

---

### 2.2 BlogMessageHeader (Target - Paper Spec)

**Purpose:** Cache-line partitioned message header
**Location:** NEW
**Size:** 64 bytes (strict partitioning)

```cpp
struct alignas(64) BlogMessageHeader {
    // --- Bytes 0-15: Receiver Writes (Stage 1) ---
    volatile uint32_t size;          // Offset 0 (4 bytes) - Payload size
    volatile uint32_t received;      // Offset 4 (4 bytes) - 0→1 completion flag
    volatile uint64_t ts;            // Offset 8 (8 bytes) - Receipt timestamp

    // --- Bytes 16-31: Delegation Writes (Stage 2) ---
    volatile uint32_t counter;       // Offset 16 (4 bytes) - Local sequence
    volatile uint32_t flags;         // Offset 20 (4 bytes) - Status flags
    volatile uint64_t processed_ts;  // Offset 24 (8 bytes) - Processing timestamp

    // --- Bytes 32-47: Sequencer Writes (Stage 3) ---
    volatile uint64_t total_order;   // Offset 32 (8 bytes) - Global sequence
    volatile uint64_t ordered_ts;    // Offset 40 (8 bytes) - Ordering timestamp

    // --- Bytes 48-63: Read-Only Metadata ---
    uint64_t client_id;              // Offset 48 (8 bytes)
    uint32_t batch_seq;              // Offset 56 (4 bytes)
    uint32_t _pad;                   // Offset 60 (4 bytes)
};
static_assert(sizeof(BlogMessageHeader) == 64, "Must be exactly 64 bytes");
```

**Ownership Boundaries:**
```
Bytes 0-15:   Receiver-only writes  (Stage 1)
Bytes 16-31:  Delegation-only writes (Stage 2)
Bytes 32-47:  Sequencer-only writes  (Stage 3)
Bytes 48-63:  Read-only (set at creation)
```

**Cache Coherence Protocol:**
```cpp
// Receiver Stage
void ReceiveMessage(BlogMessageHeader* hdr, void* payload, size_t len) {
    hdr->size = len;
    hdr->ts = rdtsc();
    // Write payload...
    hdr->received = 1;  // Atomic release
    CXL::flush_cacheline(hdr);  // Flush bytes 0-15
    CXL::store_fence();
}

// Delegation Stage (polls bytes 0-15, writes bytes 16-31)
void DelegateMessage(BlogMessageHeader* hdr, uint32_t local_seq) {
    while (!hdr->received) CXL::cpu_pause();  // Poll receiver's cache line

    hdr->counter = local_seq;
    hdr->processed_ts = rdtsc();
    CXL::flush_cacheline((char*)hdr + 16);  // Flush bytes 16-31 ONLY
    CXL::store_fence();
}

// Sequencer Stage (polls bytes 16-31, writes bytes 32-47)
void OrderMessage(BlogMessageHeader* hdr, uint64_t global_seq) {
    while (hdr->counter == 0) CXL::cpu_pause();  // Poll delegation's cache line

    hdr->total_order = global_seq;
    hdr->ordered_ts = rdtsc();
    CXL::flush_cacheline((char*)hdr + 32);  // Flush bytes 32-47 ONLY
    CXL::store_fence();
}
```

**Key Improvement:** Each stage flushes ONLY its own cache-line portion (16-byte granularity).

---

## 3. Batch Structures

### 3.1 BatchHeader (Current Implementation)

**Purpose:** Batch metadata for sequencer optimization
**Location:** `src/cxl_manager/cxl_datastructure.h:28745-28763`
**Size:** 64 bytes (1 cache line)

```cpp
struct alignas(64) BatchHeader {
    size_t batch_seq;                // Offset 0 (8 bytes) - Client batch sequence
    size_t total_size;               // Offset 8 (8 bytes) - Total batch size
    size_t start_logical_offset;     // Offset 16 (8 bytes) - Starting offset
    uint32_t broker_id;              // Offset 24 (4 bytes)
    uint32_t ordered;                // Offset 28 (4 bytes) - Sequencer flag
    size_t batch_off_to_export;      // Offset 32 (8 bytes)
    size_t total_order;              // Offset 40 (8 bytes) - Sequencer writes
    size_t log_idx;                  // Offset 48 (8 bytes) - Offset to Blog payload
    uint32_t client_id;              // Offset 56 (4 bytes)
    uint32_t num_msg;                // Offset 60 (4 bytes) - Message count
    // Total: 64 bytes (no explicit batch_complete here in this view)
};
```

**Actual Size (with extensions):**
```cpp
// Extended version used in code
struct alignas(64) BatchHeader {
    // ... fields above ...
    volatile uint32_t batch_complete;  // Completion flag for Sequencer 5
#ifdef BUILDING_ORDER_BENCH
    uint32_t gen;                      // Generation number
    uint64_t publish_ts_ns;            // Publish timestamp
#endif
};
// Size: 64 bytes (base) + 4-12 bytes (extensions) = 68-76 bytes
// PROBLEM: Spills beyond cache line!
```

**Issue:** Extensions break cache-line alignment.

---

### 3.2 BatchlogEntry (Target - Paper Spec)

**Purpose:** Immutable batch index (read-only after creation)
**Location:** NEW
**Size:** 64 bytes (strict)

```cpp
struct alignas(64) BatchlogEntry {
    uint64_t client_id;              // Offset 0 (8 bytes)
    uint64_t batch_seq;              // Offset 8 (8 bytes) - Client batch sequence
    uint32_t num_messages;           // Offset 16 (4 bytes)
    uint32_t broker_id;              // Offset 20 (4 bytes)
    uint64_t blog_start_ptr;         // Offset 24 (8 bytes) - Pointer to first message
    uint64_t blog_end_ptr;           // Offset 32 (8 bytes) - Pointer to last message
    uint64_t total_order_start;      // Offset 40 (8 bytes) - First global seqno
    uint64_t total_order_end;        // Offset 48 (8 bytes) - Last global seqno (inclusive)
    uint64_t timestamp;              // Offset 56 (8 bytes) - Batch creation time
};
static_assert(sizeof(BatchlogEntry) == 64, "Must fit in one cache line");
```

**Key Difference from BatchHeader:**
- **Immutable:** Set once by receiver, never modified
- **No `ordered` flag:** Ordering status inferred from `total_order_start != 0`
- **Pointers instead of offsets:** Direct CXL addresses for zero-copy access

**Usage:**
```cpp
// Receiver: Create batch entry
BatchlogEntry* CreateBatch(uint64_t client_id, uint64_t batch_seq,
                           void* first_msg, void* last_msg, uint32_t count) {
    BatchlogEntry* entry = AllocateBatchlogSlot();
    entry->client_id = client_id;
    entry->batch_seq = batch_seq;
    entry->num_messages = count;
    entry->blog_start_ptr = (uint64_t)first_msg;
    entry->blog_end_ptr = (uint64_t)last_msg;
    entry->total_order_start = 0;  // Not ordered yet
    entry->timestamp = rdtsc();
    CXL::flush_cacheline(entry);
    CXL::store_fence();
    return entry;
}

// Sequencer: Mark batch as ordered (write-once)
void MarkBatchOrdered(BatchlogEntry* entry, uint64_t start_seq, uint64_t end_seq) {
    entry->total_order_start = start_seq;
    entry->total_order_end = end_seq;
    CXL::flush_cacheline(entry);
    CXL::store_fence();
}

// Subscriber: Check if batch is ordered
bool IsBatchOrdered(BatchlogEntry* entry) {
    return entry->total_order_start != 0;
}
```

---

## 4. New Structures (Phase 1.1 - PBR/GOI)

### 4.1 PendingBatchEntry (Ring Buffer)

**Purpose:** Broker-local ring buffer for batches awaiting sequencing
**Location:** `src/cxl_manager/cxl_datastructure.h:28781-28795`
**Size:** 64 bytes (1 cache line)

```cpp
struct alignas(64) PendingBatchEntry {
    volatile uint64_t batch_id;      // Offset 0 (8 bytes) - 0 = empty slot
    volatile uint32_t broker_id;     // Offset 8 (4 bytes)
    volatile uint32_t client_id;     // Offset 12 (4 bytes)
    volatile uint32_t batch_seq;     // Offset 16 (4 bytes) - Client batch sequence
    volatile uint32_t num_messages;  // Offset 20 (4 bytes)
    volatile uint64_t data_offset;   // Offset 24 (8 bytes) - BrokerLog offset
    volatile uint32_t data_size;     // Offset 32 (4 bytes)
    volatile uint32_t ready;         // Offset 36 (4 bytes) - 0=not ready, 1=ready
    uint8_t padding[20];             // Offset 40-63 (20 bytes) - Ensures 64-byte alignment
};
static_assert(sizeof(PendingBatchEntry) == 64, "Must be exactly 64 bytes");
```

**Ring Buffer Layout:**
```
PBR (Per Broker) = Array of PendingBatchEntry[RING_SIZE]

Example: RING_SIZE = 1024
PBR size = 1024 * 64 = 65,536 bytes = 64 KB

Memory Layout:
[Entry 0][Entry 1][Entry 2]...[Entry 1023]
 64B      64B      64B          64B
```

**Ownership:**
- **Writer:** Owner broker (delegation thread)
- **Reader:** Sequencer thread assigned to this broker

**Usage Pattern:**
```cpp
// Broker writes to PBR (ring buffer)
void PublishBatchToPBR(PendingBatchEntry* pbr, size_t ring_size,
                       uint32_t client_id, uint32_t batch_seq,
                       uint64_t data_offset, uint32_t num_msg) {
    static size_t head = 0;
    PendingBatchEntry* slot = &pbr[head % ring_size];

    // Wait for slot to be consumed (ready=0 means sequencer processed it)
    while (slot->ready != 0) CXL::cpu_pause();

    slot->batch_id = next_batch_id++;
    slot->client_id = client_id;
    slot->batch_seq = batch_seq;
    slot->data_offset = data_offset;
    slot->num_messages = num_msg;
    slot->ready = 1;  // Signal sequencer

    CXL::flush_cacheline(slot);
    CXL::store_fence();

    head++;
}

// Sequencer polls PBR
PendingBatchEntry* PollNextBatch(PendingBatchEntry* pbr, size_t ring_size) {
    static size_t tail = 0;
    PendingBatchEntry* slot = &pbr[tail % ring_size];

    if (slot->ready == 0) return nullptr;  // No new batch

    tail++;
    return slot;
}
```

---

### 4.2 GlobalOrderEntry (Central Array)

**Purpose:** Definitive global order record (GOI)
**Location:** `src/cxl_manager/cxl_datastructure.h:28796-28809`
**Size:** 64 bytes (1 cache line)

```cpp
struct alignas(64) GlobalOrderEntry {
    volatile uint64_t global_seq_start; // Offset 0 (8 bytes) - Starting sequence
    volatile uint32_t num_messages;     // Offset 8 (4 bytes)
    volatile uint32_t broker_id;        // Offset 12 (4 bytes)
    volatile uint64_t data_offset;      // Offset 16 (8 bytes) - BrokerLog offset
    volatile uint32_t data_size;        // Offset 20 (4 bytes)
    volatile uint32_t status;           // Offset 24 (4 bytes) - 0=empty, 1=assigned, 2=replicated
    volatile uint64_t epoch;            // Offset 28 (8 bytes) - Sequencer epoch
    uint8_t padding[20];                // Offset 36-63 (20 bytes)
};
static_assert(sizeof(GlobalOrderEntry) == 64, "Must be exactly 64 bytes");
```

**GOI Layout:**
```
GOI = Array of GlobalOrderEntry[MAX_GLOBAL_SEQ]

Example: MAX_GLOBAL_SEQ = 1,048,576 (1M entries)
GOI size = 1,048,576 * 64 = 67,108,864 bytes = 64 MB

Each entry represents a batch with a range of global sequence numbers.
```

**Ownership:**
- **Writer:** Sequencer ONLY
- **Readers:** All brokers (subscribers, replication threads)

**Usage:**
```cpp
// Sequencer assigns global order
void AssignGlobalOrder(GlobalOrderEntry* goi, PendingBatchEntry* batch) {
    uint64_t seq_start = global_seq_counter.fetch_add(batch->num_messages);
    uint64_t index = seq_start;  // Could use modulo for circular buffer

    GlobalOrderEntry* entry = &goi[index % MAX_GLOBAL_SEQ];
    entry->global_seq_start = seq_start;
    entry->num_messages = batch->num_messages;
    entry->broker_id = batch->broker_id;
    entry->data_offset = batch->data_offset;
    entry->data_size = batch->data_size;
    entry->epoch = current_epoch;
    entry->status = 1;  // Assigned

    CXL::flush_cacheline(entry);
    CXL::store_fence();
}

// Subscriber reads GOI
GlobalOrderEntry* GetOrderedBatch(GlobalOrderEntry* goi, uint64_t seq) {
    GlobalOrderEntry* entry = &goi[seq % MAX_GLOBAL_SEQ];
    if (entry->status < 1) return nullptr;  // Not assigned yet
    return entry;
}
```

---

## 5. Memory Layout Summary

### 5.1 Current CXL Layout

```
┌─────────────────────────────────────────────────────────────┐
│ TInode Region                                               │
│ Size: sizeof(TInode) * MAX_TOPIC_SIZE                       │
│ = 2,624 * 1024 = 2,686,976 bytes (~2.6 MB)                  │
│ Alignment: 64-byte cache lines                              │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ Bitmap Region                                               │
│ Size: CACHELINE_SIZE * MAX_TOPIC_SIZE                       │
│ = 64 * 1024 = 65,536 bytes (64 KB)                          │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ BatchHeaders Region                                         │
│ Size: NUM_MAX_BROKERS * BATCHHEADERS_SIZE * MAX_TOPIC_SIZE  │
│ = 20 * 16MB * 1024 = 327,680 MB (320 GB!)                   │
│ Note: BATCHHEADERS_SIZE likely much smaller in practice     │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ Segment Region (Per-Broker Allocation)                     │
│ Size: (Total_CXL_Size - above) / NUM_MAX_BROKERS            │
│ Example: (128GB - 2.6MB - 64KB - 320MB) / 20 brokers        │
│ = ~6.4 GB per broker                                        │
│                                                             │
│ Contains:                                                   │
│   - MessageHeaders (64 bytes each)                          │
│   - Message payloads (variable size)                        │
│   - Segment metadata (8 bytes per segment)                  │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 Target CXL Layout (Paper Spec)

```
┌─────────────────────────────────────────────────────────────┐
│ TInode Region (Legacy Compatibility)                        │
│ Size: 2.6 MB                                                │
│ Status: Deprecated after migration                          │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ Bitmap Region (Unchanged)                                   │
│ Size: 64 KB                                                 │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ BatchHeaders Region (Unchanged)                             │
│ Size: Variable                                              │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ NEW: Bmeta Region                                           │
│ Size: sizeof(BrokerMetadata) * NUM_MAX_BROKERS              │
│ = 128 * 20 = 2,560 bytes (~2.5 KB)                          │
│ Alignment: Strict 64-byte per cache line                    │
│                                                             │
│ Layout:                                                     │
│   Broker 0: [BrokerLocalMeta][BrokerSequencerMeta]          │
│   Broker 1: [BrokerLocalMeta][BrokerSequencerMeta]          │
│   ...                                                       │
│   Broker 19: [BrokerLocalMeta][BrokerSequencerMeta]         │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ NEW: PBR Region (Pending Batch Ring)                        │
│ Size: sizeof(PendingBatchEntry) * RING_SIZE * NUM_BROKERS   │
│ = 64 * 1024 * 20 = 1,310,720 bytes (~1.25 MB)               │
│                                                             │
│ Layout:                                                     │
│   Broker 0 PBR: [Entry 0][Entry 1]...[Entry 1023]           │
│   Broker 1 PBR: [Entry 0][Entry 1]...[Entry 1023]           │
│   ...                                                       │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ NEW: GOI Region (Global Order Index)                        │
│ Size: sizeof(GlobalOrderEntry) * MAX_GLOBAL_SEQ             │
│ = 64 * 1,048,576 = 67,108,864 bytes (64 MB)                 │
│                                                             │
│ Circular buffer indexed by global_seq % MAX_GLOBAL_SEQ      │
└─────────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────────┐
│ BrokerLog Region (Blog) - Per Broker                        │
│ Size: Remaining CXL space / NUM_MAX_BROKERS                 │
│ = (128GB - overhead) / 20 = ~6.4 GB per broker              │
│                                                             │
│ Contains:                                                   │
│   - BlogMessageHeaders (64 bytes, strict partitioning)      │
│   - Message payloads (zero-copy from NIC)                   │
└─────────────────────────────────────────────────────────────┘
```

**Total Overhead:**
```
TInode (legacy):   2.6 MB
Bitmap:            0.064 MB
BatchHeaders:      ~320 MB (need to verify actual BATCHHEADERS_SIZE)
Bmeta:             0.0025 MB
PBR:               1.25 MB
GOI:               64 MB
────────────────────────────
Total overhead:    ~388 MB
Available for Blog: 128 GB - 388 MB = 127.6 GB
Per-broker Blog:   127.6 GB / 20 = 6.38 GB
```

---

## 6. Alignment & Padding Analysis

### 6.1 Cache Line Boundary Rules

**x86-64 Cache Line:** 64 bytes
**Rule:** All frequently-accessed structures MUST be 64-byte aligned

**Verification:**
```cpp
static_assert(alignof(TInode) == 64, "TInode alignment");
static_assert(alignof(offset_entry) == 64, "offset_entry alignment");
static_assert(alignof(BatchHeader) == 64, "BatchHeader alignment");
static_assert(alignof(MessageHeader) == 64, "MessageHeader alignment");
static_assert(alignof(PendingBatchEntry) == 64, "PendingBatchEntry alignment");
static_assert(alignof(GlobalOrderEntry) == 64, "GlobalOrderEntry alignment");
```

**Padding Calculation:**
```cpp
// Example: Ensure struct ends on cache-line boundary
struct Example {
    uint64_t field1;  // 8 bytes
    uint32_t field2;  // 4 bytes
    // Total: 12 bytes
    // Padding needed: 64 - 12 = 52 bytes
    uint8_t _pad[52];
};
static_assert(sizeof(Example) == 64, "Must be cache-line sized");
```

### 6.2 False Sharing Detection

**Tool:** Manual analysis of write patterns
**Methodology:**
1. Identify all writers to a structure
2. Map which bytes each writer touches
3. Check if writers touch the same cache line

**Example: offset_entry Analysis**
```
Writers:
  Broker:    Bytes 0-111 (log_offset, written, replication_done[])
  Sequencer: Bytes 64-76 (ordered, ordered_offset)

Cache Line Mapping:
  Line 0: Bytes 0-63   → Broker writes 0-63, Sequencer reads 64-76 (OVERLAP on read!)
  Line 1: Bytes 64-127 → Broker writes 64-111, Sequencer writes 64-76 (FALSE SHARING!)

Fix: Split into separate structures on distinct cache lines
```

---

## 7. Migration Checklist

### 7.1 Phase 1: Add New Structures

- [ ] Add `BrokerMetadata` definition to `cxl_datastructure.h`
- [ ] Add `BlogMessageHeader` definition
- [ ] Add `BatchlogEntry` definition
- [ ] Update `CXLManager` to allocate Bmeta/PBR/GOI regions
- [ ] Add compile-time size assertions

### 7.2 Phase 2: Dual-Write Implementation

- [ ] Modify `UpdateTInodeWritten()` to write to both TInode and Bmeta
- [ ] Add `UpdateBmeta()` helper function with cache flush
- [ ] Modify sequencer to write to both MessageHeader and BlogMessageHeader
- [ ] Add feature flag `USE_NEW_STRUCTURES` (default: false)

### 7.3 Phase 3: Switch Readers

- [ ] Modify `BrokerScannerWorker` to poll Bmeta instead of TInode
- [ ] Update subscriber to read from BlogMessageHeader
- [ ] Add rollback capability (revert to TInode on error)

### 7.4 Phase 4: Validation

- [ ] Run ordering tests (Property 3d)
- [ ] Run durability tests (Property 4a)
- [ ] Performance regression tests (<10% degradation acceptable)
- [ ] 24-hour stability test

### 7.5 Phase 5: Deprecation

- [ ] Set `USE_NEW_STRUCTURES = true` by default
- [ ] Mark TInode as deprecated in code comments
- [ ] Remove TInode dual-write after 1 release cycle
- [ ] Reclaim TInode region for BrokerLog expansion

---

## 8. Code Generation Templates

### 8.1 Structure Definition Template

```cpp
// NEW STRUCTURE: <StructureName>
// Purpose: <Brief description>
// Size: <Exact bytes> (<N> cache lines)
// Alignment: 64 bytes
// Location: src/cxl_manager/cxl_datastructure.h

struct alignas(64) <StructureName> {
    // --- Cache Line 0: <Owner> Writes ---
    volatile <type> field1;  // Offset 0 (<size> bytes) - <description>
    volatile <type> field2;  // Offset X (<size> bytes) - <description>
    // ...
    uint8_t _pad1[<N>];      // Pad to 64 bytes

    // --- Cache Line 1: <Owner> Writes (if needed) ---
    // ...
};

// Compile-time checks
static_assert(sizeof(<StructureName>) == 64, "Must be cache-line sized");
static_assert(alignof(<StructureName>) == 64, "Must be cache-line aligned");
static_assert(offsetof(<StructureName>, field1) == 0, "Field offset verification");
```

### 8.2 Access Pattern Template

```cpp
// WRITE PATTERN: <StructureName>
void Write<StructureName>(<StructureName>* ptr, <params>) {
    // 1. Write fields
    ptr->field1 = value1;
    ptr->field2 = value2;

    // 2. Flush cache line(s)
    CXL::flush_cacheline(ptr);
    // If multi-cache-line struct, flush each separately:
    // CXL::flush_cacheline((char*)ptr + 64);

    // 3. Store fence
    CXL::store_fence();
}

// READ PATTERN: <StructureName>
<type> Read<StructureName>(<StructureName>* ptr) {
    // 1. Busy-wait poll (if waiting for writer)
    while (ptr->ready_flag == 0) {
        CXL::cpu_pause();
    }

    // 2. Read fields (volatile ensures fresh read)
    <type> value = ptr->field1;

    return value;
}
```

---

## References

- **Current Structures:** `src/cxl_manager/cxl_datastructure.h`
- **Paper Spec:** `docs/memory-bank/paper_spec.md` (Tables 3, 4, 5)
- **System Patterns:** `docs/memory-bank/systemPatterns.md` (Migration strategies)
- **CXL Manager:** `src/cxl_manager/cxl_manager.cc` (Allocation logic)

---

**Document Maintenance:**
- Update byte offsets when fields are added/removed
- Regenerate `static_assert` checks after structure changes
- Verify cache-line alignment with `pahole` tool:
  ```bash
  pahole -C TInode build/bin/embarlet
  ```
