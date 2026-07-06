# Buffer Scheme: Paper vs Doc — Expert Panel Assessment

**Question:** We are writing an academic paper (Sigcomm/OSDI/SOSP) about Embarcadero. Should we include the client-side buffer scheme (QueueBuffer, num_queues, active_queues, pool of batch slots, round-robin to N queues) in the paper (design or implementation)? Or is it just engineering that shouldn’t be mentioned? Should we at least explain it in the definitive design doc, or is the doc outdated?

**Context:** The definitive design doc (EMBARCADERO_DEFINITIVE_DESIGN.md) describes broker ingestion, PBR, GOI, sequencer, replication, and ACK path in detail. It does **not** describe how the **client** assembles messages into batches, buffers them, or distributes them to N publish threads (one per broker connection). The current implementation uses QueueBuffer: a pool of batch-sized slots, N SPSC queues, one producer (main thread filling batches), N consumers (PublishThreads), round-robin push, and active_queues when broker count varies. See docs/NEW_BUFFER_BANDWIDTH_INVESTIGATION.md.

---

## Expert Panel (Simulated): Leslie Lamport, Ion Stoica, Marcos Aguilera, Scott Shenker

### Lamport (Specification and correctness)

"The specification should be complete enough that correctness is clear. The client produces batches and sends them over TCP. The **order** in which batches are sent to different brokers does not affect protocol correctness: Order 0/2/5 are defined on the broker and sequencer side. So the client buffer scheme is an **implementation optimization** for throughput and backpressure, not part of the logical specification.

**Paper:** Do not include the full buffer design. At most one sentence: 'Clients batch messages and maintain one connection per (broker, thread); batching and buffer sizing follow standard producer–consumer design.' No queue count, pool size, or active_queues in the paper.

**Doc:** If implementers need to reason about liveness (e.g. why the producer must not push to queues with no consumer), the doc should state the **principle**: match the number of logical producer–consumer lanes to the number of consumers, or bound which lanes are used. The doc does not need the full QueueBuffer API."

### Stoica (Systems and bottlenecks)

"For a systems paper, what matters is bottleneck analysis and end-to-end numbers. The doc already says: 'Multi-threaded ingestion: 4–16 brokers, 4–8 network threads per broker.' So the reader infers the client has multiple threads and multiple connections. The exact buffer (N queues, pool, active_queues) is **engineering**.

**Paper:** One sentence in Implementation or Evaluation: 'Client uses batched publish with a per-connection buffer; buffer size and queue count are tuned to match broker count and thread count.' Do not put the QueueBuffer design in the main design section.

**Doc:** The definitive design is the reference for **protocol** and **server-side** architecture. Adding a short 'Client publish path' subsection that states the design principle (bounded batching, one producer, N consumer threads, backpressure when full; when broker count varies, only use queues that have consumers) would make the doc complete and avoid the impression it is 'outdated.' No need for kQueueCapacity or slot_size in the main doc."

### Aguilera (Distributed systems and clarity)

"The design doc is authoritative for the **protocol** and **server-side** design. It currently says nothing about the client buffer. That is a **gap**, not necessarily 'outdated' — the server design is still correct. But if we have a QueueBuffer with num_queues and active_queues that affect **correctness** (e.g. producer blocks forever if we push to ghost queues) or **performance** in a non-obvious way, we should document that **somewhere**.

**Paper:** Do not include the full buffer scheme. Optionally: 'Clients batch messages into 2 MB batches and send via one TCP connection per (broker, thread); buffer sizing ensures the producer does not block on full queues that have no consumer when broker count varies.' That signals we thought about scaling without wasting space.

**Doc:** Yes, **update the doc**. Add a short subsection (e.g. §2.6 Client publish path and buffering). State: (1) Clients batch messages (e.g. 2 MB) before sending. (2) One producer (main thread) fills batches; N consumer threads (PublishThreads) each send to one broker connection. (3) Producer distributes batches to consumers via N queues (one per consumer); pool of batch-sized buffers; producer round-robins. (4) When broker count varies, only queues with active consumers are used (active_queues) so the producer never blocks on queues that are never drained. Reference docs/NEW_BUFFER_BANDWIDTH_INVESTIGATION.md for implementation and sizing details."

