# Implementation Plan: Non-Blocking Architecture Activation

**Date:** 2026-01-29
**Objective:** Enable and optimize the existing non-blocking NetworkManager architecture

---

## Overview

The codebase already contains a complete non-blocking architecture (`EMBARCADERO_USE_NONBLOCKING=true`). This plan describes the specific code changes needed to:

1. Enable it by default
2. Add safe ring gating
3. Optimize configuration
4. Fix any integration issues

---

## Phase 1: Enable Non-Blocking Mode with Safe Ring Gating

### 1.1 Add Non-Blocking Ring Gating to EmbarcaderoGetCXLBuffer

**File:** `src/embarlet/topic.cc`
**Location:** Lines 1132-1194

**Current Code (lines 1146-1171):**
```cpp
{
    // [[CRITICAL DECISION: No ring gating - trust ring size]]
    const unsigned long long int batch_headers_start =
        reinterpret_cast<unsigned long long int>(first_batch_headers_addr_);
    const unsigned long long int batch_headers_end = batch_headers_start + BATCHHEADERS_SIZE;

    absl::MutexLock lock(&mutex_);

    // Allocate space in log
    log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));

    // Allocate space for batch header (wrap within ring)
    batch_headers_log = reinterpret_cast<void*>(batch_headers_);
    batch_headers_ += sizeof(BatchHeader);
    if (batch_headers_ >= batch_headers_end) {
        batch_headers_ = batch_headers_start;
    }
    logical_offset = logical_offset_;
    logical_offset_ += batch_header.num_msg;
}
```

**Modified Code:**
```cpp
{
    const unsigned long long int batch_headers_start =
        reinterpret_cast<unsigned long long int>(first_batch_headers_addr_);
    const unsigned long long int batch_headers_end = batch_headers_start + BATCHHEADERS_SIZE;

    absl::MutexLock lock(&mutex_);

    // [[NON-BLOCKING RING GATING]]
    // Check if the next slot is free before allocating.
    // Read batch_headers_consumed_through with cache invalidation (non-coherent CXL).
    // If slot not free, return nullptr (fail-fast) instead of blocking.
    size_t next_slot_offset = static_cast<size_t>(batch_headers_ - batch_headers_start);

    // Invalidate cache before reading (critical for non-coherent CXL)
    CXL::flush_cacheline(&tinode_->offsets[broker_id_].batch_headers_consumed_through);
    CXL::load_fence();
    size_t consumed_through = tinode_->offsets[broker_id_].batch_headers_consumed_through;

    // Slot is free if:
    // 1. Ring is initialized (consumed_through == BATCHHEADERS_SIZE means all slots free), OR
    // 2. Sequencer has consumed past this slot (consumed_through >= next_slot_offset + sizeof(BatchHeader))
    //
    // Note: We compare byte offsets, not slot indices.
    // consumed_through is "first byte past last consumed slot".
    bool slot_free = (consumed_through == BATCHHEADERS_SIZE) ||
                     (consumed_through >= next_slot_offset + sizeof(BatchHeader));

    // Handle wrap-around case: if producer has wrapped but sequencer hasn't
    // The slot at next_slot_offset is free if sequencer is behind and hasn't reached it yet
    // This is already handled by the offset comparison above

    if (!slot_free) {
        // Ring full - fail fast, don't allocate
        // Caller (CXLAllocationWorker) will retry with exponential backoff
        log = nullptr;
        batch_header_location = nullptr;
        static std::atomic<size_t> ring_full_count{0};
        size_t count = ring_full_count.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 10 || count % 1000 == 0) {
            LOG(WARNING) << "EmbarcaderoGetCXLBuffer: Ring full (slot_offset=" << next_slot_offset
                         << ", consumed_through=" << consumed_through
                         << ", count=" << count << ")";
        }
        return nullptr;
    }

    // Slot is free - proceed with allocation
    // Allocate space in log
    log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));

    // Allocate space for batch header (wrap within ring)
    batch_headers_log = reinterpret_cast<void*>(batch_headers_);
    batch_headers_ += sizeof(BatchHeader);
    if (batch_headers_ >= batch_headers_end) {
        batch_headers_ = batch_headers_start;
    }
    logical_offset = logical_offset_;
    logical_offset_ += batch_header.num_msg;
}
```

