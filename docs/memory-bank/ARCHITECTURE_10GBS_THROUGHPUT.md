# High-Throughput Shared Log Architecture: 10GB/s Target

**Date:** 2026-01-29
**Goal:** Achieve sustained 10GB/s throughput with low latency for the Embarcadero shared log system

---

## Executive Summary

The current system stalls at 37.6% ACK completion (3.9M / 10.5M messages) with a 90-second timeout. Root cause analysis reveals that the **blocking I/O + mutex contention** architecture in NetworkManager cannot sustain high throughput. However, **a non-blocking architecture already exists in the codebase** but is disabled by default.

**Recommended Path:**
1. Enable and validate the existing non-blocking architecture
2. Add non-blocking ring gating for safety
3. Optimize configuration and worker counts
4. Production hardening

---

## 1. Current Architecture Analysis

### 1.1 Blocking Mode (Default - `EMBARCADERO_USE_NONBLOCKING=false`)

```
Publisher → TCP → NetworkManager (blocking recv) → GetCXLBuffer (mutex) → CXL Ring
                         ↓
              [BLOCKS HERE on slow CXL allocation]
                         ↓
              [TCP buffers overflow]
                         ↓
              [Connection timeout after ~10 batches]
```

**Code Path:** `network_manager.cc:575-861`
- `recv(blocking)` reads BatchHeader (line 581)
- `GetCXLBuffer()` acquires mutex, allocates CXL slot (line 620)
- `recv(blocking)` reads payload into CXL buffer (line 648)
- Mark `batch_complete=1` and flush (line 811)

**Problem:** 16 publishers contend on single mutex in GetCXLBuffer. While one thread holds the mutex, others block. TCP receive buffers overflow, causing connection timeouts after ~10 batches per connection.

### 1.2 Non-Blocking Mode (Exists - `EMBARCADERO_USE_NONBLOCKING=true`)

```
Publisher → TCP → PublishReceiveThread (epoll, non-blocking recv)
                         ↓
              [Drain to StagingPool - no mutex]
                         ↓
              cxl_allocation_queue (MPMC)
                         ↓
              CXLAllocationWorker → GetCXLBuffer → CXL Ring
                         ↓
              [Decoupled from socket I/O]
```

**Code Path:** `network_manager.cc:1831-2146`
- `PublishReceiveThread`: epoll + edge-triggered events (line 1870)
- `DrainHeader/DrainPayload`: non-blocking recv with MSG_DONTWAIT (line 1681, 1722)
- `StagingPool`: Lock-free MPMC queue of 32 × 2MB buffers (staging_pool.cc)
- `CXLAllocationWorker`: Async GetCXLBuffer + pipelined copy (line 2029)

**Current Configuration:**
```cpp
network.use_nonblocking = false;  // DISABLED by default
network.staging_pool_buffer_size_mb = 2;   // 2MB per buffer
network.staging_pool_num_buffers = 32;     // 32 buffers = 64MB total
network.num_publish_receive_threads = 4;   // 4 epoll threads
network.num_cxl_allocation_workers = 2;    // 2 CXL workers
```

### 1.3 Ring Buffer Allocation (`topic.cc:1132-1194`)

Current implementation has **NO ring gating**:
```cpp
// Line 1147-1153: CRITICAL DECISION - No ring gating
absl::MutexLock lock(&mutex_);
log = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
batch_headers_log = reinterpret_cast<void*>(batch_headers_);
batch_headers_ += sizeof(BatchHeader);
if (batch_headers_ >= batch_headers_end) {
    batch_headers_ = batch_headers_start;  // Wrap without checking consumed_through
}
```

**Risk:** If sequencer falls behind, producer overwrites unconsumed slots silently.

---

## 2. Root Cause of 37.6% Stall

### Evidence Chain

| Component | Observation | Implication |
|-----------|-------------|-------------|
| NetworkManager logs | 40 "batch_complete=1" per broker | Should be ~1,360 for 10GB |
| Publisher logs | "Connection closed" at ~30s | TCP timeout due to blocked recv |
| Sequencer logs | 500 batches processed, then waits | Sees old/stale data |
| Ring slot checking | No logs | Not blocking on ring |

### Why 37.6%?

