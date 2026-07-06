# Embarcadero Baseline Implementation Limitations

This document outlines the architectural and implementation shortcomings of the
CXL-Scalog and CXL-LazyLog baselines in the Embarcadero repository, including
correctness issues that were identified and fixed, and an analysis of why both
baselines converge to similar performance on CXL shared memory.

While the implementations successfully serve their primary academic
purpose—demonstrating that write-before-order baselines are fundamentally
bottlenecked by the coupling between global ordering and replica durability (as
detailed in `Paper/Text/Appendix.tex`)—they are evaluation harnesses and lack
the robustness required of production-grade distributed systems.

---

## 1. The `static` State Disaster (Multi-Topic Corruption)

In `src/cxl_manager/scalog_local_sequencer.cc`, the `ScalogSequencer` method relies on function-local `static` variables to track the state of the sequence and memory pointers:

```cpp
void ScalogLocalSequencer::ScalogSequencer(const char* topic, absl::btree_map<int, int64_t> &global_cut_delta) {
    static size_t seq = 0;
    static TInode *tinode = nullptr;
    static MessageHeader* msg_to_order = nullptr;
    static size_t batch_header_idx = 0;
    
    if(tinode == nullptr){
        tinode = tinode_;
        msg_to_order = ((MessageHeader*)((uint8_t*)cxl_addr_ + tinode->offsets[broker_id_].log_offset));
    }
    // ...
}
```

**The Impact:** Because these variables are `static`, they are shared globally across *all* instances of `ScalogLocalSequencer` within the process. If a user creates Topic A, the pointers are initialized to Topic A's memory space. If the user subsequently creates Topic B, `tinode` is no longer `nullptr`, causing Topic B's sequencer to write its sequence numbers directly into Topic A's memory space.
**Result:** The system can only ever support exactly one topic cluster-wide. Attempting to use multiple topics will result in immediate CXL memory corruption.

## 2. Ephemeral Network History Dictates Global Order (State Machine Violation)

The fundamental guarantee of a shared log is that all nodes agree on the exact same total order of messages. Original Scalog achieves this by durably storing the sequence of cuts.

Our implementation derives the total order by iterating over `global_cut_delta` and incrementing the sequence number. However, `global_cut_delta` is computed purely based on the difference between the *current* gRPC `GlobalCut` message and the *previously received* one.

* If Broker 0 receives two cuts `{b0: 5, b1: 5}` and `{b0: 10, b1: 10}`, it assigns `total_order` by interleaving chunks of 5.
* If Broker 1 experiences network jitter, restarts, or its gRPC stream buffers a message, it might receive just `{b0: 10, b1: 10}`. It will compute a single massive delta and assign `total_order` in chunks of 10.

**The Impact:** Broker 0 and Broker 1 will assign completely different `total_order` sequence numbers to the exact same messages. The consistency of the distributed system relies entirely on the ephemeral framing and timing of gRPC streams. If a node restarts, it cannot reconstruct the log correctly.

## 3. Silent PBR Ring Overwrites (No Flow Control)

When `publish_batch` writes an ordered batch to the CXL ring buffer, it executes the following logic:

```cpp
const size_t slot = batch_header_idx % kNumBatchSlots;
batch_header_[slot].ordered = 1;
// ... flush and fence ...
batch_header_idx++;
```

**The Impact:** The sequencer blindly increments the index and wraps around the ring buffer. It **never** checks `batch_headers_consumed_through` (or any equivalent subscriber progress marker) to ensure the slot is actually free.
**Result:** If the sequencer outpaces the subscriber, it will silently overwrite unread batches. Worse, because it blindly sets `ordered = 1`, a slow subscriber polling that slot will read the *new* batch thinking it's the *old* batch, resulting in silent data loss and stream corruption without any errors or warnings being thrown.

## 4. Redundant / Split Ordering Logic

The codebase contains two completely separate implementations of the exact same ordering logic:
1. `ScalogLocalSequencer::ScalogSequencer` in `src/cxl_manager/scalog_local_sequencer.cc` (which assigns order and writes to CXL).
2. `ScalogReplicationServiceImpl::ScalogSequencer` in `src/disk_manager/scalog_replication_manager.cc` (which assigns order and writes to disk).

**The Impact:** This split architecture leads to redundant work and fragmented logic. The author of the disk version actually left a comment warning about the static variables (`// Static variables are generally problematic with concurrency...`), but the CXL version completely ignored this concern. Maintaining two parallel sequencing paths increases the surface area for bugs and makes the system harder to reason about.

