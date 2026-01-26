# ORDER=4 Migration to BlogMessageHeader

**Status**: Planning for future implementation (after ORDER=5 stabilization)  
**Priority**: Medium (Phase 3 migration)  
**Estimated Scope**: Larger than ORDER=5 due to per-message sequencer writes

## Context

ORDER=4 (Sequencer 4) differs fundamentally from ORDER=5:
- ORDER=5: Sequencer assigns total_order at **batch level**; subscriber reconstructs per-message order from BatchMetadata + message index
- ORDER=4: Sequencer assigns total_order at **per-message level**; each message gets individual sequencer treatment

This means ORDER=4 requires:
1. **Per-message polling** by Delegation thread to detect when message is fully received (`received=1`)
2. **Per-message sequencer writes** to assign total_order individually
3. **Per-message boundary walking** using either `paddedSize` (v1) or computed size (v2)

## Key Challenges with BlogMessageHeader for ORDER=4

### 1. Message Polling
**Current (v1)**: `MessageHeader.paddedSize` indicates message boundary and completion status
```cpp
while (msg_hdr->paddedSize == 0) {
    // Wait for message to be fully written
    std::this_thread::yield();
}
```

**With v2**: BlogMessageHeader has `received` flag (bytes 8-11)
```cpp
while (msg_hdr->received == 0) {
    // Wait for received flag
    std::this_thread::yield();
}
```
**Migration impact**: Delegation thread must wait on `received` instead of `paddedSize`

### 2. Message Iteration
**Current (v1)**: Use `paddedSize` to step through messages
```cpp
msg_ptr = (MessageHeader*)((uint8_t*)msg_ptr + msg_ptr->paddedSize);
```

**With v2**: Must compute size from `BlogMessageHeader.size` field
```cpp
size_t padded = sizeof(BlogMessageHeader) + hdr->size;
padded = (padded + 63) & ~63;  // Align to 64B
msg_ptr = (BlogMessageHeader*)((uint8_t*)msg_ptr + padded);
```
**Migration impact**: All message iteration loops need dual-path logic

### 3. Sequencer Per-Message Writes
**Current (v1)**: Sequencer writes `MessageHeader.total_order` and `MessageHeader.ordered_ts`
```cpp
hdr->total_order = current_order;
hdr->ordered_ts = CXL::rdtsc();
CXL::flush_cacheline(hdr);
```

**With v2**: Sequencer writes same fields via BlogMessageHeader (bytes 32-39)
```cpp
v2_hdr->total_order = current_order;
v2_hdr->ordered_ts = CXL::rdtsc();
CXL::flush_blog_sequencer_region(v2_hdr);
```
**Migration impact**: No functional change, but must use v2 aware helpers

### 4. Async Completion Polling
**Current**: Clients poll `MessageHeader.total_order != 0` to detect ordering completion  
**With v2**: Same contract—clients still poll until `total_order` is set

## Implementation Plan (Post ORDER=5)

### Phase 3.1: Receiver In-Place Conversion (Similar to ORDER=5)
**File**: `src/network_manager/network_manager.cc`
- After batch fully received, if `order==4` and BlogHeader enabled:
  - Convert MessageHeader→BlogMessageHeader in-place (same as ORDER=5)
  - Flush per-message header once
  - **Single fence per batch**
- Gate: Check `HeaderUtils::ShouldUseBlogHeader()` and `order==4`

### Phase 3.2: Delegation Thread Message Polling
**File**: `src/embarlet/topic.cc` (DelegationThread)
- Update message polling logic:
  ```cpp
  if (using_blog_header) {
      BlogMessageHeader* hdr = (BlogMessageHeader*)msg_ptr;
      while (hdr->received == 0) std::this_thread::yield();
      msg_size = sizeof(BlogMessageHeader) + hdr->size;
      // Align to 64B
      msg_size = (msg_size + 63) & ~63;
  } else {
      MessageHeader* hdr = (MessageHeader*)msg_ptr;
      while (hdr->paddedSize == 0) std::this_thread::yield();
      msg_size = hdr->paddedSize;
  }
  ```

### Phase 3.3: Delegation Thread Message Iteration
**File**: `src/embarlet/topic.cc` (DelegationThread + any export paths)
- All message iteration loops need version-aware stepping:
  - v1: step by `hdr->paddedSize`
  - v2: step by `align64(sizeof(BlogMessageHeader) + hdr->size)`
- Locations:
  - `DelegationThread()` loop that processes per-message headers
  - `AssignOrder()` (Sequencer 4 main ordering loop) — currently uses `MessageHeader.paddedSize`
  - Export paths if they split messages

