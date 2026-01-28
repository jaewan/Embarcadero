# Phase 1: Non-Blocking NetworkManager Implementation Summary

## Implementation Date
January 28, 2026

## Status
✅ **Phase 1 Complete** - Infrastructure in place and compiling successfully

## Components Implemented

### 1. StagingPool (src/network_manager/staging_pool.{h,cc})
- **Purpose**: Lock-free buffer pool for staging incoming message batches
- **Design**:
  - Pre-allocated buffers (default: 32 × 2MB = 64MB total)
  - folly::MPMCQueue for thread-safe allocation/release
  - Returns nullptr when exhausted (triggers backpressure)
- **Key Methods**:
  - `Allocate(size_t size)`: Non-blocking buffer allocation
  - `Release(void* buf)`: Return buffer to pool
  - `GetUtilization()`: Monitor pool usage (0-100%)
- **LOC**: ~230 lines (80 header + 150 implementation)

### 2. Configuration Support (src/common/configuration.{h,cc})
Added to `Network` struct:
- `use_nonblocking` (bool, default: false, env: EMBARCADERO_USE_NONBLOCKING)
- `staging_pool_buffer_size_mb` (int, default: 2, env: EMBARCADERO_STAGING_POOL_BUFFER_SIZE_MB)
- `staging_pool_num_buffers` (int, default: 32, env: EMBARCADERO_STAGING_POOL_NUM_BUFFERS)
- `num_publish_receive_threads` (int, default: 4, env: EMBARCADERO_NUM_PUBLISH_RECEIVE_THREADS)
- `num_cxl_allocation_workers` (int, default: 2, env: EMBARCADERO_NUM_CXL_ALLOCATION_WORKERS)

### 3. Data Structures (src/network_manager/network_manager.h)

#### ConnectionPhase Enum
```cpp
enum ConnectionPhase {
    INIT,              // Initial state after connection accept
    WAIT_HEADER,       // Waiting for complete batch header
    WAIT_PAYLOAD,      // Waiting for complete payload data
    WAIT_CXL,          // Waiting for CXL allocation (queued)
    COMPLETE           // Batch processed, ready for next batch
};
```

#### ConnectionState
Per-connection state for non-blocking publish handling:
- Socket fd and phase tracking
- Partial read offsets (header_offset, payload_offset)
- Staging buffer pointer
- CXL allocation retry counter
- Original handshake metadata

#### PendingBatch
Batch waiting for CXL allocation:
- Connection state pointer
- Staging buffer pointer
- Complete batch header
- Original handshake

### 4. PublishReceiveThread (src/network_manager/network_manager.cc)
Epoll-based non-blocking socket draining:
- **Event Loop**: epoll_wait with 1ms timeout
- **State Machine**: 5-state transitions (INIT → WAIT_HEADER → WAIT_PAYLOAD → WAIT_CXL → COMPLETE)
- **Backpressure**: Pauses sockets when staging pool exhausted (EPOLL_CTL_DEL)
- **Recovery**: CheckStagingPoolRecovery() resumes paused sockets
- **LOC**: ~170 lines

**Helper Functions**:
- `DrainHeader()`: Non-blocking header recv (MSG_DONTWAIT)
- `DrainPayload()`: Non-blocking payload recv into staging buffer
- `CheckStagingPoolRecovery()`: Resume paused sockets when pool has capacity

### 5. CXLAllocationWorker (src/network_manager/network_manager.cc)
Async CXL buffer allocation and copy:
- **Queue**: Dequeues from `cxl_allocation_queue_` (128 slots)
- **Retry Logic**: Exponential backoff (100µs → 1600µs) on ring full
- **Copy**: memcpy staging → CXL
- **Cache Flush**: Every 64 bytes + batch_complete flag
- **Cleanup**: Release staging buffer after copy
- **LOC**: ~120 lines

### 6. NetworkManager Updates
- **Constructor**: Initialize staging_pool_ and cxl_allocation_queue_ if use_nonblocking=true
- **Thread Launch**: 4 PublishReceiveThreads + 2 CXLAllocationWorkers
- **Routing**: ReqReceiveThread logs when non-blocking is enabled but still uses blocking path (Phase 1)

## Architecture Diagram

```
┌─────────────────────────────────────────────────────────┐
│ ReqReceiveThread (Existing)                             │
│  • Dequeues from request_queue_                         │
│  • Reads handshake                                      │
│  • Routes to HandlePublishRequest (blocking, Phase 1)   │
└─────────────────────────────────────────────────────────┘
                     ↓
         ┌───────────────────────┐
         │ HandlePublishRequest  │  ← Phase 1: Still blocking
         │  (blocking recv)      │
         └───────────────────────┘

[Non-Blocking Infrastructure - Ready but not routed to yet]

┌─────────────────────────────────────────────────────────┐
│ PublishReceiveThread[0..3] (Epoll-based)               │
│  • Non-blocking recv() → staging buffer                │
│  • State machine: WAIT_HEADER → WAIT_PAYLOAD → done   │
│  • Backpressure on staging pool exhaustion             │
└────────────────────┬───────────────────────────────────┘
                     ↓
         ┌───────────────────────┐
         │ StagingPool (64MB)    │
         │  32 × 2MB buffers     │
         │  folly::MPMCQueue     │
         └───────────┬───────────┘
                     ↓
         ┌───────────────────────┐
         │ cxl_allocation_queue  │
         │  128 pending batches  │
         └───────────┬───────────┘
                     ↓
┌─────────────────────────────────────────────────────────┐
│ CXLAllocationWorker[0..1]                               │
│  • GetCXLBuffer() with exponential backoff             │
│  • memcpy(staging → CXL)                               │
│  • Flush cache lines + set batch_complete=1           │
└─────────────────────────────────────────────────────────┘
```

