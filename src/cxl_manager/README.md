# CXL Data Structures Directory

**⚠️ CRITICAL: This directory contains all CXL-resident shared memory structures.**

Any modification to structures in `cxl_datastructure.h` can introduce false sharing, race conditions, or silent data corruption across hosts. Non-cache-coherent memory requires strict discipline.

---

## The Four Laws (Enforceable via Code Review)

### Law 1: Cache-Line Alignment (64B)

**Rule:** All shared structs (`Bmeta`, `Blog` headers, `Batchlog` headers) MUST be cache-line aligned to prevent false sharing.

**Why:** On non-coherent CXL memory, if two hosts write to different fields in the same 64-byte cache line, the second write may silently overwrite the first.

**How:**
```cpp
// ✅ CORRECT
struct alignas(64) BrokerMetadata {
    // ... fields ...
};
static_assert(sizeof(BrokerMetadata) % 64 == 0, "Must be multiple of 64B");

// ❌ WRONG
struct BrokerMetadata {  // No alignas(64)
    // ... fields ...
};
```

**Verification:**
- Run: `pahole -C <StructName> build/src/cxl_manager/libcxl_manager.a`
- Check that `/* size: XXX, cachelines: Y */` shows `XXX % 64 == 0`

---

### Law 2: Single Writer Principle

**Rule:** NEVER mix fields written by the Broker and fields written by the Sequencer in the same cache line.

**Why:** Two hosts writing to the same cache line causes "ping-ponging" and potential data loss in non-coherent memory.

**How:** Split metadata into separate cache-line-aligned structs:

```cpp
// ✅ CORRECT (Paper Spec: Table 5)
struct alignas(64) BrokerLocalMeta {
    volatile uint64_t log_ptr;          // Broker writes
    volatile uint64_t processed_ptr;    // Broker writes
    volatile uint32_t replication_done; // Broker writes
    uint8_t _pad[64 - 20];              // Pad to 64B
};

struct alignas(64) BrokerSequencerMeta {
    volatile uint64_t ordered_seq;      // Sequencer writes
    volatile uint64_t ordered_ptr;      // Sequencer writes
    uint8_t _pad[64 - 16];              // Pad to 64B
};

struct BrokerMetadata {
    BrokerLocalMeta local;    // Cache line 0 (broker-owned)
    BrokerSequencerMeta seq;  // Cache line 1 (sequencer-owned)
};

// ❌ WRONG (Current offset_entry in cxl_datastructure.h:28700)
struct offset_entry {
    uint64_t broker_writes_0_63;   // Broker writes
    uint64_t sequencer_writes_64;  // Sequencer writes SAME CACHE LINE!
    // FALSE SHARING: Both hosts write to cache line 1 (bytes 64-127)
};
```

**Verification:**
- Map each field to cache lines: `(offset_of_field / 64) == cache_line_index`
- Ensure no cache line is written by more than one host

---

### Law 3: Zero-Copy Data Path

**Rule:** Do not use `std::vector`, `std::string`, or any heap-allocated containers for message payloads. Write directly to the CXL mmapped region.

**Why:** CXL memory is a shared address space. Extra copies waste bandwidth and break zero-copy semantics.

**How:**
```cpp
// ✅ CORRECT
void* payload_ptr = cxl_manager->AllocateInBlog(size);
memcpy(payload_ptr, client_buffer, size);  // Single copy: client → CXL

// ❌ WRONG
std::string temp(client_buffer, size);     // Copy 1: client → heap
cxl_manager->WritePayload(temp.data());    // Copy 2: heap → CXL
```

**For Batches:**
- Use `write()` / `writev()` directly to CXL-backed file descriptors
- Use `MSG_ZEROCOPY` socket flags where supported

---

### Law 4: Use performance_utils.h Wrappers

**Rule:** Use the `Embarcadero::CXL::flush_cacheline()` and `store_fence()` wrappers. Do not write raw assembly.

**Why:** Portability across x86_64 (clflushopt) and ARM (DC CVAU) architectures.

**How:**
```cpp
#include "common/performance_utils.h"

// ✅ CORRECT
msg_header->received = 1;
Embarcadero::CXL::flush_cacheline(msg_header);
Embarcadero::CXL::store_fence();

// ❌ WRONG
msg_header->received = 1;
_mm_clflushopt(msg_header);  // Direct intrinsic, not portable
_mm_sfence();
```

**Required Fences (Per Paper Spec §3):**
1. **Delegation Thread:** Flush after writing `msg_header.counter`
2. **Sequencer Thread:** Flush after writing `msg_header.total_order`
3. **Replication Thread:** Flush after writing `Bmeta.replication_done`

---

## Structure Inventory

### Current Structures (cxl_datastructure.h)

| Structure | Purpose | Writer(s) | Status |
|:----------|:--------|:----------|:-------|
| `TInode` | Broker metadata (monolithic) | Broker + Sequencer | ⚠️ **FALSE SHARING** |
| `offset_entry` | Per-message metadata | Broker + Sequencer | ⚠️ **FALSE SHARING** (bytes 64-76) |
| `MessageHeader` | Message envelope | Receiver → Delegation → Sequencer | ⚠️ **NOT ALIGNED** |
| `BatchHeader` | Batch metadata | Client Library | ✅ OK |
| `PendingBatchEntry` | Delegation queue entry | Delegation Thread | ✅ OK (single writer) |
| `GlobalOrderEntry` | Sequencer queue entry | Sequencer Thread | ✅ OK (single writer) |

### Target Structures (Paper Spec Migration)

