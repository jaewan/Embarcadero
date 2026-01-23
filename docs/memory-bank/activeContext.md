# Active Context: Current Session State

**Last Updated:** 2026-01-23
**Session Focus:** Phase 2 Migration - Memory Layout Alignment with Paper Spec
**Status:** Gap Analysis Complete | Implementation Planning

---

## Current Focus

**Phase 2: Aligning Memory Layout with Paper Specification**

We are migrating from the current TInode-based architecture to the paper's Bmeta/Blog/Batchlog model. The immediate priority is implementing **missing cache coherence primitives** and **restructuring core data layouts** to eliminate false sharing.

**Critical Path:**
1. ✅ Gap analysis complete (see `systemPatterns.md`)
2. ⏳ **CURRENT:** Implement cache flush primitives
3. ⏳ Add BrokerMetadata structures (Bmeta)
4. ⏳ Refactor MessageHeader for strict cache-line ownership

---

## The Scratchpad: Missing Primitives & Core TODOs

### Priority 1: Cache Coherence Protocol (CRITICAL GAP)

**Issue:** Code uses generic `__atomic_thread_fence()` but lacks explicit cache flushes required for non-coherent CXL memory.

**Reference:** `systemPatterns.md` Section 2 - Missing Primitives

#### [ ] Task 1.1: Implement CXL Cache Primitives

**File:** Create `src/common/performance_utils.h`

**Required Functions:**
```cpp
namespace Embarcadero {
namespace CXL {
    void flush_cacheline(const void* addr);  // _mm_clflushopt
    void store_fence();                       // _mm_sfence
    void load_fence();                        // _mm_lfence
    void cpu_pause();                         // _mm_pause
}
}
```

**Implementation Checklist:**
- [ ] Create `src/common/performance_utils.h` header
- [ ] Add x86-64 intrinsic implementations
- [ ] Add ARM fallback implementations (`__builtin___clear_cache`)
- [ ] Add compile-time architecture detection (`#ifdef __x86_64__`)
- [ ] Write unit test in `test/common/performance_utils_test.cc`

**Acceptance Criteria:**
- Compiles on x86-64 (Intel/AMD) with `-march=native`
- Compiles on ARM64 with graceful fallback
- No runtime overhead when CXL emulation mode is disabled

---

#### [ ] Task 1.2: Integrate Cache Flushes into Hot Path

**Locations (from systemPatterns.md Section 2):**

1. **CombinerThread** - `src/embarlet/topic.cc:30859`
   ```cpp
   // BEFORE:
   UpdateTInodeWritten(logical_offset_, ...);

   // AFTER:
   UpdateTInodeWritten(logical_offset_, ...);
   CXL::flush_cacheline(&tinode->offsets[broker_id_]);
   CXL::store_fence();
   ```

2. **BrokerScannerWorker** - `src/embarlet/topic.cc:31071`
   ```cpp
   // After assigning total_order:
   msg_to_order[broker]->total_order = seq;
   CXL::flush_cacheline(msg_to_order[broker]);
   CXL::store_fence();
   ```

3. **UpdateTinodeOrder** - `src/cxl_manager/cxl_manager.cc:37529`
   ```cpp
   // After updating tinode metadata:
   tinode->offsets[broker_id].ordered_offset = offset;
   CXL::flush_cacheline(&tinode->offsets[broker_id]);
   CXL::store_fence();
   ```

**Integration Checklist:**
- [ ] Add `#include "common/performance_utils.h"` to `topic.cc`
- [ ] Add flush after `UpdateTInodeWritten()` in CombinerThread
- [ ] Add flush after `total_order` assignment in BrokerScannerWorker
- [ ] Add flush after metadata updates in `UpdateTinodeOrder()`
- [ ] Add feature flag `ENABLE_CXL_FLUSHES` in config
- [ ] Performance regression test (verify <5% overhead)

**Acceptance Criteria:**
- No ordering violations in `test/embarlet/message_ordering_test.cc`
- Throughput degradation <5% (currently 9.31GB/s baseline)

---

### Priority 2: Memory Layout Restructuring

**Issue:** `offset_entry` mixes broker-writable and sequencer-writable fields in the same cache line (FALSE SHARING).

