# Architecture: Scalable, Correct, High-Throughput ORDER=5

**Date:** 2026-01-29
**Status:** Design Discussion
**Target:** 10+ GB/s throughput, <100μs p99 latency, strict per-client batch ordering

---

## 1. Current State Assessment

### What Works ✅
- **Non-blocking NetworkManager:** Staging pool + CXLAllocationWorker achieves ~6 GB/s receive rate
- **Ring buffer management:** `consumed_through` semantics work correctly (no data races)
- **CXL cache invalidation:** Correct flush/fence pattern for non-coherent CXL
- **Batch-level ordering:** `global_seq_.fetch_add()` provides monotonic total_order assignment

### What's Broken ❌
- **ORDER=5 semantics:** Current simplified scanner does NOT preserve per-client batch order
  - Client A sends batches A1→A2→A3 to different brokers
  - Scanner processes in ring order, NOT client batch_seq order
  - Result: A2 can get `total_order` < A1 if broker 1's scanner runs before broker 0's
- **ACK path:** Empty topic on non-head brokers → wrong TInode → client timeout
  - **Fixed in this commit:** Added validation at 3 levels (GetOffsetToAck, HandlePublishRequest, AckThread)
- **Hot-path logging:** LOG(INFO) every 100-200 batches adds ~10% overhead at high throughput
  - **Fixed in this commit:** Moved to VLOG(2-3) for high-frequency logs

---

## 2. ORDER=5 Semantic Requirements

### Definition
**ORDER=5 = Per-Client Batch Order Preserved:**
- Client sends batches with monotonic `batch_seq` (0, 1, 2, 3, ...)
- System assigns `total_order` such that for any client C:
  - If C's batch B1 has `batch_seq` < B2's `batch_seq`, then B1's `total_order` < B2's `total_order`
- Across different clients: no ordering guarantee (they can interleave arbitrarily)

### Current Implementation Gap
The simplified scanner assigns `total_order` by:
```cpp
size_t start = global_seq_.fetch_add(num_msg, std::memory_order_relaxed);
```
in the order batches are encountered in each broker's ring. This does NOT enforce per-client batch_seq order across brokers.

**Example failure scenario:**
1. Client A (client_id=123) sends:
   - Batch A1 (batch_seq=0) → Broker 0
   - Batch A2 (batch_seq=1) → Broker 1
2. Scanner threads run concurrently:
   - Broker 1's scanner processes A2 first → gets `total_order=1000`
   - Broker 0's scanner processes A1 later → gets `total_order=2000`
3. **ORDER=5 violated:** A2's `total_order` < A1's `total_order` despite A1's `batch_seq` < A2's `batch_seq`

---

## 3. Design Options for Correct ORDER=5

### Option A: Per-Client Sequencer (Original Design)
**Approach:** Each client has a state machine tracking `expected_seq` and `deferred_batches`.

**Pros:**
- Provably correct per-client ordering
- Lock-free fast path (CAS on `expected_seq`)

**Cons:**
- Complex consumed_through management (must track deferred batches)
- Memory overhead (state per active client)
- Potential head-of-line blocking if one batch is delayed

**Implementation:**
```cpp
struct ClientOrderState5 {
    absl::Mutex mu;
    std::atomic<size_t> expected_seq{0};
    std::map<size_t, BatchHeader*> deferred_batches;
};
absl::flat_hash_map<uint32_t, std::unique_ptr<ClientOrderState5>> client_order_states_5_;

// In scanner loop:
auto state = GetOrCreateClientState(client_id);
if (batch_seq == state->expected_seq.load(std::memory_order_acquire)) {
    // Fast path: in-order
    AssignOrder5AndAdvance(batch, state);
} else if (batch_seq > state->expected_seq.load()) {
    // Defer out-of-order batch
    state->mu.Lock();
    state->deferred_batches[batch_seq] = batch;
    state->mu.Unlock();
    // DO NOT advance consumed_through yet!
} else {
    // Duplicate or old batch
    LOG(WARNING) << "Stale batch";
}
```

**Critical Fix:** Update `consumed_through` only to the **minimum** of:
- Current slot offset (if processed)
- Earliest deferred batch offset (if any deferred batches exist)

---

### Option B: Centralized Sequencer with Client Queues
**Approach:** Single sequencer thread pulls batches from per-client priority queues ordered by `batch_seq`.

**Pros:**
- Simpler consumed_through (no deferred batch tracking per broker ring)
- Natural backpressure (queue full → stop pulling from that broker)

**Cons:**
- Single-threaded sequencer becomes bottleneck at high throughput
- Requires copying or queuing pointers (latency overhead)

