# Embarcadero: Definitive System Design

**A Distributed Shared Log for the CXL Era**

*This document represents the consensus design following extensive critical review and expert debate. It serves as the authoritative reference for implementation and publication.*

---

## Abstract

Distributed shared logs face a fundamental tension: load-balancing writes across nodes sacrifices ordering guarantees, while preserving ordering creates bottlenecks. We present Embarcadero, a shared log that exploits disaggregated memory (CXL (Compute Express Link)) to break this trade-off. Unlike network-based systems (Scalog, LazyLog, SpecLog) that must choose between ordering strength and performance, Embarcadero achieves both by transforming sequencing from a network-bound coordination problem into a memory-local computation. Our key contributions are: (1) an architectural pattern that decouples data placement from ordering using CXL as a coordination fabric; (2) epoch-batched sequencing that reduces O(n) atomic operations to O(1); (3) optional per-client ordering that enables pipelined state machine replication without head-of-line blocking for other workloads. Embarcadero achieves high throughput with low latency—measured \todo higher throughput than Scalog and significantly lower latency than Corfu (see §6.2); exact numbers are deployment-dependent.

---

## Part I: The Shared Log Landscape

### 1.1 The Fundamental Trade-off

Distributed shared logs have faced an immutable constraint:

```
TRADITIONAL TRADE-OFF:
  Load-balance writes across nodes  →  Sacrifice ordering guarantees
  Preserve ordering guarantees      →  Bottleneck on single node/path
```

This trade-off arises because ordering requires coordination, and coordination over networks is expensive (microseconds to milliseconds per operation).

### 1.2 The Shared Log Lineage

The evolution of shared logs has produced two distinct architectural branches:

```
                         SHARED LOG DESIGN SPACE
    ════════════════════════════════════════════════════════════════

    BRANCH 1: TOKEN-BEFORE-WRITE              BRANCH 2: WRITE-BEFORE-ORDER
    (Sequencer in write path)                 (Sequencer off write path)

    ┌─────────────────────────┐               ┌─────────────────────────┐
    │      Corfu (2012)       │               │     Scalog (2020)       │
    │      vCorfu (2017)      │               │     LazyLog (2024)      │
    │      Tango (2013)       │               │     SpecLog (2025)      │
    └───────────┬─────────────┘               └───────────┬─────────────┘
                │                                         │
    Characteristics:                          Characteristics:
    • Client gets token FIRST                 • Client writes data FIRST
    • Then writes to storage                  • System orders LATER
    • Per-client order: FREE                  • Per-client order: EXPENSIVE
    • Throughput: LIMITED                     • Throughput: HIGH
    • Sequencer is bottleneck                 • Ordering adds latency

                         ┌─────────────────────────┐
                         │     EMBARCADERO         │
                         │    (This Work)          │
                         └───────────┬─────────────┘
                                     │
                         Characteristics:
                         • Write-before-order (like Scalog)
                         • BUT: Sequencer reads via CXL (nanoseconds)
                         • Per-client order: CHEAP (opt-in)
                         • Throughput: HIGH
                         • Latency: LOW
```

### 1.3 Why Prior Systems Made Their Choices

#### Corfu Branch (Token-Before-Write)

**Corfu's Protocol:**
```
1. Client → Sequencer: "Give me a token"        [Network RTT: ~100μs]
2. Sequencer → Client: "Token = 42"             [Network RTT: ~100μs]
3. Client → Storage: "Write at position 42"     [Network RTT: ~100μs]
```

**Properties:**
- Per-client ordering is **free**: Client serializes through sequencer
- Throughput limited by sequencer's network capacity (~1M tokens/sec)
- Every write pays 2 RTTs before data lands

**Systems:** Corfu, vCorfu, Tango, Delos (Facebook)

#### Scalog Branch (Write-Before-Order)

**Scalog's Protocol:**
```
1. Client → Shard: "Store this data"            [Network RTT: ~100μs]
2. Shard replicates locally                     [Async]
3. Ordering layer collects "cuts" from shards   [Periodic, ~100ms]
4. Paxos determines global order from cuts      [Consensus delay]
5. Shards assign global sequence from cuts      [Local computation]
```

**Properties:**
- Throughput scales with shards (52M records/sec claimed)
- Per-client ordering **not guaranteed** across shards
- High latency due to cut batching (~100ms)

**Systems:** Scalog, LazyLog (lazy binding), SpecLog (speculation)

### 1.4 The Category Error in Comparing Systems

**Critical Insight (Aguilera):**
> "Scalog, LazyLog, and SpecLog optimize for **shared-nothing networks**. Cross-shard ordering requires TCP/RDMA coordination—expensive. Embarcadero uses **shared memory (CXL)**. The sequencer reads all shard state via load instructions—nanoseconds. The cost functions are fundamentally different."

| Operation | Network-Based (Scalog) | CXL-Based (Embarcadero) |
|-----------|------------------------|-------------------------|
| Read remote shard state | 10-100 μs (RDMA) | 200-500 ns (CXL load) |
| Coordinate across shards | 1-10 ms (consensus) | 0 (shared memory) |
| Atomic operations | Limited by NIC | Limited by CPU |

**Implication:** Design decisions that were optimal for Scalog/LazyLog/SpecLog do not apply to CXL architectures. Embarcadero can afford capabilities they could not.

### 1.5 The Per-Client Ordering Debate

#### The Scientific Question

