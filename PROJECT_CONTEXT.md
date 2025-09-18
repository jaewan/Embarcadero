# Embarcadero: System Design and Implementation Context

## 1. Core Thesis
The system is a distributed log designed for a disaggregated memory environment. Its core thesis is that the tight coupling of data transport and ordering logic in traditional designs is inefficient. By decoupling these, coordination can occur at memory speed, not network speed.

## 2. Design Axioms
The design is governed by three inviolable rules:
- **A1: Decouple Data and Metadata Paths.** Data payloads are written exactly once to disaggregated memory. All subsequent protocol work (sequencing, replication) operates only on fixed-size metadata.
- **A2: Principled Coordination without Coherence.** The system must be provably correct on memory fabrics without hardware coherence. Correctness is achieved via software primitives.
- **A3: Isolate Contention to Coherent Domains.** High-contention operations, like assigning global sequence numbers, are executed entirely within the cache-coherent domain of a single machine (the Sequencer). Cross-host atomic operations over the memory fabric are forbidden.

## 3. System Architecture
The system comprises Brokers, a centralized Sequencer, and a pool of Replicas, all sharing rack-scale disaggregated memory. Clients connect to Brokers via TCP/IP.

*   ### Components
    *   **Clients**: Publish batches of messages to Brokers. The client library handles batching, load balancing, and transparent failover.
    *   **Brokers**: Ingest client data, writing it to a dedicated log in disaggregated memory. They then request sequencing for the data. Any broker can serve any read request.
    *   **Sequencer**: A centralized component that polls for ordering requests and assigns a definitive global order to data batches. Its work is minimal and performed on its local memory.
    *   **Replicas**: Read newly-ordered data from disaggregated memory and copy it to their local stable storage to ensure durability.

*   ### Key Data Structures (in Disaggregated Memory)
    *   **`BrokerLog` (`\blog`)**: A dedicated, append-only log for each Broker. It stores the raw data payloads of client batches.
    *   **`Pending Batch Ring` (PBR)**: A per-broker ring buffer containing metadata about batches written to the `BrokerLog` that are awaiting sequencing.
    *   **`Global Order Index` (GOI)**: A single, central array that records the definitive global order of all batches. Each entry contains a pointer to the batch data in a `BrokerLog`, its size, and its global sequence range.

*   ### High-Level Flow
    Clients publish batches to Brokers. Brokers write the data to their `BrokerLog` and signal readiness by placing metadata in their PBR. The Sequencer polls all PBRs, assigns a global sequence number, and records this order in the GOI. Replicas use the GOI to locate, copy, and persist the data.

## 4. The Critical Path: Write Operation
1.  **Client-Side Batching & Send**:
    *   The client library uses multiple network threads, each pinned to a core with a persistent connection to a broker.
    *   Each thread owns a lock-free, single-producer single-consumer (SPSC) circular buffer for staging messages.
    *   The application thread dispatches messages to these buffers round-robin, which are allocated using hugepages to minimize TLB misses.
    *   This enables true zero-copy sends via the `send()` system call.
    *   If a broker fails, the library transparently resends unacknowledged batches to another broker.

2.  **Broker Ingestion (Zero-Copy Receive)**:
    *   In line with **Axiom A1**, the broker performs a single write of the data payload into disaggregated memory.
    *   The per-broker `BrokerLog` is mapped into the broker's virtual address space using hugepages.
    *   The broker first reads a fixed-size header to get the batch size.
    *   It reserves a contiguous block in its `BrokerLog` with a single atomic `fetch_and_add` on the log's tail pointer.
    *   It then issues a `recv()` system call that instructs the kernel to write the TCP payload *directly* into the reserved memory region in the `BrokerLog`, achieving a true zero-copy receive.
    *   **Hardware Sympathy**: All batch writes to the `BrokerLog` are padded to be strictly 64-byte aligned to match cache-line boundaries, improving performance and ensuring correctness for coordination protocols.

3.  **Ordering Request**:
    *   After the data write, the broker populates a single, cache-line-sized metadata entry in its PBR.
    *   This entry contains a pointer to the data, its size, and client metadata. A single memory-fenced write makes it visible to the sequencer.

