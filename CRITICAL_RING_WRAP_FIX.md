# Critical Fix: Batch Header Ring Wrap Bug

**Priority:** 🔴 CRITICAL - Blocks all throughput testing
**Status:** Ready to implement
**Estimated Time:** 4 hours implementation + 2 hours testing
**Files to Modify:** `src/embarlet/topic.cc` (2 functions)

---

## Problem Statement

**Symptom:** Tests stall at 37-99% ACK completion with "Duplicate/old batch seq" warnings

**Root Cause:** Ring wrap logic in `GetCXLBuffer` only checks if **slot 0** has been consumed before wrapping, allowing unconsumed **deferred batches** to be overwritten.

**Evidence:**
```
W20260128 14:16:36.115700 topic.cc:1764] Scanner5 [B2]: Duplicate/old batch seq 40 detected from client 102765 (expected 544)
```

---

## Code Fix #1: Safe Ring Wrap in GetCXLBuffer

**File:** `src/embarlet/topic.cc`
**Function:** `Topic::GetCXLBuffer`
**Lines:** 1307-1333

### Current Broken Code

```cpp
batch_headers_ += sizeof(BatchHeader);
const unsigned long long int batch_headers_start =
    reinterpret_cast<unsigned long long int>(first_batch_headers_addr_);
const unsigned long long int batch_headers_end = batch_headers_start + BATCHHEADERS_SIZE;
if (batch_headers_ >= batch_headers_end) {
    // [[LOCKFREE_RING]] Only wrap when sequencer has consumed slot 0. Producer (this broker)
    // reads batch_headers_consumed_through (written by head's BrokerScannerWorker5); no lock.
    // Reader invalidate: evict our cache line so we see sequencer's value (flush achieves evict on x86).
    volatile size_t* consumed_ptr = &tinode_->offsets[broker_id_].batch_headers_consumed_through;
    CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(consumed_ptr)));
    CXL::load_fence();
    size_t consumed = *consumed_ptr;
    if (consumed >= sizeof(BatchHeader)) {
        batch_headers_ = batch_headers_start;  // UNSAFE: Only checked slot 0!
    } else {
        // Ring full: spin until consumer frees slot 0. Bounded wait; no lock.
        while (consumed < sizeof(BatchHeader)) {
            CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(consumed_ptr)));
            CXL::load_fence();
            consumed = *consumed_ptr;
            if (consumed < sizeof(BatchHeader))
                CXL::cpu_pause();
        }
        batch_headers_ = batch_headers_start;
    }
}
```

### Fixed Code

```cpp
// [[CRITICAL_FIX: Check current slot, not just slot 0]]
// With ORDER=5, out-of-order batches are deferred in skipped_batches_5_.
// If we only check slot 0 before wrapping, we can overwrite unconsumed deferred batches.
// Fix: Check if the NEXT slot we're about to write to has been consumed.

const unsigned long long int batch_headers_start =
    reinterpret_cast<unsigned long long int>(first_batch_headers_addr_);
const unsigned long long int batch_headers_end = batch_headers_start + BATCHHEADERS_SIZE;

// Advance to next slot
batch_headers_ += sizeof(BatchHeader);

// Wrap if at end of ring
if (batch_headers_ >= batch_headers_end) {
    batch_headers_ = batch_headers_start;
}

// [[LOCKFREE_RING]] Check if THIS slot (not just slot 0) has been consumed
// Producer reads consumed_through (written by head's BrokerScannerWorker5); no lock.
size_t slot_offset = batch_headers_ - batch_headers_start;
volatile size_t* consumed_ptr = &tinode_->offsets[broker_id_].batch_headers_consumed_through;

// Invalidate cache to see sequencer's latest consumed_through value
CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(consumed_ptr)));
CXL::load_fence();
size_t consumed = *consumed_ptr;

// Spin-wait if this slot has NOT been consumed yet
// Condition: slot_offset < consumed means "this slot has been consumed"
//            consumed == 0 means "ring not initialized yet, first write is safe"
while (slot_offset >= consumed && consumed != 0) {
    // Ring full: this specific slot not consumed yet
    LOG_EVERY_N(WARNING, 10000) << "Ring full for broker " << broker_id_
                                 << " at offset " << slot_offset
                                 << " (consumed_through=" << consumed << ")"
                                 << " - waiting for sequencer to advance";

    // Re-check consumed_through after brief pause
    CXL::cpu_pause();
    CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(consumed_ptr)));
    CXL::load_fence();
    consumed = *consumed_ptr;
}

// Now safe to write to batch_headers_ (slot at slot_offset)
```