### Shenker (Architecture and venue)

"The paper’s story is: CXL changes the cost function; we get write-before-order with cheap per-client ordering. The client is 'someone who sends batches.' How they batch is **orthogonal** to that contribution.

**Paper:** At most a short implementation note or footnote. The contributions are (1) CXL as coordination fabric, (2) epoch-batched sequencing O(1) atomics, (3) optional per-client ordering. The client buffer is standard producer–consumer batching; no novel algorithm. Sigcomm/OSDI/SOSP focus on the novel bits.

**Doc:** The definitive design doc is the **single source of truth** for implementers. If the current implementation has QueueBuffer, num_queues, and active_queues, and we have learned that ghost queues and 'match active_queues to consumer count' matter for performance and liveness under varying broker count, then the doc **should** be updated. Add a short subsection under §2 (Architecture): 'Client publish path and buffering.' State the design principle and the varying-broker-count rule. Point to NEW_BUFFER_BANDWIDTH_INVESTIGATION.md for details. That way the doc stays complete and the paper stays clean."

---

## Consensus Summary

| Question | Recommendation |
|----------|----------------|
| **Include buffer scheme in the paper (design)?** | **No.** It is not a contribution; it is standard producer–consumer batching. |
| **Include buffer scheme in the paper (implementation)?** | **Optional, one sentence.** E.g. "Clients batch messages (e.g. 2 MB) and send via one connection per (broker, thread); buffer and queue count are sized to match broker count to avoid producer blocking." |
| **Is the doc outdated?** | **Partially.** The doc is not wrong; it simply does not cover the **client** publish path. So it is **incomplete** for the full system, not outdated. |
| **Explain buffer in the doc?** | **Yes.** Add a short subsection (§2.6 or similar) that states the **design principle**: batched publish, one producer + N consumer threads, N queues and pool, round-robin, and **active_queues when broker count varies**. Do not put full implementation detail (kQueueCapacity, slot_size, Folly types) in the definitive design; reference NEW_BUFFER_BANDWIDTH_INVESTIGATION.md for that. |

---

## Suggested Paper Wording (if mentioned at all)

**Implementation / Evaluation (one sentence):**  
"Clients batch messages (e.g. 2 MB) and send via one TCP connection per (broker, thread); the client buffer and queue count are sized to match the number of broker connections so the producer does not block on undrained queues when broker count varies."

**Footnote (optional):**  
"Client batching uses a pool of batch-sized buffers and N single-producer–single-consumer queues (one per publish thread); the producer round-robins sealed batches to queues. When brokers join or leave, only queues with active consumers are used."

---

## Suggested Doc Update

Add **§2.6 Client publish path and buffering** (or a short appendix) to EMBARCADERO_DEFINITIVE_DESIGN.md with:

1. **Batching:** Clients assemble messages into batches (e.g. 2 MB) before sending.
2. **Parallelism:** One logical producer (main thread filling batches); N consumer threads (PublishThreads), each with one TCP connection to a broker (or per (broker, thread)).
3. **Buffering:** Producer distributes sealed batches to consumers via N queues (one per consumer) and a pool of batch-sized buffers; producer round-robins to avoid head-of-line blocking and to match broker connections.
4. **Varying broker count:** The number of queues in use at runtime is the number of active consumers (PublishThreads). When broker count varies, only queues that have a consumer are used for round-robin, so the producer never blocks on full queues that are never drained. Queue and pool capacity are sized for a configured maximum broker count; see implementation notes (e.g. docs/NEW_BUFFER_BANDWIDTH_INVESTIGATION.md) for sizing and active_queues.

This keeps the definitive design as the single place for "why we do it this way" without turning it into an implementation manual.