**Reference:** `dataStructures.md` Section 1.2 - offset_entry Analysis

#### [ ] Task 2.1: Define BrokerMetadata Structures

**File:** `src/cxl_manager/cxl_datastructure.h` (append after line 28744)

**Add Structures:**
```cpp
// NEW: Split Bmeta per Single Writer Principle
struct alignas(64) BrokerLocalMeta {
    volatile uint64_t log_ptr;
    volatile uint64_t processed_ptr;
    volatile uint32_t replication_done;
    volatile uint32_t _reserved1;
    volatile uint64_t log_start;
    volatile uint64_t batch_headers_ptr;
    uint8_t padding[24];
};
static_assert(sizeof(BrokerLocalMeta) == 64, "Must fit in one cache line");

struct alignas(64) BrokerSequencerMeta {
    volatile uint64_t ordered_seq;
    volatile uint64_t ordered_ptr;
    volatile uint64_t epoch;
    volatile uint32_t status;
    volatile uint32_t _reserved2;
    uint8_t padding[32];
};
static_assert(sizeof(BrokerSequencerMeta) == 64, "Must fit in one cache line");

struct BrokerMetadata {
    BrokerLocalMeta local;
    BrokerSequencerMeta seq;
};
static_assert(sizeof(BrokerMetadata) == 128, "Must be exactly 2 cache lines");
```

**Checklist:**
- [ ] Add structure definitions to `cxl_datastructure.h`
- [ ] Add compile-time size assertions
- [ ] Verify alignment with `pahole -C BrokerMetadata build/bin/embarlet`
- [ ] Document ownership model in header comments

---

#### [ ] Task 2.2: Allocate Bmeta Region in CXL

**File:** `src/cxl_manager/cxl_manager.cc` (modify constructor ~line 37274)

**Add Allocation:**
```cpp
// CXLManager constructor
CXLManager::CXLManager(int broker_id, CXL_Type cxl_type, std::string head_ip) {
    // ... existing code ...

    // NEW: Allocate Bmeta region
    size_t bmeta_region_size = sizeof(BrokerMetadata) * NUM_MAX_BROKERS;
    bmeta_ = static_cast<BrokerMetadata*>(
        static_cast<void*>((uint8_t*)batchHeaders_ + BatchHeaders_Region_size)
    );

    // Zero-initialize Bmeta region (broker 0 only)
    if (broker_id_ == 0) {
        memset(bmeta_, 0, bmeta_region_size);
    }
}
```

**Checklist:**
- [ ] Add `BrokerMetadata* bmeta_;` member to `CXLManager` class
- [ ] Allocate Bmeta region after BatchHeaders
- [ ] Update memory layout calculation (see `dataStructures.md` Section 5.2)
- [ ] Add getter method `BrokerMetadata* GetBmeta(int broker_id)`
- [ ] Update total CXL size calculation to account for Bmeta overhead

---

#### [ ] Task 2.3: Implement Dual-Write Pattern

**File:** `src/embarlet/topic.cc` (modify UpdateTInodeWritten)

**Dual-Write Implementation:**
```cpp
inline void Topic::UpdateTInodeWritten(size_t written, size_t written_addr) {
    // LEGACY: Update TInode (backward compatibility)
    if (tinode_->replicate_tinode && replica_tinode_) {
        replica_tinode_->offsets[broker_id_].written = written;
        replica_tinode_->offsets[broker_id_].written_addr = written_addr;
    }
    tinode_->offsets[broker_id_].written = written;
    tinode_->offsets[broker_id_].written_addr = written_addr;

    // NEW: Update Bmeta (dual-write during migration)
    if (USE_NEW_BMETA) {
        bmeta_[broker_id_].local.processed_ptr = written_addr;
        CXL::flush_cacheline(&bmeta_[broker_id_].local);
        CXL::store_fence();
    }
}
```

**Checklist:**
- [ ] Add `BrokerMetadata* bmeta_;` member to `Topic` class
- [ ] Pass `bmeta` pointer from `TopicManager` to `Topic` constructor
- [ ] Implement dual-write in `UpdateTInodeWritten()`
- [ ] Add `USE_NEW_BMETA` feature flag to `config/embarcadero.yaml`
- [ ] Add logging to track dual-write execution