---

## 5. ACK and Ordering Correctness Fixes (Scalog + LazyLog)

During a systematic code audit, multiple correctness violations were identified
in both CXL-Scalog and CXL-LazyLog that could cause ACK semantics to
misrepresent system state. These were fixed and validated with 21 unit tests
(`test/scalog_ack_invariant_test.cc`).

### 5a. ACK1 Could Exceed Export Visibility

**Problem:** Both Scalog and LazyLog updated `tinode->offsets[broker_id_].ordered`
(the frontier that ACK level 1 reads) *before* the corresponding batch header
export slot was written and flushed. A client polling ACK1 could observe
`ordered = N` while the export path had not yet published the batch containing
message N. This means a subscriber reading ordered data would not yet see the
message that the publisher was told was ordered—violating the contract that
"ACK1 means the message is ordered and readable."

**Fix:** Deferred the `tinode->offsets[].ordered` and `ordered_offset` updates
to *after* the `publish_batch` lambda flushes the batch header to CXL memory.
Applied identically to:
- `ScalogLocalSequencer::ScalogSequencer` (Scalog)
- `LazyLogLocalSequencer::ApplyGlobalBinding` (LazyLog)

### 5b. ACK2 (Durable) Could Exceed the Ordered Frontier

**Problem:** `replication_done[broker_id]` advances as replicas persist data to
disk, independently of global ordering. Without a clamp, `ACK2` could report a
durable frontier ahead of the ordered frontier, acknowledging messages that have
no total order yet. This was originally fixed for Scalog but not extended to
LazyLog.

**Fix:** In `NetworkManager::GetOffsetToAck` and `GetOffsetToAckFast`, the
durable frontier is now clamped to `min(durable_frontier, ordered_frontier)` for
both `SCALOG` and `LAZYLOG` sequencer types (three code locations).

### 5c. LazyLog Progress Tracked Only Self-Replica (Unfair Advantage)

**Problem:** `LazyLogLocalSequencer::SendLocalProgress` reported
`replication_done[broker_id_]` from only the primary's own `offset_entry`. In a
multi-replica setup (RF > 1), this meant LazyLog's progress advanced as soon as
the *primary alone* persisted data, ignoring whether other replicas had caught
up. By contrast, Scalog's global cut takes `min` across all replicas. This gave
LazyLog a systematic and unfair advantage in benchmarks.

**Fix:** `SendLocalProgress` now computes
`min(replication_done[broker_id_])` across **all** brokers in the replication
set (using `GetReplicationSetBroker`), with proper CXL cache-line flushes and
fences. If any replica has not started, progress is reported as 0.

### 5d. LazyLog Global Sequencer: Artificial Binding Cap

**Problem:** `kMaxBindingsPerBrokerPerTick` was hardcoded to 4096, throttling
how many messages could be globally bound per tick. Scalog's global cut has no
equivalent cap. Under high throughput, this artificially limited LazyLog's
ordering rate, making comparison unfair in the opposite direction from 5c.

**Fix:** Set `kMaxBindingsPerBrokerPerTick = std::numeric_limits<int64_t>::max()`.

### 5e. Scalog Global Sequencer: RF=0 Readiness Bug

**Problem:** With `replication_factor = 0`, `ScalogGlobalSequencer::SendGlobalCut`
expected `0` replica streams per broker, causing the readiness check
`total_num_replicas != expected_replicas` to never pass once any streams
connected. Global cuts were never sent, so ordering never advanced.