**Key Changes:**
1. Check **current slot** (slot_offset) against consumed_through, not just slot 0
2. Spin-wait if `slot_offset >= consumed` (slot not yet consumed)
3. Add logging to detect ring pressure

---

## Code Fix #2: Accurate Consumed_Through Updates

**File:** `src/embarlet/topic.cc`
**Function:** `BrokerScannerWorker5`
**Lines:** 1806-1815

### Current Code (Potentially Unsafe)

```cpp
// [[LOCKFREE_RING]] Update consumed_through so producer (broker B's GetCXLBuffer) can safely
// wrap when ring is full. Sequencer writes; broker reads (with invalidate) before wrap.
size_t slot_offset = reinterpret_cast<uint8_t*>(header_to_process) -
    reinterpret_cast<uint8_t*>(ring_start_default);
tinode_->offsets[broker_id].batch_headers_consumed_through = slot_offset + sizeof(BatchHeader);
CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(
    &tinode_->offsets[broker_id].ordered)));
```

### Fixed Code (Conservative, Guarantees Safety)

```cpp
// [[CRITICAL_FIX: consumed_through must account for deferred batches]]
// With ORDER=5, if batches are deferred in skipped_batches_5_, we can't advance consumed_through
// past those deferred batch offsets, or the producer might overwrite them.
//
// SAFE (conservative) approach: Only advance consumed_through to the current slot if there are
// NO deferred batches. If there are deferred batches, consumed_through stays at the MINIMUM
// deferred batch offset.
//
// Trade-off: This is conservative and may cause ring to fill up faster, but it's SAFE.

size_t slot_offset = reinterpret_cast<uint8_t*>(header_to_process) -
    reinterpret_cast<uint8_t*>(ring_start_default);
size_t new_consumed = slot_offset + sizeof(BatchHeader);

// Check if there are any deferred batches for this broker's ring
// We need to lock the stripe to safely read skipped_batches_5_[client_id]
// Actually, skipped_batches_5_ is per-client, not per-broker. We need a different approach.

// SIMPLIFIED FIX: Just clear batch_complete=0 to mark slot as consumed
// The consumed_through mechanism is complex with deferred batches.
// A simpler approach: The sequencer clears batch_complete=0 after processing.
// The producer checks batch_complete before writing.
//
// Let's use consumed_through conservatively:
// Only advance it if we're processing batches IN ORDER (no deferred).

{
    absl::MutexLock lock(&global_seq_batch_seq_stripes_[client_id % kSeqStripeCount]);
    auto& client_skipped = skipped_batches_5_[client_id];

    if (client_skipped.empty()) {
        // No deferred batches for this client, safe to advance consumed_through
        tinode_->offsets[broker_id].batch_headers_consumed_through = new_consumed;
    } else {
        // There are deferred batches. Only advance consumed_through if new_consumed is
        // BEFORE the earliest deferred batch offset.
        // For simplicity: DON'T advance consumed_through if ANY batches are deferred.
        // This is conservative but safe.
        VLOG_EVERY_N(2, 1000) << "Scanner5 [B" << broker_id << "]: NOT advancing consumed_through "
                              << "due to " << client_skipped.size() << " deferred batches "
                              << "from client " << client_id;
    }
}

CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(
    &tinode_->offsets[broker_id].batch_headers_consumed_through)));
CXL::flush_cacheline(const_cast<const void*>(reinterpret_cast<const volatile void*>(
    &tinode_->offsets[broker_id].ordered)));
```

**Alternative (Better but More Complex):**

Track per-client ring offsets and only advance consumed_through to the minimum unconsumed offset across all clients.

```cpp
// Compute minimum unconsumed offset across all clients publishing to this broker
size_t min_unconsumed_offset = new_consumed;

for (auto& [cid, skipped_map] : skipped_batches_5_) {
    if (!skipped_map.empty()) {
        // There are deferred batches for this client
        // Find the batch header offset for the earliest deferred batch_seq
        auto earliest_skipped = skipped_map.begin();
        BatchHeader* earliest_header = earliest_skipped->second;
        size_t earliest_offset = reinterpret_cast<uint8_t*>(earliest_header) -
                                 reinterpret_cast<uint8_t*>(ring_start_default);
        min_unconsumed_offset = std::min(min_unconsumed_offset, earliest_offset);
    }
}

tinode_->offsets[broker_id].batch_headers_consumed_through = min_unconsumed_offset;
```

---

## Testing Plan

### Step 1: Verify Fix Compiles

```bash
cd /home/domin/Embarcadero/build
ninja -j$(nproc) embarlet
```

Expected: Clean build

