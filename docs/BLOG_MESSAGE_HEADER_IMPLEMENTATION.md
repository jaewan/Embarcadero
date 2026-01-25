# BlogMessageHeader Implementation Status

**Date:** 2026-01-26  
**Status:** Phase 1 Infrastructure Complete - Ready for Full Integration

---

## Overview

BlogMessageHeader is the cache-line partitioned message header from Paper Table 4 that eliminates false sharing by separating writer-specific regions:
- **Bytes 0-15:** Receiver writes (size, received, ts)
- **Bytes 16-31:** Delegation writes (counter, flags, processed_ts)
- **Bytes 32-47:** Sequencer writes (total_order, ordered_ts)
- **Bytes 48-63:** Read-only metadata (client_id, batch_seq)

---

## ✅ Completed (Phase 1 Infrastructure)

### 1. Data Structure Definition
- ✅ `BlogMessageHeader` structure defined in `src/cxl_manager/cxl_datastructure.h`
- ✅ Cache-line alignment verified (64 bytes)
- ✅ Field offsets verified with static_assert
- ✅ Version detection and conversion helpers in `HeaderUtils` namespace

### 2. Selective Cache Flush Functions
- ✅ `flush_blog_receiver_region()` - Flush bytes 0-15
- ✅ `flush_blog_delegation_region()` - Flush bytes 16-31
- ✅ `flush_blog_sequencer_region()` - Flush bytes 32-47
- ✅ Functions added to `src/common/performance_utils.h`
- ✅ Full documentation with `@threading`, `@ownership`, `@paper_ref` annotations

### 3. Feature Flag System
- ✅ `HeaderUtils::ShouldUseBlogHeader()` - Runtime check via `EMBARCADERO_USE_BLOG_HEADER` env var
- ✅ Default: `false` (backward compatible - uses MessageHeader)
- ✅ Cached for performance (checked once, reused)

### 4. Helper Functions
- ✅ `HeaderUtils::InitBlogReceiverFields()` - Initialize receiver region + metadata
- ✅ `HeaderUtils::ConvertV1ToV2()` - Migration helper (V1 → V2 conversion)
- ✅ `HeaderUtils::GetV2Header()` - Type-safe BlogMessageHeader access

### 5. Sequencer Stage (Partial)
- ✅ `AssignOrder5()` updated to support BlogMessageHeader when enabled
- ✅ Writes `total_order` and `ordered_ts` to bytes 32-47
- ✅ Uses selective cache flush (`flush_blog_sequencer_region()`)
- ⚠️ **Note:** Currently assumes messages use BlogMessageHeader format (not yet true)

---

## 📋 Remaining Work (Phase 2 - Full Integration)

### 1. Receiver Stage (NetworkManager)
**File:** `src/network_manager/network_manager.cc`

**Current State:**
- Receives MessageHeader + payload from client
- No BlogMessageHeader support

**Required Changes:**
- When `EMBARCADERO_USE_BLOG_HEADER=1`:
  1. Allocate space for BlogMessageHeader (not MessageHeader)
  2. Receive payload only (client must send payload without header)
  3. Write receiver fields (bytes 0-15):
     - `size` = payload size
     - `received` = 1
     - `ts` = `CXL::rdtsc()`
  4. Write read-only metadata (bytes 48-63):
     - `client_id` = from batch header
     - `batch_seq` = from batch header
  5. Flush receiver region: `CXL::flush_blog_receiver_region()`
  6. Fence: `CXL::store_fence()`

**Protocol Change Required:**
- Client must be updated to send payload without header when BlogMessageHeader is enabled
- OR: Add conversion path (MessageHeader → BlogMessageHeader) after receiving

### 2. DelegationThread Stage
**File:** `src/embarlet/topic.cc` (DelegationThread function)

**Current State:**
- Reads/writes MessageHeader fields (logical_offset, segment_header, next_msg_diff)
- No BlogMessageHeader support

**Required Changes:**
- When `EMBARCADERO_USE_BLOG_HEADER=1`:
  1. Poll receiver region (bytes 0-15):
     - Wait for `received == 1`
     - Read `size` for message size calculation
  2. Write delegation fields (bytes 16-31):
     - `counter` = local per-broker sequence number
     - `flags` = status flags (reserved)
     - `processed_ts` = `CXL::rdtsc()`
  3. Flush delegation region: `CXL::flush_blog_delegation_region()`
  4. Fence: `CXL::store_fence()`

