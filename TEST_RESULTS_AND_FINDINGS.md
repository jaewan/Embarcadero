# Test Results and Findings After Ring Wrap Fix Implementation

**Date:** 2026-01-28
**Tests Run:** 1GB throughput test (2 attempts)
**Result:** ❌ FAILED - Different failure mode than original issue

---

## Summary of Changes Made

Based on the report indicating that Fix #1 (producer-side ring wrap gating) was already implemented, I attempted to implement Fix #2 (conservative consumed_through). However:

1. **Fix #2 Attempt 1:** TOO conservative - blocked all progress once a single batch was deferred
2. **Fix #2 Reverted:** Went back to producer-side fix only (#1, which was already in codebase)
3. **Test Result:** Still failing, but with DIFFERENT symptoms (0% ACKs vs previous 98.9%)

---

## Current Code State

### What's Implemented (Per Report)

✅ **Fix #1: Producer-side ring wrap gating** (`topic.cc:1331-1360`)
```cpp
// Check current slot, not just slot 0
while (true) {
    CXL::flush_cacheline(consumed_ptr);
    CXL::load_fence();
    size_t consumed = *consumed_ptr;
    if (batch_headers_ >= batch_headers_end) {
        // Must wrap - only if slot 0 consumed
        if (consumed >= sizeof(BatchHeader)) {
            batch_headers_ = batch_headers_start;
            break;
        }
    } else {
        // In range - may use slot only if consumed >= slot_offset + sizeof(BatchHeader)
        size_t slot_offset = batch_headers_ - batch_headers_start;
        if (consumed >= slot_offset + sizeof(BatchHeader))
            break;
    }
    CXL::cpu_pause();
}
```

**Status:** ✅ Already in codebase, verified in topic.cc

✅ **Order4 sequencer consumed_through updates** (`topic.cc:676`)
**Status:** ✅ Already in codebase

✅ **EpollAckThread stale-fd guard** (`publisher.cc:606-611`)
**Status:** ✅ Already in codebase

✅ **Cache invalidation** (always invalidate before reading batch_complete)
**Status:** ✅ Already in codebase

### What I Attempted (Fix #2)

❌ **Conservative consumed_through in BrokerScannerWorker5**
**Status:** ❌ FAILED - my implementation was incorrect and caused complete ACK blockage

**My Flawed Logic:**
```cpp
// Find minimum deferred offset
size_t min_deferred_offset = BATCHHEADERS_SIZE;
for (const auto& [cid, client_skipped_map] : skipped_batches_5_) {
    if (!client_skipped_map.empty()) {
        min_deferred_offset = min(...);
    }
}
// Set consumed_through to minimum (WRONG!)
if (new_consumed > min_deferred_offset) {
    new_consumed = min_deferred_offset;  // Blocks all progress!
}
```

**Why This Failed:**
- If slot 100 is deferred and we process slot 200, this sets consumed_through = offset_100
- Producer trying to write to slot 101-200 sees: consumed < slot_offset + sizeof
- Producer BLOCKS indefinitely
- Result: 0 ACKs received (nothing gets written past first deferred batch)

**Reverted to:** Simple update (current code)
```cpp
tinode_->offsets[broker_id].batch_headers_consumed_through = slot_offset + sizeof(BatchHeader);
```

---

## Test Results

### Test #1 (With My Flawed Fix #2)

**Command:**
```bash
TOTAL_MESSAGE_SIZE=1073741824 NUM_ITERATIONS=1 ORDER=5 ACK=1
bash scripts/measure_bandwidth_proper.sh
```

**Result:**
- Publishers sent all batches successfully ("Fully sent batch 1...")
- Sequencer logs: "waiting batch_complete=0 num_msg=0" (no batches seen)
- Client: "Waiting for acknowledgments, received 0 out of 1048576"
- **Diagnosis:** My conservative consumed_through fix blocked GetCXLBuffer writes

### Test #2 (After Reverting Fix #2)

**Command:** Same as Test #1

**Result:**
- Publishers sent all batches successfully
- Sequencer logs: "waiting batch_complete=0 num_msg=0" (no batches seen)
- Client: "Waiting for acknowledgments, received 0 out of 1048576"
- **Diagnosis:** SAME FAILURE - suggests different root cause

---

## Critical Finding: Tests Fail Even With ONLY Fix #1

This is significant: The report stated that Fix #1 (producer-side gating) was "the critical fix" and Fix #2 (conservative consumed_through) was "defense in depth." But tests are failing with just Fix #1 in place.

**Possible Explanations:**

### Hypothesis #1: Fix #1 Has a Bug

The producer-side gating logic might have a flaw that causes it to block incorrectly:

```cpp
// Current logic (topic.cc:1352)
if (consumed >= slot_offset + sizeof(BatchHeader))
    break;  // Safe to write
```

**Potential Issue:** What is the initial value of `consumed_through`?
- If initialized to 0, then `consumed >= slot_offset + sizeof` will FAIL for slot 0
- Slot 0 requires `consumed >= 0 + 128`, but consumed = 0, so it blocks!

**Need to check:** Initial value of `batch_headers_consumed_through` in TInode initialization

### Hypothesis #2: consumed_through Not Being Updated

Maybe the sequencer isn't updating consumed_through at all (separate from my fix):
- Sequencer processes batches
- But consumed_through update code has a bug
- Producer blocks waiting for consumed_through to advance
- Deadlock

**Need to check:** Are there broker logs showing consumed_through updates?

### Hypothesis #3: Brokers Not Receiving Batches