---

### Priority 3: MessageHeader Refactoring

**Issue:** Current `MessageHeader` has false sharing (Receiver/Combiner/Sequencer write to same cache line).

**Reference:** `dataStructures.md` Section 2.2 - BlogMessageHeader

#### [ ] Task 3.1: Define BlogMessageHeader

**File:** `src/cxl_manager/cxl_datastructure.h` (append after MessageHeader)

**Add Structure:**
```cpp
// NEW: Cache-line partitioned message header
struct alignas(64) BlogMessageHeader {
    // Bytes 0-15: Receiver Writes
    volatile uint32_t size;
    volatile uint32_t received;
    volatile uint64_t ts;

    // Bytes 16-31: Delegation Writes
    volatile uint32_t counter;
    volatile uint32_t flags;
    volatile uint64_t processed_ts;

    // Bytes 32-47: Sequencer Writes
    volatile uint64_t total_order;
    volatile uint64_t ordered_ts;

    // Bytes 48-63: Read-Only Metadata
    uint64_t client_id;
    uint32_t batch_seq;
    uint32_t _pad;
};
static_assert(sizeof(BlogMessageHeader) == 64, "Must be exactly 64 bytes");
static_assert(offsetof(BlogMessageHeader, counter) == 16, "Delegation at byte 16");
static_assert(offsetof(BlogMessageHeader, total_order) == 32, "Sequencer at byte 32");
```

**Checklist:**
- [ ] Add `BlogMessageHeader` definition
- [ ] Add byte-offset assertions
- [ ] Add version field to support coexistence with `MessageHeader`
- [ ] Document ownership boundaries in comments

---

#### [ ] Task 3.2: Implement Versioned Header Pattern

**File:** `src/cxl_manager/cxl_datastructure.h`

**Add Version Wrapper:**
```cpp
enum HeaderVersion : uint16_t {
    HEADER_V1 = 1,  // Legacy MessageHeader
    HEADER_V2 = 2,  // Paper spec BlogMessageHeader
};

struct alignas(64) MessageHeaderV2 {
    HeaderVersion version;
    uint16_t _pad;
    union {
        MessageHeader v1;
        BlogMessageHeader v2;
    };
};
```

**Checklist:**
- [ ] Add `MessageHeaderV2` wrapper
- [ ] Implement header version detection logic
- [ ] Update network receiver to write versioned headers
- [ ] Update sequencer to read both header versions
- [ ] Add migration path (V1 → V2 converter function)

---

### Priority 4: Pipeline Stage Separation

**Issue:** Current `BrokerScannerWorker` combines Stages 2+3, uses mutex instead of lock-free CAS.

**Reference:** `systemPatterns.md` Section 3 - Processing Pipeline

#### [ ] Task 4.1: Extract ReceiverThreadPool

**File:** Create `src/embarlet/receiver_pool.h` and `.cc`

**Class Interface:**
```cpp
class ReceiverThreadPool {
public:
    ReceiverThreadPool(size_t num_threads, BrokerMetadata* bmeta);

    // Allocate space in Blog, write payload, set received=1
    void* AllocateAndWrite(void* payload, size_t size);

private:
    void ReceiverWorker(int thread_id);
    std::atomic<uint64_t> log_head_;  // Atomic allocation pointer
    BrokerMetadata* bmeta_;
};
```

**Checklist:**
- [ ] Create `receiver_pool.h` and `receiver_pool.cc`
- [ ] Implement atomic `log_head_` allocation (lock-free)
- [ ] Implement zero-copy write to Blog
- [ ] Set `received=1` flag after write
- [ ] Add cache flush after setting `received` flag
- [ ] Integrate with `NetworkManager`

---

#### [ ] Task 4.2: Rename CombinerThread to DelegationThread

**File:** `src/embarlet/topic.cc` (refactor around line 30800)