**Implementation:**
```cpp
struct ClientQueue {
    std::priority_queue<BatchHeader*, std::vector<BatchHeader*>, CompareBySeq> pending;
    size_t next_expected_seq{0};
};

absl::flat_hash_map<uint32_t, ClientQueue> client_queues_;

// Scanner thread: Enqueue batch
client_queues_[client_id].pending.push(batch);

// Sequencer thread: Dequeue in-order
for (auto& [client_id, queue] : client_queues_) {
    while (!queue.pending.empty() &&
           queue.pending.top()->batch_seq == queue.next_expected_seq) {
        auto* batch = queue.pending.top();
        queue.pending.pop();
        AssignOrder5(batch);
        queue.next_expected_seq++;
    }
}
```

---

### Option C: Hybrid - Per-Client Lock-Free with Central Coordinator
**Approach:** Per-client atomic `expected_seq` with central coordinator handling deferred batches.

**Pros:**
- Lock-free fast path for in-order batches
- Centralized deferred batch management (simpler debugging)

**Cons:**
- Still complex consumed_through logic

---

### Option D: Relax ORDER=5 to "Best-Effort Batch Order"
**Approach:** Document that ORDER=5 provides batch-level total_order without strict per-client ordering.

**Pros:**
- Current simplified scanner is correct for this definition
- Maximum throughput (no blocking, no deferred batches)

**Cons:**
- Breaks user expectations if they need strict per-client order
- Not suitable for use cases like distributed transactions or session-based ordering

---

## 4. Recommended Architecture: Option A with Optimizations

### 4.1 Core Design
- **Per-client state machine** with lock-free fast path
- **Deferred batch tracking** with smart `consumed_through` advancement
- **Periodic batch timeout** to avoid infinite deferrals (e.g., skip after 10s)

### 4.2 Optimizations

#### **Opt-1: Client State Eviction**
- Evict idle client states after timeout (e.g., 60s no activity)
- Use LRU or CLOCK eviction policy
- Reduces memory footprint for workloads with many ephemeral clients

#### **Opt-2: Batch Prefetching**
- When processing batch B, prefetch B+1 metadata (batch_seq, client_id)
- Reduces cache miss penalty on next iteration
- **CAUTION:** Only prefetch headers written by same producer (NetworkManager), not cross-broker

#### **Opt-3: NUMA-Aware Allocation**
- Allocate `ClientOrderState5` on same NUMA node as scanner thread
- Pin scanner threads to CPUs close to CXL memory

#### **Opt-4: Adaptive Polling**
- Use `written_addr` as coarse-grained signal (as attempted earlier)
- Only invalidate cache when `written_addr` changes
- Reduces cache invalidation overhead from ~1024 per loop to ~1 per new batch

```cpp
uint64_t last_written_addr = 0;
while (!stop_threads_) {
    uint64_t curr_written_addr = __atomic_load_n(&tinode->offsets[broker_id].written_addr, __ATOMIC_ACQUIRE);

    if (curr_written_addr == last_written_addr) {
        // No new data - pause
        cpu_pause(); continue;
    }

    // New data detected - check current slot
    CXL::flush_cacheline(current_batch_header);
    CXL::load_fence();

    if (batch_complete == 1) {
        // Process batch
        last_written_addr = curr_written_addr;
    } else {
        // Batch incomplete - mark as seen but don't update last_written_addr
        // to keep checking on next iteration
    }
}
```

**Gotcha:** Must continue checking until current slot is consumed before updating `last_written_addr`, otherwise we miss batches when multiple are written atomically.

---

## 5. Performance Target Breakdown

### 5.1 Throughput: 10 GB/s
- **Assumptions:**
  - 4 brokers × 4 threads/broker = 16 producer threads
  - 1024-byte messages
  - 1928 messages/batch (~2 MB/batch)
  - Target: 10 GB/s = 10,485,760 messages/s = 5,440 batches/s

- **Scanner processing:**
  - 4 scanner threads (one per broker)
  - Each must process ~1,360 batches/s
  - At 1360 batches/s, period = 735 μs/batch
  - Current processing time: ~180 ms / 544 batches = 331 μs/batch **✅ Sufficient**

- **Bottlenecks:**
  - NetworkManager receive: ~6 GB/s measured → need to reach 10 GB/s
  - ACK path: Empty topic issue → **FIXED**
  - Hot-path logging: ~10% overhead → **FIXED**
  - Cache invalidation overhead: ~1024 per loop → **Opt-4 targets this**

### 5.2 Latency: <100μs p99
- **End-to-end latency components:**
  - Network RTT: ~10-50 μs (localhost)
  - NetworkManager staging: ~5-10 μs (non-blocking)
  - Scanner processing: ~331 μs/batch → **Too high for p99!**
  - ACK sending: ~5-10 μs

- **Optimization:** Reduce scanner processing time via:
  - Remove VLOG calls in hot path (even VLOG has cost when level > threshold)
  - Batch multiple batches in single `global_seq_.fetch_add()` call
  - Use `written_addr` guard to avoid polling empty slots