```
40 batches × 4 brokers = 160 new batches
+ ~2000 stale batches from previous runs
= 3,856,000 messages ≈ 3,947,312 = 37.6%
```

The NetworkManager threads block on mutex contention, causing TCP buffers to overflow, connections to time out, and only ~40 batches to be received per broker before the connection fails.

---

## 3. Proposed Architecture

### 3.1 Phase 1: Enable Non-Blocking Mode (Immediate)

**Goal:** Validate existing non-blocking architecture with minimal changes.

**Changes:**

1. **Enable non-blocking mode by default:**
   ```yaml
   # config/embarcadero.yaml
   network:
     use_nonblocking: true
   ```

2. **Add non-blocking ring gating in GetCXLBuffer:**
   ```cpp
   // Inside EmbarcaderoGetCXLBuffer, under mutex:
   size_t next_slot_offset = batch_headers_ - batch_headers_start;
   size_t consumed_through;

   // Read with cache invalidation (CXL)
   CXL::flush_cacheline(&tinode_->offsets[broker_id_].batch_headers_consumed_through);
   CXL::load_fence();
   consumed_through = tinode_->offsets[broker_id_].batch_headers_consumed_through;

   // Check if slot is free
   bool slot_free = (consumed_through == BATCHHEADERS_SIZE) ||  // Initial: all slots free
                    (consumed_through >= next_slot_offset + sizeof(BatchHeader));

   if (!slot_free) {
       // Fail-fast: Ring full, return nullptr
       log = nullptr;
       batch_header_location = nullptr;
       return nullptr;
   }

   // Proceed with allocation...
   ```

3. **Handle ring-full in CXLAllocationWorker:**
   - Already has retry logic with exponential backoff (lines 2064-2080)
   - Will retry up to 10 times before dropping batch
   - Add metric for "ring_full" events

**Files to modify:**
- `config/embarcadero.yaml`: Enable non-blocking
- `src/embarlet/topic.cc`: Add non-blocking ring gating
- `src/network_manager/network_manager.cc`: Add ring_full metric

### 3.2 Phase 2: Configuration Optimization

**Tune for 10GB workload:**

| Parameter | Current | Recommended | Rationale |
|-----------|---------|-------------|-----------|
| `staging_pool_buffer_size_mb` | 2 | 2 | 2MB matches batch size |
| `staging_pool_num_buffers` | 32 | 128 | 256MB total for 10GB pipeline |
| `num_publish_receive_threads` | 4 | 8 | 1 thread per 2 publishers |
| `num_cxl_allocation_workers` | 2 | 4 | Match receive threads |
| `batch_headers_size` | 64KB | 10MB | 163,840 slots, plenty for 10GB |

**Environment variables:**
```bash
export EMBARCADERO_USE_NONBLOCKING=1
export EMBARCADERO_STAGING_POOL_NUM_BUFFERS=128
export EMBARCADERO_NUM_PUBLISH_RECEIVE_THREADS=8
export EMBARCADERO_NUM_CXL_ALLOCATION_WORKERS=4
```

### 3.3 Phase 3: Production Hardening

1. **Backpressure propagation:**
   - When staging pool is exhausted (GetUtilization() > 90%), pause accepting new connections
   - Already partially implemented (line 1961: `epoll_ctl DEL`)
   - Add connection-level backpressure to publisher

2. **Graceful degradation:**
   - On ring-full: Log warning, increment metric, wait for sequencer
   - On staging exhausted: Pause socket, resume when pool recovers
   - On CXL allocation failure after retries: Drop batch, log error

3. **Metrics and monitoring:**
   - `staging_pool_utilization`: Current % of buffers in use
   - `cxl_ring_utilization`: Current % of ring slots in use
   - `batches_dropped_ring_full`: Count of batches dropped due to ring full
   - `tcp_connection_timeouts`: Count of connection failures

---

## 4. Data Flow Comparison

### 4.1 Blocking Mode (Current Default)

