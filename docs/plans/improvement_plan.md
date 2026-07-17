# Embarcadero Resubmission Improvement Plan (v3)

**Context.** SOSP 2026 #485 rejected (3/3/2/2/3; three "new contribution" novelty marks). The PC named
two decisive issues: (1) the per-client FIFO guarantee degraded to "prefix-FIFO with causal
exceptions" — a problematic contract; (2) the evaluation setups favored Embarcadero. Two rounds of
expert-panel review of the recovery plan endorsed its direction and issued corrections, all
incorporated below. The reject was of *this execution*, not this line of work — the venue wants
the paper.

**Revision history.** v2 absorbed panel Flags 1–4 (T/R-vs-fault-domain; transformed-not-gone;
testbed-is-the-gate; throughput-is-a-bet). **v3 (this doc) absorbs a second brutal review + panel
rebuttal.** The load-bearing v3 changes: the thesis is now the **conjunction** (leg 1 ∧ leg 2),
not fault-domain alone (§Thesis, G2, I.1); the RDMA variant splits into **two** variants that
*measure* the conjunction on hardware we already own (W5, E4e); the SLO headline becomes a
**curve, not a threshold** (G3, E3, IV.1); an **ex ante porting rule** protects the waterfall
(W4, E1); a **mechanism self-audit** replaces the asserted complexity budget (G5, D9); and two
new correctness items surfaced under pressure — **assign-at-release × spatial guard** (kept from
v2, D4) and the newly found **stale-CV ACK-relay** epoch check (D2, W2).

**v4 (this doc) resolves the hardware question: there is no accessible multi-host CXL, so no
load-bearing claim may depend on it.** The correctness core is instead validated *in silicon* on
hardware we own — because one-sided RDMA to remote memory is **itself cross-host non-coherent**
(stale reads, torn multi-cacheline objects, no cheap cross-host atomics — the same hazard family
CXL has, and why FaRM/DrTM/Clover use the same single-writer/versioning disciplines). The
Embarcadero-on-RDMA variants (W5) therefore exercise the exact discipline reviewer C praised, on
real distributed hardware. Single-host CXL supplies the substrate numbers it *can* prove (latency,
bandwidth, 64 B TLP atomicity); TLA⁺ covers interleavings; multi-host CXL integration is the
disclosed residual gap (I.6). Multi-host CXL access is **upside, not a gate**.

**Target.** Venue is now a *schedule* decision, not a hardware one. **SOSP 2027 is the realistic
primary** (the full eval matrix + TLA⁺ + two RDMA variants is not credibly a ~5-month job for a
small team — see VI.3); **OSDI 2027 (~Dec 2026) only if the schedule genuinely lands.** Neither is
gated on multi-host CXL access — the paper stands on RDMA-in-silicon + single-host CXL + TLA⁺; CXL
access is upside only (W7).

---

## The thesis transformation (one sentence)

> Write-before-order logs abandoned per-client FIFO because ordering **repair** — straggler wait,
> gap fill, failure detection, retransmission — cost network rounds and died with brokers. Over
> RDMA the two properties that make repair cheap are **mutually exclusive**; CXL is the first
> substrate to provide **both at once**, so a multi-broker log can offer an **unconditional
> per-session prefix guarantee** for the first time.

**The conjunction (the actual thesis).** Cheap repair needs two things simultaneously:
*failure independence* (log state outlives any host) **and** *no shared network endpoint*
(every host reaches the state at memory speed with nothing to funnel through). Over RDMA you can
buy either but not both:

- Blogs in **broker DRAM** → state dies with the broker. You lose **failure independence**.
- Blogs on a **dedicated memory server** → state survives, but every payload write, every
  sequencer poll, and every replica/subscriber read now traverses the memory server's NIC — the
  centralized funnel the paper exists to eliminate. You lose **no-shared-endpoint**.

CXL provides both because it is **passive memory**: per-host `LD`/`ST` at 200–500 ns, no far-side
CPU/OS/NIC failure surface, no shared network endpoint. The thesis is the conjunction, the
conjunction is genuinely CXL-specific, and — the key v3 insight — **it is measurable on the
ConnectX-5 hardware we already own** by building both RDMA variants and showing each surrenders
exactly one leg (W5, E4e).

**Validated on owned hardware (v4).** We cannot access multi-host CXL, so nothing load-bearing
depends on it. The correctness core is validated *in silicon* on the ConnectX-5 hardware we own,
because **one-sided RDMA to remote memory is itself cross-host non-coherent** — stale reads, torn
multi-cacheline objects, no cheap cross-host atomics — the same hazard family CXL has (and the
reason FaRM/DrTM/Clover adopt the same single-writer/versioning disciplines). So Embarcadero-on-RDMA
exercises the exact discipline reviewer C praised, on real distributed hardware. What each source
proves:
- **Coherence-free discipline under real cross-host non-coherence** → the RDMA variants (silicon) + TLA⁺.
- **clwb+sfence → atomic 64 B TLP visibility to the device** → micro-benchmark on our one real CXL module.
- **CXL latency / bandwidth / zero-NIC polling** → real single-host CXL measurements (281/372 ns, 21 GB/s).
- **The conjunction (leg 1 ∧ leg 2 impossible over RDMA)** → W5-A vs W5-B, measured on owned RDMA.
- **Full system over *shared* CXL across hosts** → the one honest residual gap, disclosed in I.6.

Compressed for the team: **the semantics are the capability, the conjunction is the thesis, the
RDMA variants are the in-silicon evidence, and multi-host CXL is upside — not a gate.**

---

# PART A — BIG GOALS

