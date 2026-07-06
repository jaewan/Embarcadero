# Track 06 — Related work & positioning (Part I)

**Read `sessions/00-BACKGROUND.md` first.** Track id: `06-related-work`. Branch:
`session/06-related-work`. Pure writing + literature review — **no code, no testbed, start now.**

**Goal.** Draft the positioning spine: the two-independence-properties framing, honest concessions,
the novelty claims, the related-work additions + comparison table, the limitations section, the
use-cases paragraph, and the explicit scope. This is where the "new contribution" novelty marks get
earned back.

**Plan sections:** `Paper/improvement_plan.md` → **Part I in full** (I.1–I.8), plus G1/G2 (thesis),
I.4 (claims), VI.4 (panel flags). Read the current intro/related work first:
`Paper/Text/Sec1_Introduction.tex`, `Paper/Text/RelatedWork.tex`, `Paper/Text/Sec2_Background.tex`.

## You OWN (Paper `.tex` — these files only)
- `Paper/Text/RelatedWork.tex`
- A new `Paper/Text/Limitations.tex` (I.6) and `Paper/Text/UseCases.tex` (I.7) — or fold into
  existing sections; coordinate include order with the human (don't edit `Main.tex` structure
  without asking).
- Positioning edits to `Paper/Text/Sec1_Introduction.tex` **only if** Track 07 isn't also editing
  it this cycle — check the handoff notes; if contended, draft your intro spine in a new
  `Paper/Text/Intro_positioning_draft.tex` and let the human merge.

## Do NOT touch
- `src/`, `spec/`, `scripts/`, other tracks' `.tex` files, `Paper/improvement_plan.md`.
- The guarantee theorem / structure-ownership table / pseudocode / glossary — those are **Track 07**.

> **v4 framing — this is the load-bearing change.** The thesis is the **conjunction** (leg 1 ∧ leg 2),
> not "failure independence first." Failure independence *alone* is the entire RDMA-disaggregated-
> memory literature, so it is **not** the contribution. Read the plan's Thesis + v4 header
> (lines 20–76) and G2/I.1 before drafting. Items (5) and (7) below are your **W1 on-paper closures
> (~2-week deadline)** — do them first.

## Scope
- **I.1 the conjunction (intro spine) — closure #5:** lead with why cheap repair needs **two**
  properties *at once*: (leg 1) **failure independence** — log state outlives any host — *achievable
  over RDMA with a memory server, so not the thesis*; (leg 2) **no shared network endpoint** — every
  host reaches state via LD/ST, polling burns zero NIC/CQ — *achievable over RDMA only by putting
  Blogs in broker DRAM, which forfeits leg 1*. **The conjunction** is CXL-specific (passive memory:
  per-host load/store, no far-side CPU/OS/NIC) and is **measured** by W5-A vs W5-B (E4e). Raw
  200–500 ns latency is listed fourth. Pre-answer "dedicate a second NIC / use a memory server" — the
  memory-server rebuttal is now a *measured result* (W5-B), not a hand-wave.
- **I.2 T/R model demoted:** necessary condition both RDMA (~15) and CXL (~30) clear; CXL holds it
  load-independently because polling shares no endpoint with payload. Vocabulary, not the win.
- **I.3 concessions:** data/metadata separation (Corfu/LazyLog/Scalog/SOCRATES); sequencer-side
  dedup + producer fencing = Kafka's idempotent/transactional producer (our claim is *placement,
  cost, and setting*); **failure independence in isolation = the RDMA-disagg-memory literature**
  (FaRM/Clover/Clio/LegoOS/AIFM) — our claim is the *conjunction* they cannot hold at once.
- **I.4 claims — TWO contributions + corollaries (closure #7), word precisely:**
  1. **The first per-session FIFO across striped multi-broker ingestion at WBO throughput**, enabled
     by the conjunction. Include the **Kafka-partition rebuttal verbatim**: Kafka's idempotent-
     producer FIFO is per-*partition* with a single leader broker — i.e. **sticky routing**; a
     producer across partitions gets no cross-partition order. Embarcadero gives submission-order
     FIFO while **striping every batch across all brokers** — structurally impossible in Kafka and
     every WBO baseline. Contract *shape* is Kafka-like (cite it); *setting* is not.
  2. **The coherence-free coordination discipline incl. fencing on non-coherent memory** (single-
     writer/monotonic/poll; N>T ABA safety; spatial guard + wrap-fence) — **validated in silicon on
     cross-host RDMA (W5) + machine-checked (W2)**, so it does not depend on multi-host CXL.
  - *Corollaries (label as such, not parallel novelty):* lease-based failure detection on non-coherent
    memory (hint-not-safety, broker-scoped); the transport/architecture/substrate decomposition method.
- **I.5 related work:** **RDMA disaggregated memory (FaRM, Clover, Clio, LegoOS, AIFM) — MANDATORY
  new paragraph:** provides leg 1 but not the conjunction; a memory server reintroduces the shared
  endpoint. PolarDB_CXL (serious paragraph + borrow switch latencies), Rowan (OSDI'23), Swarm
  (SOSP'24), HydraRPC (ATC'24), Kafka EOS/idempotent producer, 1Pipe/NetChain/Eris, Aurora/SOCRATES/
  LogStore. **Comparison table:** system | ordering point | FIFO guarantee | failure-repair cost |
  substrate | **leg 1?** | **leg 2?** (the last two columns make the conjunction visible at a glance).
- **I.6 limitations** (mandatory): pod/single-rack scope + multi-pod sketch; switch-hop sensitivity;
  **the multi-host-CXL gap owned explicitly (v4):** state which primitive is tested where — clwb
  64 B TLP atomicity on single-host CXL; the distributed coherence-free discipline on cross-host
  RDMA (same non-coherence hazard family); interleavings in TLA⁺ — and that running the full system
  over *shared* CXL across hosts is future work, not a validated result (invoke the LegoOS /
  pre-Optane-PM precedent). Then: ACK1 speculative window; sequencer SPOF + measured failover MTTR;
  GC pressure; client-lib complexity.
- **I.7 use cases:** in-rack SMR/consensus backend; in-rack DB WAL; message bus for disaggregated DB
  components; in-pod event/feature pipelines.
- **I.8 scope (explicitly out):** no multi-sequencer/partitioned GOI; no geo-rep; no CXL 3.0
  coherence; in-fabric direct-writing clients = future-work sketch only.

## Coordinate
- Cite artifacts other tracks produce (TLA⁺ from 02, fairness appendix from 03) by reference; leave
  `\TODO{ref}` placeholders where numbers come from evaluation.
- Verify every citation exists in `Paper/` bib; add missing entries to the `.bib`.

## Done criteria
- All Part-I prose drafted in the owned `.tex` files; comparison table compiles; bib entries added;
  `\TODO`s flag data dependencies. Handoff note listing what you drafted and any intro-file contention
  with Track 07.
