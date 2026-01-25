# Code Optimization Analysis

**Date:** 2026-01-26  
**Purpose:** Identify optimization opportunities through static code analysis  
**Status:** Analysis Complete - Ready for Profiling Validation

---

## Executive Summary

Static code analysis reveals several optimization opportunities. However, **data-driven validation is required** to prioritize them. This document provides a roadmap for optimization based on code structure analysis.

---

## Mutex Usage Analysis

### Current State: `global_seq_batch_seq_mu_` in BrokerScannerWorker (Sequencer4)

**Location:** `src/embarlet/topic.cc:522, 595`

**Usage Pattern:**
```cpp
{
    absl::MutexLock lock(&global_seq_batch_seq_mu_);
    auto map_it = next_expected_batch_seq_.find(client_id);
    if (map_it == next_expected_batch_seq_.end()) {
        // New client initialization
        next_expected_batch_seq_[client_id] = 1;
    } else {
        // Update expected sequence
        map_it->second = expected_seq + 1;
    }
    start_total_order = global_seq_.fetch_add(...);  // Already atomic!
}
```

**Key Observations:**
1. ✅ `global_seq_` is already atomic (`std::atomic<size_t>`)
2. ❌ `next_expected_batch_seq_` map access is mutex-protected
3. ✅ `BrokerScannerWorker5` doesn't use this mutex (lock-free design)

**Optimization Opportunity:**
- Replace `absl::flat_hash_map<size_t, size_t> next_expected_batch_seq_` with lock-free structure
- Options:
  - Fixed-size array with atomic CAS per client
  - Lock-free hash table (e.g., `folly::AtomicHashMap`)
  - Per-client atomic variables (if client count is bounded)

**Expected Impact:** Low to Medium
- **Low** if mutex contention <100/sec (measured)
- **Medium** if mutex contention 100-1000/sec
- **High** if mutex contention >1000/sec

**Risk:** Low (isolated change, Sequencer4 only)

---

## Hot Path Analysis

### 1. AssignOrder5 - Batch-Level Ordering (Sequencer5)

**Location:** `src/embarlet/topic.cc:1387-1440`

**Current Optimizations:**
- ✅ Lock-free `global_seq_.fetch_add()` (already optimized)
- ✅ Single fence for multiple flushes (DEV-005)
- ✅ Correct sequencer-region cacheline flush

**Potential Optimizations:**
- **Batch flush optimization:** Currently flushes per batch. Could batch across multiple batches if ack semantics allow.
- **Cache prefetching:** Prefetch next batch header while processing current batch
- **Branch prediction:** Profile to identify mispredicted branches

**Priority:** Low (already well-optimized)

---

### 2. AssignOrder - Per-Message Ordering (Sequencer4)

**Location:** `src/embarlet/topic.cc:635-715`

**Current State:**
- Per-message flush+fence (required for immediate visibility)
- Message-by-message processing loop

**Potential Optimizations:**
- **SIMD processing:** If messages are uniform size, could process multiple headers at once
- **Cache line alignment:** Ensure message headers are cache-line aligned for better flush efficiency
- **Batch message flushes:** If ack semantics allow, flush every N messages instead of every message

