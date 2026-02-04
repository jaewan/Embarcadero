# Embarcadero CXL Memory Layout v2.0

**Coordination Document for Phase 1 + Phase 2 Parallel Implementation**

**Status**: ✅ APPROVED for parallel development
**Date**: January 30, 2026
**Target CXL Module Size**: 512 GB

---

## Memory Map Overview

```
┌──────────────────────────────────────────────────────────────────────────┐
│ REGION              │ OFFSET        │ SIZE     │ OWNER    │ ALIGNMENT   │
├──────────────────────────────────────────────────────────────────────────┤
│ ControlBlock        │ 0x0000_0000   │ 128 B    │ Phase 1a │ 128-byte    │
│ (Reserved)          │ 0x0000_0080   │ 3968 B   │ -        │ -           │
│ CompletionVector    │ 0x0000_1000   │ 4 KB     │ Phase 2  │ 128-byte    │
│ GOI                 │ 0x0000_2000   │ 16 GB    │ Phase 2  │ 64-byte     │
│ PBR (BatchHeader)   │ 0x4_0000_2000 │ 1 GB     │ Existing │ 64-byte     │
│ BLog                │ 0x4_4000_2000 │ ~495 GB  │ Existing │ 64-byte     │
│ TInode (legacy)     │ (TBD)         │ ~24 KB   │ Existing │ 64-byte     │
└──────────────────────────────────────────────────────────────────────────┘
```

**Total Metadata**: ~17 GB (ControlBlock + CV + GOI + PBR)
**Total Payload**: ~495 GB (BLog)

---

## Region 1: ControlBlock (Phase 1a)

**Offset**: `0x0000_0000`
**Size**: 128 bytes
**Alignment**: 128-byte (cache line + prefetcher safety)
**Owner**: Phase 1a implementation

### Structure Definition

```cpp
struct alignas(128) ControlBlock {
    // Epoch-based fencing (Phase 1a)
    std::atomic<uint64_t> epoch;            // +0   Monotonic, incremented on failures
    std::atomic<uint64_t> sequencer_id;     // +8   Current sequencer identity
    std::atomic<uint64_t> sequencer_lease;  // +16  Lease expiry timestamp (ns)

    // Cluster state
    std::atomic<uint32_t> broker_mask;      // +24  Bitmap: 1 = healthy
    std::atomic<uint32_t> num_brokers;      // +28  Active broker count

    // Progress tracking
    std::atomic<uint64_t> committed_seq;    // +32  Highest durable global_seq

    // Reserved for future use
    uint8_t _reserved[88];                  // +40  Future expansion

    uint8_t _pad[0];                        // Padded to 128 bytes
};
static_assert(sizeof(ControlBlock) == 128);
```

### Access Pattern

| Thread | Access Type | Fields | Frequency |
|--------|-------------|--------|-----------|
| Broker (Phase 1a) | Read | epoch | Every 100 batches (~50μs) |
| Sequencer (Phase 1b) | Write | epoch, committed_seq | On failover, per epoch |
| Replicas (Phase 2) | Read | epoch, committed_seq | Per GOI poll (~100μs) |

---

## Region 2: CompletionVector (Phase 2)

**Offset**: `0x0000_1000` (4 KB from base)
**Size**: 4 KB (32 brokers × 128 bytes)
**Alignment**: 128-byte per entry
**Owner**: Phase 2 implementation

### Structure Definition

```cpp
struct alignas(128) CompletionVectorEntry {
    // Highest contiguous PBR index for this broker that is sequenced AND replicated
    std::atomic<uint64_t> completed_pbr_head;  // +0   [[writer: tail replica]]

    uint8_t _pad[120];                         // +8   Pad to 128 bytes (prefetcher safety)
};
static_assert(sizeof(CompletionVectorEntry) == 128);

// Array layout in CXL
CompletionVectorEntry CompletionVector[32];  // At offset 0x0000_1000
```

### Access Pattern

| Thread | Access Type | Index | Frequency |
|--------|-------------|-------|-----------|
| Tail Replica (Phase 2) | Write | CV[broker_id] | After each replication (~200μs) |
| Broker ACK thread | Read | CV[my_broker_id] | Poll every 50-100μs |

**Key Property**: Each broker reads ONLY its own 128-byte slot (no false sharing).

---

## Region 3: Global Order Index (Phase 2)

**Offset**: `0x0000_2000` (8 KB from base)
**Size**: 16 GB (256M entries × 64 bytes)
**End**: `0x4_0000_2000`
**Alignment**: 64-byte per entry
**Owner**: Phase 2 implementation

### Structure Definition