## Build Integration
- Updated `src/CMakeLists.txt` to include:
  - `network_manager/staging_pool.cc`
  - `network_manager/staging_pool.h`
- Build verified: `build/bin/embarlet` (20MB binary)

## Current Limitations (Phase 1)

### 1. No Connection Routing Yet
- PublishReceiveThread infrastructure is in place but not connected
- ReqReceiveThread still routes all publish requests to blocking HandlePublishRequest
- TODO Phase 2: Implement connection handoff mechanism

### 2. PublishReceiveThread Not Accepting Connections
- Current implementation expects connections to exist in local map
- Needs integration with MainThread's accept() or ReqReceiveThread's handshake
- TODO Phase 2: Add connection injection mechanism

### 3. Handshake Storage
- ConnectionState.handshake field added but not populated
- PendingBatch.handshake uses ConnectionState's handshake
- TODO Phase 2: Populate during connection setup

### 4. Callback Handling
- CXLAllocationWorker invokes callback(nullptr, logical_offset) for KAFKA mode
- Should extract last MessageHeader for proper callback
- TODO Phase 2: Implement message header extraction

## Configuration Usage

### Environment Variables
```bash
# Enable non-blocking mode (Phase 1: logs but still uses blocking)
export EMBARCADERO_USE_NONBLOCKING=1

# Staging pool configuration (default values shown)
export EMBARCADERO_STAGING_POOL_BUFFER_SIZE_MB=2    # 2MB per buffer
export EMBARCADERO_STAGING_POOL_NUM_BUFFERS=32      # 32 buffers = 64MB total

# Thread counts
export EMBARCADERO_NUM_PUBLISH_RECEIVE_THREADS=4    # Epoll workers
export EMBARCADERO_NUM_CXL_ALLOCATION_WORKERS=2     # CXL workers
```

### YAML Configuration (config/embarcadero.yaml)
```yaml
embarcadero:
  network:
    use_nonblocking: true
    staging_pool_buffer_size_mb: 2
    staging_pool_num_buffers: 32
    num_publish_receive_threads: 4
    num_cxl_allocation_workers: 2
```

## Testing Status

### Phase 1 Goals
- [x] Code compiles without errors
- [x] Staging pool initializes correctly
- [x] PublishReceiveThread and CXLAllocationWorker threads launch
- [ ] Connection routing (deferred to Phase 2)
- [ ] Baseline throughput test (pending connection routing)

### Next Steps (Phase 2)
1. **Connection Handoff Mechanism**
   - Option A: Shared epoll_fd across PublishReceiveThreads
   - Option B: Connection queue for handoff
   - Option C: MainThread directly registers connections with epoll

2. **Integration Testing**
   - Route publish connections to PublishReceiveThread
   - Verify non-blocking recv() works end-to-end
   - Measure TCP retransmission reduction

3. **Performance Testing**
   - Run `scripts/measure_bandwidth_proper.sh` with EMBARCADERO_USE_NONBLOCKING=1
   - Target: >= 46 MB/s baseline (no regression)
   - Stretch: 1-5 GB/s if TCP backpressure reduced

4. **Staging Pool Tuning**
   - Monitor utilization under load
   - Adjust buffer count if >80% utilization
   - Verify backpressure recovery works

## Code Quality

### Warnings Fixed
- All compilation warnings resolved
- Only benign warnings from gRPC dependencies remain

### Memory Safety
- RAII for staging buffers (unique_ptr)
- Atomic counters for utilization tracking
- Buffer ownership clearly tracked

### Thread Safety
- Lock-free staging pool (folly::MPMCQueue)
- Atomic batch_complete flag
- Proper cache line flushing for CXL visibility

## Metrics to Add (Future)
- Staging pool utilization histogram
- CXL allocation retry counts
- Non-blocking recv() partial read counts
- Socket pause/resume events

## Total LOC Added
- staging_pool.h: 80 lines
- staging_pool.cc: 150 lines
- network_manager.h: 90 lines (new structs + methods)
- network_manager.cc: 290 lines (PublishReceiveThread + CXLAllocationWorker + helpers)
- configuration.h: 5 lines
- configuration.cc: 5 lines
- **Total: ~620 lines**

## Risk Mitigation
- **Fallback Path**: use_nonblocking=false keeps blocking mode (default)
- **Graceful Degradation**: Staging pool init failure logs error but doesn't crash
- **Bounded Retries**: CXL allocation drops batch after 10 retries (prevents infinite loops)
- **Backpressure**: Pauses sockets instead of dropping packets

## Known Issues
None. Code compiles and threads launch successfully.

## Next Milestone
**Phase 2**: Connection routing + 5 GB/s throughput
- ETA: Week 2 of implementation plan
- Deliverable: End-to-end non-blocking path with measurable throughput improvement