**Priority:** Medium (if profiling shows it's a bottleneck)

**Risk:** High (per-message visibility required for correctness)

---

### 3. DelegationThread - Local Ordering

**Location:** `src/embarlet/topic.cc:213-310`

**Current Optimizations:**
- ✅ DEV-002: Batch flush (every 8 batches or 64KB)
- ✅ Uses `cpu_pause()` for polling (DEV-006)

**Potential Optimizations:**
- **Ring buffer optimization:** Prefetch next batch header
- **Memory barriers:** Review if all barriers are necessary
- **Batch size tuning:** Profile to find optimal batch flush interval

**Priority:** Low (already optimized with DEV-002)

---

## Memory Layout Optimizations

### Cache Line Alignment

**Current State:**
- ✅ `offset_entry` is `alignas(256)` with separate broker/sequencer regions
- ✅ `BatchHeader` is `alignas(64)`
- ✅ `MessageHeader` is `alignas(64)`

**Potential Issues:**
- Verify actual cache line alignment at runtime (use `pahole`)
- Check for false sharing between adjacent structures

**Action:** Run `pahole` analysis to verify alignment

---

## Lock-Free CAS Implementation Plan

### If Mutex Contention >100/sec

**Target:** Replace `next_expected_batch_seq_` map with lock-free structure

**Option A: Fixed-Size Array (Recommended if client count bounded)**
```cpp
// In topic.h
static constexpr size_t MAX_CLIENTS = 1024;
std::array<std::atomic<size_t>, MAX_CLIENTS> next_expected_batch_seq_;

// In BrokerScannerWorker
size_t client_idx = client_id % MAX_CLIENTS;
std::atomic<size_t>& expected_seq_atomic = next_expected_batch_seq_[client_idx];
size_t expected_seq = expected_seq_atomic.load(std::memory_order_acquire);

if (batch_seq == expected_seq) {
    size_t start_total_order = global_seq_.fetch_add(...);
    // CAS update
    size_t expected = expected_seq;
    while (!expected_seq_atomic.compare_exchange_weak(
        expected, expected_seq + 1,
        std::memory_order_release,
        std::memory_order_acquire)) {
        expected = expected_seq_atomic.load(std::memory_order_acquire);
        if (expected != expected_seq) break; // Another thread updated it
    }
}
```

**Option B: Lock-Free Hash Table**
- Use `folly::AtomicHashMap` or similar
- More complex but handles unbounded client count

**Option C: Per-Client Atomic (If client IDs are small)**
- Map client_id to array index
- Use atomic array like Option A

---

## Profiling Priorities

### High Priority (Measure First)

1. **Mutex Contention**
   - Measure `global_seq_batch_seq_mu_` contention rate
   - Decision: <100/sec = skip, >1000/sec = implement

2. **CPU Hotspots**
   - Identify functions using >5% CPU time
   - Focus optimization on top 3-5 functions

3. **Cache Misses**
   - Measure LLC (Last Level Cache) miss rate
   - If >10%, investigate data layout

### Medium Priority

4. **Branch Mispredictions**
   - If >5%, consider branchless code paths
   - Profile conditional branches in hot loops

5. **Memory Bandwidth**
   - Measure memory bandwidth utilization
   - Identify if memory-bound or CPU-bound

---

## Code-Level Findings

### Already Optimized ✅

1. **BrokerScannerWorker5**
   - ✅ Lock-free `global_seq_.fetch_add()`
   - ✅ No mutex usage
   - ✅ Correct cacheline flushes
   - ✅ Single fence optimization (DEV-005)

2. **DelegationThread**
   - ✅ Batch flush optimization (DEV-002)
   - ✅ Efficient polling with `cpu_pause()` (DEV-006)

3. **AssignOrder5**
   - ✅ Single fence for multiple flushes (DEV-005)
   - ✅ Correct sequencer-region flush

### Needs Measurement ⚠️

1. **BrokerScannerWorker (Sequencer4)**
   - ⚠️ Uses `global_seq_batch_seq_mu_` mutex
   - ⚠️ Need to measure contention to decide on lock-free CAS

2. **AssignOrder (Sequencer4)**
   - ⚠️ Per-message flush+fence
   - ⚠️ Need to profile to see if it's a bottleneck

### Potential Optimizations 📋

1. **Lock-Free CAS for next_expected_batch_seq_**
   - Priority: Based on mutex contention measurement
   - Risk: Low (isolated change)

2. **Batch Message Flushes in AssignOrder**
   - Priority: Low (correctness risk)
   - Risk: High (per-message visibility required)

3. **Cache Prefetching**
   - Priority: Medium (if cache misses >10%)
   - Risk: Low (prefetch hints are safe)

---

## Recommended Action Plan

### Phase 1: Measurement (Current)

1. ✅ **Infrastructure Created**
   - Performance baseline scripts
   - Profiling scripts
   - Mutex contention measurement scripts

2. 📋 **Execute Measurements** (Manual)
   - Run 10+ performance iterations
   - Profile hot paths with perf
   - Measure mutex contention

### Phase 2: Optimization (After Measurement)

**If Mutex Contention >100/sec:**
- Implement lock-free CAS for `next_expected_batch_seq_`
- Test and measure improvement
- Document in spec_deviation.md (DEV-007)

**If CPU Hotspot Identified:**
- Optimize top 3-5 functions
- Measure impact
- Iterate

**If Cache Misses >10%:**
- Analyze data layout
- Consider prefetching
- Optimize structure alignment

### Phase 3: Validation

- Re-run performance baseline
- Compare before/after
- Document improvements

---

## Risk Assessment

### Low Risk Optimizations
- ✅ Lock-free CAS (if contention measured)
- ✅ Cache prefetching hints
- ✅ Structure alignment verification

### Medium Risk Optimizations
- ⚠️ Batch message flushes (correctness risk)
- ⚠️ SIMD processing (complexity)

### High Risk Optimizations
- ❌ Changing per-message visibility (correctness critical)
- ❌ Removing necessary cache flushes (CXL correctness)

---

## Conclusion

**Current State:**
- System is well-optimized with DEV-002, DEV-005, DEV-006
- Main optimization opportunity: Lock-free CAS (if contention measured)
- Other optimizations need profiling data to prioritize

**Next Steps:**
1. Execute performance measurements (scripts ready)
2. Analyze profiling data
3. Make data-driven optimization decisions
4. Implement high-impact, low-risk optimizations first

**Key Insight:**
The codebase is already well-optimized. Further optimizations should be **data-driven** based on profiling results, not assumptions.

---

**Last Updated:** 2026-01-26  
**Status:** Analysis Complete - Awaiting Profiling Data