**Fix:** `replicas_per_broker = max(1, num_replicas_per_broker_)`, treating
RF=0 as "one effective stream per broker" (the broker's own written frontier).

### 5f. Script Fixes for Local Benchmark Runs

**Problem:** `run_latency.sh` and `run_throughput_impl.sh` did not start
`lazylog_global_sequencer` locally, used hardcoded remote sequencer IPs, and
assumed NUMA node 2 existed (which fails on 2-node machines).

**Fix:**
- Both scripts now start `lazylog_global_sequencer` locally when needed.
- Sequencer IPs default to `127.0.0.1` for local runs.
- NUMA binding dynamically detects available nodes.

---

## 6. Why CXL-Scalog and CXL-LazyLog Converge to Similar Performance

A natural question after benchmarking is: *why do CXL-Scalog and CXL-LazyLog
produce nearly identical throughput and latency numbers?* Original Scalog and
LazyLog have substantially different architectures—one uses Paxos-backed global
cuts, the other uses a lightweight coordinator with binding rounds. On a
traditional network, their performance profiles diverge. On CXL, they converge.
This is **expected and correct**, and directly supports the paper's thesis.

### 6.1 Architectural Convergence on CXL

When both systems are adapted to CXL shared memory, their architectures collapse
to the same fundamental pattern:

| Step | CXL-Scalog | CXL-LazyLog |
|------|-----------|-------------|
| **1. Write** | Broker appends to CXL log region | Broker appends to CXL log region |
| **2. Report** | Local sequencer sends `local_cut` via gRPC to remote global sequencer | Local sequencer sends `local_progress` via gRPC to remote global sequencer |
| **3. Aggregate** | Global sequencer computes `min` across replicas per shard, emits `global_cut` | Global sequencer computes available deltas per broker, emits `global_binding` |
| **4. Apply** | Local sequencer applies cut: assigns `total_order`, publishes batch headers | Local sequencer applies binding: assigns `total_order`, publishes batch headers |
| **5. ACK** | Client reads `tinode->offsets[].ordered` | Client reads `tinode->offsets[].ordered` |

The data path (step 1) is identical—both write to CXL at memory speed. The
control path (steps 2–4) is identical in structure—both require a gRPC round
trip to a remote coordinator, which dominates ordering latency. The ACK path
(step 5) is identical—both read the same `ordered` field from the same
`TInode` structure.

### 6.2 The Bottleneck Is the Same

On a traditional network, the bottleneck distribution differs:
- **Original Scalog**: Network RTT for replication (~100μs) + disk (~500μs) + network RTT for cuts (~100μs). The disk and network costs overlap partially.
- **Original LazyLog**: Network RTT for writes (~100μs) + coordinator binding RTT (~100μs). Disk is decoupled from ordering in some configurations.

On CXL, the write path drops from ~100μs to ~500ns (200× faster). What remains:
- **CXL-Scalog**: gRPC RTT for cuts (~200–500μs) + disk (if RF > 0).
- **CXL-LazyLog**: gRPC RTT for bindings (~200–500μs) + disk (if RF > 0).

Both systems are dominated by **the same gRPC coordinator round-trip**. The
~500ns CXL write is negligible. The disk I/O (when replication is active) is
the same because both now correctly gate ordering on `min` across all replicas
(after Fix 5c). The coordinator polling/binding interval is configurable but
structurally equivalent.

### 6.3 Why This Supports the Paper's Thesis

The paper argues that Embarcadero outperforms both baselines because it:
1. **Removes the remote coordinator from the ordering path** (sequencer reads PBR directly from CXL memory each epoch).
2. **Decouples ordering from durability** (ordering is based on PBR arrival, not replication progress).

The fact that CXL-Scalog ≈ CXL-LazyLog in performance is exactly what the paper
predicts: both baselines share the same fundamental limitation (remote gRPC
coordinator coupling ordering to replication progress), and CXL eliminates all
other differentiators. Embarcadero eliminates this remaining bottleneck entirely.

### 6.4 This Is Not an Implementation Bug

One might worry that similar performance indicates a shared implementation
error (e.g., both accidentally using the same code path). This is not the case:
- Scalog uses `ScalogLocalSequencer` / `ScalogGlobalSequencer` with cut-based semantics.
- LazyLog uses `LazyLogLocalSequencer` / `LazyLogGlobalSequencer` with binding-based semantics.
- The two sequencer pairs have completely separate code paths, gRPC service definitions, and state machines.
- The convergence is architectural, not implementational.

---

## Summary

The CXL-Scalog and CXL-LazyLog implementations are purpose-built evaluation
harnesses. They correctly capture the steady-state throughput and latency
characteristics required to demonstrate the ordering-durability coupling
bottleneck highlighted in the paper. Their performance convergence on CXL is
expected: once shared memory eliminates network-based data transfer, both
systems reduce to "gRPC-coordinated write-before-order with disk-gated
progress," and Embarcadero's architecture is specifically designed to
eliminate this remaining bottleneck.

The implementations should not be viewed as complete, fault-tolerant distributed
systems. They lack flow control (§3), multi-topic isolation (§1), and
deterministic state-machine replication (§2). The correctness fixes in §5 ensure
that ACK semantics, ordered frontiers, and durability frontiers are internally
consistent, which is necessary for meaningful benchmark comparisons.