**Refactor:**
```cpp
// OLD: void Topic::CombinerThread()
// NEW: void Topic::DelegationThread()

void Topic::DelegationThread() {
    BlogMessageHeader* header = ...;

    while (!stop_threads_) {
        // Poll received flag (from Receiver)
        while (!header->received) {
            if (stop_threads_) return;
            CXL::cpu_pause();
        }

        // Assign local counter
        header->counter = local_counter_++;
        header->processed_ts = rdtsc();

        // Flush delegation's cache-line portion (bytes 16-31)
        CXL::flush_cacheline((char*)header + 16);
        CXL::store_fence();

        // Update Bmeta.processed_ptr
        bmeta_[broker_id_].local.processed_ptr = (uint64_t)header;
        CXL::flush_cacheline(&bmeta_[broker_id_].local);
        CXL::store_fence();

        header = GetNextMessage();
    }
}
```

**Checklist:**
- [ ] Rename `CombinerThread` → `DelegationThread`
- [ ] Poll `received` flag instead of `paddedSize != 0`
- [ ] Write to `counter` field (local ordering)
- [ ] Update `Bmeta.local.processed_ptr`
- [ ] Add selective cache flush (bytes 16-31 only)
- [ ] Update thread creation in `Topic` constructor

---

#### [ ] Task 4.3: Refactor BrokerScannerWorker (Sequencer)

**File:** `src/embarlet/topic.cc` (modify around line 30995)

**Refactor to Poll Bmeta:**
```cpp
void Topic::BrokerScannerWorker(int broker_id) {
    // Poll Bmeta.processed_ptr instead of BatchHeader.num_msg
    uint64_t last_processed = 0;

    while (!stop_threads_) {
        // Poll for new processed_ptr
        uint64_t current_processed = bmeta_[broker_id].local.processed_ptr;

        if (current_processed == last_processed) {
            CXL::cpu_pause();
            continue;
        }

        // Process messages from last_processed to current_processed
        ProcessMessageRange(last_processed, current_processed);
        last_processed = current_processed;
    }
}
```

**Checklist:**
- [ ] Change poll target from `BatchHeader.num_msg` to `Bmeta.processed_ptr`
- [ ] Remove `global_seq_batch_seq_mu_` mutex
- [ ] Implement lock-free CAS for `next_batch_seqno` updates
- [ ] Write to `total_order` field in BlogMessageHeader
- [ ] Update `Bmeta.seq.ordered_ptr` after ordering
- [ ] Add selective cache flush (bytes 32-47 only)

---

### Priority 5: Testing & Validation

#### [ ] Task 5.1: Update Ordering Tests

**File:** `test/embarlet/message_ordering_test.cc`

**Test Cases:**
- [ ] Test with `USE_NEW_BMETA = false` (baseline)
- [ ] Test with `USE_NEW_BMETA = true` (new path)
- [ ] Verify Property 3d (Strong Total Ordering)
- [ ] Verify no regressions in latency/throughput

---

#### [ ] Task 5.2: Performance Regression Tests

**Baseline Metrics (from git history):**
- Throughput: 9.31 GB/s (4 brokers, 1KB messages)
- Latency: p99 < 2ms (low load), p99 < 5ms (bursty)

**Acceptance Criteria:**
- Throughput degradation: <5% (acceptable: >8.8 GB/s)
- Latency regression: <10% (p99 < 2.2ms low load)
- No ordering violations in 1B message stress test

**Scripts:**
- [ ] Run `scripts/run_throughput.sh` (baseline)
- [ ] Run with new code (comparison)
- [ ] Run `scripts/run_latency.sh`
- [ ] Generate comparison plots

---

## Recent Changes

### Session 2026-01-23

**Completed:**
1. ✅ Bootstrapped Memory Bank documentation system
2. ✅ Generated gap analysis (`systemPatterns.md`)
3. ✅ Documented build/runtime environment (`techContext.md`)
4. ✅ Created byte-level data structure reference (`dataStructures.md`)
5. ✅ Identified critical missing primitives (`clflushopt`, `sfence`)
6. ✅ Identified false sharing in `offset_entry` and `MessageHeader`

**Key Findings:**
- Current code uses `__atomic_thread_fence()` but lacks explicit cache flushes
- `offset_entry` has false sharing: broker writes 0-111, sequencer writes 64-76
- `MessageHeader` has non-contiguous ownership (Receiver: 0-7, 40-63)
- No CXL simulation libraries used (NUMA binding via `tmpfs` instead)

---

## Next Session Goals

### Immediate Goal (Next 2-4 Hours)

**Implement Cache Flush Primitives (Task 1.1)**