### 1.2 Update Configuration Default

**File:** `src/common/configuration.h`
**Location:** Line 89

**Current:**
```cpp
ConfigValue<bool> use_nonblocking{false, "EMBARCADERO_USE_NONBLOCKING"};
```

**Modified:**
```cpp
ConfigValue<bool> use_nonblocking{true, "EMBARCADERO_USE_NONBLOCKING"};
```

### 1.3 Add Ring Full Metric to CXLAllocationWorker

**File:** `src/network_manager/network_manager.h`
**Location:** Add to private members

**Add:**
```cpp
std::atomic<size_t> metric_ring_full_{0};
```

**File:** `src/network_manager/network_manager.cc`
**Location:** Line 2059-2080 (CXLAllocationWorker ring-full handling)

**Modify retry logging to include metric:**
```cpp
if (!cxl_buf) {
    // CXL ring full, retry with exponential backoff
    batch.conn_state->cxl_allocation_attempts++;
    metric_cxl_retries_.fetch_add(1, std::memory_order_relaxed);
    metric_ring_full_.fetch_add(1, std::memory_order_relaxed);  // ADD THIS

    size_t ring_full_total = metric_ring_full_.load(std::memory_order_relaxed);
    if (ring_full_total <= 10 || ring_full_total % 1000 == 0) {
        LOG(WARNING) << "CXLAllocationWorker: Ring full, retry "
                     << batch.conn_state->cxl_allocation_attempts
                     << " (total_ring_full=" << ring_full_total << ")";
    }
    // ... rest of retry logic
}
```

---

## Phase 2: Configuration Optimization

### 2.1 Update Default Configuration

**File:** `src/common/configuration.h`
**Location:** Lines 90-93

**Current:**
```cpp
ConfigValue<int> staging_pool_buffer_size_mb{2, "EMBARCADERO_STAGING_POOL_BUFFER_SIZE_MB"};
ConfigValue<int> staging_pool_num_buffers{32, "EMBARCADERO_STAGING_POOL_NUM_BUFFERS"};
ConfigValue<int> num_publish_receive_threads{4, "EMBARCADERO_NUM_PUBLISH_RECEIVE_THREADS"};
ConfigValue<int> num_cxl_allocation_workers{2, "EMBARCADERO_NUM_CXL_ALLOCATION_WORKERS"};
```

**Modified (for 10GB workload):**
```cpp
ConfigValue<int> staging_pool_buffer_size_mb{2, "EMBARCADERO_STAGING_POOL_BUFFER_SIZE_MB"};
ConfigValue<int> staging_pool_num_buffers{128, "EMBARCADERO_STAGING_POOL_NUM_BUFFERS"};  // 256MB total
ConfigValue<int> num_publish_receive_threads{8, "EMBARCADERO_NUM_PUBLISH_RECEIVE_THREADS"};  // 1 per 2 pubs
ConfigValue<int> num_cxl_allocation_workers{4, "EMBARCADERO_NUM_CXL_ALLOCATION_WORKERS"};  // Match recv threads
```

### 2.2 Update YAML Configuration

**File:** `config/embarcadero.yaml`
**Location:** Add to network section

**Add:**
```yaml
network:
  use_nonblocking: true
  staging_pool_buffer_size_mb: 2
  staging_pool_num_buffers: 128
  num_publish_receive_threads: 8
  num_cxl_allocation_workers: 4
```

---

## Phase 3: Fix Integration Issues

### 3.1 Ensure Consumed-Through Initialization

The sequencer initializes `batch_headers_consumed_through` to `BATCHHEADERS_SIZE` to indicate all slots are free.

**File:** `src/embarlet/topic_manager.cc`
**Verify:** Line 95-98

```cpp
// Semantics: "first byte past last consumed slot". BATCHHEADERS_SIZE means
// "all slots [0, size) are available" (no deferred batches)
tinode->offsets[broker_id_].batch_headers_consumed_through = BATCHHEADERS_SIZE;
```

