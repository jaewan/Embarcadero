# Embarcadero Order 4/5 Design Specification

**Per-Client Ordering with Explicit Trade-offs**

*Revised with Order 4/5 differentiation, publisher-side retry, and critical bug fixes*

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Problem Definition and Constraints](#2-problem-definition-and-constraints)
3. [Ordering Semantics: Precise Definition](#3-ordering-semantics-precise-definition)
4. [Publisher-Side Batch Retention and Retry](#4-publisher-side-batch-retention-and-retry)
5. [Architecture Design Decisions](#5-architecture-design-decisions)
6. [Data Structure Design](#6-data-structure-design)
7. [Algorithm Specification](#7-algorithm-specification)
8. [Concurrency and Synchronization](#8-concurrency-and-synchronization)
9. [Failure Handling](#9-failure-handling)
10. [Security Considerations](#10-security-considerations)
11. [Performance Analysis (Honest Assessment)](#11-performance-analysis-honest-assessment)
12. [Formal Correctness Arguments](#12-formal-correctness-arguments)
13. [Migration Plan](#13-migration-plan)
14. [Testing and Validation Strategy](#14-testing-and-validation-strategy)
15. [Operational Runbook](#15-operational-runbook)

---

## 1. Executive Summary

### 1.1 What This Document Addresses

This document specifies the Order 4 and Order 5 (per-client ordering) implementations for Embarcadero. It incorporates feedback from expert panels and introduces:

- **Order 4 vs Order 5 split:** Gap-tolerant vs gap-strict per-client ordering
- **Publisher-side retry:** Batch retention until ACK enables automatic recovery
- **Critical bug fixes:** Hold buffer overflow handling, expiry ordering, committed_seq coordination
- **Honest performance expectations:** Realistic numbers based on Amdahl's law and measurement

### 1.2 Ordering Level Spectrum

| Level | Name | Per-Client Order | Gap Handling | Client Retry | Use Case |
|-------|------|------------------|--------------|--------------|----------|
| **0** | No Order | ❌ | N/A | N/A | Fire-and-forget |
| **2** | Total Order | ❌ | N/A | N/A | High throughput streaming |
| **4** | Per-Client (Gap Tolerant) | ✅ | Declare lost, continue | Optional | Event streaming, telemetry |
| **5** | Per-Client (Gap Strict) | ✅ | Hold, request retry, invalidate | Required | SMR, transactions |

### 1.3 Key Design Decisions (Summary)

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Parallelization strategy | Shard by client_id | Only correct approach for per-client ordering |
| Hold buffer structure | Heap-based expiry, per-client linked lists | O(log n) expiry, O(1) per-client access |
| Duplicate detection | Two-level (Bloom + exact) with periodic rotation | 98% fast path, bounded memory |
| Global sequence coordination | Min-of-shard-watermarks | Gap-free GOI visibility |
| Gap handling | Order 4: declare lost; Order 5: retry then invalidate | Explicit trade-off for applications |
| Publisher protocol | Retain until ACK, retry on request | Enables recovery without infinite waiting |

### 1.4 Honest Performance Expectations

| Metric | Previous Claim | Realistic Expectation | Justification |
|--------|---------------|----------------------|---------------|
| Throughput improvement | 70× | 2.5-4× | Amdahl's law (20% serial), cache contention |
| Prefetch benefit | 40% | 10-20% | Hash table indirection limits benefit |
| P99 latency | 2ms | 4-8ms | Queue wait, hold buffer, retry window |
| CXL vs RDMA benefit | "Transformative" | ~2% e2e latency | CPU processing dominates |

---

## 2. Problem Definition and Constraints

### 2.1 The Fundamental Challenge

Embarcadero allows clients to send batches to **any broker** for load balancing. This creates the ordering challenge:

```
CAUSAL ORDERING PROBLEM:

Timeline at Client:
  t=0: Client C calls publish(B1) → sent to Broker A
  t=1: Client C calls publish(B2) → sent to Broker B
  
  Client's causal expectation: B1 happened-before B2

Timeline at Sequencer:
  t=5: B2 arrives (Broker B is faster)
  t=10: B1 arrives (Broker A is slower)
  
  Sequencer's observation: B2 arrived-before B1

QUESTION: How do we reconcile client causality with arrival order?
```

### 2.2 Design Space Analysis

**Option A: Token-Before-Write (Corfu approach)**
```
1. Client requests token from sequencer
2. Sequencer returns global_seq
3. Client writes to storage with that global_seq

Properties:
  + Per-client order is FREE (client serializes through sequencer)
  + No reordering possible
  - Sequencer is throughput bottleneck (every write goes through it)
  - 2 RTTs per write (token + write)
```

**Option B: Write-Before-Order (Scalog approach)**
```
1. Client writes to any shard
2. Shards periodically report "cuts" to ordering layer
3. Ordering layer uses consensus to establish global order

Properties:
  + High throughput (writes parallelized across shards)
  - Per-client order NOT guaranteed across shards
  - High latency (cut batching, consensus)
```

**Option C: Write-Before-Order with Per-Client Tracking (Embarcadero)**
```
1. Client writes to any broker
2. Sequencer collects from all brokers
3. Sequencer tracks per-client state to reorder within timeout
4. Order 4: Timeout → declare lost, continue
   Order 5: Timeout → request retry → invalidate if no retry

Properties:
  + High throughput (like Scalog)
  + Per-client order (like Corfu) within timeout
  + Order 5 enables recovery via publisher retry
  - Memory overhead for per-client state
  - Order 5 adds complexity for strict guarantees
```

### 2.3 Why Option C?

**Scientific Justification:**

The choice depends on the cost of coordination:

| System | Coordination Cost | Per-Client Order Cost |
|--------|------------------|----------------------|
| Network (TCP/RDMA) | ~100μs RTT | O(clients × RTT) = expensive |
| CXL shared memory | ~300ns access | O(clients × 300ns) = cheap |

For network-based systems, per-client ordering requires cross-shard coordination, which is expensive. Scalog's designers correctly chose to not provide it.

For CXL-based systems, the sequencer can read all broker state in ~300ns per access. Tracking per-client state becomes feasible:

```
Cost analysis for 100K clients, 1M batches/sec:

Network coordination:
  100K clients × 100μs/coordination = 10 seconds of serialized work
  Impossible at 1M batches/sec

CXL local tracking:
  100K clients × ~200 bytes state = 20 MB (shared across 8 shards = 2.5 MB/shard, fits in L3)
  Hash lookup: ~50ns average
  Feasible at 1M batches/sec
```

**Conclusion:** CXL's low latency makes per-client tracking economically viable. This is Embarcadero's genuine contribution.

### 2.4 Constraints and Non-Goals

**Hard Constraints:**
1. **Bounded memory:** Hold buffer cannot grow unboundedly
2. **Progress guarantee:** Cannot wait forever for missing batches
3. **Throughput target:** Must not regress below single-threaded baseline

**Non-Goals (explicit scope limitations):**
1. **Exactly-once delivery:** Embarcadero provides at-least-once; applications handle idempotency
2. **Byzantine fault tolerance:** Assumes honest-but-faulty brokers
3. **Infinite retry:** Order 5 has bounded retry window; client invalidation is explicit

---

## 3. Ordering Semantics: Precise Definition

### 3.1 Order 4: Per-Client with Gap Tolerance

**Definition (Order 4 - Per-Client Ordering with Bounded Staleness):**

```
For any client C and any two batches B1, B2 from C:

IF:
  - B1.client_seq < B2.client_seq
  - Both B1 and B2 are in the GOI (Global Order Index)
  
THEN:
  B1.global_seq < B2.global_seq

ADDITIONALLY:
  If B1 is NOT in the GOI but B2 is, then B1 was "declared lost" due to:
  - Hold buffer timeout (B1 didn't arrive within MAX_WAIT_EPOCHS epochs)
  - Hold buffer overflow (system overloaded)
  - Explicit gap skip (administrator action)
```

**Behavior:**
```
Client C sends: B1(seq=1), B2(seq=2), B3(seq=3)
B1 lost in network

Epoch E: B2, B3 arrive → HOLD (waiting for B1)
Epoch E+3: Timeout expires (MAX_WAIT_EPOCHS=3)
  → Gap declared for seq=1
  → B2, B3 released → OUTPUT
  → next_expected = 4

GOI: [B2, B3] (B1 missing, gap logged)

If B1 arrives later: REJECTED (seq=1 < next_expected=4)
```

**Suitable for:**
- Event streaming (Kafka-like)
- Log aggregation  
- Telemetry/metrics
- Workloads where gaps are acceptable

**Client protocol:**
- Publisher MAY retain batches until ACK (optional)
- Publisher MAY retry on timeout (best-effort)
- No guarantee retry will succeed (gap may already be declared)

### 3.2 Order 5: Per-Client with Gap Strict (Retry-or-Invalidate)

**Definition (Order 5 - Per-Client Ordering with Session Atomicity):**

```
For any client C:

GUARANTEE:
  All batches from C in the GOI are in client_seq order with NO GAPS.
  
  Either:
  - ALL batches from a client session are ordered correctly, OR
  - The client session is INVALIDATED and no further batches accepted

MECHANISM:
  If batch B is missing:
  1. Subsequent batches are HELD (not declared lost)
  2. Retry request sent to client via broker
  3. If retry arrives → continue normally
  4. If retry doesn't arrive within EXTENDED_TIMEOUT → client INVALIDATED
  5. All held batches from invalidated client are DROPPED
```

**Behavior:**
```
Client C sends: B1(seq=1), B2(seq=2), B3(seq=3)
B1 lost in network

Epoch E: B2, B3 arrive → HOLD (waiting for B1)
Epoch E+3: Standard timeout (Order 4 would declare lost here)

Order 5 INSTEAD:
  → Send "retry_request(client=C, missing_seq=1)" to client via broker
  → Continue holding B2, B3
  → Start extended timeout (EXTENDED_TIMEOUT_EPOCHS=30, ~15ms)

CASE A: Client retries successfully
  Epoch E+5: Client resends B1 (same client_seq=1, new batch_id)
  → B1 OUTPUT
  → B2, B3 drained → OUTPUT
  → GOI: [B1, B2, B3] ✓ No gap

CASE B: Client fails to retry
  Epoch E+33: Extended timeout expires
  → Client C INVALIDATED
  → B2, B3 DROPPED (not in GOI)
  → Client state cleared
  → Any future batches from C rejected until re-registration

CASE C: Client sends "cannot_retry" (batch not in buffer)
  → Immediate invalidation
  → B2, B3 DROPPED
```

**Suitable for:**
- State machine replication
- Distributed transactions
- Exactly-once processing
- Applications where gaps are unacceptable

**Client protocol:**
- Publisher MUST retain batches until ACK (required)
- Publisher MUST handle retry_request messages
- Publisher MUST handle client_invalidated messages
- On invalidation: clear buffer, re-register with new client_id or reset sequence

### 3.3 What "Declared Lost" vs "Invalidated" Means

| Aspect | Order 4: Declared Lost | Order 5: Invalidated |
|--------|------------------------|---------------------|
| Scope | Single batch | Entire client session |
| Recovery | Client can retry (may fail) | Client MUST re-register |
| Subsequent batches | Released, continue | DROPPED |
| Gap in GOI | Yes (logged) | No (session terminated) |
| Notification | Gap log, metrics | Explicit invalidation message |

### 3.4 Client-Visible Behavior

**Order 4 - What clients can rely on:**

1. If both `publish(B1)` and `publish(B2)` are ACKed, then B1.global_seq < B2.global_seq in the GOI.
2. If `publish(B1)` times out, check gap log or retry with new batch_id.
3. Late retries may be rejected if gap already declared.

**Order 4 - What clients CANNOT rely on:**

1. ❌ If I send B1 before B2, both will be in the GOI. (B1 may be declared lost)
2. ❌ My retry will succeed. (Gap may already be declared)

**Order 5 - What clients can rely on:**

1. If both `publish(B1)` and `publish(B2)` are ACKed, then B1.global_seq < B2.global_seq with NO gap.
2. If a batch times out, client will receive retry_request.
3. If retry succeeds, session continues normally.
4. If retry fails, client receives explicit invalidation.
5. Either ALL my batches are ordered, or I'm told to restart.

**Order 5 - What clients CANNOT rely on:**

1. ❌ The system will wait forever. (Extended timeout is bounded)
2. ❌ Session survives network partition > EXTENDED_TIMEOUT.

### 3.5 Comparison with Alternatives

| Property | Corfu | Scalog | Embarcadero Order 4 | Embarcadero Order 5 |
|----------|-------|--------|---------------------|---------------------|
| Per-client ordering | ✓ Always | ✗ Never | ✓ Within timeout | ✓ Always (or invalidate) |
| Gaps possible | ✗ | N/A | ✓ | ✗ |
| Throughput | Low | High | High | High |
| Latency | 2 RTT | High (cuts) | 1 RTT + epoch | 1 RTT + epoch |
| Client complexity | Low | Low | Low | Medium (handle retry) |
| Recovery mechanism | N/A | N/A | Best-effort retry | Guaranteed retry or invalidate |

**Embarcadero's position:** Order 4 for event streaming (gaps ok), Order 5 for SMR (session atomicity required).

---

## 4. Publisher-Side Batch Retention and Retry

### 4.1 Design Principles

The publisher-side protocol enables Order 5's retry mechanism and improves Order 4's reliability:

1. **Retain batches until ACK:** Don't GC from buffer until explicit acknowledgment
2. **Handle retry requests:** Resend batch with same client_seq, new batch_id
3. **Handle invalidation:** Clear buffer, re-register for new session

### 4.2 Publisher Implementation

```cpp
class Publisher {
private:
    // Pending batches: client_seq → (batch, send_timestamp)
    std::map<uint64_t, std::pair<Batch, Timestamp>> pending_;
    uint64_t next_seq_ = 1;
    uint64_t acked_through_ = 0;  // Highest contiguously ACKed seq
    
    // Configuration
    static constexpr size_t MAX_PENDING = 100000;  // Backpressure threshold
    static constexpr Duration CLIENT_TIMEOUT = 100ms;  // Client-side timeout
    
    // Order level
    OrderLevel order_level_;  // ORDER_4 or ORDER_5
    
public:
    // Returns false if backpressure (too many pending)
    [[nodiscard]] bool publish(Payload&& payload) {
        if (pending_.size() >= MAX_PENDING) {
            return false;  // Backpressure: caller should wait
        }
        
        Batch batch;
        batch.client_seq = next_seq_++;
        batch.batch_id = generate_unique_id();
        batch.payload = std::move(payload);
        batch.order_level = order_level_;
        
        pending_[batch.client_seq] = {batch, now()};
        send_to_broker(batch);
        return true;
    }
    
    // Called when broker sends cumulative ACK
    void on_ack(uint64_t acked_seq) {
        // GC all batches up to acked_seq
        while (acked_through_ < acked_seq) {
            pending_.erase(++acked_through_);
        }
    }
    
    // Called when sequencer requests retry (Order 5, or Order 4 best-effort)
    void on_retry_request(uint64_t missing_seq, uint64_t deadline_epoch) {
        auto it = pending_.find(missing_seq);
        if (it == pending_.end()) {
            // Batch not in buffer—already GC'd or bug
            LOG(ERROR) << "Retry requested for seq=" << missing_seq 
                       << " but not in pending buffer";
            
            if (order_level_ == ORDER_5) {
                // Order 5: Must notify sequencer we can't retry
                send_cannot_retry(missing_seq);
                // Expect invalidation soon
            }
            return;
        }
        
        // Resend with SAME client_seq, NEW batch_id (for dedup)
        Batch retry = it->second.first;
        retry.batch_id = generate_unique_id();  // New ID prevents false dedup
        retry.flags |= FLAG_RETRY;
        
        LOG(INFO) << "Retrying batch seq=" << missing_seq;
        send_to_broker(retry);
    }
    
    // Called when client is invalidated (Order 5 only)
    void on_client_invalidated(uint64_t last_valid_seq, 
                                std::vector<uint64_t> dropped_seqs) {
        LOG(ERROR) << "Client invalidated by sequencer. "
                   << "Last valid seq: " << last_valid_seq
                   << ", Dropped: " << dropped_seqs.size() << " batches";
        
        // Clear pending buffer
        pending_.clear();
        
        // Application decides: re-register or fail
        notify_application_invalidated(last_valid_seq, dropped_seqs);
    }
    
    // Periodic: Check for client-side timeouts (belt-and-suspenders)
    void check_timeouts() {
        auto now_ts = now();
        for (auto& [seq, entry] : pending_) {
            if (seq <= acked_through_) continue;  // Already ACKed
            
            if (now_ts - entry.second > CLIENT_TIMEOUT) {
                // Client-side timeout—preemptively retry
                LOG(WARNING) << "Client-side timeout for seq=" << seq << ", retrying";
                on_retry_request(seq, 0);  // Self-triggered retry
                entry.second = now_ts;  // Reset timer
            }
        }
    }
};
```

### 4.3 Protocol Messages

```cpp
// Broker → Client: Cumulative ACK
struct AckMessage {
    uint64_t acked_through_seq;  // All seqs ≤ this are durable
};

// Sequencer → Client (via Broker): Retry Request
struct RetryRequestMessage {
    uint64_t client_id;
    uint64_t missing_seq;        // The seq that didn't arrive
    uint64_t deadline_epoch;     // Retry must arrive before this epoch
    OrderLevel order_level;      // ORDER_4 or ORDER_5
};

// Client → Sequencer (via Broker): Cannot Retry (Order 5 only)
struct CannotRetryMessage {
    uint64_t client_id;
    uint64_t missing_seq;
    enum Reason { NOT_IN_BUFFER, CLIENT_RESTARTED, GAVE_UP };
    Reason reason;
};

// Sequencer → Client (via Broker): Client Invalidated (Order 5 only)
struct ClientInvalidatedMessage {
    uint64_t client_id;
    uint64_t last_valid_seq;     // Highest seq that made it to GOI
    std::vector<uint64_t> dropped_seqs;  // Seqs that were dropped
};
```

### 4.4 Buffer Sizing

```
MAX_PENDING determines publisher memory usage and backpressure:

Memory: MAX_PENDING × avg_batch_size
  At 100K batches × 1KB = 100MB per client
  At 100K batches × 64KB = 6.4GB per client

Recommendations:
  - Small batches (≤4KB): MAX_PENDING = 100,000
  - Medium batches (4-64KB): MAX_PENDING = 10,000
  - Large batches (>64KB): MAX_PENDING = 1,000

Backpressure: When pending_.size() >= MAX_PENDING, publish() returns false.
Application must wait or drop.
```

---

## 5. Architecture Design Decisions

### 5.1 Decision 1: Shard by Client ID

**The Decision:** Route all batches from client C to shard `hash(C) % num_shards`.

**Why This Is The Only Correct Approach:**

```
PROOF BY CONTRADICTION:

Assume we use a different sharding strategy (e.g., round-robin, by broker):

1. Client C sends B1(seq=1) → routed to Shard 0
2. Client C sends B2(seq=2) → routed to Shard 1

3. Shard 0 and Shard 1 process independently (no shared state)
4. Shard 1 might assign global_seq=100 to B2
5. Shard 0 might assign global_seq=101 to B1

Result: B1.global_seq > B2.global_seq, violating Order 4/5.

THEREFORE: All batches from the same client MUST go to the same shard.

The only sharding function that guarantees this: hash(client_id) % num_shards
```

**Conclusion:** Client-ID sharding is a mathematical necessity, not a design choice.

### 5.2 Decision 2: Epoch-Based Processing

**The Decision:** Process batches in fixed time windows (epochs) rather than continuously.

**Quantitative Analysis:**

| Approach | Atomics/sec (at 10M batches/s, 8 shards) | Contention |
|----------|------------------------------------------|------------|
| Continuous | 10,000,000 | Severe (~100ns each = 1 second serialized) |
| Epoch (τ=500μs) | 8 × 2000 = 16,000 | Minimal (<2ms serialized) |

**Epoch duration trade-off:**

| τ (μs) | Batches/epoch | Latency added | Throughput impact |
|--------|---------------|---------------|-------------------|
| 100 | ~5,000 | +50μs avg | Higher sync overhead |
| 500 | ~25,000 | +250μs avg | Balanced |
| 1000 | ~50,000 | +500μs avg | Lower sync overhead |

**Optimal τ:** 500μs provides good balance.

### 5.3 Decision 3: Min-Watermark for committed_seq

**The Decision:** `committed_seq = min(shard_watermarks[0..S-1])`

**Implementation with Dedicated Coordinator:**

```cpp
class CommittedSeqCoordinator {
    std::array<std::atomic<uint64_t>, NUM_SHARDS> shard_watermarks_;
    std::array<std::atomic<uint64_t>, NUM_SHARDS> shard_epochs_;
    std::atomic<uint64_t> committed_seq_{0};
    std::atomic<bool> shutdown_{false};
    
public:
    // Called by each shard after GOI write
    void report_completion(size_t shard_id, uint64_t watermark, uint64_t epoch) {
        shard_watermarks_[shard_id].store(watermark, std::memory_order_release);
        shard_epochs_[shard_id].store(epoch, std::memory_order_release);
    }
    
    // Coordinator thread loop
    void run() {
        while (!shutdown_.load(std::memory_order_acquire)) {
            uint64_t min_watermark = UINT64_MAX;
            
            for (size_t s = 0; s < NUM_SHARDS; ++s) {
                uint64_t wm = shard_watermarks_[s].load(std::memory_order_acquire);
                min_watermark = std::min(min_watermark, wm);
            }
            
            if (min_watermark != UINT64_MAX && min_watermark > 0) {
                // Advance committed_seq (monotonic)
                uint64_t current = committed_seq_.load(std::memory_order_relaxed);
                while (min_watermark > current) {
                    if (committed_seq_.compare_exchange_weak(current, min_watermark,
                            std::memory_order_release, std::memory_order_relaxed)) {
                        break;
                    }
                }
            }
            
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
};
```

---

## 6. Data Structure Design

### 6.1 Client State: Compact and Cache-Friendly

```cpp
// Compact client state: 40 bytes
struct alignas(64) ClientStateCompact {
    uint64_t next_expected;      // Next client_seq we expect
    uint64_t highest_sequenced;  // Highest client_seq output
    uint64_t window_base;        // Duplicate detection window base
    uint32_t last_active_epoch;  // For GC
    uint32_t flags;              // ORDER_4, ORDER_5, RETRY_REQUESTED, INVALIDATED
    
    // Order 5 specific
    uint64_t retry_requested_epoch;  // When retry was first requested
    uint64_t missing_seq;            // Which seq we're waiting for
};
```

### 6.2 Hold Buffer: Heap-Based Expiry with Per-Client Lists

**Critical Fix from Expert Review:** The expiry heap should be keyed by `(expiry_epoch, client_id, client_seq)` so expired batches come out in correct order:

```cpp
class HoldBuffer {
public:
    static constexpr size_t MAX_ENTRIES = 100000;
    
    struct Entry {
        BatchInfo batch;
        uint64_t expiry_epoch;
        uint32_t next_same_client;
        uint32_t prev_same_client;
        bool valid;
    };
    
private:
    std::array<Entry, MAX_ENTRIES> entries_;
    uint32_t free_head_ = 0;
    size_t size_ = 0;
    
    // Expiry heap: ordered by (expiry_epoch, client_id, client_seq)
    // This ensures expired batches come out in per-client order
    using HeapKey = std::tuple<uint64_t, uint64_t, uint64_t, uint32_t>;
    //                         expiry    client_id  client_seq entry_idx
    std::priority_queue<HeapKey, std::vector<HeapKey>, std::greater<>> expiry_heap_;
    
    absl::flat_hash_map<uint64_t, uint32_t> client_heads_;
    
public:
    enum class AddResult { ADDED, DUPLICATE, OVERFLOW };
    
    AddResult add(BatchInfo&& batch, uint64_t current_epoch, 
                  int timeout_epochs = 3) {
        if (size_ >= MAX_ENTRIES) {
            return AddResult::OVERFLOW;
        }
        
        uint64_t cid = batch.client_id;
        uint64_t cseq = batch.client_seq;
        
        if (client_contains(cid, cseq)) {
            return AddResult::DUPLICATE;
        }
        
        uint32_t idx = allocate_slot();
        uint64_t expiry = current_epoch + timeout_epochs;
        
        entries_[idx].batch = std::move(batch);
        entries_[idx].expiry_epoch = expiry;
        entries_[idx].valid = true;
        
        link_to_client_sorted(idx, cid, cseq);
        
        // Heap key includes client_id and client_seq for ordering
        expiry_heap_.emplace(expiry, cid, cseq, idx);
        
        ++size_;
        return AddResult::ADDED;
    }
    
    // Expire batches - returns in (client_id, client_seq) order
    // due to heap key ordering
    std::vector<BatchInfo> expire(uint64_t current_epoch) {
        std::vector<BatchInfo> expired;
        
        while (!expiry_heap_.empty()) {
            auto& [exp_epoch, cid, cseq, idx] = expiry_heap_.top();
            if (exp_epoch > current_epoch) break;
            
            expiry_heap_.pop();
            
            Entry& e = entries_[idx];
            if (!e.valid) continue;
            
            expired.push_back(std::move(e.batch));
            unlink_and_free(idx);
            --size_;
        }
        
        // Already sorted by (client_id, client_seq) due to heap order
        return expired;
    }
    
    // ... drain_client, etc. same as before
};
```

### 6.3 Deduplication: Two-Level with Rotation

(Same as original document - no changes needed)

---

## 7. Algorithm Specification

### 7.1 Main Processing Loop (Per Shard)

```cpp
void SequencerShard::process_epoch(EpochInput& input, EpochOutput& output) {
    std::vector<BatchInfo> batches;
    batches.reserve(32768);
    while (auto b = queue_.try_pop()) {
        batches.push_back(std::move(*b));
    }
    
    // Partition by ordering level
    std::vector<BatchInfo> level0, level4, level5;
    for (auto& b : batches) {
        switch (b.order_level) {
            case ORDER_0: level0.push_back(std::move(b)); break;
            case ORDER_4: level4.push_back(std::move(b)); break;
            case ORDER_5: level5.push_back(std::move(b)); break;
        }
    }
    
    std::vector<BatchInfo> ready;
    ready.reserve(batches.size());
    
    // Level 0: Simple dedup
    for (auto& b : level0) {
        if (!dedup_.check_and_insert(b.batch_id)) {
            ready.push_back(std::move(b));
        }
    }
    
    // Level 4: Per-client with gap tolerance
    process_order4(level4, ready, input.epoch_number);
    
    // Level 5: Per-client with retry/invalidate
    process_order5(level5, ready, input.epoch_number);
    
    // Drain and expire hold buffers
    drain_hold_buffer_all_clients(ready);
    expire_hold_buffer_order4(ready, input.epoch_number);
    expire_hold_buffer_order5(ready, input.epoch_number);
    
    // Check Order 5 extended timeouts
    check_order5_extended_timeouts(ready, input.epoch_number);
    
    // Assign global sequences
    if (!ready.empty()) {
        uint64_t base = global_seq_.fetch_add(ready.size(), std::memory_order_relaxed);
        for (size_t i = 0; i < ready.size(); ++i) {
            ready[i].global_seq = base + i;
        }
        output.watermark = base + ready.size() - 1;
    }
    
    output.batches = std::move(ready);
}
```

### 7.2 Order 4 Processing

```cpp
void SequencerShard::process_order4(
    std::vector<BatchInfo>& batches,
    std::vector<BatchInfo>& ready,
    uint64_t current_epoch
) {
    if (batches.empty()) return;
    
    std::sort(batches.begin(), batches.end(),
              [](const auto& a, const auto& b) { return a.client_id < b.client_id; });
    
    auto it = batches.begin();
    while (it != batches.end()) {
        uint64_t cid = it->client_id;
        auto group_end = std::find_if(it, batches.end(),
            [cid](const auto& b) { return b.client_id != cid; });
        
        std::sort(it, group_end,
                  [](const auto& a, const auto& b) { return a.client_seq < b.client_seq; });
        
        process_order4_client(cid, it, group_end, ready, current_epoch);
        it = group_end;
    }
}

void SequencerShard::process_order4_client(
    uint64_t cid,
    BatchIterator begin, BatchIterator end,
    std::vector<BatchInfo>& ready,
    uint64_t current_epoch
) {
    auto& state = clients_[cid];
    state.last_active_epoch = current_epoch;
    
    if (state.next_expected == 0) {
        state.next_expected = 1;
        state.flags = FLAG_ORDER_4;
    }
    
    for (auto it = begin; it != end; ++it) {
        auto& batch = *it;
        uint64_t seq = batch.client_seq;
        
        if (is_duplicate(cid, seq, state)) {
            stats_.duplicates++;
            continue;
        }
        
        if (seq == state.next_expected) {
            // In-order
            if (!dedup_.check_and_insert(batch.batch_id)) {
                ready.push_back(std::move(batch));
                mark_sequenced(cid, seq, state);
                state.next_expected++;
                
                auto drained = hold_buffer_order4_.drain_client(cid, state.next_expected);
                for (auto& d : drained) {
                    if (!dedup_.check_and_insert(d.batch_id)) {
                        ready.push_back(std::move(d));
                        mark_sequenced(cid, d.client_seq, state);
                    }
                }
            }
        } else if (seq > state.next_expected) {
            // Out-of-order: add to hold buffer
            auto result = hold_buffer_order4_.add(std::move(batch), current_epoch, 
                                                   MAX_WAIT_EPOCHS_ORDER4);
            
            if (result == HoldBuffer::AddResult::OVERFLOW) {
                // CRITICAL FIX: Do NOT advance next_expected
                // Just drop the batch, let held batches drain/expire normally
                stats_.batches_dropped++;
                LOG(WARNING) << "Order 4 hold buffer overflow, batch dropped: "
                             << "client=" << cid << " seq=" << seq;
            }
        }
        // seq < next_expected: duplicate/late, already handled by is_duplicate
    }
}
```

### 7.3 Order 5 Processing

```cpp
void SequencerShard::process_order5(
    std::vector<BatchInfo>& batches,
    std::vector<BatchInfo>& ready,
    uint64_t current_epoch
) {
    if (batches.empty()) return;
    
    std::sort(batches.begin(), batches.end(),
              [](const auto& a, const auto& b) { return a.client_id < b.client_id; });
    
    auto it = batches.begin();
    while (it != batches.end()) {
        uint64_t cid = it->client_id;
        auto group_end = std::find_if(it, batches.end(),
            [cid](const auto& b) { return b.client_id != cid; });
        
        std::sort(it, group_end,
                  [](const auto& a, const auto& b) { return a.client_seq < b.client_seq; });
        
        process_order5_client(cid, it, group_end, ready, current_epoch);
        it = group_end;
    }
}

void SequencerShard::process_order5_client(
    uint64_t cid,
    BatchIterator begin, BatchIterator end,
    std::vector<BatchInfo>& ready,
    uint64_t current_epoch
) {
    auto& state = clients_[cid];
    
    // Check if client is invalidated
    if (state.flags & FLAG_INVALIDATED) {
        stats_.rejected_invalidated += std::distance(begin, end);
        return;  // Reject all batches from invalidated client
    }
    
    state.last_active_epoch = current_epoch;
    
    if (state.next_expected == 0) {
        state.next_expected = 1;
        state.flags = FLAG_ORDER_5;
    }
    
    for (auto it = begin; it != end; ++it) {
        auto& batch = *it;
        uint64_t seq = batch.client_seq;
        
        // Check if this is a retry for the missing seq
        if ((state.flags & FLAG_RETRY_REQUESTED) && 
            seq == state.missing_seq &&
            (batch.flags & FLAG_RETRY)) {
            // Retry received! Clear retry state
            state.flags &= ~FLAG_RETRY_REQUESTED;
            state.missing_seq = 0;
            LOG(INFO) << "Order 5 retry received: client=" << cid << " seq=" << seq;
        }
        
        if (is_duplicate(cid, seq, state)) {
            stats_.duplicates++;
            continue;
        }
        
        if (seq == state.next_expected) {
            // In-order
            if (!dedup_.check_and_insert(batch.batch_id)) {
                ready.push_back(std::move(batch));
                mark_sequenced(cid, seq, state);
                state.next_expected++;
                
                auto drained = hold_buffer_order5_.drain_client(cid, state.next_expected);
                for (auto& d : drained) {
                    if (!dedup_.check_and_insert(d.batch_id)) {
                        ready.push_back(std::move(d));
                        mark_sequenced(cid, d.client_seq, state);
                    }
                }
            }
        } else if (seq > state.next_expected) {
            // Out-of-order: add to hold buffer with extended timeout
            auto result = hold_buffer_order5_.add(std::move(batch), current_epoch,
                                                   EXTENDED_TIMEOUT_EPOCHS);
            
            if (result == HoldBuffer::AddResult::OVERFLOW) {
                // Order 5: Drop batch but don't advance state
                stats_.batches_dropped++;
                LOG(WARNING) << "Order 5 hold buffer overflow: client=" << cid;
            }
            
            // Request retry if not already requested
            if (!(state.flags & FLAG_RETRY_REQUESTED)) {
                state.flags |= FLAG_RETRY_REQUESTED;
                state.retry_requested_epoch = current_epoch;
                state.missing_seq = state.next_expected;
                
                send_retry_request(cid, state.next_expected,
                                   current_epoch + EXTENDED_TIMEOUT_EPOCHS);
            }
        }
    }
}
```

### 7.4 Order 5 Extended Timeout Check

```cpp
void SequencerShard::check_order5_extended_timeouts(
    std::vector<BatchInfo>& ready,
    uint64_t current_epoch
) {
    for (auto& [cid, state] : clients_) {
        if (!(state.flags & FLAG_ORDER_5)) continue;
        if (!(state.flags & FLAG_RETRY_REQUESTED)) continue;
        if (state.flags & FLAG_INVALIDATED) continue;
        
        if (current_epoch > state.retry_requested_epoch + EXTENDED_TIMEOUT_EPOCHS) {
            // Extended timeout expired → INVALIDATE
            LOG(WARNING) << "Order 5 client invalidated: client=" << cid
                         << " missing_seq=" << state.missing_seq;
            
            state.flags |= FLAG_INVALIDATED;
            state.flags &= ~FLAG_RETRY_REQUESTED;
            stats_.clients_invalidated++;
            
            // Drop all held batches from this client
            auto dropped = hold_buffer_order5_.drop_client(cid);
            stats_.batches_dropped += dropped.size();
            
            // Collect dropped seqs for notification
            std::vector<uint64_t> dropped_seqs;
            for (const auto& d : dropped) {
                dropped_seqs.push_back(d.client_seq);
            }
            
            // Notify client
            send_client_invalidated(cid, state.next_expected - 1, dropped_seqs);
        }
    }
}
```

### 7.5 Order 4 Expiry (With Gap Declaration)

```cpp
void SequencerShard::expire_hold_buffer_order4(
    std::vector<BatchInfo>& ready,
    uint64_t current_epoch
) {
    auto expired = hold_buffer_order4_.expire(current_epoch);
    
    // Expired batches are already sorted by (client_id, client_seq)
    // due to heap key ordering
    
    for (auto& batch : expired) {
        uint64_t cid = batch.client_id;
        uint64_t seq = batch.client_seq;
        
        auto& state = clients_[cid];
        
        // Declare gap if needed
        if (seq >= state.next_expected) {
            uint64_t gap_size = seq - state.next_expected;
            if (gap_size > 0) {
                emit_gap_event(cid, state.next_expected, seq - 1);
                stats_.gaps_declared += gap_size;
            }
            state.next_expected = seq + 1;
        }
        
        mark_sequenced(cid, seq, state);
        
        if (!dedup_.check_and_insert(batch.batch_id)) {
            ready.push_back(std::move(batch));
        }
    }
}
```

### 7.6 Cannot Retry Handling (Order 5)

```cpp
void SequencerShard::on_cannot_retry(uint64_t cid, uint64_t seq, CannotRetryReason reason) {
    auto it = clients_.find(cid);
    if (it == clients_.end()) return;
    
    auto& state = it->second;
    
    if (!(state.flags & FLAG_ORDER_5)) return;
    if (state.flags & FLAG_INVALIDATED) return;
    
    if ((state.flags & FLAG_RETRY_REQUESTED) && state.missing_seq == seq) {
        LOG(WARNING) << "Order 5 cannot_retry received: client=" << cid
                     << " seq=" << seq << " reason=" << reason;
        
        // Immediate invalidation
        state.flags |= FLAG_INVALIDATED;
        state.flags &= ~FLAG_RETRY_REQUESTED;
        stats_.clients_invalidated++;
        
        auto dropped = hold_buffer_order5_.drop_client(cid);
        stats_.batches_dropped += dropped.size();
        
        std::vector<uint64_t> dropped_seqs;
        for (const auto& d : dropped) {
            dropped_seqs.push_back(d.client_seq);
        }
        
        send_client_invalidated(cid, state.next_expected - 1, dropped_seqs);
    }
}
```

---

## 8. Concurrency and Synchronization

### 8.1 Thread Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              THREAD MODEL                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│  ┌──────────────┐                                                           │
│  │ Timer Thread │ (1)                                                       │
│  └──────┬───────┘                                                           │
│         │ Epoch tick                                                        │
│         ▼                                                                   │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐    │
│  │ Collector 0  │  │ Collector 1  │  │ Collector 2  │  │ Collector 3  │    │
│  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘  └──────┬───────┘    │
│         │                 │                 │                 │            │
│         └────────────┬────┴────────┬────────┴────────┬────────┘            │
│                      │             │                 │                      │
│                      ▼             ▼                 ▼                      │
│               ┌─────────────────────────────────────────┐                   │
│               │            MPSC Queues (8)              │                   │
│               │  Route by: hash(client_id) % 8          │                   │
│               └───────┬─────┬─────┬─────┬─────┬────────┘                   │
│                       │     │     │     │     │                            │
│                       ▼     ▼     ▼     ▼     ▼                            │
│  ┌──────────────┐ ┌──────────────┐     ┌──────────────┐ ┌──────────────┐   │
│  │ Shard 0      │ │ Shard 1      │ ... │ Shard 6      │ │ Shard 7      │   │
│  │ Order4 HB    │ │ Order4 HB    │     │ Order4 HB    │ │ Order4 HB    │   │
│  │ Order5 HB    │ │ Order5 HB    │     │ Order5 HB    │ │ Order5 HB    │   │
│  └──────┬───────┘ └──────┬───────┘     └──────┬───────┘ └──────┬───────┘   │
│         │                │                    │                │            │
│         │ fetch_add()    │                    │                │            │
│         ▼                ▼                    ▼                ▼            │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                    global_seq_ (single atomic)                       │   │
│  └─────────────────────────────────────────────────────────────────────┘   │
│         │                │                    │                │            │
│         ▼                ▼                    ▼                ▼            │
│  ┌─────────────────────────────────────────────────────────────────────┐   │
│  │                 shard_watermark_[0..7] (per shard)                   │   │
│  └───────────────────────────────┬─────────────────────────────────────┘   │
│                                  │                                          │
│                                  ▼                                          │
│  ┌──────────────┐     ┌─────────────────────────────────┐                  │
│  │ Coordinator  │────►│ committed_seq_ = min(watermarks) │                  │
│  │   Thread     │     └─────────────────────────────────┘                  │
│  └──────┬───────┘                                                           │
│         │                                                                   │
│         ▼                                                                   │
│  ┌──────────────┐                                                           │
│  │ GOI Writer   │ (1)                                                       │
│  └──────────────┘                                                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 8.2 MPSC Queue with Livelock Prevention

```cpp
template<typename T>
class MPSCQueue {
public:
    std::optional<T> try_pop() {
        uint64_t head = head_.load(std::memory_order_relaxed);
        uint64_t tail = tail_.load(std::memory_order_acquire);
        
        if (head >= tail) {
            return std::nullopt;
        }
        
        // Wait for slot to be ready with TIMEOUT
        constexpr int MAX_READY_SPINS = 10000;  // ~10μs
        int spins = 0;
        
        while (!ready_[head & mask_].load(std::memory_order_acquire)) {
            if (++spins > MAX_READY_SPINS) {
                // Producer likely crashed after reserving slot
                LOG(ERROR) << "MPSC: Producer timeout, skipping slot " << head;
                stats_.producer_timeouts++;
                
                ready_[head & mask_].store(false, std::memory_order_relaxed);
                head_.store(head + 1, std::memory_order_release);
                
                return std::nullopt;  // Skip this slot
            }
            CPU_PAUSE();
        }
        
        T item = std::move(buffer_[head & mask_]);
        ready_[head & mask_].store(false, std::memory_order_relaxed);
        head_.store(head + 1, std::memory_order_release);
        
        return item;
    }
    
    // ... try_push same as before
};
```

---

## 9. Failure Handling

(Same as original document with additions below)

### 9.1 Order 5 Specific Failure Handling

**Publisher crash during retry window:**
- Sequencer sends retry_request
- Publisher crashes before responding
- Extended timeout expires
- Sequencer invalidates client
- When publisher restarts, receives "invalidated" on first batch
- Publisher re-registers with fresh state

**Sequencer crash during retry window:**
- Held batches lost (in-memory)
- On recovery, client state rebuilt from GOI
- Pending retry_requests not persisted
- Clients will timeout and retry
- If no client action, effectively invalidated (client must re-register)

**Network partition during Order 5:**
- Retry_request may not reach client
- Client may retry speculatively (client-side timeout)
- If partition heals within EXTENDED_TIMEOUT, retry may succeed
- If not, client invalidated

---

## 10. Security Considerations

### 10.1 Per-Client Rate Limiting (Enhanced)

```cpp
struct ClientRateLimits {
    static constexpr size_t MAX_HELD_PER_CLIENT = 1000;
    static constexpr size_t MAX_GAP_SIZE = 10000;
    static constexpr size_t MAX_BATCHES_PER_EPOCH = 10000;
    
    // New: Limit gap declarations per client (DoS protection)
    static constexpr size_t MAX_GAPS_PER_MINUTE = 100;
    static constexpr size_t MAX_GAP_SEQUENCES_PER_MINUTE = 10000;
};

// Track per-client gap rates
struct ClientGapRate {
    uint64_t gaps_this_minute = 0;
    uint64_t sequences_this_minute = 0;
    uint64_t minute_start_epoch = 0;
};

void SequencerShard::maybe_blacklist_client(uint64_t cid, 
                                             uint64_t gap_size,
                                             uint64_t current_epoch) {
    auto& rate = gap_rates_[cid];
    
    // Reset counter each minute (~120K epochs at τ=500μs)
    if (current_epoch - rate.minute_start_epoch > 120000) {
        rate.gaps_this_minute = 0;
        rate.sequences_this_minute = 0;
        rate.minute_start_epoch = current_epoch;
    }
    
    rate.gaps_this_minute++;
    rate.sequences_this_minute += gap_size;
    
    if (rate.gaps_this_minute > ClientRateLimits::MAX_GAPS_PER_MINUTE ||
        rate.sequences_this_minute > ClientRateLimits::MAX_GAP_SEQUENCES_PER_MINUTE) {
        
        LOG(WARNING) << "Client " << cid << " blacklisted for excessive gaps";
        
        // Temporarily blacklist
        auto& state = clients_[cid];
        state.flags |= FLAG_BLACKLISTED;
        stats_.clients_blacklisted++;
        
        // Blacklist expires after 5 minutes
        state.blacklist_until_epoch = current_epoch + 600000;
    }
}
```

---

## 11. Performance Analysis (Honest Assessment)

### 11.1 Throughput Model (Revised)

**Single-threaded baseline:**
```
Measured: 1.8M batches/sec
Bottleneck: CPU processing in sequencer thread
```

**Scatter-gather (8 shards) - Revised estimate:**
```
Amdahl's Law:
  - Parallel portion: ~80% (per-client processing)
  - Serial portion: ~20% (epoch sync, GOI write, committed_seq, retry coordination)
  - Max speedup: 1 / (0.2 + 0.8/8) = 1 / 0.3 = 3.3×

Additional overheads:
  - Cache contention (8 shards share L3): -10%
  - Atomic contention on global_seq: -5%
  
Realistic estimate: 3.3 × 0.85 ≈ 2.8×
Conservative range: 2.5-4× improvement → 4.5-7M batches/sec
```

### 11.2 Order 4 vs Order 5 Overhead

| Aspect | Order 4 | Order 5 | Difference |
|--------|---------|---------|------------|
| Hold buffer timeout | 3 epochs (1.5ms) | 30 epochs (15ms) | 10× longer hold |
| Memory per held batch | Same | Same | None |
| Retry messages | Best-effort | Required | Network overhead |
| Client state | 40 bytes | 56 bytes | +40% |
| CPU per batch | ~125ns | ~150ns | +20% |
| Throughput impact | Baseline | -5-10% | Small |

### 11.3 Latency Model (Revised with Order 5)

**Order 4 P99 (no gaps):**
```
~1050μs ≈ 1ms (same as before)
```

**Order 5 P99 (retry succeeds):**
```
L_p99 = L_base + T_retry_round_trip

T_retry_round_trip:
  - Sequencer detects gap: ~500μs (one epoch)
  - Send retry_request: ~100μs
  - Client receives, resends: ~100μs
  - Retry arrives: ~100μs
  
L_p99 ≈ 1ms + 800μs ≈ 1.8ms
```

**Order 5 P99.9 (retry near timeout):**
```
L_p999 = L_base + EXTENDED_TIMEOUT × τ
       = 1ms + 30 × 500μs
       = 1ms + 15ms
       = 16ms
```

---

## 12. Formal Correctness Arguments

### 12.1 Invariants (Extended for Order 5)

**I6: Order 5 Session Atomicity**
```
∀ client C with Order 5:
  EITHER:
    All batches from C in GOI are gap-free (no missing client_seqs)
  OR:
    C is marked INVALIDATED and no batches from C are in GOI after invalidation

Proof:
1. Order 5 holds batches until gap is filled or timeout expires.
2. If gap is filled (retry succeeds), batches released in order.
3. If timeout expires, client is invalidated and all held batches dropped.
4. No partial release occurs—either all or none.
5. Therefore, GOI contains gap-free sequence or nothing (session atomicity).
```

**I7: Order 5 No Gap After Retry**
```
∀ client C with Order 5:
  If retry for seq=S succeeds:
    next_expected was S
    After retry: next_expected = S + 1 (or higher if drain)
    All held batches with seq > S are now ≥ next_expected
    They will be drained in order

Proof:
1. Retry clears FLAG_RETRY_REQUESTED.
2. Batch with seq=S is processed as in-order.
3. drain_client() releases all consecutive batches.
4. No gap is declared—retry filled the gap.
```

### 12.2 Cross-Epoch Correctness (Explicit)

**Theorem: Hold buffer correctly handles cross-epoch scenarios.**

```
Scenario:
  Epoch E: B2(seq=2) arrives, held (waiting for B1)
  Epoch E+1: B1(seq=1) arrives

Trace:
  Epoch E:
    B2 arrives, seq=2, next_expected=1
    B2 → hold_buffer (expiry = E + timeout)
    Output: []
    next_expected unchanged (still 1)
  
  Epoch E+1:
    B1 arrives, seq=1, next_expected=1
    B1 → OUTPUT (in-order)
    next_expected = 2
    drain_hold_buffer(cid, &next_expected):
      B2.seq = 2 == next_expected
      B2 → OUTPUT
      next_expected = 3
    Output: [B1, B2]
  
  Global seq assignment:
    B1.global_seq = base
    B2.global_seq = base + 1
  
  Result: Correct order preserved across epochs.
```

---

## 13. Migration Plan

(Same as original with addition below)

### 13.1 Phase 2.5: Shadow Mode (Added)

**Duration:** 2 weeks (after Phase 2, before Phase 3)

**Purpose:** Validate sharding correctness by running both modes in parallel.

```cpp
void Sequencer::process_with_shadow() {
    // Run single-threaded
    auto result_single = process_single_threaded(epoch_input);
    
    // Run sharded
    auto result_sharded = process_sharded(epoch_input);
    
    // Compare results
    if (!results_equivalent(result_single, result_sharded)) {
        LOG(ERROR) << "Shadow mode mismatch!";
        // Log details for debugging
        // Continue with single-threaded result (safe)
    }
    
    // Use single-threaded result (shadow mode doesn't affect production)
    return result_single;
}
```

---

## 14. Testing and Validation Strategy

### 14.1 Order 4 vs Order 5 Specific Tests

```cpp
// Order 4 specific
TEST(Order4, GapDeclarationOnTimeout) {
    // Missing batch times out
    // Verify gap declared, subsequent batches released
}

TEST(Order4, RetryAfterGapDeclared) {
    // Gap declared, then original batch arrives
    // Verify rejected (seq < next_expected)
}

TEST(Order4, HoldBufferOverflowNoAdvance) {
    // Hold buffer overflows
    // Verify next_expected NOT advanced
    // Verify held batches drain/expire normally
}

// Order 5 specific
TEST(Order5, RetrySuccess) {
    // Missing batch, retry_request sent
    // Retry arrives
    // Verify all batches in GOI, no gap
}

TEST(Order5, RetryTimeout) {
    // Missing batch, retry_request sent
    // No retry arrives
    // Verify client invalidated
    // Verify all held batches dropped
}

TEST(Order5, CannotRetryInvalidation) {
    // Missing batch, retry_request sent
    // Client sends cannot_retry
    // Verify immediate invalidation
}

TEST(Order5, PublisherRetryProtocol) {
    // End-to-end with real publisher
    // Drop batch at network level
    // Verify retry_request received
    // Verify retry sent
    // Verify batch in GOI
}

TEST(Order5, SessionAtomicity) {
    // 1000 batches, drop batch 500
    // Verify either all 1000 or client invalidated
    // No partial state (e.g., 499 + gap + 501-1000)
}
```

### 14.2 Benchmark Modifications

```cpp
struct BenchmarkConfig {
    // Existing
    uint32_t brokers;
    uint32_t producers;
    uint64_t duration_sec;
    
    // New: Order level distribution
    double order0_ratio = 0.0;
    double order4_ratio = 0.1;  // 10% Order 4
    double order5_ratio = 0.0;  // 0% Order 5 (default)
    // Remainder is Order 2 (total order)
    
    // New: Failure injection
    double drop_probability = 0.0;        // Simulate network drops
    double retry_success_rate = 1.0;      // Fraction of retries that succeed
    Duration retry_delay = 0ms;           // Delay before retry arrives
    
    // New: Realistic parameters
    size_t num_clients = 10000;           // was 100
    size_t batch_size = 65536;            // was 1024
};

// Correctness verification
bool verify_order4_order5_correctness(const std::vector<GOIEntry>& goi) {
    std::map<uint64_t, std::vector<GOIEntry>> by_client;
    for (const auto& e : goi) {
        by_client[e.client_id].push_back(e);
    }
    
    for (auto& [cid, entries] : by_client) {
        // Sort by global_seq
        std::sort(entries.begin(), entries.end(),
                  [](const auto& a, const auto& b) { 
                      return a.global_seq < b.global_seq; 
                  });
        
        // Verify client_seq order matches global_seq order
        for (size_t i = 1; i < entries.size(); ++i) {
            if (entries[i].client_seq <= entries[i-1].client_seq) {
                LOG(ERROR) << "Order violation: client=" << cid;
                return false;
            }
        }
        
        // For Order 5: verify no gaps in client_seq
        if (entries[0].order_level == ORDER_5) {
            for (size_t i = 1; i < entries.size(); ++i) {
                if (entries[i].client_seq != entries[i-1].client_seq + 1) {
                    LOG(ERROR) << "Order 5 gap violation: client=" << cid;
                    return false;
                }
            }
        }
    }
    
    return true;
}
```

---

## 15. Operational Runbook

### 15.1 Order 5 Specific Metrics

```yaml
panels:
  - title: "Order 5 Metrics"
    queries:
      - "sequencer_order5_retry_requests_total"
      - "sequencer_order5_retries_received_total"
      - "sequencer_order5_retries_succeeded_total"
      - "sequencer_order5_clients_invalidated_total"
      - "sequencer_order5_batches_dropped_on_invalidation_total"
      - "sequencer_order5_extended_hold_buffer_size"
```

### 15.2 Order 5 Specific Alerts

```yaml
rules:
  - alert: Order5HighInvalidationRate
    expr: rate(sequencer_order5_clients_invalidated_total[5m]) > 10
    for: 5m
    annotations:
      summary: "High Order 5 client invalidation rate"
      description: "More than 10 clients/sec invalidated. Check network reliability."
      
  - alert: Order5RetryFailureRate
    expr: |
      rate(sequencer_order5_retries_succeeded_total[5m]) / 
      rate(sequencer_order5_retry_requests_total[5m]) < 0.9
    for: 5m
    annotations:
      summary: "Order 5 retry success rate below 90%"
      description: "Clients failing to retry. Check client health and buffer sizing."
```

### 15.3 Troubleshooting: Order 5 Invalidations

```
Symptoms:
  - sequencer_order5_clients_invalidated_total increasing
  - Clients reporting "session invalidated"

Diagnosis:
  1. Check retry success rate:
     $ promql 'sequencer_order5_retries_succeeded_total / sequencer_order5_retry_requests_total'
     
  2. If low retry success rate:
     - Check client logs for retry_request handling
     - Check client buffer size (MAX_PENDING)
     - Check network latency client → broker
     
  3. Check extended timeout vs network latency:
     $ echo "EXTENDED_TIMEOUT_EPOCHS * τ = $(( 30 * 500 ))μs = 15ms"
     $ ping client-host
     
  4. If network latency > extended timeout:
     - Increase EXTENDED_TIMEOUT_EPOCHS
     - Or switch to Order 4 for this workload

Resolution:
  - If client buffer too small: Increase MAX_PENDING
  - If network unreliable: Increase EXTENDED_TIMEOUT_EPOCHS or use Order 4
  - If client buggy: Fix retry_request handler
```

---

## Summary

This document specifies Embarcadero's Order 4 and Order 5 per-client ordering implementations:

**Order 4 (Gap Tolerant):**
- Per-client ordering with bounded staleness
- Gaps declared on timeout, subsequent batches continue
- Suitable for event streaming, telemetry
- Publisher retry optional (best-effort)

**Order 5 (Gap Strict):**
- Per-client ordering with session atomicity
- Missing batch → request retry → invalidate if no retry
- Suitable for SMR, transactions
- Publisher retry required (must handle retry_request)

**Key improvements over original design:**
1. Explicit Order 4 vs Order 5 split with clear semantics
2. Publisher-side batch retention enabling reliable retry
3. Critical bug fix: Hold buffer overflow doesn't advance next_expected
4. Realistic performance expectations: 2.5-4× improvement
5. Enhanced security: Per-client gap rate limiting

The design achieves Embarcadero's goal of providing per-client ordering at high throughput, with explicit trade-offs that applications can choose based on their requirements.