---

## 6. Correctness Verification Strategy

### 6.1 Invariants
1. **Per-client batch order:** For client C, if batch B1's `batch_seq` < B2's `batch_seq`, then B1's `total_order` < B2's `total_order`
2. **Consumed_through safety:** Producer never overwrites a slot until `consumed_through` >= slot_offset
3. **No duplicate total_order:** Each `total_order` value assigned exactly once
4. **No batch loss:** Every batch with `batch_complete=1` eventually gets assigned `total_order`

### 6.2 Testing
- **Unit test:** Single client, multiple brokers, verify batch order preserved
- **Stress test:** 100 clients × 1000 batches each, verify no order violations
- **Correctness oracle:** Subscriber reconstructs per-client message order, checks monotonicity
- **Failure injection:** Drop batches, delay batches, kill brokers mid-flight

### 6.3 Monitoring
- **Metrics:**
  - Per-client: `max_deferred_batches`, `total_order_gaps`
  - Global: `scanner_processing_time_p99`, `ack_latency_p99`
  - Errors: `stale_batch_count`, `out_of_order_warnings`

---

## 7. Implementation Roadmap

### Phase 1: Restore Per-Client Ordering (1-2 days)
1. Re-add `ClientOrderState5` struct and per-client state tracking
2. Implement lock-free fast path with CAS on `expected_seq`
3. Implement deferred batch handling with correct `consumed_through` advancement
4. Add unit tests for single-client, multi-broker scenarios

### Phase 2: Optimize Scanner (1 day)
1. Implement `written_addr` guard to reduce cache invalidation overhead
2. Remove VLOG calls from hot path (add compile-time flag for diagnostics)
3. Add NUMA-aware allocation for client states
4. Measure throughput: target 8-10 GB/s

### Phase 3: Latency Optimization (1 day)
1. Profile scanner loop to identify remaining hot spots
2. Batch multiple batches in single `global_seq_.fetch_add()` if possible
3. Add prefetching for next batch metadata
4. Measure p99 latency: target <100 μs

### Phase 4: Production Hardening (1-2 days)
1. Add timeout for deferred batches (skip after 10s)
2. Add client state eviction (LRU after 60s idle)
3. Add comprehensive monitoring and alerting
4. Stress test with 1000+ concurrent clients

---

## 8. Risks and Mitigation

| Risk | Impact | Mitigation |
|------|--------|------------|
| Per-client ordering adds latency | High | Use lock-free fast path; optimize deferred batch handling |
| Deferred batches fill ring | High | Add timeout to skip stuck batches; increase ring size |
| Client state memory leak | Medium | Add LRU eviction policy |
| Scanner CPU contention | Medium | Pin scanner threads to dedicated cores; use NUMA-aware allocation |
| ACK path still broken | High | **FIXED in this commit** - added validation at 3 levels |
| Hot-path logging overhead | Medium | **FIXED in this commit** - moved to VLOG |

---

## 9. Success Criteria

✅ **Functional:**
- ORDER=5 preserves per-client batch order in 100% of test cases
- No batch loss or duplicate total_order under normal operation
- Graceful degradation under broker failure

✅ **Performance:**
- Throughput: 10 GB/s sustained for 60s
- Latency: p99 < 100 μs end-to-end
- CPU: <80% utilization per core at peak load

✅ **Operational:**
- No memory leaks under 24h stress test
- Monitoring dashboards show all key metrics
- Runbook documents failure modes and recovery procedures

---

## 10. Open Questions

1. **Should ORDER=5 enforce cross-client fairness?**
   - Current design: Clients can starve if one client floods the system
   - Option: Add per-client rate limiting or fair queuing

2. **What happens if client reuses batch_seq?**
   - Current design: Treated as duplicate (logged, ignored)
   - Option: Allow batch_seq wrap-around after 2^64 batches

3. **Should we support dynamic ORDER level changes?**
   - E.g., start with ORDER=0 for bulk load, switch to ORDER=5 for transactions
   - Requires draining all in-flight batches before switching

---

## 11. References

- **Root Cause Analysis:** docs/INVESTIGATION_618888_ACK_STALL.md (previous ACK debugging)
- **Ring Buffer Fix:** ROOT_CAUSE_RING_GATING_BUG.md (consumed_through semantics)
- **BlogHeader Integration:** c90b5c9 (ORDER=5 batch-level ordering commit)
- **Non-Blocking Architecture:** 7f0044a (current baseline)

---

**Next Steps:**
1. Review this architecture with team
2. Get sign-off on Option A (Per-Client Sequencer)
3. Implement Phase 1 (restore per-client ordering)
4. Benchmark and iterate

**Author:** Claude Sonnet 4.5
**Reviewers:** [TBD]