```cpp
struct alignas(64) GOIEntry {
    // The definitive ordering
    uint64_t global_seq;                       // +0   Unique global sequence number
    uint64_t batch_id;                         // +8   Matches PBREntry.batch_id

    // Payload location
    uint16_t broker_id;                        // +16  Which broker's BLog
    uint16_t epoch_sequenced;                  // +18  Epoch when sequenced
    uint64_t blog_offset;                      // +20  Offset in BLog (64-bit for >4GB logs)
    uint32_t payload_size;                     // +28  Payload size
    uint32_t message_count;                    // +32  Message count

    // Replication progress (Phase 2 chain protocol)
    std::atomic<uint32_t> num_replicated;      // +36  0..R (chain token)

    // Per-client ordering info (Phase 1b will populate when Level 5 enabled)
    uint64_t client_id;                        // +40  Source client (0 if Level 0)
    uint64_t client_seq;                       // +48  Client sequence (if Level 5)

    // ACK path: PBR index for CV updater
    uint32_t pbr_index;                        // +56  Index in PBR[broker_id]
    uint8_t _pad[4];                           // +60  Pad to 64 bytes
};
static_assert(sizeof(GOIEntry) == 64);

// Array layout in CXL
GOIEntry GOI[256 * 1024 * 1024];  // At offset 0x0000_2000, 256M entries
```

### Access Pattern

| Thread | Access Type | Operation | Frequency |
|--------|-------------|-----------|-----------|
| Sequencer (Phase 1b) | Write | Assign global_seq, write GOI[seq] | Per epoch (~500μs for batch) |
| Replicas (Phase 2) | Read+Write | Poll GOI, copy data, increment num_replicated | Per entry (~200μs) |
| CV Updater (Phase 2) | Read | Read GOI to track contiguous pbr_index | Per update (~200μs) |

---

## Region 4: PBR / BatchHeader (Existing)

**Offset**: `0x4_0000_2000` (16 GB + 8 KB from base)
**Size**: 1 GB (32 brokers × 512K entries × 64 bytes)
**End**: `0x4_4000_2000`
**Alignment**: 64-byte per entry
**Owner**: Existing code, Phase 1b will enhance

### Current Structure (BatchHeader)

```cpp
struct alignas(64) BatchHeader {
    size_t batch_seq;                          // Client's monotonic sequence
    uint32_t client_id;
    uint32_t num_msg;
    volatile uint32_t batch_complete;          // Readiness flag
    uint32_t broker_id;
    volatile uint32_t ordered;                 // Set by sequencer
    size_t total_size;
    size_t start_logical_offset;
    size_t log_idx;                            // Offset in BLog
    size_t total_order;                        // Global sequence
    size_t batch_off_to_export;                // Export chain
};
```

### Phase 1b Enhancement (Optional Dual Path)

**Phase 1b sequencer** can:
1. **Read**: `BatchHeader` (existing PBR-like structure)
2. **Write both**:
   - `BatchHeader.total_order` (backward compat with existing replication)
   - `GOI[global_seq]` (Phase 2 new path)

**No conflict**: Phase 1b reads BatchHeader, writes both. Phase 2 reads GOI.

---

## Region 5: BLog (Existing)

**Offset**: `0x4_4000_2000` (17 GB + 8 KB from base)
**Size**: ~495 GB (32 brokers × ~15 GB each)
**Alignment**: 64-byte per batch
**Owner**: Existing code

No changes needed for Phase 1 or Phase 2.

---

## Coordination Protocol Between Phases

### During Phase 1a (You)

**You add**:
```cpp
// In cxl_datastructure.h or new cxl_layout.h
extern ControlBlock* control_block;  // At CXL base + 0x0

void init_cxl_phase1a(void* cxl_base) {
    control_block = reinterpret_cast<ControlBlock*>(cxl_base);
    control_block->epoch.store(1, std::memory_order_release);
    control_block->sequencer_id.store(getpid(), std::memory_order_release);
    // ... initialize other fields
}
```

**I will not touch**: `ControlBlock` (offset 0x0 - 0x80)

---

### During Phase 2 (Me)

**I add**:
```cpp
// In cxl_datastructure.h or new cxl_layout.h
extern CompletionVectorEntry* completion_vector;  // At CXL base + 0x1000
extern GOIEntry* goi;                             // At CXL base + 0x2000

void init_cxl_phase2(void* cxl_base) {
    completion_vector = reinterpret_cast<CompletionVectorEntry*>(
        static_cast<char*>(cxl_base) + 0x1000);
    goi = reinterpret_cast<GOIEntry*>(
        static_cast<char*>(cxl_base) + 0x2000);

    // Initialize CV
    for (int i = 0; i < 32; i++) {
        completion_vector[i].completed_pbr_head.store(0, std::memory_order_release);
    }

    // Initialize GOI (can be lazy)
    memset(goi, 0, 256ULL * 1024 * 1024 * sizeof(GOIEntry));
}
```