```
Time →
Publisher1: [SEND]------------------>[WAIT ACK]
Publisher2: [SEND]------------------>[WAIT ACK]
...
Publisher16: [SEND]----------------->[WAIT ACK]

NetworkManager:
Thread1: [recv HDR][mutex WAIT][GetCXL][mutex WAIT][recv DATA][complete]
Thread2:          [recv HDR]   [mutex WAIT...........][GetCXL][recv DATA]
...
Thread16:                                             [recv HDR][mutex WAIT........

Sequencer: ........................[batch1][batch2]...[batch40]...[TIMEOUT]

Problem: Threads block on mutex, can't drain TCP → connection timeout
```

### 4.2 Non-Blocking Mode (Proposed)

```
Time →
Publisher1: [SEND]------------------>[WAIT ACK]
Publisher2: [SEND]------------------>[WAIT ACK]
...
Publisher16: [SEND]----------------->[WAIT ACK]

PublishReceiveThread (epoll):
[drain1][drain2][drain3]...[drain16][drain1]...(continuous, non-blocking)

StagingPool: [buf1 alloc][buf2 alloc]...[buf128 alloc]...[buf1 free][buf1 alloc]...

CXLAllocationWorker:
Worker1: [GetCXL][copy][complete][GetCXL][copy][complete]...
Worker2: [GetCXL][copy][complete][GetCXL][copy][complete]...
Worker3: [GetCXL][copy][complete][GetCXL][copy][complete]...
Worker4: [GetCXL][copy][complete][GetCXL][copy][complete]...

Sequencer: [batch1][batch2][batch3]...[batch1360]...[DONE]

Benefit: Socket drain decoupled from CXL allocation → no TCP timeout
```

---

## 5. Non-Blocking Ring Gating Design

### 5.1 Slot Availability Check

The sequencer updates `batch_headers_consumed_through` after processing each batch:
```cpp
// topic.cc:1553-1558 (BrokerScannerWorker5)
size_t slot_offset = header_to_process - ring_start;
tinode_->offsets[broker_id].batch_headers_consumed_through = slot_offset + sizeof(BatchHeader);
CXL::flush_cacheline(&tinode_->offsets[broker_id].batch_headers_consumed_through);
CXL::store_fence();
```

The producer checks this before allocating:
```cpp
// Proposed addition to EmbarcaderoGetCXLBuffer
size_t slot_offset = batch_headers_ - batch_headers_start;
size_t consumed_through = tinode_->offsets[broker_id_].batch_headers_consumed_through;

// Slot is free if:
// 1. Ring is empty (consumed_through == BATCHHEADERS_SIZE), OR
// 2. Consumed has passed this slot (consumed_through >= slot_offset + sizeof(BatchHeader))
bool slot_free = (consumed_through == BATCHHEADERS_SIZE) ||
                 (consumed_through >= slot_offset + sizeof(BatchHeader));
```

### 5.2 Wrap-Around Handling

When the ring wraps, consumed_through may be behind the producer:
```
Ring: [0.......................BATCHHEADERS_SIZE]
       ^---- producer wraps here
              ^---- consumed_through here

After wrap:
producer at offset 64 (slot 1)
consumed_through at offset 8000 (slot 125)

Check: consumed_through >= 64 + 64 = 128?  NO (8000 >= 128 is YES)
```

The check works because consumed_through always represents "first byte past last consumed slot":
- Initial: `BATCHHEADERS_SIZE` (all slots free)
- After consuming slot at offset N: `N + sizeof(BatchHeader)`

### 5.3 Thread Safety

All access is under `mutex_`:
```cpp
absl::MutexLock lock(&mutex_);

// 1. Read consumed_through (with CXL invalidation)
CXL::flush_cacheline(&tinode_->offsets[broker_id_].batch_headers_consumed_through);
consumed_through = tinode_->offsets[broker_id_].batch_headers_consumed_through;

// 2. Check availability
if (!slot_free) {
    // Return nullptr without modifying batch_headers_
    return nullptr;
}

// 3. Allocate (only if available)
batch_headers_log = batch_headers_;
batch_headers_ += sizeof(BatchHeader);
if (batch_headers_ >= batch_headers_end) {
    batch_headers_ = batch_headers_start;
}
```

---

## 6. Expected Results

### 6.1 Throughput Targets