| Structure | Source | Writer(s) | Migration Status |
|:----------|:-------|:----------|:-----------------|
| `BrokerMetadata` (`Bmeta`) | Paper Table 5 | Split: Broker ∣ Sequencer | 🔴 **NOT IMPLEMENTED** |
| `BlogMessageHeader` | Paper Table 4 | Receiver → Delegation → Sequencer | 🔴 **NOT IMPLEMENTED** |
| `BatchlogEntry` | Paper Table 3 | Client Library | 🔴 **NOT IMPLEMENTED** |

---

## Common Pitfalls & How to Avoid Them

### Pitfall 1: "I added a field to TInode and tests pass locally"

**Problem:** Local NUMA emulation is cache-coherent (same host). Real CXL across hosts is NOT.

**Detection:**
```bash
# Check if field crosses cache-line boundary
pahole -C TInode build/src/cxl_manager/libcxl_manager.a | grep -A2 "new_field"
```

**Fix:** Add field to the correct sub-struct (`BrokerLocalMeta` or `BrokerSequencerMeta`), not to a shared cache line.

---

### Pitfall 2: "I see `UpdateTInodeWritten()` but no cache flush"

**Problem:** The Delegation thread writes `processed_ptr`, but sequencer may read stale value from its cache.

**Location:** `src/embarlet/topic.cc:30859`

**Fix:**
```cpp
// After updating TInode fields
Embarcadero::CXL::flush_cacheline(&tnode->processed_ptr);
Embarcadero::CXL::store_fence();
```

**References:**
- Paper §3.2: "After Delegation writes header, flush."
- `docs/memory-bank/activeContext.md`: Task 1.2 - Integration Locations

---

### Pitfall 3: "Why does `offset_entry` have 128 bytes?"

**Analysis:**
```
offset_entry (128 bytes = 2 cache lines):

Cache Line 0 (bytes 0-63):
  - Broker writes: received, offset, counter, etc. (bytes 0-63)

Cache Line 1 (bytes 64-127):
  - Broker writes: batch_header fields (bytes 64-111)
  - Sequencer writes: total_order, ordered_offset (bytes 64-76)

  ⚠️ FALSE SHARING: Both write to cache line 1!
```

**Fix:** Split into two structs on separate cache lines:
```cpp
struct alignas(64) BrokerMessageMeta { /* broker fields */ };
struct alignas(64) SequencerMessageMeta { /* sequencer fields */ };
```

---

## Code Review Checklist

Before committing changes to this directory, verify:

- [ ] **Alignment Check:**
  - All shared structs have `alignas(64)`
  - `static_assert(sizeof(X) % 64 == 0, "...")`
- [ ] **Ownership Mapping:**
  - Create a table mapping each field to its writer (Broker, Sequencer, Replication, etc.)
  - Ensure no two writers share a cache line
- [ ] **Volatile Semantics:**
  - All fields read by remote hosts are marked `volatile`
  - Prevents compiler reordering across flush boundaries
- [ ] **Cache Flush Integration:**
  - After writing to CXL, call `flush_cacheline()` + `store_fence()`
  - See `docs/memory-bank/activeContext.md` Task 1.2 for hot path locations
- [ ] **Padding Verification:**
  - Use `pahole` to check for unintended padding or holes
  - Example: `pahole -C BrokerMetadata build/src/cxl_manager/libcxl_manager.a`
- [ ] **Migration Compatibility:**
  - If changing existing structs, implement dual-write pattern
  - Write to both old (TInode) and new (Bmeta) for 2 release cycles
  - See `docs/memory-bank/systemPatterns.md` §5.2 for rollback strategy

---

## Quick Reference

### Where is this structure used?

| Structure | Definition | Primary Usage | Tests |
|:----------|:----------|:--------------|:------|
| `TInode` | `cxl_datastructure.h:28696` | `cxl_manager.cc:37179` (UpdateTinodeOrder) | `test_cxl_manager.cc` |
| `offset_entry` | `cxl_datastructure.h:28750` | `topic.cc:30859` (CombinerThread) | `test_topic.cc` |
| `MessageHeader` | `cxl_datastructure.h:28780` | `topic.cc:31071` (BrokerScannerWorker) | `test_ordering.cc` |

### Related Documentation

- **Paper Spec:** `/home/domin/Embarcadero/docs/memory-bank/paper_spec.md`
  - Table 4: Blog Message Header (Target Layout)
  - Table 5: Broker Metadata (Target Layout)
  - §4: Concurrency & Coherence Laws
- **Migration Plan:** `docs/memory-bank/systemPatterns.md`
  - §1.3: Memory Layout Migration (TInode → Bmeta)
  - §2.2: Missing Primitives (clflushopt, sfence)
- **Active Tasks:** `docs/memory-bank/activeContext.md`
  - Task 2.1: Define BrokerMetadata structures
  - Task 2.2: Allocate Bmeta region in CXL
- **Byte-Level Layouts:** `docs/memory-bank/dataStructures.md`
  - §1: TInode False Sharing Analysis
  - §4: BlogMessageHeader Target Layout

---

## Contact / Questions

If you need to modify structures in this directory:

1. **Read First:** `docs/memory-bank/dataStructures.md` for byte-level layouts
2. **Check Migration Status:** `docs/memory-bank/activeContext.md` for current phase
3. **Run Validation:**
   ```bash
   # Check alignment
   pahole -C YourStruct build/src/cxl_manager/libcxl_manager.a

   # Check for false sharing
   scripts/check_cacheline_conflicts.sh src/cxl_manager/cxl_datastructure.h
   ```
4. **Ask:** If uncertain about ownership or cache-line boundaries, consult the Paper Spec (Table 5) or ask the team.

---

**Last Updated:** 2026-01-23
**Migration Phase:** Phase 2 (Cache Primitives + Metadata Split)
**Maintainer:** Systems Architecture Team
