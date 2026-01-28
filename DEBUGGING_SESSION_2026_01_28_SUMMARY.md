# Debugging Session Summary - Ring Wrap Fix

**Date:** 2026-01-28
**Status:** 🟡 PARTIAL SUCCESS - Bugs found but fix incomplete

---

## Bugs Found

### Bug #1: Wrong Ring Base Address (FIXED)
**Location:** Lines 1881 and 901 in topic.cc
**Problem:** Used `first_batch_headers_addr_` (broker 0's ring) instead of per-broker ring address
**Impact:** Incorrect offset calculations for deferred batches
**Fix:** Use `ring_start_default` and computed per-broker address
**Result:** ✅ 1GB test passes (454-473 MB/s, 100% ACKs)

### Bug #2: Race Condition in Initialization (FOUND, FIX ATTEMPTED)
**Location:** Line 100-102 vs Line 177 in topic.cc
**Problem:** Sequencer5 thread starts at line 177, but min_deferred_offset_per_broker_ initialization at line 100-102 (comes AFTER in code flow)
**Impact:** Atomic array initialized to 0 by default, threads read 0 before initialization runs
**Fix:** Moved initialization to line 141 (before thread starts)
**Result:** ⚠️ 10GB test still fails with "Duplicate/old batch seq" warnings

---

## Test Results

### 1GB Test (After Bug #1 Fix)
- ✅ Test #1: 455.11 MB/s, 100% ACKs
- ✅ Test #2: 473.83 MB/s, 100% ACKs
- ✅ No "Duplicate/old batch seq" warnings
- ✅ Test completes successfully

### 10GB Test (After Bug #1 + Bug #2 Fixes)
- ❌ Test fails at 18.8-27.4% ACKs
- ❌ 34,708-57,373 "Duplicate/old batch seq" warnings
- ❌ Ring wrap still happening despite fixes

---

## Root Cause Analysis

The ring wrap fix requires THREE components to work together:

1. **Correct offset calculation** (Fixed in Bug #1) ✅
2. **Proper initialization** (Fixed in Bug #2) ✅
3. **Conservative consumed_through updates** (NOT VERIFIED) ❓

The issue is that despite fixing #1 and #2, the 10GB test still fails. This suggests:
- ProcessSkipped5 may not be calling RecalculateMinDeferredOffset
- The capping logic may not be executing
- There may be another bug in the logic

---

## Key Findings from Logs

### 1GB Test Logs:
- No "Capping consumed_through" logs (good - no ring pressure)
- No "ProcessSkipped5" logs (expected - added LOG_EVERY_N(100))
- min_deferred stays at BATCHHEADERS_SIZE (correct - no deferred batches)

### 10GB Test Logs:
- 34,708+ "Duplicate/old batch seq" warnings
- Pattern: Seeing seq 2, 8, 9, etc. when expecting seq 1024
- This means ring wrapped and overwrote deferred batches
- NO "Capping consumed_through" logs (BAD - should be capping!)
- NO "ProcessSkipped5" logs (suspicious)

---

## Hypothesis: Why Fix Isn't Working

### Theory #1: ProcessSkipped5 Not Running
- LOG_EVERY_N(INFO, 100) should have triggered multiple times in 10GB test
- But no logs appear
- Maybe ProcessSkipped5 is returning early (ready_batches empty)?

### Theory #2: Capping Logic Not Executing
- No "Capping consumed_through" logs
- This means either:
  - min_deferred >= BATCHHEADERS_SIZE (not being updated)
  - safe_consumed == natural_consumed (no capping needed - WRONG!)

### Theory #3: RecalculateMinDeferredOffset Has Bug
- May not be computing minimum correctly
- May not be storing result properly
- May not be called at all

---

## Next Steps to Debug

### Step 1: Verify Initialization
Add LOG at initialization to confirm it runs:
```cpp
LOG(INFO) << "Initialized min_deferred_offset_per_broker_ to " << BATCHHEADERS_SIZE;
```

### Step 2: Verify ProcessSkipped5 Is Called
Change LOG_EVERY_N to LOG to see ALL calls:
```cpp
LOG(INFO) << "ProcessSkipped5: ready_batches=" << ready_batches.size();
```

### Step 3: Verify RecalculateMinDeferredOffset Runs
Add LOG to show it's being called and what it computes:
```cpp
LOG(INFO) << "RecalculateMinDeferredOffset [B" << broker_id << "]: "
          << skipped_batches_5_.size() << " clients, computed min=" << min_offset;
```

### Step 4: Verify Capping Logic
Add LOG to show min_deferred value every time:
```cpp
LOG_EVERY_N(INFO, 100) << "consumed_through: natural=" << natural_consumed
                        << " min_deferred=" << min_deferred;
```

---

## Performance Bottleneck Analysis

Current throughput: **454-473 MB/s** (1GB test)
Target throughput: **9-10 GB/s**
**Gap: 20× slower than target**

### Possible Bottlenecks:

1. **GetCXLBuffer mutex contention**
   - 16 publishers contend on single lock
   - Each allocation blocks others
   - Solution: Lock-free ring buffer

2. **Sequencer serialization**
   - BrokerScannerWorker5 processes batches one at a time
   - Single-threaded bottleneck
   - Solution: Parallel sequencing or SIMD

3. **CXL cache overhead**
   - Frequent flush_cacheline + load_fence
   - Non-coherent CXL requires invalidation
   - Solution: Reduce flush granularity, batch operations

4. **Logging overhead**
   - LOG_EVERY_N still has cost (checking counter)
   - May be slowing hot path
   - Solution: Remove all logging in production

5. **Network tuning**
   - TCP parameters may not be optimized
   - Socket buffer sizes
   - Solution: Tune kernel parameters

---

## Recommended Action Plan

### Immediate (Correctness)
1. Add extensive logging to debug why fix isn't working in 10GB test
2. Verify ProcessSkipped5 + RecalculateMinDeferredOffset are called
3. Verify min_deferred is being updated correctly
4. Fix any remaining bugs in the logic

### Short-term (Performance)
1. Profile with `perf record -g` to identify hot paths
2. Remove all non-essential logging
3. Optimize identified bottlenecks

### Long-term (Architecture)
1. Implement lock-free GetCXLBuffer
2. Parallelize sequencer (SIMD or multiple threads)
3. Optimize CXL cache operations
4. Consider zero-copy optimizations

---

## Code Changes Made

### topic.h
- Added: `min_deferred_offset_per_broker_` atomic array
- Added: `RecalculateMinDeferredOffset()` helper function

### topic.cc
**Line 1882:** Fixed defer code to use `ring_start_default` ✅
```cpp
size_t deferred_slot_offset = batch_addr - reinterpret_cast<uint8_t*>(ring_start_default);
```

**Line 901-902:** Fixed RecalculateMinDeferredOffset to compute per-broker address ✅
```cpp
uint8_t* ring_start = reinterpret_cast<uint8_t*>(cxl_addr_) +
                      tinode_->offsets[broker_id].batch_headers_offset;
```

**Line 141-146:** Moved initialization before thread start ✅
```cpp
for (size_t i = 0; i < MAX_BROKERS_TRACKED; ++i) {
    min_deferred_offset_per_broker_[i].store(BATCHHEADERS_SIZE, std::memory_order_relaxed);
}
```

**Line 858-865:** Recalculate after ProcessSkipped5 ✅
```cpp
for (int broker_id : affected_brokers) {
    RecalculateMinDeferredOffset(broker_id);
}
```

**Line 1936-1963:** Conservative consumed_through capping ✅
```cpp
size_t min_deferred = min_deferred_offset_per_broker_[broker_id].load(std::memory_order_acquire);
size_t safe_consumed = std::min(natural_consumed, min_deferred);
```

---

## Lessons Learned

1. **Read-only code review caught first bug** - Wrong ring base address
2. **Race conditions are subtle** - Initialization order matters
3. **Logging is essential for debugging** - Can't debug without visibility
4. **Scale matters** - 1GB test passes, 10GB test fails (different code paths)
5. **Multiple bugs can compound** - Fixed 2 bugs, but 3rd bug remains

---

## Current Status

**Correctness:** ⚠️ 1GB works, 10GB fails
**Performance:** 🔴 454-473 MB/s (need 9-10 GB/s, 20× improvement)
**Next Action:** Add logging to debug why 10GB test fails

**Estimated Time to Fix:**
- Debug + fix remaining bug: 2-4 hours
- Performance optimization: 1-2 weeks (depending on bottleneck)