| Metric | Current | Phase 1 | Phase 2 | Phase 3 |
|--------|---------|---------|---------|---------|
| Throughput (1GB) | 456 MB/s | 1-2 GB/s | 5-7 GB/s | 9-10 GB/s |
| Throughput (10GB) | 46 MB/s (stall) | 500 MB/s | 3-5 GB/s | 9-10 GB/s |
| ACK Completion | 37.6% | 100% | 100% | 100% |
| TCP Retransmissions | 1.1M | <10K | <1K | <100 |

### 6.2 Latency Targets

| Percentile | Current | Target |
|------------|---------|--------|
| p50 | 1-50ms | <100µs |
| p99 | 100ms+ | <1ms |
| p99.9 | Timeout | <10ms |

### 6.3 Resource Utilization

| Resource | Current | Target |
|----------|---------|--------|
| CPU (NetworkManager) | 100% (blocked) | 40-60% |
| Memory (Staging) | 64MB | 256MB |
| CXL Ring Usage | 2% | 20-40% |
| Network Bandwidth | 46 MB/s | 10 GB/s |

---

## 7. Implementation Plan

### Week 1: Phase 1 - Enable Non-Blocking Mode
- [ ] Enable `use_nonblocking=true` by default
- [ ] Add non-blocking ring gating to EmbarcaderoGetCXLBuffer
- [ ] Add ring_full metric to CXLAllocationWorker
- [ ] Run 1GB test, verify 100% ACK completion
- [ ] Run 10GB test, verify improvement over 37.6%

### Week 2: Phase 2 - Configuration Optimization
- [ ] Increase staging pool to 128 buffers (256MB)
- [ ] Increase receive threads to 8
- [ ] Increase CXL workers to 4
- [ ] Profile and identify remaining bottlenecks
- [ ] Run 10GB test, target 3-5 GB/s

### Week 3: Phase 3 - Production Hardening
- [ ] Implement proper backpressure propagation
- [ ] Add comprehensive metrics
- [ ] Add graceful error handling
- [ ] Stress testing (100GB, multiple runs)
- [ ] Run 10GB test, target 9-10 GB/s

### Week 4: Validation and Documentation
- [ ] Full test suite validation
- [ ] Performance regression tests
- [ ] Update documentation
- [ ] Prepare for production deployment

---

## 8. Risk Assessment

### High Risk
- **Ring gating check overhead:** One CXL read + invalidate per allocation under mutex. Mitigation: Benchmark to verify <1µs overhead.
- **Staging pool exhaustion:** 256MB may not be enough for burst traffic. Mitigation: Monitor utilization, increase if needed.

### Medium Risk
- **CXLAllocationWorker bottleneck:** 4 workers may not keep up with 8 receive threads. Mitigation: Profile and increase if needed.
- **Sequencer falling behind:** If sequencer is slow, ring will fill. Mitigation: Profile sequencer, optimize if needed.

### Low Risk
- **Backward compatibility:** Non-blocking mode changes may affect existing clients. Mitigation: Test with all client configurations.

---

## 9. Code References

| Component | File | Lines | Purpose |
|-----------|------|-------|---------|
| Blocking handler | network_manager.cc | 575-861 | Current default path |
| Non-blocking PublishReceiveThread | network_manager.cc | 1831-2023 | Epoll + staging |
| CXLAllocationWorker | network_manager.cc | 2029-2146 | Async CXL allocation |
| StagingPool | staging_pool.h/cc | - | Buffer management |
| EmbarcaderoGetCXLBuffer | topic.cc | 1132-1194 | Ring allocation |
| BrokerScannerWorker5 | topic.cc | 1414-1586 | Sequencer polling |
| Configuration | configuration.h | 88-93 | Non-blocking config |

---

## 10. Conclusion

The key insight is that **a non-blocking architecture already exists** but is disabled. The immediate fix is to:

1. Enable non-blocking mode
2. Add safe ring gating (fail-fast, not blocking)
3. Tune configuration for 10GB workload

This approach is **lower risk and faster to implement** than designing a new architecture from scratch. The existing non-blocking code follows the same design principles outlined in previous discussions:

- Decouple socket draining from CXL allocation
- Use staging pool to absorb bursts
- Use lock-free queues for thread communication
- Apply backpressure when resources are exhausted

The main addition needed is **non-blocking ring gating** to prevent silent overwrites while avoiding the deadlock caused by blocking gating.
