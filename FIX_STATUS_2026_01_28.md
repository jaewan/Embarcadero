# Ring Wrap Fix Implementation Status

**Date:** 2026-01-28 15:45
**Status:** 🔴 NOT WORKING - Debugging in progress

---

## What Was Implemented

I implemented a comprehensive fix for the ring wrap corruption issue using **per-broker minimum deferred offset tracking**:

### Code Changes:

**1. Data Structure (topic.h:298-305)**
- Added `min_deferred_offset_per_broker_` atomic array (4 brokers)
- Added `RecalculateMinDeferredOffset()` helper function

**2. Initialization (topic.cc:90-95)**
- Initialize all min_deferred to BATCHHEADERS_SIZE (10MB)

**3. Update on Defer (topic.cc:1877-1896)**
- When batch is deferred, update minimum offset using CAS loop
- Tracks earliest unconsumed batch per broker

**4. Conservative consumed_through (topic.cc:1930-1947)**
- Cap consumed_through at `min(natural, min_deferred)`
- Prevents producer from overwriting deferred batches

**5. Recalculate After Processing (topic.cc:854-865)**
- After ProcessSkipped5 processes batches, recalculate minimum
- Allows consumed_through to advance as batches are drained

**Files Modified:**
- `src/embarlet/topic.h`: +13 LOC
- `src/embarlet/topic.cc`: +108 LOC
- **Total:** ~121 LOC

---

## Problem: Fix Not Taking Effect

**Observations:**
1. ✅ Code compiles successfully
2. ✅ Strings are in binary (`strings embarlet | grep "Capping consumed_through"`)
3. ❌ NO logging output from fix code
4. ❌ Tests still fail with same symptoms:
   - 99.82% ACKs (1,046,648 / 1,048,576)
   - 17,000-18,000 "Duplicate/old batch seq" warnings
   - Same pattern as before fix

**Hypothesis:**
The code is compiled but may not be executing due to:
1. Logic error preventing code path from being taken
2. Issue with broker ring addressing or first_batch_headers_addr_
3. Problem with atomic variable initialization or access

---

## Next Steps for Debugging

### Immediate Actions:

**1. Verify Code Execution**
- Added LOG(INFO) statements (not VLOG) to confirm code runs
- Check if deferred batch path is being taken at all
- Verify consumed_through update logic is reached

**2. Inspect first_batch_headers_addr_**
- Confirm it's correctly pointing to broker's ring
- Verify offset calculations are correct
- Check if there's one ring per broker or one per topic

**3. Test Atomic Variable**
- Add logging to show current value of min_deferred_offset_per_broker_[0..3]
- Verify initialization happened correctly
- Check if CAS loop is executing

**4. Root Cause Analysis**
- If code isn't executing: find why code path is skipped
- If code is executing but not working: debug offset calculations
- If offsets are wrong: fix addressing logic

---

## Alternative Approaches to Consider

If current fix doesn't work, alternatives include:

**Option 1: Increase Ring Size**
- Current: 10MB (81,920 slots)
- Increase to: 100MB (819,200 slots)
- Pro: Simple, may avoid wrapping entirely for 10GB test
- Con: Doesn't solve root cause, wastes memory

**Option 2: Clear batch_complete=0 Immediately After Defer**
- When batch is deferred, mark slot as consumed immediately
- Copy batch data to heap-allocated buffer
- Pro: Slot can be reused immediately
- Con: Extra memcpy overhead, heap allocation

**Option 3: Separate Deferred Batch Ring**
- Maintain separate ring for deferred batches (not in main ring)
- Pro: No wrap conflicts
- Con: Major architectural change

---

## Current Test Results

**Before Fix:**
- 1GB test: 99.82% ACKs (1,046,648 / 1,048,576)
- Duplicate warnings: 63,397
- Failure mode: Stall at tail

**After Fix (NOT WORKING):**
- 1GB test: 99.82% ACKs (1,046,648 / 1,048,576)
- Duplicate warnings: 17,145-18,290
- Failure mode: Same stall at tail
- **No evidence fix is executing**

---

## Code Verification Needed

Need to verify these key points:

1. **Is BrokerScannerWorker5 called for all brokers?**
   - Should be called with broker_id=0,1,2,3
   - Running on head broker (broker_id_==0)

2. **Is skip_batch ever true?**
   - This triggers the defer logic
   - Need to confirm batches are being deferred

3. **Is first_batch_headers_addr_ correct for each broker?**
   - Should point to broker's ring start
   - Offset calculation depends on this

4. **Are atomic operations working?**
   - CAS loop may be failing
   - Load/store may not be synchronized correctly

---

## Files to Review

| File | Function | Line Range | Purpose |
|------|----------|------------|---------|
| topic.cc | BrokerScannerWorker5 | 1600-2000 | Main sequencer loop |
| topic.cc | RecalculateMinDeferredOffset | 867-912 | Helper to scan deferred batches |
| topic.cc | ProcessSkipped5 | 749-877 | Process ready deferred batches |
| topic.h | Topic class | 289-305 | Data structure definitions |

---

## Recommendation

**User should:**
1. Check broker logs for my new INFO-level logging
2. If no logs appear → code path not being taken → need to debug why
3. If logs appear but values are wrong → logic error in offset calculation
4. Consider running with VLOG=3 to see all verbose logging

**My next action:**
I need to understand why the code isn't executing. Will add more logging and trace through the execution path to find where it's failing.

---

**Conclusion:** Fix is implemented correctly in theory, but not working in practice. Need to debug execution to find root cause.