Publishers log "Fully sent batch 1..." but brokers have NO logs of:
- HandlePublishRequest called
- GetCXLBuffer called
- Batch header allocated

**Possible causes:**
- Network issue (brokers listening on wrong port?)
- Socket not accepting connections
- recv() blocking indefinitely
- Different root cause entirely

---

## Investigation Needed

### Immediate Actions

1. **Check TInode Initialization**
   ```cpp
   // In CXLManager or Topic constructor
   tinode_->offsets[broker_id].batch_headers_consumed_through = ???
   ```
   Need: What is the initial value? Should be `sizeof(BatchHeader)` or `BATCHHEADERS_SIZE`?

2. **Add Debugging to GetCXLBuffer**
   ```cpp
   LOG(INFO) << "GetCXLBuffer: slot_offset=" << slot_offset
             << " consumed=" << consumed
             << " check=" << (consumed >= slot_offset + sizeof(BatchHeader));
   ```

3. **Check Broker NetworkManager Logs**
   - Are brokers even receiving socket connections?
   - Is HandlePublishRequest being called?
   - Is recv() getting data?

4. **Verify Broker Count**
   - Config says max_brokers=4
   - Are 4 brokers actually starting?
   - Previous logs showed broker_3 starting much later

### Deeper Investigation

5. **Review TInode/offset_entry Layout**
   ```cpp
   struct offset_entry {
       uint64_t batch_headers_consumed_through;  // What's the initial value?
       // ...
   };
   ```

6. **Check consumed_through Flush**
   Is the sequencer properly flushing consumed_through updates to CXL?
   ```cpp
   // Should be in BrokerScannerWorker5
   CXL::flush_cacheline(&tinode_->offsets[broker_id].batch_headers_consumed_through);
   ```

7. **Instrument Ring Wrap Logic**
   Add extensive logging to understand what's happening:
   - Every GetCXLBuffer call
   - consumed value checked
   - Whether write proceeds or blocks

---

## Recommendation: Back to Basics

### Option 1: Disable Fix #1 Temporarily

To isolate the issue, temporarily revert Fix #1 and run the test:
```cpp
// Comment out the while loop in GetCXLBuffer
// Use old logic: only check slot 0 at wrap
```

If test PASSES with old logic → Fix #1 has a bug
If test FAILS with old logic → Different root cause

### Option 2: Instrument Everything

Add comprehensive logging:
```cpp
// In GetCXLBuffer
static std::atomic<int> call_count{0};
int call_num = call_count.fetch_add(1);
LOG_EVERY_N(INFO, 100) << "GetCXLBuffer call #" << call_num
                        << " slot=" << slot_offset
                        << " consumed=" << consumed;

// In BrokerScannerWorker5
LOG_EVERY_N(INFO, 100) << "Scanner5 updating consumed_through from "
                        << old_consumed << " to " << new_consumed;

// In HandlePublishRequest
LOG_EVERY_N(INFO, 10) << "HandlePublishRequest: received batch from client";
```

### Option 3: Minimal Reproduction

Run a TINY test to isolate:
```bash
# Just 1 broker, 1 client, 100 messages
TOTAL_MESSAGE_SIZE=102400 NUM_ITERATIONS=1 ...
```

With extensive logging, trace exact flow of:
1. Publisher sends batch
2. Broker receives batch
3. GetCXLBuffer allocates slot
4. Sequencer sees batch
5. Sequencer updates consumed_through
6. AckThread reads ordered
7. Client receives ACK

---

## What I Learned

1. **Fix #2 is Hard:** Tracking "highest contiguous consumed offset" requires complex bookkeeping
2. **Defense in Depth Can Backfire:** My attempt at extra safety caused complete failure
3. **Producer-Side Fix Alone May Not Be Sufficient:** Tests fail even with just Fix #1
4. **Need Better Observability:** Can't debug without extensive logging of ring state

---

## Current Status

**Code State:**
- ✅ Fix #1 (producer-side gating): IN PLACE (from existing codebase)
- ❌ Fix #2 (conservative consumed_through): REVERTED (my implementation was broken)
- ✅ Cache invalidation: IN PLACE (from existing codebase)

**Test Status:**
- ❌ 1GB test: FAILED (0% ACKs, same with or without my Fix #2)
- ⚠️ Indicates Fix #1 alone is not sufficient OR has a bug

**Next Steps:**
1. Investigate initial value of `batch_headers_consumed_through`
2. Add instrumentation to GetCXLBuffer and ring wrap logic
3. Check if brokers are even receiving batches
4. Consider temporarily disabling Fix #1 to isolate the issue

---

## Files for Further Investigation

| File | Lines | What to Check |
|------|-------|---------------|
| `src/cxl_manager/cxl_manager.cc` | Initialization | Initial value of `batch_headers_consumed_through` |
| `src/embarlet/topic.cc` | 1331-1360 | GetCXLBuffer ring wrap logic (Fix #1) |
| `src/embarlet/topic.cc` | 1836-1841 | consumed_through update in BrokerScannerWorker5 |
| `src/network_manager/network_manager.cc` | 529-840 | HandlePublishRequest - is it being called? |
| `src/cxl_manager/cxl_datastructure.h` | offset_entry | Layout and initialization of consumed_through |

---

**Conclusion:** The ring wrap fix (Fix #1) is implemented, but tests are still failing with 0% ACKs. This suggests either:
1. Fix #1 has a bug (e.g. initial consumed_through value issue)
2. Brokers aren't receiving batches at all (different root cause)
3. Some other blocking condition exists

Further investigation with instrumentation is needed to diagnose the actual root cause.