**I will not touch**:
- `ControlBlock` (Phase 1a)
- Sequencer logic (Phase 1b)
- BatchHeader writes (existing)

---

### Integration Point (Week 6)

**After both phases merge**, Phase 1b sequencer adds dual-write:

```cpp
// Phase 1b sequencer (your code)
void Sequencer::process_epoch(EpochBuffer& buf) {
    uint64_t base = global_seq_.fetch_add(buf.count, std::memory_order_relaxed);

    for (size_t i = 0; i < buf.count; i++) {
        BatchInfo& batch = buf.batches[i];
        uint64_t global_seq = base + i;

        // Existing path (backward compat)
        batch.header->total_order = global_seq;
        batch.header->ordered = 1;

        // NEW: Phase 2 path (once Phase 2 merged)
        #ifdef PHASE2_GOI_ENABLED
        goi[global_seq] = GOIEntry{
            .global_seq = global_seq,
            .batch_id = batch.batch_id,
            .broker_id = batch.broker_id,
            .epoch_sequenced = current_epoch_,
            .blog_offset = batch.header->log_idx,
            .payload_size = batch.header->total_size,
            .message_count = batch.header->num_msg,
            .num_replicated = 0,
            .client_id = batch.header->client_id,
            .client_seq = batch.header->batch_seq,
            .pbr_index = batch.pbr_index,
        };
        #endif
    }
}
```

---

## Safety Invariants

### Isolation

| Phase | Writes To | Reads From | Conflict Risk |
|-------|-----------|------------|---------------|
| Phase 1a | ControlBlock (0x0 - 0x80) | - | ✅ None |
| Phase 1b | BatchHeader, (later: GOI) | BatchHeader | ✅ None (BatchHeader existing) |
| Phase 2 | GOI, CV | GOI, ControlBlock.epoch | ✅ None (ControlBlock read-only) |

### Alignment Guarantees

All structures are cache-line aligned (64 or 128 bytes) to prevent false sharing between:
- Phase 1a ControlBlock (128-byte aligned)
- Phase 2 CompletionVector entries (128-byte aligned per broker)
- Phase 2 GOI entries (64-byte aligned)

### Atomic Operations

| Region | Type | Order | Phase |
|--------|------|-------|-------|
| ControlBlock.epoch | atomic<uint64_t> | acquire/release | Phase 1a |
| CV[i].completed_pbr_head | atomic<uint64_t> | acquire/release | Phase 2 |
| GOI[j].num_replicated | atomic<uint32_t> | relaxed | Phase 2 |

---

## Testing Strategy

### Phase 1a Validation

```bash
# Verify ControlBlock is at offset 0
hexdump -C /dev/dax0.0 -s 0 -n 128

# Verify epoch increments on sequencer restart
embarcadero_test --test=epoch_fencing
```

### Phase 2 Validation

```bash
# Verify GOI is at offset 0x2000
hexdump -C /dev/dax0.0 -s 0x2000 -n 64

# Verify CV is at offset 0x1000
hexdump -C /dev/dax0.0 -s 0x1000 -n 128

# Test chain replication
embarcadero_test --test=chain_replication

# Test CV-based ACK
embarcadero_test --test=completion_vector_ack
```

### Integration Test (Week 6)

```bash
# Verify dual-write: BatchHeader.total_order == GOI[seq].global_seq
embarcadero_test --test=dual_write_consistency

# Verify replication works from both paths
embarcadero_test --test=replication_dual_path

# Verify ACK works from both TInode and CV
embarcadero_test --test=ack_dual_path
```

---

## Merge Strategy

### Week 6 Merge Checklist

- [ ] Both branches compile independently
- [ ] Memory layout matches this spec (verified via hexdump)
- [ ] No overlapping CXL regions
- [ ] Phase 1b sequencer can optionally write to GOI (ifdef)
- [ ] Phase 2 replication works with existing BatchHeader.total_order
- [ ] Integration tests pass (dual-write consistency)
- [ ] Performance regression: <5% (temporary dual-write overhead)
- [ ] Backward compatibility: TInode ACK path still works

---

## Deprecation Plan (Phase 3)

After both phases are merged and validated:

1. **Remove TInode ACK path** (brokers poll CV only)
2. **Remove BatchHeader.total_order dual-write** (sequencer writes GOI only)
3. **Remove TInode structure** (25 KB per broker saved)
4. **Switch to GOI-only replication**

Estimated savings: ~800 KB metadata per topic (TInode removal).

---

## Document Version

| Version | Date | Changes |
|---------|------|---------|
| 2.0 | 2026-01-30 | Initial layout for Phase 1 + Phase 2 parallel development |

---

**APPROVED FOR IMPLEMENTATION** ✅
**Status**: Active coordination document
**Next Review**: Week 6 (integration checkpoint)