**Success Criteria:**
- `src/common/performance_utils.h` created with 4 functions
- Compiles on x86-64 with `-march=native`
- Unit test passes
- Ready for integration into hot path

**Steps:**
1. Create header file with inline functions
2. Add x86-64 intrinsics (`_mm_clflushopt`, `_mm_sfence`, `_mm_lfence`, `_mm_pause`)
3. Add ARM fallback (`__builtin___clear_cache`, `__sync_synchronize`)
4. Add architecture detection macros
5. Write unit test (test cache flush on mock CXL memory)
6. Update CMakeLists.txt to include new header

---

### Short-Term Goal (Next Session or Two)

**Integrate Cache Flushes into Hot Path (Task 1.2)**

**Success Criteria:**
- Flushes added to CombinerThread, BrokerScannerWorker, UpdateTinodeOrder
- Feature flag `ENABLE_CXL_FLUSHES` in config
- No ordering violations in tests
- Performance regression <5%

---

### Medium-Term Goal (This Week)

**Define and Allocate BrokerMetadata (Tasks 2.1, 2.2)**

**Success Criteria:**
- `BrokerMetadata` structures defined in `cxl_datastructure.h`
- Bmeta region allocated in CXL (2.5 KB for 20 brokers)
- Memory layout verified with `pahole`
- Dual-write pattern implemented

---

### Long-Term Goal (This Sprint)

**Complete Phase 2 Migration**

**Deliverables:**
- All Priority 1-3 tasks complete
- BlogMessageHeader implemented
- Pipeline stages separated
- Performance validated (>8.8 GB/s)
- Documentation updated

---

## Blockers & Dependencies

### Current Blockers: NONE

**All prerequisites met:**
- ✅ Gap analysis complete
- ✅ Memory Bank documentation complete
- ✅ Build environment understood
- ✅ No missing dependencies

### Future Dependencies

**Task 4.1 (ReceiverThreadPool) depends on:**
- Task 2.2: Bmeta allocation complete
- Task 3.1: BlogMessageHeader defined

**Task 4.3 (BrokerScannerWorker refactor) depends on:**
- Task 1.2: Cache flushes integrated
- Task 2.3: Dual-write pattern working

---

## Quick Reference

### Key Files for Next Session

**To Create:**
- `src/common/performance_utils.h` (Task 1.1)
- `test/common/performance_utils_test.cc` (Task 1.1)

**To Modify:**
- `src/embarlet/topic.cc` (Tasks 1.2, 4.2, 4.3)
- `src/cxl_manager/cxl_datastructure.h` (Tasks 2.1, 3.1)
- `src/cxl_manager/cxl_manager.cc` (Task 2.2)

**To Reference:**
- `docs/memory-bank/systemPatterns.md` (Section 2: Missing Primitives)
- `docs/memory-bank/dataStructures.md` (Section 1.3, 2.2: Target structures)
- `docs/memory-bank/paper_spec.md` (Section 4: Concurrency Laws)

### Commands for Next Session

```bash
# Build with debug symbols
cd /home/domin/Embarcadero/build
cmake -DCMAKE_BUILD_TYPE=Debug .. && make -j$(nproc)

# Run ordering test
./bin/throughput_test --mode both --message_size 1024 --total_size 1GB

# Verify structure alignment
pahole -C BrokerMetadata bin/embarlet

# Performance baseline
scripts/run_throughput.sh
```

---

## Session Notes

**Working Theory:**
The current system achieves 9.31 GB/s without explicit cache flushes because:
1. CXL emulation uses NUMA `tmpfs` (cache-coherent within same host)
2. All brokers + sequencer run on same physical machine in tests
3. Real CXL hardware (non-coherent) will REQUIRE flushes

**Validation Plan:**
- Test current code on real CXL hardware (if available)
- OR use memory barrier injection tool to simulate non-coherence
- Verify ordering violations occur WITHOUT flushes on real CXL

**Risk Mitigation:**
- Keep dual-write pattern for 2 release cycles minimum
- Add feature flag rollback capability
- Performance regression threshold: 5% (not 10%)

---

**Last Edit:** 2026-01-23 (Session end)
**Next Review:** Start of next session (estimate: within 24 hours)