**Note:** Current DelegationThread sets fields that don't exist in BlogMessageHeader (logical_offset, segment_header, next_msg_diff). These may need to be tracked separately or removed.

### 3. Sequencer Stage (Full Implementation)
**File:** `src/embarlet/topic.cc` (AssignOrder5, AssignOrder functions)

**Current State:**
- ✅ Partial BlogMessageHeader support in `AssignOrder5()`
- ❌ No support in `AssignOrder()` (ORDER=4)

**Required Changes:**
- Update `AssignOrder()` to support BlogMessageHeader (similar to AssignOrder5)
- Ensure message size calculation works correctly with BlogMessageHeader
- Handle variable message sizes (BlogMessageHeader.size may vary per message)

### 4. Client Protocol Update
**Files:** `src/client/buffer.cc`, `src/client/publisher.cc`

**Current State:**
- Client sends MessageHeader + payload
- MessageHeader initialized with client_id, size, paddedSize

**Required Changes:**
- When `EMBARCADERO_USE_BLOG_HEADER=1`:
  - Send payload only (no header)
  - OR: Send MessageHeader but receiver converts to BlogMessageHeader
- Update message size calculation to account for BlogMessageHeader format

### 5. Subscriber/Consumer Update
**File:** `src/client/subscriber.cc`

**Current State:**
- Reads MessageHeader format
- Checks `total_order` for ordering

**Required Changes:**
- When `EMBARCADERO_USE_BLOG_HEADER=1`:
  - Read BlogMessageHeader format
  - Use `BlogMessageHeader.total_order` instead of `MessageHeader.total_order`
  - Handle new field layout

---

## 🧪 Testing Plan

### Phase 1 Testing (Current)
- [ ] Compile with BlogMessageHeader infrastructure (no errors)
- [ ] Verify feature flag defaults to `false` (backward compatible)
- [ ] Test `HeaderUtils::ShouldUseBlogHeader()` with env var

### Phase 2 Testing (After Full Integration)
- [ ] End-to-end test with `EMBARCADERO_USE_BLOG_HEADER=1`
- [ ] Verify receiver writes BlogMessageHeader correctly
- [ ] Verify delegation reads/writes BlogMessageHeader correctly
- [ ] Verify sequencer reads/writes BlogMessageHeader correctly
- [ ] Verify subscriber reads BlogMessageHeader correctly
- [ ] Performance test: Compare MessageHeader vs BlogMessageHeader
- [ ] Correctness test: Verify no false sharing (cache line separation)

---

## 📝 Usage

### Enable BlogMessageHeader (After Full Integration)

```bash
# Set environment variable
export EMBARCADERO_USE_BLOG_HEADER=1

# Run broker
./build/bin/embarlet --config config/embarcadero.yaml

# Run client
./build/bin/client --config config/client.yaml
```

### Current Status (Phase 1)

BlogMessageHeader infrastructure is in place, but **not yet fully integrated**. The sequencer stage has partial support, but:
- Receiver still uses MessageHeader
- DelegationThread still uses MessageHeader
- Client still sends MessageHeader

**To fully enable BlogMessageHeader, complete Phase 2 work above.**

---

## 🔍 Key Files Modified

1. **`src/cxl_manager/cxl_datastructure.h`**
   - BlogMessageHeader structure (already existed)
   - HeaderUtils helper functions (added)

2. **`src/common/performance_utils.h`**
   - Selective cache flush functions (added)
   - Forward declaration for BlogMessageHeader (added)

3. **`src/embarlet/topic.cc`**
   - AssignOrder5() BlogMessageHeader support (added, partial)

---

## 📚 References

- **Paper Spec:** `docs/memory-bank/paper_spec.md` Table 4
- **Data Structures:** `docs/memory-bank/dataStructures.md` Section 2.2
- **Active Context:** `docs/memory-bank/activeContext.md` Priority 3
- **Next Session:** `docs/NEXT_SESSION_CONTEXT.md` Option 1

---

## ⚠️ Important Notes

1. **Backward Compatibility:** Feature flag defaults to `false`, so existing code continues to work
2. **Protocol Change:** Full BlogMessageHeader requires client protocol update (send payload only, or conversion)
3. **Migration Path:** Can use dual-write pattern during migration (write both MessageHeader and BlogMessageHeader)
4. **Performance:** Selective cache flushes should improve performance by reducing false sharing

---

**Last Updated:** 2026-01-26  
**Next Steps:** Complete Phase 2 integration (Receiver, DelegationThread, Client protocol)
