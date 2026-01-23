# Product Context: Embarcadero

## 1. Vision & Problem Statement
Embarcadero is a **Distributed Shared Log** designed for the era of **Disaggregated Memory (CXL)**. It challenges the decades-old trade-off in distributed systems between performance, strong ordering, and high availability.

*   **The Bottleneck:** Existing systems (Kafka, Corfu) rely on point-to-point network messages for coordination (ordering, replication).
*   **The Solution:** We use rack-scale shared memory as a **Coordination Fabric**. We replace complex network protocols (Paxos) with simple atomic CPU instructions on shared memory.
*   **The Result:** 1.3x-2.7x better throughput than SOTA, with single-digit microsecond latency and strong total ordering.

## 2. Engineering Philosophy (The "Vibe")
*   **Memory over Network:** We do not send network packets for coordination. We flip bits in shared CXL memory.
*   **Zero-Copy Everything:** From NIC to CXL. No `memcpy` allowed in the hot path.
*   **Lock-Free & Polling:** We despise locks. We use atomic polling on cache-lines (wait-free/lock-free designs).
*   **Hardware-Aware:** We explicitly handle **Non-Coherent Memory**. We manually flush caches (`clflushopt`) and fence memory (`sfence`). We pad data to cache-line boundaries.

## 3. Critical Guarantees (The Spec)
The system implementation MUST provide the following guarantees (default configuration):

### Property 3d: Strong Total Ordering
The system satisfies:
1.  **Basic Publisher Ordering:** If client A publishes `m1` and gets an ACK before client B publishes `m2`, no subscriber delivers `m2` before `m1`.
2.  **FIFO Publisher Ordering:** If a single client publishes `m1` before `m2`, no subscriber delivers `m2` before `m1`.
3.  **Weak Total Ordering:** If any subscriber delivers `m1` before `m2`, all subscribers deliver `m1` before `m2`.

### Property 4a: Full Durability
A message is acknowledged to the publisher **only after**:
1.  It has been replicated across multiple brokers.
2.  It has been written to stable storage (Disk/SSD) on the replicas.
