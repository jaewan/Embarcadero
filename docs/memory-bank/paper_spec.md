# Technical Specification: Embarcadero Reference Design

**Source:** NSDI '26 Paper
**Authority:** Reference design - Check `spec_deviation.md` FIRST for approved improvements
**Usage:** Follow this IF AND ONLY IF no deviation documented in `spec_deviation.md`

---

## ⚠️ IMPORTANT: Specification Hierarchy

```
1. spec_deviation.md (approved improvements) - CHECK THIS FIRST
   ↓
2. paper_spec.md (THIS FILE) - Reference design
   ↓
3. Engineering judgment - Document as deviation proposal
```

**If spec_deviation.md documents a different approach, that approach is the source of truth.**

This file preserves the paper's original design as a reference point for:
- Understanding the baseline architecture
- Comparing deviations against the reference
- Justifying why deviations are better

---

## 1. System Architecture & Topology
*   **Nodes:** Broker nodes + Disaggregated Memory (CXL) + Client Library.
*   **Memory Model:** Shared address space, **Non-Cache-Coherent** across hosts.
*   **Sequencer:** Centralized, multi-threaded, runs on a **single designated broker**.
*   **Replication:** "Dual Replication Strategy"
    *   *Metadata (`Bmeta`):* Replicated to **CXL Memory** (Chain replication, f+1 units).
    *   *Data Payloads (`Blog`):* Replicated to **Local Disk** of other brokers (preserves CXL bandwidth for critical path).

---

## 2. Memory Layout (Shared CXL Structures)

### A. Broker Metadata (`Bmeta`) — Table 5
*Scope: Per-Broker | Purpose: Coordination*
*Constraint: Split into two parts to enforce Single Writer Principle (different cache lines).*

| Part | Field | Writer | Description |
| :--- | :--- | :--- | :--- |
| **Local Struct** | `log_ptr` | Owner Broker | Pointer to start of `Blog`. |
| | `processed_ptr` | Owner Broker | Pointer to last locally-ordered message. |
| | `replication_done` | Owner Broker | Replication status map/counter. |
| **Sequencer Struct** | `ordered_seq` | Sequencer | Global seqno of last ordered msg for this broker. |
| | `ordered_ptr` | Sequencer | Pointer to end of last ordered msg for this broker. |

### B. Broker Log (`Blog`) — Table 4
*Scope: Per-Broker | Purpose: Append-Only Data Storage*
*Constraint: Headers **must be cache-line aligned** (pad to 64B) to prevent false sharing and ensure single-cacheline atomicity.*

| Field | Type | Writer | Description |
| :--- | :--- | :--- | :--- |
| `size` | int | Receiver | Size of payload. |
| `ts` | timestamp | Receiver | Receipt time. |
| `received` | Flag (0→1) | Receiver | Set when payload write completes. |
| `counter` | int | Delegation | Local per-broker sequence number. |
| `total_order` | int | Sequencer | **Global** sequence number. |

### C. Batch Headers Log (`Batchlog`) — Table 3
*Scope: Per-Broker | Purpose: Sequencer Optimization*
*Content:* Client ID, Batch SeqNo, Message Count, Pointers to `Blog` data.

### D. Memory Allocation
*   Allocated in **1 GB log segments**.
*   A **Manager broker** handles allocation/reclamation; its state is in CXL for failover.

---

## 3. The Processing Pipeline (Algorithms)

### Stage 1-2: Ingest & Local Ordering

**Receiver Threads (Pool):**
1.  **Allocate:** Atomic-increment pointer to reserve space in `Blog` + `Batchlog`.
2.  **Write:** Zero-copy payload directly to CXL.
3.  **Commit:** Set `msg_header.received = 1`.

**Delegation Thread (Single per Broker):**
1.  **Poll:** Scan `Blog` for `received == 1`.
2.  **Order:** Assign local `counter` to message header.
3.  **Publish:** Update `Bmeta.processed_ptr`.
4.  **Fence:** `clflushopt(&msg_header)` then `sfence`.

### Stage 3: Global Ordering (Sequencer)

*Constraint: One thread per broker. All threads on a single designated host.*

1.  **Poll:** Read `Bmeta.processed_ptr` of assigned broker.
2.  **Validate FIFO:** Check batch seqno against `next_batch_seqno[client_id]` map. Defer if out-of-order.
3.  **CAS Update:** `CAS(&next_batch_seqno[client_id], expected, expected+1)`.
4.  **Global Order:** Write `total_order` into message header in `Blog`.
5.  **Commit:** Update `Bmeta.ordered_ptr` and `Bmeta.ordered_seq`.

### Stage 4: Replication

**Replication Threads (Pool per Broker):**
1.  **Poll:** Read `ordered_ptr` from a Primary Broker's `Bmeta`.
2.  **Read:** Pull payload from Primary's `Blog` (CXL read).
3.  **Write:** Append to **local disk** (SSD). Call `fsync`.
4.  **Ack:** Update own `Bmeta.replication_done`.

### Stage 5-6: Ack & Delivery

**Ack Threads:**
1.  **Poll:** `replication_done` counters of replica brokers.
2.  **ACK:** Once `f+1` replicas confirm, send ACK to publisher.

**Sender Threads:**
1.  Receive subscriber pull request.
2.  Deliver messages where `replication_done >= f+1` from `Blog`.

---

## 4. Concurrency & Coherence (The "Laws")

**Context:** CXL memory is **Non-Cache-Coherent** across hosts.

| Principle | Rule | Implementation |
| :--- | :--- | :--- |
| **1. Single Writer** | A cache line is written by **at most one host** at a time. | `Bmeta` split into Local/Sequencer structs on separate cache lines. Message header ownership transfers from Broker → Sequencer after `processed_ptr` advances. |
| **2. Monotonicity** | Shared values (pointers, counters, flags) **only increase**. | Readers on stale caches eventually converge. |
| **3. Flush & Poll** | Writers: `clflushopt` + `sfence`. Readers: busy-wait poll. | After Delegation writes header, flush. Sequencer polls `processed_ptr`. |

---

## 5. Failure Modes & Recovery

*Assumption: External membership service provides reliable failure detection.*

| Failure | Impact | Recovery |
| :--- | :--- | :--- |
| **Client** | None. System is stateless w.r.t. clients. | Publisher resends unACKed batches. Duplicates discarded via `next_batch_seqno`. |
| **Broker** | Loses compute, not data. | Clients redirect to other brokers. **Failed broker's `Blog` remains in CXL**, sequencer continues processing its data. |
| **Sequencer Host** | Ordering halts. | New sequencer starts on another broker. Recovers `ordered_seq` and `next_batch_seqno` **from CXL**. |
| **CXL Memory Unit (Metadata)** | `Bmeta` lost. | Recovered from chain-replicated copy on another CXL unit (f+1 replicas). |
| **CXL Memory Unit (Data)** | `Blog` segment lost. | **Service pause.** Recover from disk replicas on other brokers. Duration ∝ data volume. |

---

## 6. Client API (Table 2)

| Method | Behavior |
| :--- | :--- |
| `Create(flags)` | Create log with ordering (`STRONG`, `WEAK`, `NONE`) and durability (`FULL`, `SINGLE`, `NONE`). |
| `Init()` | Connect to all brokers. |
| `Publish(void* msg)` | **Non-blocking.** Copies to client buffer, returns immediately. |
| `Poll(msg_id)` | Block until `msg_id` is ACKed. |
| `Poll(msg_id_range)` | Block until range is ACKed. |
| `GetNext()` | Deliver next pending message (subscriber). |
| `GetAll()` | Deliver all pending messages (subscriber). |