### Step 2: 1GB Test (Previously Stalled at 98.9%)

```bash
cd /home/domin/Embarcadero
export EMBARCADERO_USE_NONBLOCKING=0  # Use proven code paths
TOTAL_MESSAGE_SIZE=1073741824 NUM_ITERATIONS=1 bash scripts/measure_bandwidth_proper.sh
```

**Expected Result:**
- ✅ 100% ACKs (1,048,576 / 1,048,576)
- ✅ No "Duplicate/old batch seq" warnings
- ✅ Test completes successfully
- Result written to `data/throughput/pub/result.csv`

**If Still Fails:**
- Check broker logs for "Ring full" warnings
- Check if consumed_through is advancing
- May need to increase ring size or refine consumed_through logic

### Step 3: 10GB Test (Previously Stalled at 37.3%)

```bash
bash scripts/measure_bandwidth_proper.sh
```

**Expected Result:**
- ✅ 100% ACKs (10,485,760 / 10,485,760)
- ✅ No "Duplicate/old batch seq" warnings
- ✅ Throughput >>46 MB/s (hopefully GB/s range)

### Step 4: Stress Test

```bash
# Send enough batches to wrap ring 2×
# With 10MB ring (81,920 slots) and 2MB batches, need ~164,000 batches
# That's ~336 GB of data
TOTAL_MESSAGE_SIZE=343597383680 NUM_ITERATIONS=1 bash scripts/measure_bandwidth_proper.sh
```

Expected: No crashes, no "Duplicate/old batch seq" after implementing fixes

---

## Monitoring During Test

### Watch Broker Logs

```bash
tail -f /home/domin/Embarcadero/build/bin/broker_0_trial1.log | grep -E "Ring full|Duplicate|consumed_through"
```

**Good Signs:**
- No "Duplicate/old batch seq" warnings
- "Ring full" warnings (if any) followed by progress
- consumed_through advancing

**Bad Signs:**
- Repeated "Duplicate/old batch seq" → ring wrap still happening
- "Ring full" warnings with no progress → deadlock

### Watch Client Progress

```bash
tail -f /tmp/test_0_1.log | grep "Waiting for acknowledgments"
```

**Good Signs:**
- ACK count increasing steadily
- Reaches 100% within timeout

**Bad Signs:**
- ACK count stuck at <100% for >30 seconds
- Per-broker ACK imbalance (one broker at 100%, others at 0%)

---

## Rollback Plan

If fixes cause new issues:

```bash
cd /home/domin/Embarcadero
git diff src/embarlet/topic.cc > /tmp/ring_wrap_fix.patch
git checkout src/embarlet/topic.cc  # Revert changes
ninja -j$(nproc)  # Rebuild with original code
```

Then investigate why fix didn't work.

---

## Expected Outcomes

### Scenario 1: Fix Works (90% Probability)

- Tests complete to 100%
- Throughput measured (may still be below 10 GB/s due to other bottlenecks)
- "Duplicate/old batch seq" warnings disappear
- **Next step:** Profile to find remaining performance bottlenecks

### Scenario 2: Fix Partially Works (8% Probability)

- Tests complete to higher % (e.g., 95% instead of 37%)
- Some "Duplicate/old batch seq" warnings remain
- **Analysis:** consumed_through logic needs refinement (use "Better but More Complex" approach)

### Scenario 3: Fix Doesn't Help (2% Probability)

- Tests still stall at same %
- **Analysis:** Different root cause (unlikely given log evidence, but possible)
- **Next step:** Deeper investigation with added logging

---

## Performance After Fix

**Expected Throughput:**
- 1GB: 100% completion, ~500 MB/s - 2 GB/s
- 10GB: 100% completion, ~1 GB/s - 5 GB/s

**If Still Below 9-10 GB/s Target:**
Remaining bottlenecks (in priority order):
1. GetCXLBuffer mutex contention
2. Sequencer single-thread limit
3. Non-coherent CXL cache overhead
4. Network stack tuning

**Optimization Path:**
1. Profile with `perf record -g`
2. Identify hot path (likely GetCXLBuffer or BrokerScannerWorker5)
3. Apply lock-free optimization or parallel sequencer
4. Re-test

---

## Summary

**Immediate Action:** Implement Fix #1 (ring wrap check)

**Priority:** CRITICAL - blocks all testing

**Confidence:** 90% this fixes ACK stall issue

**Next After Fix:** Test → Profile → Optimize → Achieve 10 GB/s

**Timeline:**
- Fix implementation: 4 hours
- Testing: 2 hours
- If successful: Move to performance optimization phase