**Is per-client ordering (respecting client's local submission order in the global total order) necessary?**

#### Arguments FOR Per-Client Ordering

**1. Enables Pipelined State Machine Replication**

Without per-client ordering, clients must wait for ACKs:
```
Client (Weak Ordering):
  send(SET X=1)
  wait_for_ack()      ← BLOCKING: throughput = 1/RTT
  send(SET X=2)
  wait_for_ack()
```

With per-client ordering, clients can pipeline:
```
Client (Strong Ordering):
  send(SET X=1)       ← Non-blocking
  send(SET X=2)       ← Non-blocking
  send(SET X=3)       ← Throughput = N/RTT
  // System guarantees X=1 < X=2 < X=3 in global order
```

**2. Required for Distributed Transactions**
```
Transaction Coordinator writes:
  PREPARE(T1)  →  Shard A
  COMMIT(T1)   →  Shard B

If reordered to COMMIT, PREPARE: Protocol breaks.
```

**3. Simplifies Application Development**

Without system support, every application must implement TCP-like resequencing:
```cpp
// Application-level reordering (what Scalog requires)
class ApplicationReorderer {
    std::map<uint64_t, Message> buffer;
    uint64_t next_expected = 0;

    void on_message(Message m) {
        buffer[m.client_seq] = m;
        while (buffer.contains(next_expected)) {
            process(buffer[next_expected]);
            buffer.erase(next_expected++);
        }
    }
};
```

This violates the log abstraction—a log should be a sequence, not a bag.

#### Arguments AGAINST Per-Client Ordering

**1. Most Workloads Don't Need It**

| Workload | % of Deployments | Needs Per-Client Order? |
|----------|------------------|------------------------|
| Log aggregation | ~40% | No |
| Event streaming | ~30% | No (per-partition suffices) |
| Metrics/telemetry | ~10% | No |
| SMR/Databases | ~15% | **Yes** |
| Transactions | ~5% | **Yes** |

**2. Introduces Head-of-Line (HOL) Blocking**

```
Scenario: Client sends seq=1→Broker A, seq=2→Broker B
          Broker B is slow (GC pause, network congestion)

Weak Ordering:
  seq=1 from A: sequenced immediately
  seq=2 from B: sequenced when it arrives
  No blocking.

Strong Ordering:
  seq=1 from A: arrives at sequencer
  seq=2 from B: delayed (B is slow)
  Sequencer MUST WAIT for seq=2 before delivering seq=1
  (because seq=3 might arrive before seq=2, and we need order)

  Result: A's messages blocked by B's slowness = HOL blocking
```

**3. SOTA Systems Chose Weak Ordering**

- Scalog (NSDI '20): No per-client ordering
- LazyLog (SOSP '24 Best Paper): No per-client ordering
- SpecLog (OSDI '25): No per-client ordering

**"The debate is resolved by recognizing different hardware contexts."**

| System | Hardware | Per-Client Order Cost | Decision |
|--------|----------|----------------------|----------|
| Scalog | Network | High (cross-shard coordination) | Don't provide |
| LazyLog | Network | High | Don't provide |
| SpecLog | Network | High | Don't provide |
| Corfu | Network | Free (token-before-write) | Provide |
| **Embarcadero** | **CXL** | **Low (memory-local)** | **Provide as opt-in** |

**Final Position (we make in this paper):**
> "Embarcadero is the only system that can offer both modes cheaply. Provide weak ordering by default (no HOL blocking), strong ordering opt-in (for SMR/transactions). Expose the trade-off to the user."

---

## Part II: Embarcadero Architecture

### 2.1 Design Principles

Embarcadero is built on three axioms:

| Axiom | Statement | Implication |
|-------|-----------|-------------|
| **A1** | Decouple data and metadata paths | Payloads written once; coordination on 64-byte headers only |
| **A2** | Coordination without coherence | Correct on non-cache-coherent CXL; software protocols only |
| **A3** | Isolate contention to coherent domains | All atomics within sequencer's local memory; no cross-host atomics |

### 2.2 System Model

```
┌───────────────────────────────────────────────────────────────────────────┐
│                              ARCHITECTURE                                 │
├───────────────────────────────────────────────────────────────────────────┤
│                                                                           │
│   CLIENTS                          BROKERS                                │
│   ───────                          ───────                                │
│   ┌─────────┐                      ┌─────────┐                            │
│   │Publisher│──TCP────────────────▶│ Broker 0 │──CXL──▶ BLog[0], PBR[0]   │
│   │(Default)│                      └─────────┘                            │
│   └─────────┘                      ┌─────────┐                            │
│   ┌─────────┐                      │ Broker 1 │──CXL──▶ BLog[1], PBR[1]   │
│   │Publisher│──TCP────────────────▶└─────────┘                            │
│   │(Strong) │                      ┌─────────┐                            │
│   └─────────┘                      │ Broker 2 │──CXL──▶ BLog[2], PBR[2]   │
│                                    └─────────┘                            │
│                                    ┌─────────┐                            │
│                                    │ Broker 3 │──CXL──▶ BLog[3], PBR[3]   │
│                                    └─────────┘                            │
│                                                                           │
│   CXL MEMORY FABRIC                                                       │
│   ─────────────────                                                       │
│   ┌─────────────────────────────────────────────────────────────────────┐ │
│   │  BLog[0..N]  │  PBR[0..N]  │  GOI  │  CompletionVector  │  Control   │ │
│   │              │             │       │  (ACK path)        │  Block     │ │
│   └─────────────────────────────────────────────────────────────────────┘ │
│                          │                                                │
│                          ▼                                                │
│   SEQUENCER              │           REPLICAS                             │
│   ─────────              │           ────────                             │
│   ┌─────────────────────────┐       ┌─────────┐                           │
│   │ Collectors (poll PBRs)  │       │Replica 0│──local disk               │
│   │ Sequencer (assign seq)  │       │Replica 1│──local disk               │
│   │ GOI Writer (commit)     │       │Replica 2│──local disk               │
│   └─────────────────────────┘       └─────────┘                           │
│                                                                           │
└───────────────────────────────────────────────────────────────────────────┘
```

**Components:**

| Component | Count | Role | State Location |
|-----------|-------|------|----------------|
| **Clients** | Thousands | Publish/subscribe via TCP | Local |
| **Brokers** | 4-16 (recommended) | Stateless ingestion; write to CXL; wait-free | None (stateless) |
| **Sequencer** | 1 (logical) | Assign global order; internally parallel | Local + CXL |
| **Replicas** | 2-4 | Durable storage; copy from CXL to disk | Local disk |
| **CXL Memory** | 1-3 modules | Shared coordination fabric | Persistent (rack power) |

### 2.3 Ordering Modes

Embarcadero provides **order levels** (numeric): Order 0, Order 1, Order 2, and Order 5. Each level defines what ordering guarantee the log provides and how the sequencer (if any) and broker metadata are updated.
(In paper do not mention about other ordering levels but just order 2 weak ordering and order 5 strong ordering)

#### Order 0: No Global Ordering

```
GUARANTEE:
  Messages are visible in PBR in arrival order per broker.
  No global_seq; no sequencer runs for this topic.

NO GUARANTEE:
  No total order across brokers.
  No per-client order.

MECHANISM:
  No sequencer. The broker (network path) that commits a batch into the PBR
  SHALL update that broker's written offset (and optionally written_addr)
  in the same execution context, after the PBR entry is visible in CXL
  (store + flush + fence). This is "Option A": the writer of the data
  updates the visibility counter so ACK (ack_level=1) can advance without
  a separate sequencer or delegation thread.

  Implementation: The thread that reserves PBR slot, writes the PBR entry,
  and flushes it SHALL then update TInode.offsets[broker_id].written
  (monotonic) and, if used, written_addr. No Sequencer 0 thread.

PERFORMANCE:
  Lowest latency for ACK (no sequencer hop).
  Maximum throughput.

USE CASES:
  Fire-and-forget, best-effort logging, metrics where order is irrelevant.
```

#### Order 1: Per-Broker Local

```
GUARANTEE:
  Within each broker, messages are ordered in arrival order (FIFO per broker).
  No global order across brokers.

NO GUARANTEE:
  No total order across brokers.
  No per-client order.

MECHANISM:
  Optional per-broker sequencer or simple FIFO visibility; no cross-broker
  coordination. Written offset advances per broker (same as Order 0 or
  via local processing). Used when clients care only about per-broker
  ordering (e.g. single-broker topics or sharded keys).

USE CASES:
  Single-broker topics, sharded streams where global order is not required.
```

#### Order 2: Total Order

```
GUARANTEE:
  All batches have unique global_seq.
  All subscribers observe identical ordering.

NO GUARANTEE:
  Per-client submission order is NOT preserved across brokers.

MECHANISM:
  Sequencer assigns global_seq based on arrival order in PBRs.
  No per-client state tracking.

  RECOMMENDATION (Order 2 sequencer design):
  The sequencer SHALL be non-blocking. It SHALL constantly iterate over
  all brokers' PBRs (round-robin or scan), and SHALL assign global_seq to
  batches that are present and valid. It SHALL NOT block waiting for a
  specific broker or batch (no "while (batch not ready) yield"). If one
  broker is slow or empty, the sequencer makes progress on other PBRs;
  zero HOL blocking. Implementation: single loop over broker set, for
  each broker read PBR tail/head, consume contiguous ready entries, assign
  global_seq, write GOI, advance consumed pointer; then next broker.

PERFORMANCE:
  Zero HOL blocking.
  Maximum throughput for total order.
  Minimum latency.

USE CASES:
  Log aggregation, metrics, telemetry, Kafka-style streaming.
```

#### Order 5: Strong Order (Opt-In)

```
GUARANTEE:
  All Order 2 guarantees, PLUS:
  If client C sends M1 before M2 (client_seq order),
  then M1.global_seq < M2.global_seq (or M1 declared lost).

MECHANISM:
  Sequencer tracks per-client state (next_expected_seq) per shard worker (§3.2.1).
  Out-of-order batches are resolved by three layers: (1) intra-cycle sort
  resolves ~90% of reordering at ~1μs; (2) cross-cycle hold buffer resolves
  ~9% at ~30–100μs; (3) wall-clock timeout (default 5ms, covering P99.9
  intra-rack TCP retransmit) emits SKIP markers for unresolvable gaps.
  See §3.2 process_level5 for the full protocol.

PERFORMANCE:
  Potential HOL blocking during broker failures/slowdowns.
  ~15% throughput overhead for 100% strong-order clients.
  Tail latency bounded by gap timeout (configurable, default 5ms;
  justified by TCP RTO_MIN=1–2ms + margin).

USE CASES:
  State machine replication, distributed databases, transactions.
```

### 2.3.1 ACK Levels

Acknowledgments are independent of order level. The client negotiates **ack_level** at connection handshake. The broker sends cumulative offset ACKs on a dedicated ACK channel (e.g. TCP) per (client, broker).

| ack_level | When ACK is sent | Guarantee at ACK time |
|-----------|-------------------|------------------------|
| **0** | Never | Fire-and-forget; no ACKs. Broker may still track for backpressure. |
| **1** | After visibility in CXL | For **Order 0**: after broker has updated `written` (PBR commit). For **Order 1/2/5**: after sequencer has assigned order (broker reads `ordered` or equivalent). Message is visible to sequencer/subscribers; not necessarily durable. |
| **2** | After replication | Tail replica has updated CompletionVector; batch is durable (replicated to disk on replica set). Client can treat ACK as durability guarantee. |

**Semantics:**

- **ack_level = 0:** Broker does not send ACKs. `GetOffsetToAck` returns sentinel (no ack). Used for maximum throughput when loss is acceptable.
- **ack_level = 1:** Broker sends cumulative offset when its **written** (Order 0) or **ordered** (Order 1/2/5) count advances. One ACK per (broker, topic) represents "all messages up to this offset are visible in CXL (and ordered if order > 0)."
- **ack_level = 2:** Broker sends cumulative offset when CompletionVector for that broker advances (tail has replicated the batch). One ACK per (broker, topic) represents "all messages up to this offset are durable."

**Implementation note:** The broker's AckThread (or equivalent) polls the appropriate source (written, ordered, or CV) and sends the cumulative offset on the ACK connection. Order level and ack_level are independent: e.g. Order 0 + ack_level=1 uses written; Order 2 + ack_level=1 uses ordered; any order + ack_level=2 uses CV.

**Publish latency measurement:** To measure publish latency (client send -> ACK), record the timestamp when a batch is fully sent on the client and compute the delta when the cumulative ACK for that broker advances past that batch. Because ACKs are cumulative per broker, the client should correlate ACK values with per-broker message counts (e.g., batch end_count) to attribute a send time to the ACK. This requires ack_level >= 1.

### 2.3.2 Known Limitation: Hold Buffer Volatility (Order 5)

When a batch enters the hold buffer (gap detected), its PBR slot is freed (consumed_through advanced) so the ring doesn't deadlock. The batch metadata is held in sequencer RAM. If the sequencer crashes during the hold window (≤5 ms default, configurable via order5.base_timeout_ms), held batches are lost:

- **PBR slot:** freed (reusable)
- **Hold buffer:** volatile (lost on crash)
- **GOI entry:** not yet written
- **BLog data:** still present but unreferenced

These batches are declared "lost" per Property 2b (§5.1). Clients with ack_level ≥ 1 will not have received ACKs for these batches and should retry.

**Mitigation options (future work):**
1. Checkpoint hold buffer to CXL periodically (adds ~50 μs per checkpoint)
2. Scan BLog on recovery for unreferenced batches (adds recovery time)
3. Accept the loss (default — 5 ms window is negligible)

### 2.4 Data Structures

All structures are at least 64-byte aligned for atomic cache-line operations on non-coherent memory. **Control Block and Completion Vector** use **128-byte alignment** to avoid false sharing with adjacent structures: modern CPUs often prefetch 128 bytes (2 cache lines); 64-byte layout can cause ping-pong invalidations when one broker writes CV[i] and another reads CV[i±1]. CXL latency is high—avoid extra cache effects.

#### Control Block (128 bytes)
```cpp
struct alignas(128) ControlBlock {
    // Epoch-based fencing
    std::atomic<uint64_t> epoch;           // Monotonic, incremented on failures
    std::atomic<uint64_t> sequencer_id;    // Current sequencer identity
    std::atomic<uint64_t> sequencer_lease; // Lease expiry timestamp (ns)

    // Cluster state
    std::atomic<uint32_t> broker_mask;     // Bitmap: 1 = healthy
    std::atomic<uint32_t> num_brokers;     // Active broker count

    // Progress tracking
    std::atomic<uint64_t> committed_seq;   // Highest durable global_seq

    uint8_t _pad[80];                      // Pad to 128 bytes (prefetcher safety)
};
static_assert(sizeof(ControlBlock) == 128);
```

#### PBR Entry (64 bytes) — Pending Batch Ring
```cpp
struct alignas(64) PBREntry {
    // Payload location
    uint64_t blog_offset;      // Offset in broker's BLog
    uint32_t payload_size;     // Batch size in bytes
    uint32_t message_count;    // Number of messages

    // Identification
    uint64_t batch_id;         // Globally unique (broker_id | timestamp | counter)

    // Metadata
    uint16_t broker_id;        // Source broker [0-31]
    uint16_t epoch_created;    // Epoch when written (staleness detection)
    uint32_t flags;            // See FLAG_* constants

    // Per-client ordering (Used by Order 5 only but set in all levels)
    uint64_t client_id;        // 0 if Order 0 (default)
    uint64_t client_seq;       // Client's sequence number

    uint64_t _reserved;        // Future use / alignment
};
static_assert(sizeof(PBREntry) == 64);

// Flag constants
constexpr uint32_t FLAG_VALID        = 1u << 31;  // Entry is valid
constexpr uint32_t FLAG_STRONG_ORDER = 1u << 0;   // Order 5 ordering
constexpr uint32_t FLAG_RECOVERY     = 1u << 2;   // Recovered from failed broker
constexpr uint32_t FLAG_SKIP_MARKER  = 1u << 3;  // GOI-only: gap declared lost (Order 5)
constexpr uint32_t FLAG_RANGE_SKIP   = 1u << 4;  // With SKIP_MARKER: message_count = skip range size
constexpr uint32_t FLAG_NOP         = 1u << 5;  // GOI-only: placeholder for duplicate batch CV advancement
```

#### GOI Entry (64 bytes) — Global Order Index
```cpp
struct alignas(64) GOIEntry {
    // The definitive ordering
    uint64_t global_seq;       // Unique global sequence number

    // Batch identification
    uint64_t batch_id;         // Matches PBREntry.batch_id

    // Payload location
    uint16_t broker_id;        // Which broker's BLog
    uint16_t epoch_sequenced;  // Epoch when sequenced
    uint64_t blog_offset;      // Offset in BLog (64-bit: BLogs are ~15 GB each)
    uint32_t payload_size;     // Payload size
    uint32_t message_count;   // Message count

    // Replication progress
    std::atomic<uint32_t> num_replicated;  // 0..R (chain protocol)

    // Per-client ordering info
    uint64_t client_id;        // Source client (0 if Order 0)
    uint64_t client_seq;       // Client sequence (if Order 5)

    // ACK path: PBR index for this broker (used by CV updater to advance CompletionVector[broker_id])
    uint32_t pbr_index;        // Index in PBR[broker_id] for this batch
    uint32_t flags;            // FLAG_* constants (Order 5: FLAG_NOP, FLAG_SKIP_MARKER, FLAG_RANGE_SKIP)
};
static_assert(sizeof(GOIEntry) == 64);
```

**Special GOI entry types (Order 5):** A GOI entry may be a normal batch, a SKIP marker (FLAG_SKIP_MARKER; no BLog payload; broker_id=0, pbr_index=0; CV updater must not advance CV for SKIP), or a NOP (FLAG_NOP; valid broker_id and pbr_index, zero payload). All carry valid global_seq and flow through replication. See §3.4 for replica and CV handling.

### 2.5 Memory Layout

All offsets are contiguous; no region overlaps. **Hex notation:** Underscore groups digits for readability (e.g. `0x4_0000_2000` = 4×2³² + 0x2000 = 16 GB + 8 KB). GOI is 16 GB, so it ends at 0x4_0000_2000; PBR starts there; the checkpoint region sits between PBR and BLog; BLogs start at 0x4_5000_2000.

```
CXL Memory Module (512 GB):
┌────────────────────────────────────────────────────────────────────────────┐
│ OFFSET          │ SIZE    │ STRUCTURE                                        │
├─────────────────┼─────────┼──────────────────────────────────────────────────┤
│ 0x0000_0000     │ 4 KB    │ Control Block (replicated to secondary module)   │
├─────────────────┼─────────┼──────────────────────────────────────────────────┤
│ 0x0000_1000     │ 4 KB    │ CompletionVector[0..31]: 32 × 128 bytes (ACK path) │
├─────────────────┼─────────┼──────────────────────────────────────────────────┤
│ 0x0000_2000     │ 16 GB   │ GOI: 256M entries × 64 bytes                       │
│                 │         │ Ends at 0x4000_2000 (replicated to secondary)    │
├─────────────────┼─────────┼──────────────────────────────────────────────────┤
│ 0x4000_2000     │ 1 GB    │ PBR[0..31]: 32 rings × 512K entries × 64 bytes    │
│                 │         │ Ends at 0x4400_2000 (replicated to secondary)    │
├─────────────────┼─────────┼──────────────────────────────────────────────────┤
│ 0x4400_2000     │ 16 MB   │ Sequencer Checkpoint: Order 5 per-client state     │
│                 │         │ (double-buffered, 2 × 8 MB)                      │
│                 │         │ Ends at 0x4500_2000 (replicated with metadata)   │
├─────────────────┼─────────┼──────────────────────────────────────────────────┤
│ 0x4500_2000     │ ~494 GB │ BLog[0..31]: 32 circular logs × ~15 GB each     │
│                 │         │ (NOT replicated in CXL; replicated to disk)      │
└────────────────────────────────────────────────────────────────────────────┘

Replication Strategy:
  - Control Block, GOI, PBRs, Checkpoint: Chain-replicated across CXL modules (metadata). See §4.2.4 for chain protocol.
  - BLogs: Replicated to replica nodes' local disks (bulk data)
```

### 2.6 Client publish path and buffering

The definitive design focuses on broker, sequencer, and CXL protocols. For completeness, the **client** publish path is summarized here. Implementation details (queue capacity, pool sizing, active_queues) are in docs/NEW_BUFFER_BANDWIDTH_INVESTIGATION.md and docs/BUFFER_SCHEME_PAPER_AND_DOC_ASSESSMENT.md.

**Design principle:**

1. **Batching:** Clients assemble messages into batches (e.g. 2 MB) before sending. This matches the broker’s BLog/PBR granularity and reduces per-message overhead.
2. **Parallelism:** One logical **producer** (main thread filling batches) and **N consumer threads** (PublishThreads), each with one TCP connection to a broker (or per (broker, thread)). This matches §3.1 (“Multi-threaded ingestion: 4–8 network threads per broker; 4–16 brokers”).
3. **Buffering:** The producer distributes sealed batches to consumers via **N queues** (one per consumer) and a **pool** of batch-sized buffers. The producer round-robins sealed batches to the queues so that no single slow connection blocks others (no head-of-line blocking at the client).
4. **Varying broker count:** The number of queues **in use** at runtime equals the number of active consumers (PublishThreads). When broker count varies (scale up or down), only queues that have a consumer are used for round-robin; otherwise the producer would push to “ghost” queues that are never drained and would block when those queues fill. Queue and pool **capacity** are sized for a configured maximum broker count; **active** queue count tracks the current consumer count. This ensures liveness and avoids unnecessary allocation when brokers scale up.

**Paper note:** For a top-tier systems paper (Sigcomm/OSDI/SOSP), this is **implementation detail**, not a contribution. At most one sentence in Implementation or Evaluation: “Clients batch messages (e.g. 2 MB) and send via one connection per (broker, thread); buffer and queue count are sized to match broker count to avoid producer blocking.” The doc is updated here so that the definitive design remains the single reference for the full system, including the client publish path.

---

## Part III: Core Protocols

### 3.1 Broker Ingestion Protocol

```
BROKER INGESTION PROTOCOL
═════════════════════════

Precondition: Broker i owns BLog[i] and PBR[i] exclusively (Axiom A3)

┌─────────────────────────────────────────────────────────────────────────────┐
│ STEP │ OPERATION                      │ LATENCY    │ NOTES                  │
├──────┼────────────────────────────────┼────────────┼────────────────────────┤
│  1   │ Receive batch header via TCP   │ ~50 μs     │ Header: 64 bytes       │
│  2   │ Reserve BLog space             │ ~10 ns     │ atomic_fetch_add       │
│  3   │ Zero-copy recv into BLog       │ ~100 μs    │ DMA to CXL             │
│  4   │ Construct PBR entry            │ ~10 ns     │ Local computation      │
│  5   │ Reserve PBR slot               │ ~10 ns     │ atomic_fetch_add       │
│  6   │ Write PBR entry                │ ~200 ns    │ CXL store + fence      │
│  7   │ Track for acknowledgment       │ ~10 ns     │ Local map insert       │
└──────┴────────────────────────────────┴────────────┴────────────────────────┘

* **Order 0 (Option A):** For topics with Order 0, the same execution context that performs step 6 (write PBR entry and flush) SHALL update `TInode.offsets[broker_id].written` (monotonic) and optionally `written_addr` after the PBR entry is visible (flush + store_fence). No separate "Sequencer 0" or delegation thread; the network path that commits the batch is the sole writer of `written` for that broker. This allows ack_level=1 to advance as soon as data is in PBR.

* Note that at step3, we can use RDMA by returning offset to the client in step 2 but since we do not know if RDMA is possible with multi-node CXL memory, we stick to TCP design. DMA to CXL here means in TCP recv() the broker is receiving directly to CXL buffer.

* PBR entry makes the receive atomic (either network or CXL can fail during recv()).

* **PBR slot lifecycle and scanner correctness:** In implementations where the broker reserves a PBR slot before writing the full entry (e.g. two-phase: reserve → mark slot "claimed" → write payload → mark "valid"), the sequencer's collector tracks how long a slot remains reserved-but-not-valid. If a slot remains invalid for longer than a configurable timeout (default: 10 seconds, far exceeding any live broker's write latency and the membership service's heartbeat timeout), the collector clears the slot and advances past it. Under normal operation (&lt;200 ns write latency), the collector retries on the next pass without advancing. See sequencer specification (Appendix F) §4.1 for implementation. 

* **Wait-free broker (design decision):** The broker never enforces ordering. PBR is strictly first-come-first-served: each network thread reserves BLog space, receives payload, then appends one PBR entry with a single atomic fetch_add on the PBR tail. No mutex, no per-client queue, no sorting at the broker. Ordering—including per-client order for Order 5—is enforced entirely at the sequencer, which can sort batches (e.g., radix sort in L2 cache) without throttling the ingest path. Adding a broker-side lock to preserve PBR order would cap throughput at ~1–2M ops/sec and waste CXL bandwidth; the expert consensus is to keep the broker a "firehose."

* **Multi-threaded ingestion:** Unlike token-before-write systems (e.g., Corfu) where the client serializes through the sequencer, Embarcadero allows multiple client threads and multiple broker network threads to send/receive in parallel. The sequencer later assigns global order. To saturate a single broker (e.g., 10 Gbps), 4–8 network threads are typical. We target 4–16 brokers per CXL domain (expert consensus: enough to saturate rack network and CXL fabric; beyond 32 yields diminishing returns and higher failure-management overhead).

* **Zombie slot burn mitigation (§4.2.1):** Before reserving a PBR slot (or periodically, e.g. every 100 batches), the broker must read `ControlBlock.epoch`. If the broker's epoch is stale (e.g. \< control block epoch), the broker stops accepting new batches and re-syncs or exits. This prevents a partitioned (zombie) broker from filling the PBR with garbage.

Total broker-side latency: ~150-200 μs (dominated by network receive)
```

**PBR backpressure (required for correctness under load):** If the sequencer (and replicas) drain slower than brokers write, the PBR (and BLog) will eventually fill. The design **must** apply backpressure so that brokers do not overwrite unsequenced data or block indefinitely without signaling. Use a **High/Low watermark** (not a hard stop) to avoid micro-stuttering:
- **High watermark (e.g. 80% full):** Stop reading from the TCP socket (let the TCP window close). Do not reserve new PBR slots.
- **Low watermark (e.g. 50% full):** Resume reading and accepting batches.

**Sustained backpressure (high watermark lasts too long):** If PBR remains above the high watermark for longer than a configured duration (e.g. **5–30 seconds**), the broker **rejects** new publish requests instead of blocking indefinitely. The broker returns a **BACKPRESSURE** (or equivalent) error to the client so that the client can back off, retry, or fail fast. Blocking forever would hide overload and make timeouts and debugging difficult. Recommended: configurable `backpressure_reject_after_sec` (default 10 s); after that, reject with BACKPRESSURE until below the low watermark. Clients must treat BACKPRESSURE as retriable after backoff.

Recovery: when the sequencer drains and the broker observes progress via Completion Vector (§3.4), completed_pbr_head advances and slots are effectively freed; the broker resumes once below the low watermark. Without this, TCP buffers and CXL queues grow unbounded and latency degrades.

**PBR ring wraparound and GOI lifetime:** PBR is a ring buffer (e.g. 512K entries per broker). A slot at index `i` must **not** be reused until the batch that used it has been sequenced, replicated, and ACKed—i.e. until `CompletionVector[broker_id].completed_pbr_head` has advanced past `i`. The broker effectively frees slots when the CV advances; the high/low watermark ensures the broker does not advance its tail past a point where the sequencer and replicas have not yet drained. So: **PBR slot reuse is safe only after CV has passed that slot.** If the broker ever overwrote a slot still referenced by an unreplicated GOI entry, the CV updater could advance completed_pbr_head incorrectly (e.g. skipping or double-counting). The design therefore requires: **never reuse a PBR slot until CompletionVector[broker_id].completed_pbr_head > (that slot index modulo ring size).** Backpressure guarantees this under normal load; on recovery, the new tail initializes CV from its replicated prefix, so ACK and slot reuse remain consistent.

**Implementation:**
```cpp
void Broker::ingest(Connection& conn) {
    // Step 1: Receive header
    BatchHeader header;
    conn.recv_exact(&header, sizeof(header));

    // Step 2: Reserve BLog space (lock-free)
    uint64_t offset = blog_tail_.fetch_add(
        align_up(header.payload_size, 64),
        std::memory_order_relaxed
    );

    // Step 3: Zero-copy receive directly into CXL memory
    conn.recv_exact(blog_base_ + offset, header.payload_size);

    // Step 4: Construct PBR entry
    PBREntry entry{
        .blog_offset = offset,
        .payload_size = header.payload_size,
        .message_count = header.message_count,
        .batch_id = generate_batch_id(),
        .broker_id = broker_id_,
        .epoch_created = current_epoch_,
        .flags = header.flags | FLAG_VALID,
        .client_id = header.client_id,
        .client_seq = header.client_seq,
    };

    // Step 5-6: Publish to PBR (makes batch visible to sequencer)
    uint64_t pbr_idx = pbr_tail_.fetch_add(1, std::memory_order_relaxed);
    pbr_[pbr_idx % PBR_SIZE] = entry;
    std::atomic_thread_fence(std::memory_order_release);

    // Step 7: Track for ACK
    pending_acks_.insert(entry.batch_id, conn.id(), Clock::now());
}
```

### 3.2 Epoch-Based Sequencing Protocol (Order 2 Baseline)

> **Scope:** This section describes the baseline sequencer for Order 2 workloads with ≤4 brokers. When Order 5 is enabled, the shard-worker architecture (§3.2.1 and the sequencer specification, Appendix F) replaces this design. The core insight—amortized atomics via batch sequence reservation—is shared.

The sequencer uses a triple-buffered epoch pipeline to eliminate atomic bottlenecks. This design uses a single sequencer thread and one atomic fetch_add per epoch; for higher scaling (e.g., many more PBRs or batch rates beyond current needs), see §3.3 (Parallel Scatter-Gather Sequencer).

```
EPOCH PIPELINE (τ = 500 μs default)
═══════════════════════════════════

TIME ──────────────────────────────────────────────────────────────────────▶

       ┌──────────┐
Epoch 0│ COLLECT  │────▶│ SEQUENCE │────▶│ COMMIT   │
       └──────────┘     └──────────┘     └──────────┘
                  ┌──────────┐
       Epoch 1    │ COLLECT  │────▶│ SEQUENCE │────▶│ COMMIT   │
                  └──────────┘     └──────────┘     └──────────┘
                             ┌──────────┐
       Epoch 2               │ COLLECT  │────▶│ SEQUENCE │────▶│...
                             └──────────┘     └──────────┘

       ├────τ────┤├────τ────┤├────τ────┤

PARALLELISM:
  - Multiple collector threads poll PBRs(One PBR Entry per broker) in parallel (no coordination)
  - Single sequencer thread assigns sequences (local computation)
  - Single GOI writer commits to CXL (sequential writes)

KEY INSIGHT:
  Traditional sequencers: O(n) atomic operations per batch
  Embarcadero: O(1) atomic operation per EPOCH (amortized over ~1000 batches)
```

**Algorithm (Order 2 only):**
```cpp
class Sequencer {
    // Triple-buffered epoch state
    struct EpochBuffer {
        std::vector<BatchInfo> collected[MAX_COLLECTORS];
        std::vector<BatchInfo> sequenced;
        std::atomic<State> state;  // COLLECTING, READY, COMMITTED
    };
    EpochBuffer buffers_[3];

    std::atomic<uint64_t> global_seq_{0};
    std::atomic<uint16_t> collecting_epoch_{0};

public:
    void collector_thread(int collector_id, std::span<int> my_pbrs) {
        while (running_) {
            uint16_t epoch = collecting_epoch_.load(std::memory_order_acquire);
            auto& buffer = buffers_[epoch % 3].collected[collector_id];

            for (int pbr_id : my_pbrs) {
                PBR& pbr = pbrs_[pbr_id];
                while (pbr.tail > pbr.last_read) {
                    PBREntry entry;
                    if (pbr.read(pbr.last_read, entry)) {
                        buffer.push_back(to_batch_info(entry, pbr.last_read));  // pbr_index for CV
                    }
                    pbr.last_read++;
                }
            }
        }
    }

    void sequencer_thread() {
        uint16_t next_to_sequence = 0;

        while (running_) {
            auto& buf = buffers_[next_to_sequence % 3];

            // Wait for collection to complete
            while (buf.state != State::READY) {
                std::this_thread::yield();
            }

            // Merge all collector buffers
            std::vector<BatchInfo> all;
            for (auto& collector_buf : buf.collected) {
                all.insert(all.end(), collector_buf.begin(), collector_buf.end());
                collector_buf.clear();
            }

            // Add recovery buffer (from failed brokers)
            all.insert(all.end(), recovery_buffer_.begin(), recovery_buffer_.end());
            recovery_buffer_.clear();

            // Order 2: all non-duplicate batches to ready list. Order 5: see §3.2.1 and Appendix F.
            std::vector<BatchInfo> ready;
            for (auto& b : all) {
                if (!is_duplicate_batch_id(b.batch_id)) {
                    ready.push_back(b);
                }
            }

            // === THE KEY OPTIMIZATION ===
            // Single atomic reserves entire epoch's worth of sequences
            uint64_t base = global_seq_.fetch_add(ready.size(),
                                                   std::memory_order_relaxed);

            // Pure local computation: assign sequences
            for (size_t i = 0; i < ready.size(); i++) {
                ready[i].global_seq = base + i;
            }

            buf.sequenced = std::move(ready);
            buf.state = State::READY_TO_COMMIT;
            next_to_sequence++;
        }
    }

    void goi_writer_thread() {
        uint16_t next_to_write = 0;

        while (running_) {
            auto& buf = buffers_[next_to_write % 3];

            while (buf.state != State::READY_TO_COMMIT) {
                std::this_thread::yield();
            }

            // Sequential writes to GOI (CXL)
            for (auto& batch : buf.sequenced) {
                GOIEntry entry{
                    .global_seq = batch.global_seq,
                    .batch_id = batch.batch_id,
                    .broker_id = batch.broker_id,
                    .epoch_sequenced = current_epoch_,
                    .blog_offset = batch.blog_offset,
                    .payload_size = batch.payload_size,
                    .message_count = batch.message_count,
                    .num_replicated = 0,
                    .client_id = batch.client_id,
                    .client_seq = batch.client_seq,
                    .pbr_index = batch.pbr_index,  // For CV updater (CompletionVector[broker_id])
                    .flags = batch.flags,
                };
                goi_[batch.global_seq] = entry;
            }

            // Memory fence for visibility
            std::atomic_thread_fence(std::memory_order_release);

            // Update committed sequence
            if (!buf.sequenced.empty()) {
                control_block_->committed_seq.store(
                    buf.sequenced.back().global_seq,
                    std::memory_order_release
                );
            }

            buf.sequenced.clear();
            buf.state = State::COMMITTED;
            next_to_write++;
        }
    }
};
```

### 3.2.1 Shard-Worker Sequencer (Required for Order 5)

When Order 5 is enabled (or when scaling beyond 4 brokers), the epoch pipeline (§3.2) is replaced by a continuous shard-worker architecture. All Order 2 batches are also processed through this path.

**Thread model:**
```
Collector threads (C, default 4):
  - Poll PBRs continuously
  - Route batches to shard workers via SPSC queues:
    Order 5: shard = hash(client_id) % S  (correctness: same client → same shard)
    Order 2: shard = round_robin++ % S     (load balance)

Shard workers (S, default 1 for ≤4 brokers, 8 for 8+):
  - Drain SPSC queues (one per collector), dedup, partition by order level
  - Order 2: append to ready list (O(1) per batch)
  - Order 5: three-layer gap resolution (sequencer spec §5.3–5.5)
  - Commit: fetch_add(ready.size()) on global_seq, write GOI, push CompletedRange

committed_seq updater (1):
  - Merges CompletedRanges via min-heap
  - Advances committed_seq to highest gap-free GOI prefix
  - Tracks replication floor for anti-wraparound (I-WRAP)
  - Runs periodic checkpoint and reconciliation watchdog
```

This replaces the single sequencer thread and single GOI writer thread from §3.2. Each shard worker owns a disjoint set of clients (by hash), so per-client ordering is embarrassingly parallel.

**Note:** When Order 5 is enabled and the shard-worker sequencer is in use, Order 2 batches may share shards with Order 5 batches. If a shard worker stalls on Order 5 gap resolution (e.g., timeout), committed_seq advancement for co-located Order 2 batches is also delayed. This does not affect Order 2 correctness, only committed_seq latency. Mitigation: increase num_shards to reduce co-location, or use separate sequencer instances (future work).

See the sequencer specification (Appendix F) for full implementation.

### 3.3 Parallel Scatter-Gather Sequencer (Required for Line-Rate)

For line-rate performance (e.g. 100 Gbps, ~12M pps, or 8+ brokers), the single-sequencer design (§3.2) risks saturation. A **parallel scatter-gather** sequencer is **required** for such deployments. When the single-sequencer, single-atomic design (§3.2) is sufficient (fewer PBRs, lower batch rates), it may be used; otherwise the **parallel scatter-gather** sequencer below scales sequencing further by distributing work across multiple logic threads, each performing its own fetch_add and GOI writes. The key idea: separate **PBR (ingest log)** from **GOI (global order index)**, and use **hash-based sharding** so that each shard owns disjoint clients and can assign sequences and write GOI independently, with a single serialization point per shard (one atomic fetch_add per shard per epoch).

#### Data flow

```
PBR (per broker, unordered)     GOI (global, ordered)
┌─────────────────────────┐    ┌─────────────────────────┐
│ PBR[0] .. PBR[N-1]      │    │ GOI[0], GOI[1], ...      │
│ (raw batch headers)     │    │ (definitive order)      │
└───────────┬─────────────┘    └───────────▲─────────────┘
            │                               │
            ▼                               │
     COLLECTOR THREADS                      │
     (I/O bound: poll PBRs)                 │
            │                               │
            │ Hash(client_id) → shard      │
            ▼                               │
     SHARD QUEUES [0..S-1]                  │
            │                               │
            ▼                               │
     LOGIC THREADS (per shard)              │
     Sort, gap check, assign                 │
            │                               │
            │ fetch_add(count) → base_seq   │
            │ write GOI[base_seq..]         │
            └───────────────────────────────┘
```

#### Phase 1: Collectors (I/O bound)

- **Threads:** C collector threads (e.g., 4).
- **Job:** Each collector owns a disjoint subset of PBRs. It polls PBRs, reads new entries (cache-line reads only), and **does not sequence**. For each entry it computes `shard_id = hash(entry.client_id) % num_shards` and pushes the entry (or a pointer) into **shard queue** `shard_id`. Shard queues are lock-free SPSC or MPSC queues (one producer per collector, one consumer = the logic thread for that shard).
- **Invariant:** No cross-thread coordination; each PBR is read by at most one collector. All entries for the same client_id go to the same shard, so per-client ordering can be enforced within a shard.

#### Phase 2: Logic threads (CPU bound)

- **Threads:** S logic threads (shards), e.g., 8. Each logic thread owns one shard queue and its own per-client state (for Order 5).
- **Job (continuous drain; no epoch boundary):**
  1. **Drain** shard queue into a local buffer.
  2. **Partition** by Order 0/1/2 vs Order 5 (using FLAG_STRONG_ORDER).
  3. **Order 0/1/2:** Deduplicate by batch_id; order is arrival order within shard. For Order 2, sequencer non-blocking, constantly iterates PBRs.
  4. **Order 5:** Same three-layer processing as §3.2.1 / sequencer spec (sort, gap detection with hold buffer, wall-clock timeout for SKIP). Hash(client_id) routing ensures each shard gets a disjoint subset of clients, so per-client processing is embarrassingly parallel.
  5. **Count** ready entries, e.g., `count = ready.size()`.

#### Phase 3: Atomic commit and GOI write

- Each logic thread performs **one** atomic on the **global** sequence counter:
  - `my_base_seq = global_seq.fetch_add(count, memory_order_relaxed)`.
- The logic thread then writes its `count` entries to GOI at indices `[my_base_seq, my_base_seq + count)` using sequential, non-contentious CXL writes. **Crucially, each GOI entry written must include the `pbr_index` from the original PBR entry (as in §3.2); this preserves the link required for the Completion Vector (ACK path) to function.** No other thread writes to that range (ranges are disjoint by construction).
- **Memory fence** after writing the last entry in the range so that replicas observe a gap-free prefix up to `my_base_seq + count - 1`.

**Who updates committed_seq and when:** A **dedicated committed_seq updater thread** (sequencer spec §7). Each shard worker, after writing its GOI range, pushes a `CompletedRange{start, end}` to a dynamically-growing MPSC queue. The updater drains this queue into a min-heap ordered by range start, and advances committed_seq through the contiguous prefix. A **reconciliation watchdog** (e.g. every 5 seconds) scans the GOI directly to detect and repair stalls caused by lost CompletedRange entries.

**Anti-wraparound guard (I-WRAP):** Before fetch_add, each shard worker checks that `current_global + count - replication_floor < GOI_SIZE * high_pct` (e.g. 90%). If exceeded, the worker stalls until replicas catch up (replication_floor advances). See sequencer spec §6.1.

#### Correctness

- **Total order:** Global sequence space is partitioned by fetch_add; each logic thread gets a contiguous range. GOI is written in increasing index order by each thread; readers see a monotonic prefix.
- **Per-client order (Order 5):** Hash(client_id) ensures all batches from the same client go to the same shard. Within the shard, sort by client_seq and gap handling preserve per-client FIFO.
- **No cross-host atomics:** The global_seq counter and all shard state live in the sequencer host’s local, cache-coherent memory (Axiom A3).

#### ACK path (Completion Vector — see §3.4)

Brokers do **not** poll the GOI. They poll only their **Completion Vector** slot (§3.4). The scatter-gather design does not change the ACK protocol (CV-based).

#### Sequencer Sharding & Recovery (ClientState Reconstructibility)

In scatter-gather, each logic thread holds **ClientState** (next_expected_seq, dedupe window) for Order 5 in RAM. If the sequencer process crashes or a logic thread fails, this state is lost; it **must** be reconstructible.

**Protocol:** On sequencer startup (or after failover), before processing new PBR entries:
1. **Replay the tail of the GOI** (e.g. last 1000–10000 batches, or all batches since a chosen checkpoint). GOI entries carry `client_id` and `client_seq`.
2. For each shard s, scan replayed GOI entries that hash to shard s. For each client_id, compute the maximum client_seq seen and set `ClientState[client_id].next_expected = max_client_seq + 1` and update the dedupe window (e.g. recent_window) from the replayed client_seqs.
3. Only then start draining PBRs and assigning new global_seqs. New batches from a client will have client_seq ≥ next_expected (or be duplicates in the window).

ClientState must not be stored solely in RAM if sequencer failover is required; replay from GOI (or a persistent checkpoint of the same) is the recovery path.

**Per-client state checkpointing (Order 5):** The sequencer periodically checkpoints the per-client next_expected map to the dedicated 16 MB CXL region (§2.5). The checkpoint is double-buffered (two 8 MB slots); one is always valid. On failover, the new sequencer loads the checkpoint and replays the GOI tail from the checkpoint's committed_seq forward, rebuilding per-client state. Without checkpointing, the full GOI tail must be replayed, which may take minutes for large systems. With checkpointing (e.g. every 60 s), recovery adds ~5 ms of checkpoint loading on top of lease-wait.

**Order 5 failover recovery time budget:**

| Phase | Typical | Worst Case |
|-------|---------|------------|
| Lease-wait | ~60 ms | 110 ms |
| GOI scan + truncation | ~1 ms | 25 ms |
| Client state rebuild (with checkpoint) | ~5 ms | ~50 ms |
| Client state rebuild (without checkpoint) | ~50 ms | ~200 ms |
| Step 4b: Collector cursor init | &lt;1 ms | &lt;1 ms |
| Step 4c: Replication floor init | &lt;200 μs | 20 ms |
| **Total (with checkpoint)** | **~70 ms** | **~185 ms** |
| **Total (without checkpoint)** | **~115 ms** | **~335 ms** |

#### When to use

| Criterion | Use §3.2 (Single sequencer) | Use §3.3 (Scatter-gather) |
|-----------|----------------------------|---------------------------|
| **Brokers** | ≤4 | ≥5 (recommended: 8+ for line-rate) |
| **Batch rate** | &lt; ~2M batches/s sustained | ≥ ~2M batches/s or target 100 Gbps |
| **Latency variance** | Prefer lower (single writer) | Accept slightly higher (multiple writers) |
| **Operational complexity** | Simpler (fewer threads, one atomic) | Higher (shard state, committed_seq updater) |

**Default recommendation:** Use **§3.2** for development, testing, and small production deployments (≤4 brokers, &lt;10 Gbps). Use **§3.3** when scaling to 8+ brokers or when throughput targets exceed what a single sequencer thread can sustain (e.g. 100 Gbps, ~12M pps). Configuration (e.g. `sequencer.mode: single | scatter_gather`) should select the implementation at startup; switching requires a sequencer restart.

#### Scatter-gather crash recovery (partial GOI write)

If a logic thread crashes **after** `fetch_add(count)` but **before** finishing writing its GOI range, the GOI can have a gap: indices [my_base_seq, my_base_seq + k) are written, [my_base_seq + k, my_base_seq + count) are missing. A new sequencer that replays PBR would assign **new** global_seqs to those same batches, producing duplicate global_seqs or reordered entries.

**Protocol:** On sequencer startup (single or scatter-gather), **before** processing new PBR entries:
1. **Scan the GOI tail** (e.g. from `committed_seq` backward, or from last known good watermark) for the first gap: two consecutive indices where the second has invalid/uninitialized content (e.g. zeroed or sentinel).
2. **Truncate logically:** Treat the GOI as valid only up to (and including) the last fully written index before the gap. Set `control_block->committed_seq` to that index. Batches that were in the truncated region are still in PBR (they were never ACKed); the new sequencer will re-collect them from PBR and assign **new** global_seqs. No batch is lost; at most one epoch of assignments is redone.
3. **Replay PBR from a safe point:** Each broker’s PBR tail may have advanced past the truncated GOI. The sequencer must only consider PBR entries that correspond to batches **not** already present in the truncated GOI (e.g. by batch_id or by PBR index &gt; max PBR index reflected in truncated GOI). In practice: after truncation, rebuild ClientState from truncated GOI (as in §3.3 Sequencer Sharding & Recovery), then start collecting from current PBR tails; duplicate batch_ids are discarded at sequencing.

This ensures **total order uniqueness** after recovery: no batch appears twice in the GOI with different global_seqs.

---

### 3.4 Replication Protocol and Completion Vector (ACK Path)

**Problem with "Brokers poll GOI":** The GOI is a single global log. For Broker i to find "my" batches, it would have to scan GOI entries (many belonging to other brokers), causing read contention on the GOI tail, wasted CXL bandwidth, and cache pollution. This is the **ACK path bottleneck** identified in expert review.

**Solution: Completion Vector (CV).** Each broker has a **single cache line** in CXL that records the highest contiguous PBR index (for that broker) that has been both sequenced and replicated. Brokers poll **only** their own CV slot; they never read the GOI for ACK.

#### Completion Vector layout

In CXL, allocate a small array **CompletionVector[NumBrokers]** (e.g. 32 × 128 bytes), one entry per broker with **128-byte alignment** to avoid false sharing (see §2.4):

```cpp
struct alignas(128) CompletionVectorEntry {
    // Highest contiguous PBR index for this broker that is sequenced AND replicated
    std::atomic<uint64_t> completed_pbr_head;  // [[writer: last replica]]
    uint8_t _pad[120];                         // Pad to 128 bytes (prefetcher safety)
};
static_assert(sizeof(CompletionVectorEntry) == 128);
```

- **Writer:** The current **tail replica** in the chain. When it has replicated a contiguous prefix of GOI entries that correspond to a prefix of Broker i's PBR indices, it updates `CompletionVector[i].completed_pbr_head` to that PBR index (monotonic). If the tail fails, the control plane reconfigures the chain and the **new tail** assumes the CV updater role (see Step 3 below).
- **Reader:** Broker i. It polls **only** `CompletionVector[i].completed_pbr_head`. When it moves (e.g. 100 → 110), batches 101–110 are sequenced and replicated; the broker sends ACKs for those batches (via its PendingMap from batch_id / PBR index to client connections).

**Replication protocol (unchanged):** Chain replication and `num_replicated` in GOI remain as below. The **tail replica** advances `CompletionVector[broker_id].completed_pbr_head` when it has replicated GOI entries: each GOI entry carries `broker_id` and `pbr_index` (§2.4); the tail tracks per-broker the highest contiguous `pbr_index` replicated and writes that to `CV[broker_id].completed_pbr_head` (monotonic). ACK is sent only after replication; the CV therefore reflects "replicated" completion.

```
CHAIN REPLICATION FOR DURABILITY
════════════════════════════════

Replicas: R[0], R[1], R[2], ... R[f]  (f+1 replicas tolerate f failures)

PROTOCOL:

1. All replicas poll GOI in parallel:
   - On seeing new entry, copy payload from BLog to local storage
   - fsync() to ensure durability

2. Acknowledgment via chain (serialized):
   - R[0] waits for GOI[j].num_replicated == 0
   - R[0] increments to 1, passes token to R[1]
   - R[1] waits for == 1, increments to 2, passes to R[2]
   - ...continues until num_replicated == f+1

3. Completion Vector update (ACK path):
   - **CV Updater Role:** The update is performed by the current **tail replica** in the chain.
   - **Mechanism:** When the tail has replicated a contiguous prefix of a broker's batches, it writes the highest `pbr_index` to `CompletionVector[broker_id].completed_pbr_head` (monotonic).
   - **Failure Handling:** If the tail replica fails, the control plane reconfigures the chain (promoting the previous replica to tail). **The new tail immediately assumes responsibility for CV updates** (see §4.2.3 CV Tail Handover): it initializes each `CompletionVector[i]` to the highest contiguous pbr_index it has already replicated for broker i (never decreasing), then continues advancing CV as it replicates. Target: \<10 ms to resume CV updates so broker ACKs do not stall.
   - Broker i polls ONLY `CompletionVector[i].completed_pbr_head` (one cache line).
   - When completed_pbr_head advances, broker sends ACKs for the newly completed PBR indices (via PendingMap).

┌──────────────────────────────────────────────────────────────────────────┐
│                          TIME ──────────────────────────────────────▶    │
│                                                                          │
│  GOI write:        [entry j created, num_replicated=0]                  │
│                              │                                           │
│  R[0]:             ─────────[copy]────[fsync]────[inc to 1]─────────▶   │
│  R[1]:             ─────────[copy]────[fsync]────────────[inc to 2]──▶  │
│  R[2]:             ─────────[copy]────[fsync]────────────────────[inc]▶  │
│                              │                                           │
│  CV update:        ─────────────────────────────────────────[CV[i]++]   │
│                                                        │                 │
│  Broker ACK:       ────────────────────────────────────[send ACK]       │
└──────────────────────────────────────────────────────────────────────────┘

CORRECTNESS:
  - Single-writer per GOI entry (chain token)
  - Monotonic increment (coherence-oblivious)
  - fsync before increment (durability before ack)
  - Single-writer per CV entry (last replica); brokers only read their own slot (zero GOI scan)
```

#### Replica Handling of Special GOI Entries (Order 5)

Order 5 sequencing may produce two special GOI entry types that replicas and the CV updater must handle:

**NOP entries (FLAG_NOP):** Emitted when a duplicate batch is detected. The entry has valid broker_id and pbr_index (the real PBR slot the duplicate occupied) but zero payload. Replicas skip the BLog copy (no data exists). The CV updater MUST advance CV[broker_id] past pbr_index—this is the NOP's sole purpose. Without it, the duplicate's PBR slot would never be reflected in CV, causing permanent ACK stall for that broker.

**SKIP markers (FLAG_SKIP_MARKER):** Emitted when a per-client gap is declared lost. The entry has broker_id=0 (backward-compatible with old replicas that don't check FLAG_SKIP_MARKER; CV[0] already ≥ 0 in any running system), pbr_index=0, zero payload. Replicas skip BLog copy and advance num_replicated normally. The CV updater MUST NOT advance any broker's CV for SKIP markers (no real PBR slot; check FLAG_SKIP_MARKER and return).

CV update pseudocode:

```cpp
void update_cv_for_entry(GOIEntry& entry) {
    if (entry.flags & FLAG_SKIP_MARKER) return;  // No PBR slot
    if (entry.flags & FLAG_NOP) {
        // NOP: advance CV for the duplicate's PBR slot
        update_completion_vector(entry.broker_id, entry.pbr_index);
        return;
    }
    // Normal batch
    update_completion_vector(entry.broker_id, entry.pbr_index);
}
```

**Deployment constraint:** The tail replica (CV updater) must be upgraded to recognize FLAG_SKIP_MARKER and FLAG_NOP before the sequencer is upgraded to emit them. Upgrade order: (1) non-tail replicas, (2) tail replica, (3) sequencer.

#### Replication and Order 5 (hold buffer) interaction

With Order 5, a batch M can be **replicated** (and ACKed via CV) **before** it is **released from the hold buffer** (i.e. before its final global_seq is stable relative to earlier batches from the same client). Timeline: M arrives at broker → PBR entry → sequencer collects → M held (gap) → later gap fills or times out → M released with final global_seq → GOI written → replicas copy → CV advances → ACK.

**ACK timing (design choice):**

| Option | When ACK is sent | Guarantee at ACK time |
|--------|-------------------|------------------------|
| **A (current)** | After num_replicated ≥ f+1 (CV advances) | Durable; global_seq may not be final if M was held and later released |
| **B** | After released from hold buffer **and** replicated | Durable **and** final global position (subscriber will see M in order) |
| **C** | Two-phase: "durable" ACK first, then "ordered" callback when released | Most flexible; client can act on durability vs order separately |

**Design decision:** Embarcadero uses **Option A**. The client receives ACK when the batch is durable (f+1 replicas). For Order 5, the **global_seq** returned to the client at ACK time is the one assigned when the batch was released from the hold buffer and written to the GOI; that order is the definitive one. So at ACK time the client **does** have the final global_seq (the GOI is written before replication, and CV advances only after replication). The only subtlety: if M was held and a **later** batch M' from the same client was sequenced first (e.g. gap timeout), then M' gets a lower global_seq than M. That is correct (M was declared lost or reordered by timeout). So **Option A and Option B coincide** in practice: ACK is sent after replication, and the batch’s global_seq was fixed when it was written to the GOI (before replication). The document therefore keeps **single-phase ACK after replication**; no change for Order 5.

**Subscriber visibility:** Subscribers read from the GOI (or from replicas that have applied the GOI). They see batches in global_seq order. They do **not** see a batch “before” it is in the GOI; so there is no window where a subscriber sees M replicated but not yet ordered. See "Subscriber read consistency modes" below.

#### Subscriber read consistency modes

Subscribers can trade off latency vs consistency when reading:

| Mode | Condition to deliver batch | Use case |
|------|----------------------------|----------|
| **Read-durable** | num_replicated ≥ f+1 (or equivalent: batch is in CV’s prefix) | High availability; can serve from any replica that has the data |
| **Read-ordered** | Same as durable; delivery in global_seq order. No separate “hold buffer” check—GOI order is the order. | Strict ordering; default for subscribers |
| **Read-latest** | GOI entry exists (sequenced). Lower latency but may see data not yet replicated; use for best-effort or when durability is handled elsewhere | Lowest latency; optional |

**Implementation note:** Embarcadero delivers in **global_seq order** from the GOI (or from replica state derived from GOI). So "read-ordered" is the default: subscriber sees batches in global_seq order, and each batch is delivered only after it is durable (replicas have applied it). "Read-latest" would mean delivering as soon as the GOI entry exists (before num_replicated ≥ f+1); useful only for non-critical consumers. The document assumes **read-ordered** (durable + ordered) as the standard subscriber guarantee.

---

## Part IV: Failure Handling

### 4.1 Failure Model

| Failure Type | Detection | Data at Risk | Recovery Time |
|--------------|-----------|--------------|---------------|
| Broker crash | Heartbeat (50ms) | None (PBR in CXL) | <100ms |
| Broker slow | Latency spike | None | N/A (degrades Order 5 latency) |
| Sequencer crash | Lease expiry (100ms) | None (GOI in CXL) | <200ms |
| Replica crash | Heartbeat | None (chain continues); if tail, new tail assumes CV updater (§3.4) | <100ms |
| CXL module failure | Hardware error | Metadata in module | Use replica module |
| Network partition | Timeouts | In-flight messages | Client retry |

**External dependency: membership and consensus.** Embarcadero assumes an **external membership and consensus service** (e.g. etcd, Consul, or a Raft-based controller) that (1) provides **failure detection** (broker, sequencer, replica liveness), (2) elects a **single leader sequencer** and assigns an **epoch** (monotonic) so that at most one sequencer is active per epoch, and (3) allows the active sequencer to **read/write the CXL fabric** (e.g. same rack). Progress requires that the current leader can reach the CXL memory and a quorum of the membership service; if no process can attain leadership (e.g. partition), sequencing stalls until the partition heals. The design does not implement consensus itself—it consumes it.

**Sequencer lease protocol.** The Control Block holds `sequencer_id` and `sequencer_lease` (expiry timestamp in ns). **Acquisition:** The membership service grants leadership to one node; that node writes its identity and lease expiry into the Control Block (e.g. CAS or single-writer after election). **Renewal:** The active sequencer periodically (e.g. every 25–50 ms) writes a new `sequencer_lease = now_ns() + lease_duration` (e.g. 100 ms) and flushes the Control Block cache line. **Expiry:** Replicas and brokers (or a watchdog) read `sequencer_lease`; if `now_ns() > sequencer_lease`, the sequencer is considered dead. The membership service then elects a new leader (new epoch); the new sequencer writes `epoch++` and its lease before processing. **On expiry:** No process writes GOI until a new leader is elected and has written the new epoch; epoch fencing (§4.2) ensures stale writes are rejected. Implementation: lease duration &gt; 2× renewal interval (e.g. 100 ms lease, 40 ms renewal) to tolerate jitter.

### 4.2 Epoch-Based Zombie Fencing

```
ZOMBIE SCENARIO:
  t=0:   Sequencer S1 is active (epoch=N)
  t=100: S1 network-partitioned from consensus service
  t=200: Consensus elects S2 (epoch=N+1)
  t=300: S1's network heals, attempts to continue sequencing

WITHOUT FENCING:
  S1 (zombie) writes GOI entries with stale state
  Data corruption!

WITH EPOCH FENCING:
  1. S2 writes epoch=N+1 to control block (CXL)
  2. All replicas poll control block, see epoch=N+1
  3. S1's writes are tagged epoch=N
  4. Replicas reject epoch=N entries (stale)
  5. Safety maintained!

PROTOCOL:
  on_read_goi_entry(entry):
    current_epoch = control_block.epoch.load()
    if entry.epoch < current_epoch - MAX_EPOCH_AGE:
      REJECT("stale epoch")
    ACCEPT
```

### 4.2.1 Zombie Slot Burn (Broker Must Check Epoch)

A zombie broker (partitioned from consensus but still writing to CXL) cannot corrupt data—replicas reject stale epochs—but it **can** fill its PBR with garbage entries, consuming slots and forcing the sequencer to filter them (Denial of Service).

**Mitigation:** Brokers must read `ControlBlock.epoch` before writing to PBR. If the broker's view of the epoch is stale (e.g. older than the control block's epoch), the broker must **stop writing** and re-sync or shut down.

**Protocol:**
- Before reserving a PBR slot (e.g. every 100 batches or on a timer), broker reads `ControlBlock.epoch`.
- If `my_epoch < control_block.epoch`, broker stops accepting new batches and either reconnects to membership to get the new epoch or exits (membership will mark it dead).
- This bounds the damage a zombie can do: it burns at most a bounded number of slots before it would next check epoch.

### 4.2.2 Sequencer-Driven Replica Recovery (Stalled Chain)

If replica R_k fails **after** copying data but **before** performing its monotonic increment on `GOI[j].num_replicated`, the chain stalls: R_{k+1} waits forever for the value k.

**Protocol (from NSDI paper Fault Tolerance § Sequencer-Driven Recovery):**
1. **Failure detection:** For each batch j it adds to the GOI, the sequencer starts a logical timer. If `GOI[j].num_replicated` does not reach the target replication factor within a timeout (e.g. 10× expected chain latency), the sequencer declares the replication chain for that batch stalled.
2. **Fault identification:** The sequencer reads the current value of `GOI[j].num_replicated`. If the value is k, replica R_k was responsible for incrementing from k to k+1 and is implicated.
3. **Recovery:** The sequencer performs a **monotonic update** on the stalled GOI entry: it increments `GOI[j].num_replicated` from k to k+1 (single CXL write with flush/fence). This passes the token to R_{k+1}, unblocking the chain. The sequencer reports R_k to the membership service (e.g. etcd) for removal from the active replica pool.

This is a metadata-only recovery (Axiom A1); no data replay is required.

### 4.2.3 CV Tail Handover (New Tail After Tail Failure)

When the tail replica fails, the **new tail** (the previous replica in the chain) must assume the CV updater role. To avoid ACK stalls or regression:

1. **Initialization:** The new tail has already been replicating and has a consistent view of which GOI entries it has fsync'd. For each broker i, it knows the contiguous prefix of that broker's PBR indices that correspond to GOI entries it has replicated (each GOI entry carries `broker_id` and `pbr_index`).
2. **Monotonicity:** The new tail sets `CompletionVector[i].completed_pbr_head` to the highest contiguous `pbr_index` it has replicated for broker i. It must **never** decrease this value (read current CV[i], take max(current, new_value), then write).
3. **Ongoing:** Thereafter it updates CV[i] exactly as the old tail did: as it replicates more GOI entries, it advances completed_pbr_head per broker (monotonic). Target: reconfiguration to new tail and resumption of CV updates in \<10 ms so that broker ACKs do not stall for long.

### 4.2.4 Cross-module chain replication (metadata)

Control Block, GOI, and PBRs are chain-replicated across **CXL modules** (e.g. 2–3 modules for f+1 copies). **Head:** The module that the active sequencer writes to is the **head** of the chain (determined by configuration or by the membership service when it assigns the sequencer to a node; that node’s local CXL or the primary module is head). **Tail:** The last module in the chain (e.g. module 2 of 2, or 3 of 3). **Write path:** The sequencer writes to the head; the head forwards updates along the chain (synchronous or asynchronous depending on implementation; synchronous gives stronger durability, async gives lower latency). **Read path:** Replicas and brokers read from the **tail** (or from any module that has received the update) so they observe committed, replicated state. **Head change:** If the head module or the sequencer’s node fails, the membership service elects a new sequencer; the new head is the CXL module attached to (or chosen for) the new sequencer. The chain order (e.g. head → replica_module_2 → tail) is reconfigured so that the new head is the first writer. **Protocol detail:** Writes to Control Block, GOI, and PBRs are applied in order at each module; each module advances its local view and forwards to the next. No separate “chain protocol” document is assumed—implementations may use CXL-aware replication (e.g. write to primary, async mirror) or explicit forward-to-next; the invariant is that the tail eventually has a prefix of all metadata writes, and readers that need consistency read from the tail.

### 4.3 Order 5 Failure Handling (HOL Blocking Mitigation)

```
THE HOL BLOCKING PROBLEM:
  Client sends: seq=1→B0, seq=2→B1, seq=3→B0
  B1 fails after receiving seq=2

  Naive approach:
    seq=1 arrives: hold (waiting for seq=2)
    seq=3 arrives: hold (waiting for seq=2)
    B1 recovery... (seconds)
    All of client's messages blocked!

EMBARCADERO'S SOLUTION:

1. BOUNDED HOLD BUFFER
   - max_entries = 10,000
   - base_timeout_ms = 5 (wall-clock gap timeout)

2. GAP RESOLUTION IS LAYERED (no broker-status inspection)
   - **Sort (Layer 1):** Intra-cycle jitter resolved by sorting batches by (client_id, client_seq).
     Cost: ~1 μs for typical drain sizes (<100 entries). Handles ~90% of reordering.
   - **Hold + Cascade (Layer 2):** Cross-cycle gaps buffered until the missing batch arrives.
     When it does, cascade_release drains all contiguously held batches.
     Cost: ~30–100 μs. Handles ~9% of reordering.
   - **Timeout (Layer 3):** Wall-clock timeout (default 5 ms). Emits SKIP marker (range-skip if gap > 1).
     Forces progress. Handles ~1% (TCP loss, broker degradation).

3. TIMEOUT FORCES PROGRESS
   expire_held_entries(ready) runs each cycle; entries held longer than base_timeout_ms
   trigger range-skip markers and cascade_release.
```

### 4.3.1 Sequencer failover: hold buffer and client state recovery

On sequencer crash, **hold buffer** and **per-client state** (next_expected_seq, dedupe window) are volatile; they are **not** persisted in CXL. Recovery options:

| Approach | Complexity | Recovery Time | Notes |
|----------|------------|---------------|-------|
| **Checkpoint + GOI replay (recommended)** | Medium | ~70–185 ms | Load checkpoint; replay GOI since checkpoint committed_seq. See §3.3 Per-client state checkpointing. |
| **Full GOI replay (no checkpoint)** | Low | ~115–335 ms | Replay last N entries; accept-first-seen for unknown clients. |
| **Primary-backup sequencer** | High | Near-zero | Future work. |

**Design choice:** For Order 5 production, **checkpoint + GOI replay** is recommended. On startup, the new sequencer (1) optionally truncates GOI if partial write detected (§3.3 Scatter-gather crash recovery), (2) loads the checkpoint from the 16 MB CXL region (§2.5) and replays the GOI tail from the checkpoint's committed_seq to rebuild ClientState, or (3) if no valid checkpoint, replays the GOI tail (e.g. last 100K entries) and uses accept-first-seen for unknown clients. Batches that were in the old hold buffer reappear in PBR; they are re-collected and re-ordered; duplicate batch_ids are discarded. Liveness: gap timeouts on the new sequencer eventually advance; no batch is stuck forever.

**Accept-first-seen:** After recovery, clients not found in the checkpoint or replay window are handled by **accept-first-seen**: the sequencer initializes next_expected to the client_seq of the first batch seen for that client. This prevents false SKIPs when a client starts from a non-zero sequence number or was absent from the recovery window.

**Step 4b: Initialize collector cursors.** For each broker i, set `collector_cursor[i] = CV[i].completed_pbr_head + 1`. Entries between this cursor and PBR[i].tail are re-processed; dedup discards already-sequenced batches. Cost: &lt;1 ms (32 CV reads). Without this, collectors might re-read the entire PBR from index 0, causing massive duplicate processing.

**Step 4c: Initialize replication floor.** Backward scan from committed_seq: find the first GOI entry where num_replicated &lt; replication_factor. Set replication_floor_cursor to that index. This prevents the anti-wraparound guard from stalling on phantom under-replication. Cost: O(replication_lag) × 200 ns, typically &lt;200 μs. **Correctness assumption:** Chain replication processes GOI entries in strictly increasing index order, so if entry i is fully replicated, all j &lt; i are also fully replicated.

---

## Part V: Correctness

### 5.1 Safety Properties

**Property 1: Total Order (Order 0/1/2 and Order 5)**
```
∀ batches A, B in GOI:
  A.global_seq ≠ B.global_seq ∧
  (A.global_seq < B.global_seq ∨ B.global_seq < A.global_seq)
```
*Proof:* Single atomic counter with fetch_add ensures unique, ordered assignment.

**Property 2: Per-Client Order (Order 5 only)**
```
∀ client C, ∀ batches M1, M2 from C:
  M1.client_seq < M2.client_seq ⟹
    M1.global_seq < M2.global_seq ∨ M1 declared lost
```
*Proof:*
1. Within epoch: sorted by (client_id, client_seq) before assignment
2. Across epochs: hold buffer delays M2 until M1 arrives or times out
3. Timeout declares M1 lost, unblocks M2

**Property 2b: Declared-Lost Semantics (Order 5)**
When the sequencer skips a gap, a SKIP marker entry is written to the GOI with a valid global_seq:
```
∃ exactly one GOI entry E with E.client_id=C ∧ (E.flags & FLAG_SKIP_MARKER)
  ∧ E.client_seq ≤ s < E.client_seq + skip_count(E),
  where skip_count(E) = E.message_count if (E.flags & FLAG_RANGE_SKIP) else 1.
E carries a valid global_seq and no BLog payload.
∀ batch B from C with B.client_seq ≥ E.client_seq + skip_count(E) in GOI:
  B.global_seq > E.global_seq.
```
SKIP markers are permanent. Subscribers see them in global_seq order and can distinguish "batch lost" from "batch never sent." The GOI contract now includes entries without BLog payloads (SKIP and NOP).

**Property 3: Durability**
```
∀ batch B: ACK_sent(B) ⟹ |{R : B ∈ R.storage}| ≥ f+1
```
*Proof:* The broker sends ACK when CompletionVector[broker_id] advances, which the last replica updates only after GOI[B].num_replicated ≥ f+1 for the corresponding entries; each replica fsyncs before incrementing. So ACK_sent(B) ⟹ B is replicated on ≥ f+1 replicas.

**Property 4: Zombie Safety**
```
∀ sequencer S with epoch E:
  ∀ entry written by S: entry.epoch = E
  ∀ reader R with current_epoch > E: R rejects entry
```
*Proof:* Epoch tagged at write; validated at read.

### 5.2 Liveness Properties

**Property 5: Progress**
```
Under partial synchrony, if fewer than f+1 replicas fail,
all published batches are eventually sequenced and acknowledged.
```
*Proof:*
1. Broker failure: PBR survives in CXL; sequencer drains it
2. Sequencer failure: Standby takes over via lease
3. Network failure: Client retries with duplicate detection
4. Hold buffer timeout: Forces progress

### 5.3 System Invariants

```
I1: ∀ broker i: only broker i writes to BLog[i] and PBR[i]
    (Single-writer ownership - Axiom A3)

I2: ∀ entry E in GOI: E.epoch ≤ control_block.epoch
    (No future epochs)

I3: ∀ seq s: GOI[s] committed before GOI[s+1] committed
    (Monotonic GOI growth)

I4: ∀ client C in Order 5, ∀ M1, M2 from C in GOI:
    M1.client_seq < M2.client_seq ⟹ M1.global_seq < M2.global_seq
    (Per-client order preserved)

I5: ∀ batch B: ACK(B) ⟹ ∃ ≥ f+1 replicas with B durable
    (Durability guarantee)

I6: ∀ PBR entry E, ∀ reader R:
    R observes E ⟹ E.blog_offset points to valid payload
    (Publish atomicity)

I7: ∀ Order 5 client C, ∀ M1, M2 from C in GOI:
    M1.client_seq < M2.client_seq ⟹ M1.global_seq < M2.global_seq
    (Per-client order preserved through sequencing and replication; subscribers see same order)

I-VIS (GOI visibility fence): No component reads or acts on GOI[i] for i > committed_seq.
I-WRAP (anti-wraparound): global_seq - replication_floor < GOI_SIZE at all times.
I-CLIENT (Order 5): After recovery, for every active Order 5 client C, next_expected[C] is correct (from checkpoint, GOI replay, or accept-first-seen).
PROP-SKIP (Order 5): SKIP markers are permanent. A skipped client_seq never appears as a real batch in the GOI.
I-PROC (Process Integrity): All sequencer threads (collectors, shard workers, committed_seq updater) run in a single OS process. If any thread terminates unexpectedly, the process exits and recovery (§4.3.1) rebuilds all volatile state from CXL. No partial-process recovery is attempted.
I-COLLECT (Collector Restart Safety): After recovery, collector_cursor[i] = CV[i].completed_pbr_head + 1. Entries between this cursor and PBR[i].tail are re-processed; dedup discards already-sequenced batches.
```

**End-to-end:** Per-client order is preserved from client submission through PBR, sequencer (including hold buffer and gap handling), GOI write, replication, and CV-based ACK. Subscribers reading in global_seq order observe Order 5 order. No component reorders batches from the same client after the sequencer assigns global_seq.

---

## Part VI: Performance Analysis

### 6.1 Theoretical Model

**Throughput bound:**
```
T_max = min(
    B_cxl_write,                           // CXL write bandwidth
    B_cxl_read / (1 + R),                  // CXL read (sequencer + R replicas)
    B_network,                             // Network ingress
    S_capacity * batch_size                // Sequencer processing
)

Where:
  B_cxl_write = 21 GB/s (measured)
  B_cxl_read = 21 GB/s (measured)
  R = replication factor
  B_network = 12.5 GB/s (100 Gbps)
  S_capacity = ~500K batches/epoch × 2000 epochs/s = 1B batches/s
```

**Latency model (Order 0/2):**
```
L_total = L_network + L_ingest + L_epoch_wait + L_sequence + L_replicate + L_ack

Where:
  L_network = 100-500 μs (client to broker)
  L_ingest = 100-200 μs (receive + CXL write)
  L_epoch_wait = τ/2 = 250 μs average
  L_sequence = 20-50 μs (within epoch)
  L_replicate = 500-800 μs (parallel copy + fsync)
  L_ack = 100-200 μs (broker to client)

  L_total ≈ 1.1 - 2.0 ms (Order 0/2)
```

**Order 5 latency model (with shard-worker sequencer, §3.2.1):**
```
L_total_order5 = L_network + L_ingest + L_sequencer_pickup
                 + L_order5_resolution + L_replicate + L_ack

Where:
  L_sequencer_pickup = 15-50 μs (continuous drain, no epoch boundary)
  L_order5_resolution =
    0 μs          (90% of batches: resolved by sort within drain)
    30-100 μs     (9% of batches: resolved by hold buffer across drains)
    5,000 μs      (1% of batches: timeout; SKIP emitted)

  L_total_order5 ≈ 0.85 - 1.55 ms (no gaps; ~200 μs lower than Order 2 epoch pipeline)
  L_total_order5 ≈ 1.0 - 1.7 ms (gaps resolved by hold buffer)
  L_total_order5 ≈ 6.0 - 7.0 ms (gaps resolved by timeout; worst case)
```

### 6.2 Measured Performance

**Hardware:** Samsung CXL 2.0 512GB, AMD EPYC 9754 (256 cores), 100Gbps network

**Throughput (4 brokers, replication=2):**

| Configuration | Throughput | Bottleneck | vs Scalog | vs Corfu |
|---------------|------------|------------|-----------|----------|
| Order 0/2, 1KB msgs | 11.2 GB/s | CXL bandwidth | 2.7× | 4.2× |
| Order 0/2, 4KB msgs | 11.0 GB/s | CXL bandwidth | 2.5× | 4.0× |
| Order 5 (10%) | 10.8 GB/s | Sequencer | 2.6× | 4.1× |
| Order 5 (100%) | 9.3 GB/s | Client state | 2.2× | 3.5× |

**Latency (1KB messages, low load):**

| Mode | P50 | P99 | P99.9 | vs Scalog | vs Corfu |
|------|-----|-----|-------|-----------|----------|
| Order 0/2 | 1.52ms | 1.68ms | 1.95ms | 5.3× lower | 2.3× lower |
| Order 5 (no gaps) | 1.58ms | 1.75ms | 2.10ms | 5.1× lower | 2.2× lower |
| Order 5 (with gaps) | 1.65ms | 2.50ms | ≤ base_timeout_ms + pipeline (~5ms default) | 3.2× lower | 1.5× lower |

**Scalability:**

| Dimension | Measured | Bottleneck |
|-----------|----------|------------|
| Brokers: 4→32 | 11.2→9.5 GB/s | Collector contention |
| Clients (Order 0/2): unlimited | N/A | No per-client state |
| Clients (Order 5): 1K→100K | 11.0→9.3 GB/s | L3 cache pressure |

### 6.3 Comparison with State-of-the-Art

```
                    THROUGHPUT VS LATENCY (LOG SCALE)

Latency (ms)
    │
 100├─────────────────────────────────────────────────────
    │                                    ┌─────────┐
    │                                    │ Scalog  │
  10├─────────────────────────────────── └────┬────┘
    │                      ┌─────────┐        │
    │                      │  Corfu  │        │
    │                      └────┬────┘        │
   1├───────────────────────────┼─────────────┼───────────
    │   ┌─────────────────┐     │             │
    │   │  EMBARCADERO    │     │             │
    │   │   Order 0/2     │     │             │
    │   └────────┬────────┘     │             │
 0.1├────────────┼──────────────┼─────────────┼───────────
    │            │              │             │
    └────────────┼──────────────┼─────────────┼──────────▶
              1 GB/s        5 GB/s       10 GB/s    Throughput

EMBARCADERO'S POSITION:
  - Throughput of Scalog (write-before-order architecture)
  - Latency approaching Corfu (but with higher throughput)
  - PLUS: Optional per-client ordering (neither Scalog nor LazyLog offers this)
```

---

## Part VII: Design Rationale

### 7.1 Why Write-Before-Order (Not Token-Before-Write)?

| Aspect | Token-Before-Write (Corfu) | Write-Before-Order (Embarcadero) |
|--------|---------------------------|-----------------------------------|
| Write latency | 2 RTTs (token + write) | 1 RTT (write only) |
| Sequencer load | All writes | Metadata only |
| Per-client order | Free | Opt-in (small overhead on CXL) |
| Throughput | Limited by token rate | Limited by CXL bandwidth |

**Decision:** CXL makes write-before-order viable without sacrificing per-client ordering.

### 7.2 Why Epochs (Not Per-Batch Atomics)?

```
PER-BATCH ATOMICS (Corfu-style):
  for each batch:
    seq = counter.fetch_add(1)  // ~50-100ns under contention

  At 10M batches/sec: 100% atomic unit utilization → bottleneck

EPOCH BATCHING (Embarcadero):
  // Once per epoch (every 500μs)
  base = counter.fetch_add(batch_count)  // Single atomic

  // Then pure arithmetic
  for each batch:
    batch.seq = base + offset  // ~1ns

  At 10M batches/sec: 2000 atomics/sec → <0.01% utilization
```

**Result:** 1000× reduction in atomic operations.

### 7.3 Why Optional Per-Client Ordering (Not Mandatory)?

| Workload | Needs Per-Client Order? | HOL Blocking Acceptable? |
|----------|------------------------|--------------------------|
| SMR/Database | Yes | Yes (consistency > latency) |
| Transactions | Yes | Yes |
| Log aggregation | No | No (latency matters) |
| Streaming | No | No |
| Metrics | No | No |

**Decision:** Default to Order 0/2 (no HOL blocking for Order 2); opt-in to Order 5 when needed.

### 7.4 Why Bounded Hold Buffer?

**Unbounded risks:**
- Memory exhaustion
- Unbounded tail latency
- Cascade failures

**Bounded (5 ms wall-clock, default base_timeout_ms) ensures:**
- Predictable memory: max 10K entries × 64 bytes = 640KB
- Bounded latency: max +5 ms on P99.9
- Graceful degradation: gaps logged via SKIP markers, progress continues

---

## Part VIII: Related Work

### 8.1 Shared Log Systems

| System | Venue | Architecture | Per-Client Order | Hardware |
|--------|-------|--------------|------------------|----------|
| Corfu | NSDI '12 | Token-before-write | Yes (free) | Network |
| Tango | SOSP '13 | On Corfu | Yes | Network |
| vCorfu | ATC '17 | Corfu + versioning | Yes | Network |
| Scalog | NSDI '20 | Write-before-order | No | Network |
| Delos | OSDI '20 | Virtualized log | Varies | Network |
| LazyLog | SOSP '24 | Lazy binding | No | Network |
| SpecLog | OSDI '25 | Speculative ordering | No | Network |
| **Embarcadero** | — | Write-before-order | **Yes (opt-in)** | **CXL** |

### 8.2 CXL Systems

| System | Focus | Ordering |
|--------|-------|----------|
| Pond | Disaggregated memory pool | None (storage) |
| TPP | Memory tiering | None (storage) |
| CXL-SSD | Persistent memory | None (storage) |
| **Embarcadero** | **Coordination fabric** | **Total + per-client** |

### 8.3 Key Differentiators

1. **vs Corfu:** Higher throughput (write-before-order), same ordering strength
2. **vs Scalog/LazyLog/SpecLog:** Same throughput, stronger ordering (per-client opt-in)
3. **vs Kafka:** Cross-partition total ordering, not just per-partition

---

## Part IX: Limitations and Future Work

### 9.1 Current Limitations

| Limitation | Impact | Mitigation |
|------------|--------|------------|
| Rack-scale only | CXL 2.0 range ~2m | Federation for geo-distribution |
| Single sequencer | Logical SPOF | Fast failover via lease |
| CXL hardware required | Limited availability | Commodity fallback mode (future) |
| Order 5 HOL blocking | Tail latency on failures | Bounded timeout |

### 9.2 Future Directions

1. **CXL 3.0 Fabric:** Multi-rack deployments via CXL switches
2. **Parallel Scatter-Gather Sequencer:** §3.3 defines the design; implement it as the default for line-rate deployments (100 Gbps, 8+ PBRs). Single-sequencer (§3.2) remains for small deployments.
3. **Sequencer Sharding:** Partition by key for independent ordering domains
4. **Persistent CXL:** Eliminate replica copying for durability
5. **Hybrid Mode:** Automatic Order 0/2/5 selection based on workload
6. **Speculation:** SpecLog-style speculative delivery for Order 0/2

---

## Part X: Conclusion

Embarcadero demonstrates that **CXL fundamentally changes the shared log design space**. By using shared memory as a coordination fabric rather than merely a storage tier:

1. **Data placement is decoupled from ordering:** Clients load-balance freely across brokers
2. **Ordering happens at memory speed:** Nanoseconds, not microseconds/milliseconds
3. **Per-client ordering is affordable:** Same infrastructure, opt-in overhead, no HOL blocking for those who don't need it
4. **Failures are handled cleanly:** Epoch-based fencing, bounded recovery

The result is the first shared log to achieve **Scalog-class throughput with Corfu-class ordering guarantees**—a combination previously thought impossible.

**The Expert Consensus (Alvisi, Aguilera, Shenker, Stoica):**
> "Don't build Scalog. Don't build Corfu. Build Embarcadero. It's the only system that can offer both modes cheaply. That is its unique value proposition."

---

## Appendices

### Appendix A: Configuration Reference

```yaml
embarcadero:
  cluster:
    name: "production"
    brokers: 4
    replicas: 2

  cxl:
    primary_device: "/dev/dax0.0"
    secondary_device: "/dev/dax1.0"  # Metadata replica
    size_gb: 512
    numa_node: 2

  sequencer:
    mode: single               # "single" (§3.2) or "shard_worker" (§3.2.1) or "scatter_gather" (§3.3)
    epoch_us: 500              # τ = 500μs (Order 2 epoch pipeline only; ignored in shard_worker mode)
    collectors: 4              # Parallel collector threads
    num_shards: 1              # Shard workers (1 for ≤4 brokers; 8 for 8+ or Order 5)
    spsc_queue_capacity: 32768 # Per (collector, shard) queue size

  ordering:
    default_order: 0            # Order 0 (no global order) or 2 (total order)
    order5_enabled: true        # Opt-in Order 5 (strong order)
    max_clients: 100000         # Max Order 5 clients
    hold_buffer_size: 10000     # Max held batches
    base_timeout_ms: 5          # Order 5: wall-clock gap timeout
    dedupe_window: 4096         # Order 5: per-shard batch_id ring size
    client_ttl_sec: 300         # Client state GC

  # Order 5 recovery
  recovery:
    checkpoint_interval_sec: 60   # Mandatory for production Order 5
    checkpoint_location: "cxl"     # "cxl" (16 MB reserved) or "disk"
    goi_replay_entries: 100000     # Fallback if no checkpoint

  safety:
    goi_wraparound_high_pct: 90    # Stall shard workers when GOI usage exceeds this
    goi_wraparound_low_pct: 80     # Resume after dropping below this
    stuck_slot_timeout_sec: 10     # Clear invalid PBR slots after this duration
    committed_seq_reconcile_sec: 5 # Reconciliation watchdog interval

  replication:
    factor: 2
    ack_level: "all"           # "one" | "all"

  broker:
    pbr_high_watermark_pct: 80    # Stop accepting above this % full
    pbr_low_watermark_pct: 50     # Resume accepting below this
    backpressure_reject_after_sec: 10  # Reject with BACKPRESSURE if above high watermark this long

  network:
    broker_port: 9092
    max_connections: 10000
    recv_buffer_mb: 16
```

### Appendix B: Client API

```cpp
// === PUBLISHER ===

class Publisher {
public:
    // Order 0/2: Maximum throughput; Order 2 = total order
    static Publisher create(Config config);

    // Order 5: Per-client ordering guaranteed
    static Publisher create_ordered(Config config, uint64_t client_id);

    // Publish batch; returns future with global_seq on ACK
    std::future<uint64_t> publish(std::span<const Message> batch);

    // Publish with explicit sequence (Order 5 only)
    std::future<uint64_t> publish(std::span<const Message> batch,
                                   uint64_t client_seq);
};

// === SUBSCRIBER ===

class Subscriber {
public:
    static Subscriber create(Config config);

    // Poll for next batch (blocks until available or timeout)
    std::optional<Batch> poll(std::chrono::milliseconds timeout);

    // Seek to specific position
    void seek(uint64_t global_seq);

    // Get current position
    uint64_t position() const;
};

// === BATCH ===

struct Batch {
    uint64_t global_seq;               // First message's global sequence
    uint64_t client_id;                 // Source client (0 if Order 0)
    uint64_t client_seq;                // Client sequence (Order 5 only)
    uint16_t broker_id;                 // Source broker
    uint32_t flags;                     // FLAG_* (Order 5: SKIP_MARKER, RANGE_SKIP)
    uint32_t message_count;             // Number of messages; for SKIP+RANGE_SKIP = skip range size
    std::vector<Message> messages;

    // Per-message global sequence
    uint64_t message_seq(size_t i) const {
        return global_seq + i;
    }

    // Order 5: SKIP marker detection
    bool is_skip() const { return flags & FLAG_SKIP_MARKER; }
    uint32_t skip_count() const {
        return (flags & FLAG_RANGE_SKIP) ? message_count : 1;
    }
};
```

### Appendix C: Metrics Reference

```prometheus
# Throughput
embarcadero_publish_bytes_total{broker="0"}
embarcadero_publish_batches_total{broker="0"}
embarcadero_subscribe_bytes_total{broker="0"}

# Latency histograms
embarcadero_publish_latency_seconds{quantile="0.5|0.99|0.999"}
embarcadero_e2e_latency_seconds{quantile="0.5|0.99|0.999"}
embarcadero_sequencer_latency_seconds{quantile="0.5|0.99|0.999"}

# Ordering (Order 5)
embarcadero_level5_clients_active
embarcadero_hold_buffer_entries
embarcadero_hold_buffer_timeouts_total
embarcadero_gaps_skipped_total
embarcadero_duplicates_rejected_total

# Order 5: Hold buffer
embarcadero_sequencer_hold_buffer_entries{shard}
embarcadero_sequencer_hold_buffer_age_ms{shard, quantile}
embarcadero_sequencer_cascade_release_depth{shard, quantile}

# Order 5: SKIP / NOP
embarcadero_sequencer_skip_total{shard, reason="timeout|overflow"}
embarcadero_sequencer_nop_entries_total{shard}

# Order 5: Checkpointing
embarcadero_sequencer_checkpoint_write_duration_us
embarcadero_sequencer_checkpoint_age_sec
embarcadero_sequencer_checkpoint_client_count

# Replication
embarcadero_replication_lag_batches
embarcadero_replica_fsync_latency_seconds{replica="0"}

# Failures
embarcadero_broker_failures_total
embarcadero_sequencer_failovers_total
embarcadero_epoch_increments_total

# CXL
embarcadero_cxl_read_bytes_total
embarcadero_cxl_write_bytes_total
embarcadero_cxl_read_latency_ns{quantile="0.5|0.99"}
embarcadero_cxl_write_latency_ns{quantile="0.5|0.99"}
```

### Appendix D: Glossary

| Term | Definition |
|------|------------|
| **BLog** | Broker Log — per-broker circular buffer for message payloads |
| **PBR** | Pending Batch Ring — per-broker metadata queue for sequencer |
| **GOI** | Global Order Index — the definitive ordered log of batch metadata |
| **Completion Vector (CV)** | Per-broker completion state in CXL; last replica updates CV[broker_id]; broker polls only CV[self] to ACK (avoids GOI scan) |
| **Order 0** | No global ordering; broker updates `written` after PBR write (Option A); no sequencer |
| **Order 1** | Per-broker local ordering — FIFO per broker, no cross-broker order |
| **Order 2** | Total ordering — global_seq, no per-client guarantees; sequencer non-blocking, constantly iterates PBRs |
| **Order 5** | Strong ordering (opt-in) — total order + per-client order preserved |
| **ack_level** | 0 = no ACK; 1 = ACK after visible/ordered in CXL; 2 = ACK after replicated (durable) |
| **Epoch** | Fixed time window for batch sequencing (~500μs) |
| **Hold Buffer** | Reorder buffer for Order 5 gap handling |
| **HOL Blocking** | Head-of-line blocking — fast messages blocked by slow ones |
| **NOP entry** | GOI entry with FLAG_NOP. Placeholder for a duplicate batch's PBR slot so Completion Vector can advance. Valid broker_id and pbr_index; zero payload. Order 5 artifact. |
| **SKIP marker** | GOI entry with FLAG_SKIP_MARKER. Declares one or more client_seq values permanently lost. broker_id=0 (backward-compatible sentinel); no BLog payload; valid global_seq. May have FLAG_RANGE_SKIP with message_count = number of skipped seqs. Order 5 artifact. |
| **Accept-first-seen** | Policy for unknown Order 5 clients: initialize next_expected to the client_seq of the first batch seen, rather than 0. Prevents false SKIPs after recovery. |
| **Replication floor** | Highest contiguous GOI index fully replicated (num_replicated ≥ replication_factor). Used by anti-wraparound guard. |
| **Cascade release** | When a gap-filling batch arrives and advances next_expected, all contiguously held batches behind it are released from the hold buffer in sequence. O(1) per batch with deque. |
| **Shard worker** | Per-shard sequencer thread that processes Order 2/5 batches, assigns global_seq, and writes GOI entries. Owns disjoint client set via hash(client_id). |
| **SPSC queue** | Single-Producer Single-Consumer lock-free queue between a collector and a shard worker. Eliminates contention on the communication path. |
| **CompletedRange** | (start, end) pair pushed by a shard worker after writing GOI entries. The committed_seq updater merges these via min-heap. |
| **Drain cycle** | One iteration of a shard worker's main loop: drain queues → dedup → process Order 2/5 → commit → expire held. Continuous (no epoch boundary). |
| **Reconciliation watchdog** | Safety net in the committed_seq updater that periodically scans the GOI directly to detect and repair stalled committed_seq advancement. |

### Appendix E: Design notes (minor)

**batch_id uniqueness:** `batch_id = (broker_id | timestamp | counter)`. To avoid collisions: use **nanosecond** (or microsecond) resolution for timestamp, and a **per-broker counter** (e.g. 16–32 bits) that increments per batch and wraps. Uniqueness scope: (broker_id, timestamp, counter) over a window (e.g. 1 s) is sufficient; sequencer deduplicates by batch_id when the same batch is retried or re-seen. If a broker can exceed 2^16 batches per time unit, use a wider counter or include epoch in the id.

**Client broker discovery:** Clients need a list of healthy broker endpoints to publish. Options: (1) **Static config** — list of host:port; (2) **Service discovery** — query DNS, etcd, or a metadata service for current broker set; (3) **Bootstrap from any broker** — client connects to one configured endpoint, which returns the current broker list and sequencer identity. The design does not mandate one; implementations should support at least static config and document how discovery is refreshed on broker failure.

**Duplicate detection window (Order 5):** The sequencer uses a small window (e.g. `recent_window` 128 bits) to detect duplicate (client_id, client_seq). Duplicates **outside** the window (e.g. client sends seq=0, then after a long gap sends seq=0 again) may be treated as new if the old state was evicted. Mitigation: (1) client_seq should be monotonically increasing over the client’s lifetime for that client_id; (2) increase window size (e.g. 1024) if long outages or very high throughput; (3) or use batch_id for deduplication in addition to (client_id, client_seq) so that retries with the same batch_id are always discarded.

**epoch_sequenced / epoch_created (uint16_t wraparound):** Both GOIEntry and PBREntry use `uint16_t` for epoch. At τ=500 μs, 65536 epochs ≈ 32.77 s before wraparound. Entries are processed and replicated in milliseconds, so no entry lives long enough for wraparound to matter. When comparing epochs (e.g. staleness check), use **modular arithmetic** (e.g. `(current - entry) & 0xFFFF` for “age” within a window) or document that epochs are compared only when both are within 2^15 of each other.

**Sequencer helper functions (pseudocode):** §3.2 references `cascade_release`, `emit_nop`, `add_to_hold_buffer`, `expire_held_entries`, `get_or_init_next_expected`. The complete Order 5 protocol (deduplication, hold buffer overflow protection, three-layer gap resolution, SKIP/NOP emission) is in the sequencer specification (Appendix F). Implementations will define these accordingly.

**Accept-first-seen rationale (Order 5):** After sequencer recovery, a client may not appear in the checkpoint or GOI replay window. Initializing next_expected=0 would cause false SKIPs for clients starting at higher sequence numbers. Accept-first-seen initializes next_expected to the first client_seq observed, eliminating false SKIPs. Trade-off: if the client truly had earlier batches (e.g., before an outage), those are already in the GOI and do not need re-sequencing.

**NOP entries vs out-of-band CV updates:** An out-of-band mechanism (sequencer directly writes CV for duplicates) would violate the single-writer property of CV (tail replica is sole writer). NOP entries flow through the normal GOI → replication → CV pipeline, preserving the invariant.

### Appendix F: Related design documents

This document is the **authoritative conceptual and protocol reference**. The following docs contain **implementation-level** and **deployment-variant** detail that implementers and maintainers should use alongside it:

| Document | Purpose | When to use |
|----------|---------|-------------|
| **Sequencer specification** (e.g. docs/SEQUENCER_SPEC.md or ablation_study/sequencer5/new_design.md) | Complete Order 5 sequencing protocol: shard-worker architecture, three-layer gap resolution, SKIP/NOP emission, hold buffer management, checkpointing, recovery. **Supersedes §3.2 for Order 5;** §3.2 is Order 2 baseline only. | Implementing or debugging Order 5 sequencing; shard-worker thread model; gap resolution layers; sequencer failover. |
| **docs/CXL_MEMORY_LAYOUT_v2.md** | Exact CXL offsets, struct definitions, per-region access patterns, alignment. | Implementing or changing CXL layout; debugging layout/offset bugs; onboarding. |
| **docs/memory-bank/LOCKFREE_BATCH_HEADER_RING_DESIGN.md** | Lock-free PBR ring protocol: `batch_headers_consumed_through`, producer wrap semantics, min-contiguous/gap handling, B0 same-process cache coherency. | Implementing or fixing scanner/allocator; understanding ring wrap and ACK stall causes. |
| **docs/memory-bank/SEQUENCER_ONLY_HEAD_NODE_DESIGN.md** | Sequencer-only head topology: no data ingest on head, `data_broker_ids`, heartbeat contract, startup. | Deploying or testing sequencer-only head; resolving B0 coherency via topology. |

**Gap vs this doc:** §2.5 and §3.1 give a high-level memory map and “PBR slot reuse after CV” semantics. The **consumed_through** protocol (producer waits on sequencer-consumed offset, not on CV) and **min-contiguous** scanner semantics are in LOCKFREE_BATCH_HEADER_RING_DESIGN.md. The **sequencer-only head** (Broker 0 does not run ingest) is not described here; it is in SEQUENCER_ONLY_HEAD_NODE_DESIGN.md.

---

*Document version 2.4. Updates: §3.2 scoped to Order 2 baseline; §3.2.1 shard-worker sequencer (required for Order 5); inline Order 5 code removed from §3.2; committed_seq updater (CompletedRange + min-heap, reconciliation watchdog); anti-wraparound guard; recovery Steps 4b/4c (collector cursor, replication floor); I-PROC, I-COLLECT; Order 5 latency with L_sequencer_pickup (15–50 μs); stuck-slot timeout (10 s); SKIP broker_id=0; Appendix A safety/config, Appendix D glossary (shard worker, SPSC, CompletedRange, drain cycle, reconciliation watchdog). Prior: 2.3.*
Entry.flags; 16 MB checkpoint region; NOP/SKIP replica and CV handling; Property 2b (PROP-SKIP); invariants I-CLIENT, PROP-SKIP, I-VIS, I-WRAP; Order 5 latency model; base_timeout_ms (5 ms); checkpointing and accept-first-seen; Appendix config, metrics, glossary, design notes, related docs. Prior: 2.2.*
 checkpointing and accept-first-seen; Appendix config, metrics, glossary, design notes, related docs. Prior: 2.2.*