4.  **Sequencing**:
    *   Embodying **Axiom A3**, a centralized, multi-threaded sequencer polls the PBRs. Each thread polls a dedicated PBR to eliminate contention.
    *   Upon finding a new entry, a thread enters a tiny critical section on its *local memory*, validates client sequence numbers (for idempotency), and reserves a global sequence range with an atomic `fetch_add` on a counter in its *local, coherent DRAM*.
    *   Finally, it populates the corresponding entry in the GOI, making the batch's order and location visible to all Replicas.

## 5. Durability: Metadata Chain Replication
This protocol ensures data is durably stored on multiple replicas and is built on primitives that work on non-coherent memory (**Axiom A2**).

*   ### Coordination Primitives for Incoherent Memory
    *   **Single-Writer Ownership**: Only one host may write to a shared cache line at any given time.
    *   **Monotonic Update**: Writers must only write monotonically increasing values (e.g., counters) to prevent readers from seeing inconsistent state.
    *   **Poll-based State Transitions**: Readers poll memory locations for state changes, which are triggered by observing specific monotonic values.

*   ### Protocol Flow
    1.  For any batch, a deterministic function maps it to an ordered chain of replicas.
    2.  All assigned replicas poll the GOI. Once the entry appears, they *in parallel* start copying the data from the `BrokerLog` in disaggregated memory to their local durable storage.
    3.  They coordinate durability confirmation *serially* using the `num_replicated` field in the GOI entry as a write token.
    4.  Replica $R_i$ polls the field until it observes the value $i$. It then gains ownership, atomically increments the field to $i+1$, and passes ownership to the next replica in the chain.

*   ### Acknowledgment
    *   An "Ack thread" on the primary broker polls the `num_replicated` field.
    *   Once the counter reaches the desired replication factor, it sends an acknowledgment to the client.

## 6. Read Path (Subscribers)
*   **"Read-From-Anywhere" Architecture**: Because the entire log is in disaggregated memory, any broker can serve a read request for data ingested by any other broker. A client's `read(global_sequence)` request is load-balanced across all active brokers.
*   **Order Reconstruction**: The broker uses the sequence number to find the entry in the GOI. The GOI provides a direct memory pointer to the data and also includes the batch's base global sequence number and the number of messages it contains. The client library receives the message batch and reconstructs the total order by delivering messages sequentially to the application.

## 7. Fault Tolerance & Recovery
*   **Epoch-Based Safety**:
    *   An external service (`etcd`) manages cluster membership and elects the Sequencer, enforcing a monotonically increasing epoch number for any membership change.
    *   The active epoch number is mirrored into a replicated control block in disaggregated memory.
    *   All metadata (in PBR and GOI) is immutably tagged with the epoch of its creation. Nodes only act on metadata that matches their current view of the epoch.

*   **Hybrid Replication Strategy**:
    *   **Metadata (PBR, GOI)**: The system's source of truth. These structures are synchronously chain-replicated to a separate, failure-independent disaggregated memory module. The overhead is a single extra cache-line write.
    *   **Data Payloads (`BrokerLog`)**: Replicated by Replica nodes from disaggregated memory to their own local durable storage to avoid doubling memory capacity and bandwidth costs.

*   **Zombie Fencing**:
    *   If a Sequencer is partitioned but can still access memory (a "zombie"), its writes will be tagged with a stale epoch.
    *   Correct nodes periodically poll the replicated control block for the authoritative epoch. They will see the epoch mismatch in the zombie's GOI writes and discard them as invalid.

*   **Deterministic Recovery**:
    *   When a new sequencer is elected, it performs a fast metadata scan:
        1. Reads the replicated GOI to find the last committed batch from the prior epoch.
        2. Scans the replicated PBRs for any batches ingested but not yet ordered.
        3. Resumes ordering from that globally consistent state. Recovery is a metadata scan, not a log replay.

## 8. Implementation Highlights
*   **Language**: C++ (~9,800 lines).
*   **Architecture**: Thread-per-core, shared-nothing design to minimize lock contention, context switching, and inter-core communication. Threads and their critical data are pinned to specific cores.
*   **Networking**: Custom-built using non-blocking sockets for fine-grained control. Generic RPC frameworks (gRPC) and even optimized queues (Folly's MPMC) were found to be bottlenecks.
*   **Replication Path**: Replicas read data directly from the source broker's `BrokerLog` in disaggregated memory into their own network buffers, avoiding intermediate copies.