### G1. Strengthen the guarantee: unconditional per-session prefix FIFO
Eliminate the anomaly taxonomy (`Hole`, `Causal_Late`, silent reorder) entirely. Replace the hold
timeout with a **session lease**: sessions + sequencer dedup + client retransmit + suffix
*fencing* (session death, never gap-skip). The guarantee becomes one boxed theorem stating **both**
Property 1 (total order) and Property 2 (per-session prefix) and their composition (D1, panel #11).
Honest framing (panel Flag 2): this **transforms** the PC's #1 objection into the standard,
bounded, per-session availability tradeoff every fenced-session system has — it does not make the
tradeoff disappear. The guarantee is a **capability enabled by the conjunction thesis, not a
parallel contribution** (panel #2): present it as such. Keep the Kafka citation for dedup/fencing,
but state the Kafka-partition rebuttal (I.4) — Embarcadero's setting is structurally impossible in
Kafka.

### G2. Re-center the thesis: the conjunction, proved by contrast
Lead with the **conjunction** (failure independence ∧ no-shared-endpoint), not leg 1 alone
(panel #1). The prior "leg 1 is irreducible" framing was wrong: failure independence *alone* is
the whole RDMA-disaggregated-memory literature (FaRM, Clover, Clio, LegoOS, AIFM). What is
irreducible is having leg 1 **and** leg 2 at once, which over RDMA is impossible (see Thesis).
Demote T/R ≫ 1 to the **model layer** (panel Flag 1): a necessary condition both RDMA and CXL
clear, measured in E6. Pre-answer "dedicate a second NIC / use a memory server" explicitly — the
memory-server rebuttal is now a *measured result* (W5-B, E4e), not a hand-wave.

### G3. Make the evaluation isolate causes — methodology as a contribution
Fairness rules stated up front and never broken; an **ex ante porting rule** (W4) that keeps the
decomposition waterfall an instrument rather than an argument; the waterfall itself
(transport vs. architecture vs. substrate gain) as the fairness instrument; the **failure suite**
(including the *baselines'* failure behavior) as the thesis instrument. Headline metric is the
**throughput-vs-SLO-threshold curve** across 1–100 ms, not a single pre-committed threshold
(panel #3): no magic number to game from either side, and the crossover point is itself an honest
result. Assume the raw-throughput gap compresses to ~1.2–1.6× and do not bet the paper on it
(panel Flag 4). All predictions — including **per-baseline knee locations** (panel #4) — written
to the repo **before** running headline experiments.

### G4. Machine-check the hardest claims; validate the discipline in silicon on owned RDMA
TLA⁺ spec of the session protocol under {broker crash × sequencer failover × client retransmit ×
lease false-positive × **stale-CV ACK relay**}, shipped as an artifact. **The coherence-free discipline is validated in silicon on the RDMA variants** (W5), which run
genuinely cross-host on owned ConnectX-5 hardware and exercise real cross-host non-coherence — the
correctness-critical part. N-way CXL contention, aggregate polling, and switch
behavior remain **modeled** — say "mechanism validated, scale modeled," never "validated in
silicon" unqualified (panel #7). **Multi-host CXL is not a gate (v4):** it is unavailable, so no load-bearing claim depends on it.
The residual integration gap (system over *shared* CXL across hosts) is disclosed in I.6; any CXL
access that lands is pure upside (W7).

### G5. Fix presentation so added machinery reads as *organized* — with a real self-audit
One boxed guarantee theorem, one structure-ownership table, pseudocode for the four roles, a
glossary, renamed terms. Complexity budget is enforced by a **mechanism self-audit table** (D9),
not asserted: every mechanism maps to one line of the guarantee/availability contract or is cut.
Honest accounting (panel #8): the redesign **deletes** v1's five bespoke anomaly mechanisms (hold-
timeout semantics, `Hole`, `Causal_Late`, per-entry expiry, idle-armed force-expiry) and replaces
them with timers/dedup/fencing every reviewer already has schemas for — recognizable machinery is
cheaper to review than counted machinery. The self-audit already yields two deletions (fold session
HWM into the `SESSION_FENCED` payload; demote redirect-notice push to an evaluated optimization).
A presentation score of 1 (reviewer E) lets a PC reject content it likes; never again.

### G6. Process discipline
Pre-registration of predictions (throughput compression + knee locations); one script per figure;
≥3 trials with error bars everywhere; documented + open-sourced baseline ports with **calibration
against original published numbers** and **baseline-author sanity-check emails** (panel #10);
artifact evaluation planned from day one; external cold reads before submission; a reviewer-
question traceability map (Part V) so every question from this round is answered *in the text*.

---

# PART B — DETAILED PLANS

## Part I. Thesis, positioning, and related work

**I.1 The conjunction (intro spine).** The paper leads with why cheap repair needs two properties
at once and why no non-CXL substrate delivers both:
1. *Failure independence*: log state (Blog/PBR/GOI/CV) resides in a fault domain separate from
   every host; broker death loses only in-flight TCP bytes the client still holds. **Achievable
   over RDMA** with a memory server — so not, by itself, the thesis.
2. *No shared network endpoint*: every host reaches the state via `LD`/`ST` with nothing to funnel
   through; polling consumes zero NIC/CQ capacity. **Achievable over RDMA** only by putting state
   in broker DRAM — which forfeits property 1.
3. *The conjunction*: CXL delivers 1 ∧ 2 because it is passive memory with per-host load/store and
   no far-side CPU/OS/NIC. Over RDMA, 1 and 2 are mutually exclusive (Thesis). This is the
   irreducible, CXL-specific claim, and E4e/W5 measures it directly.
4. Raw load latency (200–500 ns) is listed fourth — supporting, not load-bearing.

*Validation note (v4):* the discipline that makes the conjunction safe is exercised on real
cross-host non-coherence via RDMA (W5) — RDMA remote memory is non-coherent in the same way CXL is
— so "we never tested the non-coherence discipline in silicon" is false. What we do not test is the
*integration* of the discipline with the CXL substrate across hosts; that is the disclosed gap (I.6),
not the correctness argument.

**I.2 T/R regime model — demoted, not deleted.** Keep as analytical vocabulary:
"a hold buffer is viable only when T/R ≫ 1, which both RDMA (~15) and CXL (~30) clear in
isolation; CXL additionally holds T/R load-*independently* because polling shares no endpoint with
payload." Extend the model from jitter to failures: repair round = detect + retransmit +
re-sequence. Validated empirically in E6.

**I.3 Concessions (one sentence each, early).**
- Data/metadata separation: Corfu, LazyLog, Scalog, SOCRATES all do it. Not our claim.
- Sequencer-side dedup + producer fencing: concept is Kafka's idempotent/transactional producer
  (PID/epoch/seq). Our claim is *placement, cost, and setting* (see I.4), not the concept.
- Failure independence in isolation: the RDMA-disaggregated-memory literature (FaRM, Clover, Clio,
  LegoOS, AIFM). Our claim is the *conjunction* with no-shared-endpoint, which that literature
  cannot hold simultaneously (I.1).

**I.4 Claims (what is actually novel) — two contributions plus corollaries.**
1. **The first per-session FIFO across striped multi-broker ingestion at WBO throughput**, enabled
   by the conjunction thesis (memory-speed, state-complete repair).
   *Kafka-partition rebuttal, stated verbatim in the paper:* Kafka's idempotent-producer FIFO is
   per-*partition*, and a partition has a single leader broker — so Kafka's per-producer FIFO **is
   sticky routing**; a producer writing across partitions gets no cross-partition order at all.
   Embarcadero provides submission-order FIFO while **striping every batch across all brokers** —
   structurally impossible in Kafka and in every WBO baseline evaluated. The contract *shape* is
   Kafka-like (hence we cite it); the *setting* is not.
2. **The coherence-free coordination discipline including fencing on non-coherent memory**
   (single-writer/monotonic/poll; N>T ABA safety; spatial guard + wrap-fence with no bounded-flush
   assumption — reviewer C already praised these). **Validated in silicon on real cross-host RDMA
   non-coherence (W5) and machine-checked (W2)**, so this claim does not depend on multi-host CXL.
- *Corollaries (framed as such, not as parallel novelty):* lease-based failure detection on
  non-coherent memory with the principle *detection is a performance hint, never a safety input*
  (scoped to brokers — see D2); and the transport-vs-architecture-vs-substrate decomposition
  methodology on real CXL.

**I.5 Related-work additions.** **RDMA disaggregated memory (FaRM, Clover, Clio, LegoOS, AIFM) —
mandatory new paragraph**: these provide failure independence but not the conjunction; a memory
server reintroduces the shared endpoint (I.1). PolarDB_CXL (software coherence for buffer pools
over a switch vs. coherence-free ordering pipeline — complementary; borrow their switch latencies
for the sensitivity model), Rowan (OSDI'23), Swarm (SOSP'24), HydraRPC (ATC'24), Kafka
EOS/idempotent producer, 1Pipe / NetChain / Eris-line network sequencing, Aurora / SOCRATES /
LogStore. Comparison table: system | ordering point | FIFO guarantee | failure-repair cost |
substrate | **leg 1?** | **leg 2?** (the last two columns make the conjunction visible at a glance).

**I.6 Limitations section (mandatory; reviewers A & B).** CXL-pod / single-rack scope with a
concrete multi-pod sketch (Embarcadero as fast intra-pod tier under a Scalog-style cross-pod cut);
switch-hop latency sensitivity (show T/R survives 3× load latency; cite XConn/PolarDB numbers);
**the multi-host-CXL gap, owned explicitly (v4):** we validate the coherence-free discipline in
silicon on cross-host RDMA (same non-coherence hazard family), the CXL substrate's latency/
bandwidth/TLP-atomicity single-host, and the protocol via TLA+ — we do **not** run the full system
over *shared* CXL across hosts, because accessible multi-host CXL does not exist to us. State which
enforcement primitive is tested where (clwb 64 B TLP atomicity: single-host CXL; distributed
discipline: cross-host RDMA; interleavings: TLA+) and that multi-host CXL integration is future
work, not a validated result. Invoke the LegoOS / pre-Optane-PM precedent: emerging-hardware systems
earn acceptance on faithful validation of the mechanism on available components, honestly disclosed.
Then: ACK1 speculative-read window; sequencer as a managed single point with measured failover MTTR;
GC pressure; client-library complexity.

**I.7 Use cases paragraph (reviewer B's "where would this be deployed").** In-rack SMR/consensus
backend (etcd-class metadata services); distributed DB WAL within a rack (log-is-the-database
lineage, now over CXL); message bus for disaggregated DB components (PolarDB_CXL's own setting);
in-pod event/feature pipelines.

**I.8 Scope discipline — explicitly out.** No multi-sequencer / partitioned GOI (future-work
paragraph only); no geo-replication; no CXL 3.0 coherence speculation; in-fabric direct-writing
clients = future-work sketch only. The paper's risk profile is complexity, not thinness.

---

## Part II. Design changes

### D1. Session-based FIFO protocol (the semantic fix)
- Client opens a session → `(client_id, session_epoch)`; per-session batch sequence numbers.
- Client library buffers unACKed batches; retransmits batch *n* to a **different** broker.
  **Adaptive δ** (panel #5), TCP-RTO-style: δ = SRTT + 4·RTTVAR on observed ACK latency, with the
  lease-driven NACK (D2) as the **primary** retransmit trigger and the timer as a **backstop for
  silent loss**. Safety never depends on the detector — a plain timer suffices; leases only shrink
  the constant. Survivor selection by **rendezvous hashing on `(client_id, batch_seq)`** to spread
  the retransmit herd across survivors (panel #5).
- Sequencer tracks `expected_seq` per session; commits a session's batches only in order; rejects
  `seq < expected` (dedup) and never commits past a gap.
- **Hold expiry ⇒ session fencing, never gap-skip.** A gap that outlives retransmission for the
  full lease means the client is dead/partitioned: fence the session; the uncommitted suffix
  never appears. Reconnecting client receives `SESSION_FENCED`.
- **Session HWM folded into the `SESSION_FENCED` payload** (self-audit yield, panel #8): the fence
  response carries the per-session committed high-water mark, so a fenced-but-live client resubmits
  *exactly* the uncommitted suffix under its new session → exactly-once across fencing with **no
  separate HWM-query mechanism**. Clients that lose their own state need app-level idempotence
  (same as every system; cite Kafka).
- **Guarantee theorem (boxed in §2 of the paper) — states BOTH properties + composition + the one
  divergence (panel #11):**
  > *(Total order)* The committed GOI is a single total order.
  > *(Per-session prefix)* Each session's committed batches embed in that order as a submission-
  > order prefix of its stream; a batch is ACKed iff committed; late and duplicate arrivals are
  > rejected, never reordered.
  > *(Reader agreement)* Speculative (ACK1) and durable-frontier readers observe prefixes of the
  > same total order; positions never reorder. **Sole divergence:** under CXL-module loss in the
  > ACK1→ACK2 window, a speculative reader may have delivered a payload at position *k* that a
  > durable reader later observes as `ERR_DATA_LOSS` at *k* — payload *visibility* differs, order
  > does not.
  `Hole` and `Causal_Late` cease to exist.
- **Honest availability statement (panel Flag 2):** a gapped session stalls until retransmit
  (~δ) or fence; when clients stripe across all brokers, a broker kill gaps **essentially every**
  active session — the win is *duration* (≈δ, ~10 ms vs 660 ms) and *independent per-session
  recovery*, plus a **bounded reconvergence transient** on survivors (D4 removes the GOI-reservation
  burst; the *ingestion* reconvergence is measured, not eliminated — E4a). Never "only one stream
  stalls."
- Design-alternatives paragraph: NACK-and-resume instead of fencing (rejected: retransmit already
  covers transient loss; expiry ⇒ client truly unreachable ⇒ fencing is right); dual-write to two
  brokers upfront (Aurora-style; 2× ingress cost); sticky routing (measured in E5).

### D2. CXL heartbeat leases (the availability corollary — separate from D1)
- Each broker increments a monotonic heartbeat counter line every ~100 µs (single-writer,
  monotonic — existing discipline, no new machinery).
- Sequencer (already polling at ~50 µs rounds) suspects after k misses (~1 ms vs 660 ms TCP);
  drives the D1 NACK.
- **Principle, correctly scoped (panel #6):** *failure detection is a performance hint, never a
  safety input* — **for brokers**. A false broker suspicion produces a duplicate that dedup
  discards (measured E4c). For the **sequencer**, a false-positive election is likewise **safe with
  no temporal assumption** (epoch tags + spatial guard, the paper's pride); what depends on the
  ~seconds out-of-band fence is **GOI-reclamation liveness only** (the wrap-fence), not safety.
  Disclose the sequencer slow path; keep safety/liveness sharply distinct.
- **New correctness requirement — stale-CV ACK relay (found chasing panel #6):** if `S_old`, after
  election, advances CV entries for batches whose GOI entries readers will discard by epoch, a
  broker could relay **ACK1 for an entry that never becomes visible**, violating the theorem's
  "ACKed iff committed." **Requirement: brokers validate the active control-block epoch on the ACK
  relay path** before advancing any client ACK. This interleaving is in the W2 TLA⁺ list.
- **Redirect-notice push demoted to an evaluated optimization** (self-audit yield, panel #8): the
  adaptive timer (D1) suffices for correctness; surviving brokers pushing redirect notices only
  shrinks the constant. Client library keeps ≥2 warm broker connections regardless.
- Novelty note: lease-based membership on non-coherent shared memory has not been shown; it drops
  out of the single-writer/monotonic discipline for free.

### D3. Commit-driven PBR frontier + emergent flow control (correctness requirement + deletion)
- **Requirement (panel review):** the hold buffer is volatile sequencer process memory
  (`src/embarlet/topic.h:882`, `buffer_manager.cc` skipped-batch maps). For sequencer-failover
  safety, held batches must be **reconstructible from the PBRs**: a PBR slot must not be reclaimed
  at scan time — only at commit. `S_new` rescans from the committed frontier.
- **Consequence — delete per-session credit windows:** with commit-driven frontiers, total
  uncommitted descriptors ≤ aggregate PBR capacity (4 brokers × 1024 slots × 128 B ≈ 512 KB — the
  hold buffer provably fits in L2). Backpressure emerges naturally: a gapped session holds its PBR
  slots → ring fills → broker stops accepting → TCP pushes back to clients. One provisioning
  inequality replaces a mechanism: **session lease < PBR fill time**.
- Note the fairness coupling honestly: a fenced-timescale stall can fill a ring and couple other
  sessions on that broker; the lease < fill-time inequality is what prevents it. State it.

### D4. GOI assignment at release, not reservation at collection
- Held batches are **excluded** from the epoch's `fetch_add`; sequence numbers are assigned at
  *release* (commit) time. Removes the backlog-burst artifact in §7.4 and keeps `committed_seq`
  monotonic for the spatial guard.
- Interacts with failover: the named TLA⁺ scenario is *a retransmitted duplicate arriving at
  `S_new` inside the spatial-guard window*. Model it first, code second.
- Implementation locus: the triple-buffered epoch collector path in `src/embarlet/topic.cc`.
  Feasibility check is a week-1 item (W1).

### D5. ACK1/ACK2: two boxed contracts, split by fault domain
- **Broker/sequencer failure:** no committed data or order is ever lost (metadata chain-replicated
  across two modules; payloads survive in CXL; sessions repair per D1/D2).
- **CXL-module failure in the ACK1→ACK2 window:** order survives (GOI replicated); payload may not
  — surfaced as `ERR_DATA_LOSS` at the preserved position, never silently (this is the theorem's
  sole divergence case, D1).
- **Subscribers default to the durable frontier** (min of order and replication frontiers);
  speculative ACK1-reads are opt-in per subscription. Default-safe, opt-in-fast.
- Quantify the window (µs-scale) and state what an application may assume at each level — the direct
  answer to reviewer C's fourth question.

### D6. Garbage collection / trim (reviewer E caught its absence)
- Per-subscriber consume cursors in a CV-like single-writer region.
- GC frontier = min(subscriber cursors, replication frontier); Blog reclaimed as a ring below the
  frontier; GOI reclamation gated by the existing wrap-fence (completes that section's story). One
  subsection; without it the system reads as a 48-second demo. (Table-stakes machinery, not a
  novelty line — see D9 self-audit.)

### D7. Sequencer failover completeness
- Session tables (`expected_seq`, committed HWM) chain-replicated with the rest of the metadata;
  `S_new` recovery = read tables + rescan PBRs from committed frontiers (possible because of D3).
- Broker rejoin: fresh broker epoch; **never reuse a dead broker's PBR region** (allocate fresh);
  zombie-broker delayed flushes land in quarantined regions.
- Measure failover MTTR (E4b).

### D8. Answers reviewers asked for that are design content
- **Structure-ownership table** (single highest-value page for reviewing experience):
  structure | size & sizing rule | single writer | readers | flush discipline | failure handling
  — covering Blog, PBR, GOI, CV, control block, heartbeat lines, session tables, subscriber cursors.
- Sizing models: Blog = ingest rate × GC horizon; PBR = (N > T) slots × 128 B; GOI 16 GB ≈ 48 s at
  peak → tied to GC + wrap-fence.
- Polling economics (reviewer D): 4 PBRs at 50 µs rounds ≈ 80 K loads/s × 64 B ≈ **5 MB/s against
  21 GB/s** — one sentence defuses "isn't polling overhead significant?" Report sequencer CPU at
  idle/peak and the adaptive spin→pause policy (E8).

### D9. Mechanism self-audit table (enforces the complexity budget — panel #8)
A table in the paper (and maintained in-repo): **mechanism → the one guarantee/availability line it
serves → keep/cut/demote**. Rules: a mechanism that maps to no contract line is cut; evaluation
infrastructure (RDMA variants, injection harness) and table-stakes (GC) are labeled as such, not
counted as system novelty. Net complexity vs. v1 is reported honestly: **−5 deleted anomaly
mechanisms** (hold-timeout semantics, `Hole`, `Causal_Late`, per-entry expiry, idle-armed force-
expiry) replaced by recognizable timers/dedup/fencing; **−2 further deletions from this audit**
(session HWM folded into `SESSION_FENCED`, D1; redirect push demoted to optimization, D2). Ship the
table; do not assert the budget.

---

## Part III. Implementation work items

### W1. Week-1 verification + on-paper closure (before any code beyond this list)
**Code/measurement checks:**
1. **Coupled repair item (panel #5):** δ, the repair-time headline, and the E2E tail are **one
   problem**. Our E2E P99 is 86.7 ms vs 745 µs append P50 — a 100× gap. The 1.0 Mops/s KV result
   proves delivery *can* be fast, so likely a subscriber poll-scheduling/batching artifact.
   Diagnose and fix (or fully explain) **before writing a word**; δ cannot be set until the append
   P99.9-under-load is known (else clients spuriously retransmit in steady state).
2. **In-order ACK invariant:** confirm the sequencer commits per-session batches strictly in order
   and ACKs only on GOI commit, including across epoch-collector seals. If violated, D1's fencing
   needs rework at the collector layer.
3. **GOI assign-at-release feasibility** in the triple-buffered collector (D4).
4. **PBR frontier semantics:** scan-driven or commit-driven today? (`src/embarlet/topic.cc`.)
   Determines the size of the D3 change.

**On-paper closures due within two weeks (panel meta-directive; no code needed):**
5. Conjunction-thesis rewrite + RDMA-disaggregated-memory engagement (I.1, I.5).
6. Two-variant W5 redesign spec (below).
7. Two-claim novelty compression + Kafka-partition rebuttal (I.4).
8. SLO-**curve** substitution + per-baseline knee predictions (IV.1, E3).
9. Ex ante porting rule (W4).
10. Mechanism self-audit table (D9).
11. Add the stale-CV ACK-relay scenario to the TLA⁺ list (D2, W2).
Commit the IV.1 predictions file at the end of week 1.

### W2. TLA⁺ specification (weeks 1–3; artifact)
- State machine: sessions, PBR publication (two-phase, ABA guard), hold/fence, dedup, GOI commit,
  spatial guard + wrap-fence, leases.
- Scenario list (minimum): broker crash pre/post PBR publication; retransmit races; **duplicate
  arriving at `S_new` inside the guard window** (the named one, D4); lease false positive
  (SIGSTOP); **stale-CV ACK relay after election** (new, D2); client crash mid-stream; sequencer
  failover during an open gap; module failure between ACK1/ACK2.
- Deliverables: spec + model-check configs in repo; cited in the paper; part of the artifact.

### W3. Core protocol implementation (weeks 2–6)
- Sequencer: session tables + dedup (one comparison on the fast path), suffix fencing, D3 frontier
  change, D4 assign-at-release, lease monitor, broker-status control block, **control-block epoch
  check on the ACK relay path** (D2). **Fast path must not regress** — before/after microbenchmark
  required (E8).
- Client library: sessions, unACKed-batch buffer, adaptive-δ retransmit (SRTT/RTTVAR), rendezvous-
  hash survivor selection, ≥2 warm broker connections, `SESSION_FENCED`+HWM-in-payload recovery.
- Broker: heartbeat writer; fresh-PBR-region rejoin; ACK-path epoch validation. (Redirect-notice
  push only if time allows — it is a demoted optimization, D2.)

### W4. Baseline CXL-transport ports (weeks 2–6, parallel owner) — governed by the porting rule
- **Ex ante porting rule (panel #4, write down before any port):** *protocol-preserving
  substitution only* — same messages, same state machine, transport swapped. Anything that alters
  a baseline's serialization structure is a **redesign, not a port**, and is forbidden. This keeps
  the E1 "baseline-on-CXL" leg from being a free parameter that can be tuned into
  "Embarcadero-minus-hold-buffer," which would make the waterfall definitionally meaningless.
- Shared-memory mailbox RPC built from *our own* primitives (single-writer rings, clwb/sfence) for:
  Corfu token path, Scalog cut/report path, LazyLog coordinator path — transport swap only.
- **Calibration (panel #10):** show each reimplementation-on-TCP reproduces its original paper's
  published numbers within a stated tolerance on comparable hardware → converts "trust our port"
  into "our port matches theirs where theirs exists."
- **Baseline-author sanity check (panel #10):** email the LazyLog and Scalog authors to review the
  port descriptions; "baseline authors reviewed our adaptation" is one sentence worth an appendix.
- **Fairness appendix per baseline:** kept / changed / why. Open-source the ports.

### W5. RDMA variants — TWO, and now the PRIMARY in-silicon evidence (weeks 5–9; panel #1, v4)
The RDMA variants are **promoted from ablation to primary evidence** (v4): with no accessible
multi-host CXL, they are the only place the coherence-free discipline runs against *real* cross-host
non-coherence, and the only place the conjunction is *measured*. Both run **genuinely cross-host**
on c2/c4/moscxl ConnectX-5 hardware. RDMA remote memory is non-coherent in the same hazard family as
CXL (stale/torn reads, no cheap cross-host atomics), so the single-writer/monotonic/poll/fence
discipline is exercised in silicon here — this is what closes the "non-coherence never tested" risk.
- **W5-A — Blogs in broker DRAM (loses leg 1).** Same architecture, control plane over one-sided
  verbs (PBR polling via RDMA READ, leases via RDMA WRITE). Kill the broker *host* → its DRAM Blog
  dies with it → measure data loss / re-replication cost. This is the **leg-1 failure contrast**.
- **W5-B — Blogs on a dedicated memory server (loses leg 2).** State survives host death, but every
  payload write, sequencer poll, and replica/subscriber read funnels through the memory server's
  NIC → measure the **leg-2 funnel**: bandwidth ceiling at the memserver NIC, poll/payload CQ
  contention, latency inflation under load. This is the direct, on-owned-hardware proof that
  "failure independence over RDMA costs the shared endpoint."
- Purposes: (a) waterfall leg 3 (E1) uses W5-A; (b) R-inflation-under-load / T/R load dependence
  (E6) uses both; (c) the **conjunction contrast** (E4e) uses the pair.
- Also reproduce reviewer D's ~30 M tokens/s naive RDMA sequencer and co-opt it: token *rate* was
  never the binding constraint; the token RTT on the client critical path and the repair loop are.

### W6. Non-coherence fault-injection harness (weeks 4–8; adversarial stress beyond RDMA)
- Interpose at the CXL accessor layer: delayed-flush injection, stale-read injection, torn-window
  adversary. Runs the full pipeline under adversarial schedules **worse than RDMA naturally
  produces** — the point is to widen the stale/torn windows past anything the hardware would emit.
- **Role clarified (v4):** real cross-host non-coherence is now covered *in silicon* by the RDMA
  variants (W5); the injection harness is the *adversarial* supplement that pushes the discipline
  past benign hardware behavior. It still validates against our own fault model and cannot surface a
  truly unmodeled behavior (panel #7 circularity) — state that — but paired with W5's real silicon
  non-coherence, the combined story is "real non-coherence (RDMA) + adversarial non-coherence
  (injection) + machine-checked interleavings (TLA+)," materially stronger than injection alone.

### W7. Multi-host CXL access pursuit (background; pure upside, NOT a gate — v4)
- The paper does not depend on this. Pursue it anyway as a credibility bump, low-priority and in
  parallel: XConn Apollo switch dev systems; Samsung CMM-B / MemVerge pooled-memory boxes; Samsung
  MSL and SK hynix academic programs; direct contact with PolarDB-CXL authors; CXL consortium
  interop labs. Berkeley affiliation opens most doors; the ask is weeks of remote access, not
  ownership. (Note for reviewers: multi-host CXL 2.0 *exists* — XConn switches; the PolarDB-CXL
  team built one — so the honest framing is "access-gated," not "impossible.")
- If it lands *before freeze*: add a real cross-host CXL data point — host-level failure
  independence, lease timing across a switch, switch-hop latency. This is the single biggest
  credibility upgrade available and turns "discipline validated on RDMA" into "validated on both."
- If it never lands (the base case): the paper is complete on owned hardware. No venue decision,
  no scope change, no "which paper exists" fork depends on it. The old week-8 existential gate is
  **deleted**.

### W8. Applications (weeks 8–12)
- YCSB A–F over the SMR-KV against CXL-Corfu / CXL-LazyLog backends; client-skew (Zipf) and
  key-skew variants. Optional second app (etcd-style metadata service) if time allows.

---

## Part IV. Evaluation plan

### IV.0 Methodology rules (stated in the eval intro; never broken)
1. Every number = median ± range of ≥3 trials (5 for tail claims). No single-trial cells, ever.
2. No headline includes a co-located publisher. Module-saturation is a clearly-labeled ceiling
   microbenchmark only.
3. Every baseline gets a **protocol-preserving** CXL adaptation (W4 rule), calibrated against
   original numbers and documented in the fairness appendix.
4. msgs/s and GB/s both reported; message-size sweep for headline experiments.
5. Latency reported as latency-vs-load curves at fractions of each system's own peak — no
   past-saturation tails.
6. One script per figure, committed; artifact evaluation planned from the start.

### IV.1 Pre-registered predictions (commit to repo BEFORE running E1/E2; panels Flag 4 + #3 + #4)
- Raw throughput vs CXL-upgraded baselines compresses to **~1.2–1.6×** (Table 4's 0.92–1.14×
  epoch-vs-batch result and the N=2 all-remote gaps both point here).
- **Low-load latency is largely transport** (baselines-on-CXL close most of it — predict it
  openly); **the knee is architecture.** Pre-register **per-baseline knee locations**: where each
  latency-vs-load curve turns up (Corfu at token-serialization saturation; Scalog/LazyLog at the
  cut-cadence floor).
- The architectural signatures: (a) knee arrives far later for Embarcadero (no token RTT, no cut
  cadence on the ACK path); (b) failure behavior (repair ≈δ, per-session, vs seal/cut and
  reconfiguration stalls); (c) per-ordered-message atomic/CPU cost; (d) the leg-1/leg-2 conjunction
  contrast (E4e).
- **Headline is a curve, not a threshold (panel #3):** report **throughput vs. SLO threshold across
  1–100 ms** for every system. No single number to game; the crossover point is an honest result.
  Raw throughput is supporting material.

### IV.2 Experiment matrix
- **E1 — Decomposition waterfall (fairness instrument).** Four legs: baseline-on-TCP →
  baseline-on-CXL-transport (W4, **porting-rule-constrained**) → Embarcadero-on-RDMA (W5-A) →
  Embarcadero-on-CXL. Factors transport vs architecture vs substrate gain. **Explicit note in the
  plan and paper: E1 can shoot the author** — if baseline-on-CXL closes the knee too, the SLO
  headline is transport, and the paper retreats to E4/E6 (a narrower, hardware-dependent paper).
  Publish whatever it says.
- **E2 — All-remote throughput, message-size sweep.** N ∈ {1,2,3+} remote publishers (add client
  machines or bond NICs to remove the ingress cap); sizes {128 B, 512 B, 1 KB, 4 KB, 16 KB};
  msgs/s and GB/s.
- **E3 — Latency vs load + SLO curve (headline).** P50/P99/P99.9 at 10/50/70/90% of each system's
  peak; derive the **throughput-vs-SLO-threshold curve (1–100 ms)** per system. Append AND
  end-to-end (E2E fixed per W1.1). Overlay pre-registered knee predictions.
- **E4 — Failure suite (thesis instrument).**
  (a) Broker kill, M independent client sessions: **all-session** stall-duration distribution
      (expect ≈δ, vs 660 ms today), independent recovery, **plus the reconvergence-transient
      measurement on survivors with a pre-registered predicted shape** (panel #5); no global
      zero-throughput hole;
  (b) sequencer failover MTTR incl. session-table recovery and PBR rescan;
  (c) SIGSTOP broker (lease false positive): safety via dedup, duplicate-rate overhead;
  (d) client crash: suffix fenced, never delivered; fenced-client HWM-in-payload recovery;
  (e) **conjunction contrast (the money figure, panel #1):** W5-A RDMA broker-*host* kill (DRAM
      Blog lost; measure repair/loss — the leg-1 cost) vs. W5-B RDMA memory-server funnel
      (bandwidth ceiling, poll/payload contention — the leg-2 cost) vs. CXL (neither cost).
      CXL side shown as **process-level** failure independence + single-host substrate numbers
      (host-level CXL is upside only, if W7 lands); label the process-vs-host distinction honestly.
      The leg-1/leg-2 *contrast itself* is fully measured on owned RDMA and carries the thesis;
  (f) **baselines under failure:** Scalog shard failure (seal/cut stall), Corfu reconfiguration —
      render their failure behavior visible; flips §7.4 from our weakest section to theirs.
- **E5 — FIFO design-space study** (replaces the >1341× strawman): sticky routing (uniform +
  Zipf-skewed clients; behavior across a broker failover) vs client stop-and-wait (one line, naive
  bound) vs Embarcadero native. Throughput, tails, failover behavior.
- **E6 — Model validation.** tc-netem jitter injection swept against the hold window: predicted vs
  observed repair success as jitter → T; **both RDMA variants'** R inflation under payload load
  (T/R load-dependence — W5-B is where it bites hardest). Elevates the regime model to measurement.
- **E7 — Applications.** YCSB A–F on SMR-KV vs CXL-Corfu/CXL-LazyLog; ops/s + latency; both skew
  axes.
- **E8 — Overheads.** Polling CPU + CXL bandwidth (~5 MB/s vs 21 GB/s); sequencer CPU idle/peak;
  hold-buffer occupancy under jitter (bounded by PBR capacity per D3 — show it); **spurious-
  retransmit-rate vs δ curve** (panel #5); fast-path before/after regression for the session
  machinery.
- **E9 — Sensitivity.** RF ∈ {1,2,3} with the explicit fault-model argument for in-rack RF=2 +
  dual-module metadata (Aurora's 6 is cross-AZ — different fault model; say so); epoch/round
  interval (why 500 µs); session-lease sweep incl. lease < PBR-fill inequality; switch-hop latency
  model (T/R at 1×/2×/3× load latency).
- **E10 — Sequencer scaling v2.** Against the batched CXL-Corfu sequencer (the honest baseline
  reviewer C demanded) and the reproduced RDMA token server; per-message-mutex strawman deleted.
  Keep the per-batch vs per-epoch ablation with its true framing: 27,000–53,000× fewer atomics and
  6–17% P99, not throughput.

### IV.3 Headline decision tree (after E1–E4 land, ~week 12)
- SLO-curve dominance across the range + failure suite strong (expected) → headline as
  pre-registered (curve + E4).
- Small-message msgs/s gap > 2× all-remote → add as secondary headline.
- Waterfall shows substrate ≫ architecture (E1 shoots us) → lean on E4e (conjunction contrast) +
  E6; the honesty of the decomposition carries the section. **State that this is a narrower framing**
  (substrate-comparative), still fully supported on owned hardware.
- Any result contradicting a pre-registered prediction → report it and revise the claim, not the
  experiment.

---

## Part V. Writing plan

### V.1 Paper structure (v2)
1. **Intro** — the conjunction thesis; two independence properties + why RDMA can't hold both;
   contributions sized to what's measured (two claims + corollaries).
2. **Problem & guarantees** — Properties 1/2; the **boxed theorem (both properties + composition +
   the one divergence)** up front; the ACK1/ACK2 contracts; what is explicitly not guaranteed.
3. **The repair model** — T/R extended to failures (necessary-condition framing); substrate table
   with **leg-1/leg-2 columns** and the memory-server rebuttal pre-answered by E4e.
4. **Design** — discipline invariants; pipeline; **pseudocode for broker / sequencer / replica /
   repair loops**; sessions; leases (broker-scoped hint principle); frontier + assign-at-release;
   structure-ownership table; **mechanism self-audit table**.
5. **Failure handling** — fencing (spatial guard + wrap-fence, kept nearly as-is — it was praised);
   session/failover recovery; stale-CV epoch check; GC; TLA⁺ summary.
6. **Implementation** — incl. the fault-injection harness (with circularity caveat) and testbed
   description (consistent machine naming).
7. **Evaluation** — rules first; E1 waterfall; E3 SLO curve; E4 failure suite (E4e = money figure);
   the rest.
8. **Related work** — comparison table w/ leg columns; RDMA-disaggregated-memory + PolarDB_CXL.
9. **Limitations** (I.6). 10. Conclusion.

### V.2 Presentation artifacts (each answers a named review complaint)
- Boxed theorem, both properties + divergence (PC issue #1; reviewers C & panel #11).
- Structure-ownership/flush table (reviewers A, E).
- **Mechanism self-audit table** (panel #8 — makes the complexity honest and visible).
- Pseudocode ×4 (reviewer E).
- Substrate table with leg-1/leg-2 columns (panel #1).
- Glossary; **rename batching "epoch" → "round"** (E's dual-use confusion); unify Broker vs Server;
  neutral machine names (host-A/B/C) defined once (B's setup-consistency complaint).
- Grayscale-legible, larger figures (B's nit).
- Every term defined before first use — audit against reviewer E's list verbatim.
- **Abstract/intro claim precision (v4):** claim the architecture is *demonstrated over
  RDMA-disaggregated memory and characterized on real CXL*, and that multi-host CXL is the substrate
  it is designed for — never imply the full system ran over multi-host CXL. One sentence, load-bearing
  for integrity; a reviewer who catches an overclaim here discounts everything else.

### V.3 Reviewer-question traceability (the next draft answers every question in-text)
- C-Q1 (transport gap) → E1 waterfall (porting-rule-constrained) + I.1 conjunction framing.
- C-Q2 (co-location) → IV.0 rule 2 + E2.
- C-Q3 (FIFO vs availability knob) → D1/D2 separation + E4a + honest striping/reconvergence.
- C-Q4 (ACK1 window semantics) → D5 + the theorem's divergence clause.
- C (restart/retry order) → D1 session HWM in `SESSION_FENCED` payload.
- C (single-trial P99, strawmen) → IV.0 rules 1/5; E10; E5.
- D (RDMA sequencer rate; polling overhead; msgs/s; Rowan/Swarm; **why not RDMA**) → W5-A/B +
  E4e conjunction contrast; D8 polling economics; IV.0 rule 4; I.5.
- B (applicability, PolarDB, principles novelty, YCSB, skew, RF) → I.6/I.7; I.5; I.3/I.4; E7; E5; E9.
- A (limitations, fairness judgment, config details, term sloppiness) → I.6; W4 rule+calibration
  +author-check+appendix; D8 table; V.2.
- E (comprehensibility) → V.1/V.2 wholesale.

### V.4 Review process
- Two external cold reads (one systems generalist, one shared-log expert) at weeks 16–18.
- Internal red-team playing the D-archetype: attack the striping claim, the fencing semantics, the
  **waterfall legs (is any baseline-on-CXL a covert redesign?)**, and the **conjunction claim (is
  W5-B a fair memory-server, or a strawman funnel?)**.

---

## Part VI. Schedule, gates, risks

### VI.1 Timeline (target OSDI'27, ~Dec 2026 deadline; today = July 2026)
| Weeks | Workstream |
|---|---|
| 1–2 | W1 checklist (coupled δ/E2E-tail item, ACK invariant, assign-at-release, PBR frontier) **+ the seven on-paper closures**; start W2 TLA⁺; start W7 hardware pursuit; commit IV.1 predictions file |
| 2–6 | W3 core protocol (owner 1); W4 ports under the porting rule + calibration (owner 2); W2 TLA⁺ continues |
| 4–8 | W6 fault-injection harness |
| 5–9 | W5-A **and** W5-B RDMA variants |
| 8 | (No hardware gate — v4.) W5-A/W5-B cross-host RDMA results in; discipline-in-silicon confirmed; checkpoint eval progress |
| 8–14 | Evaluation matrix E1–E10 (scripts-per-figure); W8 apps |
| ~12 | Headline decision tree (IV.3) |
| 12–16 | Writing (structure V.1); figures |
| 16–18 | External cold reads; fairness appendix polish; artifact packaging |
| 18 | Freeze; submit |

**Scope-cut gate (added).** If at **week 10** the E1–E10 matrix is not on track, cut in this order:
E9 sensitivity extras → E7 second app → E5 skew variants. Do **not** cut E1, E4e (leg-1/leg-2
contrast), E6, or **either RDMA variant (W5-A/W5-B)** — those carry the thesis in the absence of
multi-host CXL. W5-B is now load-bearing, so it is no longer a scope-cut candidate.

### VI.2 Gates
- **Week 2:** W1 items pass or the design plan is amended (esp. if the in-order-ACK invariant is
  violated in the collector — D1 rework trigger).
- **Week 8:** W5 cross-host RDMA results confirm the discipline runs in silicon and the conjunction
  is measurable. If not, the *evidence base* is at risk — escalate. **No hardware/venue gate here
  anymore (v4).**
- **Week 10:** eval-matrix scope-cut gate (above).
- **Week 12:** headline decision from data, per the pre-registered tree.

### VI.3 Risk register
| Risk | Likelihood | Mitigation |
|---|---|---|
| Throughput gap compresses vs upgraded baselines | High (expected) | Pre-registered SLO **curve** + failure suite carry the paper (IV.1) |
| **E1 shoots us (substrate ≫ architecture; SLO win is transport)** | Medium | Porting rule bounds it (W4); fallback is the narrower conjunction/E4e paper (IV.3, W7) |
| Multi-host CXL unavailable (the base case, v4) | Certain | Not fatal, not a gate: discipline validated in silicon on cross-host RDMA (W5), CXL substrate proven single-host, protocol machine-checked; residual integration gap owned in I.6 (LegoOS / pre-Optane precedent) |
| Hardware-purist reviewer rejects "CXL paper w/o multi-host CXL" | Medium | Minimize to a minority view: the conjunction makes CXL *necessary* (RDMA can't hold both legs, measured E4e); real single-host CXL numbers; honest Limitations; round 1 already tolerated single-host. Cannot be zeroed — accept as residual |
| E2E 87 ms tail is fundamental, not a bug | Low-Med | Diagnose week 1 (coupled to δ); if fundamental, own it and let the SLO curve show the crossover honestly |
| In-order-ACK invariant broken in collector | Low-Med | Week-1 check; D1 rework path identified (collector layer) |
| Assign-at-release × spatial-guard interleaving bug | Medium | TLA⁺ first, code second; named scenario in W2 |
| **Stale-CV ACK relay violates "ACKed iff committed"** | Medium | ACK-path epoch check (D2/W3); TLA⁺ scenario (W2) |
| W5-B dismissed as a strawman memory server | Medium | Build the best memory server we can (batched, kernel-bypass); red-team it (V.4); it only needs to show the *funnel is inherent*, not that it's badly built |
| Complexity re-triggers a presentation reject | Medium | Self-audit table (D9); net −7 mechanisms; theorem/tables/pseudocode compress; cold reads |
| Overlapping PC recognizes the paper | Certain | Every prior question answered in-text (V.3); guarantee visibly stronger; conjunction thesis + E4e are new; methodology visibly stricter |
| Timeline infeasible for OSDI'27 | **High** | Week-10 scope-cut gate; SOSP'27 is the realistic primary (hardware no longer gates venue, v4) — do not force a thin OSDI submission |

### VI.4 Panel corrections — compliance record
**Round 1 (v2):**
1. Flag 1 (T/R vs fault domain) → thesis off T/R; T/R demoted to model layer. (I.1, I.2)
2. Flag 2 (transformed, not gone) → guarantee as bounded per-session tradeoff; sessions vs
   leases/retransmit separated; striping claim corrected. (D1, D2, E4a)
3. Flag 3 (testbed is the gate) → week-8 go/no-go. (W7)
4. Flag 4 (throughput is a bet) → predictions pre-registered; failure suite elevated. (IV.1, E4)

**Round 2 (v3):**
1. **#1 (leg 1 alone fails; conjunction is the thesis):** thesis = leg 1 ∧ leg 2, proved by the
   two RDMA variants; RDMA-disaggregated-memory literature engaged. (Thesis, G2, I.1, I.5, W5,
   E4e)
2. **#2 (not "precisely Kafka"):** novelty compressed to two claims; Kafka-partition rebuttal
   stated verbatim; Kafka cite kept for dedup/fencing. (G1, I.3, I.4)
3. **#3 (SLO curve, not threshold):** headline is the 1–100 ms throughput-vs-SLO curve; W1.1 gates
   the curve's validity, not a number. (G3, IV.1, E3)
4. **#4 (knee is architecture; porting rule):** per-baseline knee predictions pre-registered; ex
   ante protocol-preserving porting rule; E1-can-shoot-the-author stated. (IV.1, W4, E1)
5. **#5 (thundering herd / δ coupling):** adaptive δ (SRTT+4·RTTVAR); lease-NACK primary, timer
   backstop; rendezvous-hash survivor selection; reconvergence-transient measured; δ/tail folded
   into one W1 item. (D1, W1, E4a, E8)
6. **#6 (hint-not-safety scoped; new bug):** principle scoped to brokers; sequencer false positive
   is safe (liveness-only fence); **stale-CV ACK-relay epoch check** added. (D2, W2, W3)
7. **#7 (testbed closes less; injection circular):** "mechanism validated, scale modeled"; W6
   circularity caveat; gate names both possible papers. (I.6, W6, W7)
8. **#8 (complexity budget):** self-audit table replaces assertion; net −7 mechanisms; two audit
   deletions (HWM into fence payload; redirect push demoted). (G5, D9, D1, D2)
9. **#10 (port discretion):** porting rule + calibration-against-originals + baseline-author
   sanity-check emails, instead of porting the original artifacts. (W4)
10. **#11 (theorem omits total order):** boxed theorem states both properties, composition, and the
    single ACK1→ACK2 divergence. (D1)

**Round 3 (v4) — the hardware reality:**
1. No accessible multi-host CXL → no load-bearing claim depends on it; the week-8 existential gate
   is deleted. (W7, VI.1, VI.2)
2. Discipline validated *in silicon* on cross-host RDMA, which is non-coherent in the same hazard
   family as CXL — this closes the "non-coherence never tested" landmine on owned hardware.
   (Thesis validation note, I.1, I.4, W5, W6)
3. CXL role reframed honestly: substrate numbers proven single-host; integration disclosed as the
   residual gap; LegoOS / pre-Optane precedent invoked; abstract must not overclaim. (I.6, V.2)
4. Venue is purely a schedule decision (SOSP'27 primary / OSDI'27 if schedule allows), never a
   hardware one; multi-host CXL access is pure upside. (W7, Target)

**Scoreboard calibration (v4).** Of the four "still open" items from the round-2 review:
#2 closes **on paper today** (Kafka-partition), #3 closes with the **curve substitution**, and the
**silicon question is resolved by v4** — the discipline is validated in silicon on cross-host RDMA
(W5), so it is no longer open; multi-host CXL is upside, not a gate. The one genuinely open item is
**#1's conjunction claim** — closable on paper in two weeks and *provable on hardware we already
own* via W5-A/W5-B. One open, and it is scheduled.

---

*One-line summary: the semantics are the capability, the conjunction is the thesis, the RDMA
variants are the in-silicon evidence (RDMA is non-coherent too, so the discipline is really tested),
single-host CXL supplies the substrate numbers, and multi-host CXL is upside — not a gate. Every
remaining open question is closable on paper or measurable on hardware we already own.*