### Phase 3.4: Sequencer Per-Message Writes
**File**: `src/embarlet/topic.cc` (AssignOrder)
- Update `AssignOrder()` to use version-aware writes:
  ```cpp
  if (using_blog_header) {
      BlogMessageHeader* hdr = (BlogMessageHeader*)msg_ptr;
      hdr->total_order = order;
      hdr->ordered_ts = CXL::rdtsc();
      CXL::flush_blog_sequencer_region(hdr);
  } else {
      MessageHeader* hdr = (MessageHeader*)msg_ptr;
      hdr->total_order = order;
      hdr->ordered_ts = CXL::rdtsc();
      CXL::flush_cacheline(hdr);
  }
  ```
- **IMPORTANT**: Maintain DEV-005 batched fence pattern:
  - Flush per-message header (or batch of headers if large batch)
  - **One fence per batch** (not per-message!)

### Phase 3.5: Subscriber Parsing (ORDER=4 with BatchMetadata)
**File**: `src/client/subscriber.cc`
- For ORDER=4, subscriber still receives BatchMetadata (with `header_version`)
- **However**: With ORDER=4, `batch_total_order` should already be set in message headers by sequencer
- Parse logic:
  - If `header_version==2`: parse as BlogMessageHeader
  - Read `hdr->total_order` (already set by sequencer, not reconstructed like ORDER=5)
  - Return message pointer as before

### Phase 3.6: Export Path Updates
**File**: `src/network_manager/network_manager.cc` (SubscribeNetworkThread)
- Already handled in export-split-update for ORDER=5
- For ORDER=4 with large message splitting, reuse same logic:
  - If `using_blog_header`: compute size from `BlogMessageHeader.size`
  - Ensure alignment and no mid-message cuts

## Risks and Mitigations

| Risk | Likelihood | Mitigation |
|------|-----------|-----------|
| **Message boundary corruption** | Medium | Comprehensive bounds checking; test with sizes 64B, 4KB, 64KB, 1MB |
| **Polling deadlock** (wrong flag) | Low | Use same `received==1` as ORDER=5; test with congestion |
| **Per-message fence overhead** | Medium | **Strictly enforce batched fences**; benchmark DEV-005 pattern |
| **Subscriber parse regression** | Low | Parse both v1 and v2; compare against baseline |
| **Large message splitting edge case** | Low | Test alignment on 4K boundary; verify no corruption |

## Testing Strategy

### Unit Tests
- `test/embarlet/message_ordering_test.cc`: extend to cover ORDER=4 + BlogMessageHeader
  - Test per-message ordering with size variations
  - Test Delegation thread polling on `received` flag
  - Test message boundary detection

### Integration Tests
- `test/e2e/test_segment_allocation.sh`: run with `EMBARCADERO_USE_BLOG_HEADER=1` for ORDER=4
- New test: `test_order4_blog_header.sh`
  - Multi-broker, ORDER=4, 1-minute steady workload
  - Verify total_order correctness per message
  - Verify no data loss or reordering

### Benchmarks
- `scripts/run_throughput.sh` with `order_level=4` and feature flag enabled
  - Compare baseline vs BlogMessageHeader v2
  - Measure:
    - Throughput (msg/sec)
    - Latency (p50, p99)
    - Batch fence counts (verify one fence per batch, not per-message)

## Rollout Plan

1. **Develop & test** (1-2 sprints)
   - Implement all Phase 3.1-3.6 steps
   - Run unit + integration tests
   - Validate benchmark results

2. **Stabilize** (1 sprint)
   - Run 24h+ stress tests
   - Monitor for data loss, corruption, hangs
   - Check fence/flush overhead

3. **Gradual rollout**
   - Feature flag default: `EMBARCADERO_USE_BLOG_HEADER=0` (disabled)
   - Opt-in for initial users
   - Monitor production for issues

4. **Full migration** (when ORDER=5 stable + ORDER=4 proven)
   - Set feature flag default to `1`
   - Deprecate v1 code path (future task)

## Success Criteria

- ✅ All ORDER=4 tests pass with BlogMessageHeader enabled
- ✅ No performance regression vs baseline (throughput ≥ 99% of v1)
- ✅ Latency p50/p99 unchanged or better
- ✅ Fence count verification: exactly 1 fence per batch (not per-message)
- ✅ 24h stress test with zero data loss/corruption

## Dependencies & Blockers

- **Blocker**: ORDER=5 stabilization (currently in progress)
- **Dependency**: Receiver in-place conversion logic (shared with ORDER=5)
- **Prerequisite**: Feature flag + selective flush helpers (already implemented for ORDER=5)

## Follow-On Work (Post ORDER=4)

1. **Remove version detection heuristics**: Replace `HeaderUtils::DetectVersion()` with explicit version field everywhere
2. **Deprecate v1 code paths**: After 2-3 releases with v2 stable
3. **Optimize for v2-only**: Remove dual-path logic, clean up code
4. **Memory layout optimization**: Revisit BlogMessageHeader field ordering for ORDER=4 workloads