**This should already be correct.** Verify that this initialization happens before any allocation.

### 3.2 Ensure Cache Invalidation in Sequencer

The sequencer must always invalidate before reading `batch_complete`:

**File:** `src/embarlet/topic.cc`
**Location:** Lines 1453-1454 (BrokerScannerWorker5)

**Verify:**
```cpp
CXL::flush_cacheline(current_batch_header);
CXL::load_fence();
```

**This is already correct.** The invalidation happens before every read.

### 3.3 Ensure Consumed-Through Update After Processing

**File:** `src/embarlet/topic.cc`
**Location:** Lines 1553-1558 (BrokerScannerWorker5)

**Verify:**
```cpp
size_t slot_offset = reinterpret_cast<uint8_t*>(header_to_process) -
    reinterpret_cast<uint8_t*>(ring_start_default);
tinode_->offsets[broker_id].batch_headers_consumed_through = slot_offset + sizeof(BatchHeader);
CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(
    &tinode_->offsets[broker_id].batch_headers_consumed_through)));
CXL::store_fence();
```

**This is already correct.** The update and flush happen after each batch is processed.

---

## Phase 4: Testing Plan

### 4.1 Unit Tests

1. **Ring gating test:** Allocate slots until consumed_through is reached, verify nullptr return
2. **Wrap-around test:** Allocate, consume, wrap, verify correct slot availability check
3. **Concurrent test:** Multiple threads allocating, verify no race conditions

### 4.2 Integration Tests

1. **1GB test with non-blocking mode:**
   ```bash
   EMBARCADERO_USE_NONBLOCKING=1 ./scripts/run_throughput.sh 1GB 1024
   ```
   Expected: 100% ACK, >500 MB/s

2. **10GB test with non-blocking mode:**
   ```bash
   EMBARCADERO_USE_NONBLOCKING=1 ./scripts/run_throughput.sh 10GB 1024
   ```
   Expected: 100% ACK, improvement over 37.6%

3. **Ring stress test:** Fill ring beyond consumed_through, verify retry/recovery

### 4.3 Performance Benchmarks

| Test | Metric | Baseline | Target |
|------|--------|----------|--------|
| 1GB  | Throughput | 456 MB/s | 1-2 GB/s |
| 1GB  | ACK % | 100% | 100% |
| 10GB | Throughput | 46 MB/s | 1+ GB/s |
| 10GB | ACK % | 37.6% | 100% |

---

## Summary of Changes

### Files Modified

| File | Change | Risk |
|------|--------|------|
| `src/embarlet/topic.cc` | Add non-blocking ring gating | Medium - core allocation path |
| `src/common/configuration.h` | Update defaults | Low - config only |
| `src/network_manager/network_manager.cc` | Add ring_full metric | Low - logging only |
| `src/network_manager/network_manager.h` | Add metric member | Low - declaration only |
| `config/embarcadero.yaml` | Add network config | Low - config only |

### Code Additions

| Component | Lines of Code | Complexity |
|-----------|---------------|------------|
| Ring gating check | ~30 | Medium |
| Ring full logging | ~10 | Low |
| Metric addition | ~5 | Low |
| Config updates | ~10 | Low |

**Total:** ~55 lines of code changes

### Rollback Plan

If issues are found:
1. Set `EMBARCADERO_USE_NONBLOCKING=false` to revert to blocking mode
2. Or revert the ring gating code in topic.cc

---

## Appendix: Quick Start Commands

### Enable non-blocking mode for testing:
```bash
export EMBARCADERO_USE_NONBLOCKING=1
export EMBARCADERO_STAGING_POOL_NUM_BUFFERS=128
export EMBARCADERO_NUM_PUBLISH_RECEIVE_THREADS=8
export EMBARCADERO_NUM_CXL_ALLOCATION_WORKERS=4
```

### Run 10GB throughput test:
```bash
./scripts/run_throughput.sh 10GB 1024
```

### Check metrics:
```bash
grep -E "(ring_full|staging_exhausted|batch_complete)" /tmp/embarcadero*.log
```
