# Track 06 — Related work & positioning — Handoff

Track id: `06-related-work`. Intended branch: `session/06-related-work` (see "Branch" below).
Date: 2026-07-05.

## What I drafted (all Part-I prose; both W1 on-paper closures done)

- **`Paper/Text/RelatedWork.tex` — full rewrite (closures #5 + #7).**
  Re-spined around the **conjunction** (leg 1 = failure independence, leg 2 = no shared
  network endpoint). New/expanded paragraphs: OBW logs; WBO logs; **mandatory
  RDMA-disaggregated-memory paragraph** (FaRM/DrTM/Clover/Clio/LegoOS/AIFM provide leg 1
  only — explicitly *not* our contribution); CXL systems & non-coherent coordination
  (DirectCXL/HydraRPC/PolarDB-MP/Pasha/Tigon — software-coherence vs our coherence-free
  pipeline); in-network sequencing (NetChain/NetPaxos/Harmon/1Pipe/Eris); **Kafka
  idempotent/transactional producer with the Kafka-partition rebuttal stated verbatim**
  ("per-partition FIFO *is* sticky routing; we stripe every batch across all brokers");
  log-is-the-database (Aurora/Socrates).
  **Comparison table `tab:sys-compare`** (system | ordering point | per-client FIFO |
  repair cost | substrate | **Leg 1** | **Leg 2**). Rows include the two Embarcadero/RDMA
  variants (A=broker DRAM → loses leg 1; B=memory server → loses leg 2) and
  Embarcadero/CXL (both). Only the CXL row has both checks — the conjunction thesis in
  one figure.

- **`Paper/Text/Sec1_Introduction.tex` — positioning re-spine (closure #5 + #7).**
  Inserted the repair→conjunction diagnosis; reframed the CXL paragraph around passive
  memory holding both legs; **demoted raw 200–500 ns latency to a supporting factor**;
  **pre-answered the "second NIC / memory server" rebuttal as a measured result (W5-B)**;
  replaced the commented-out contributions block with the **two precise contributions +
  corollaries** (per-session FIFO across striped ingestion; coherence-free discipline
  validated in silicon on RDMA + TLA⁺); added the **claim-precision sentence** (V.2):
  demonstrated over RDMA-disagg + characterized on single-host CXL, multi-host CXL is the
  disclosed residual gap — never implied the full system ran over multi-host CXL.

- **`Paper/Text/Limitations.tex` — NEW (I.6).** `\label{sec:limitations}`. Single-rack
  scope + multi-pod sketch; **"what is validated on which substrate"** (discipline→RDMA
  silicon; substrate latency/bw/TLP-atomicity→single-host CXL; interleavings→TLA⁺; full
  system over shared CXL = disclosed gap; LegoOS/pre-Optane precedent; "mechanism
  validated, scale modeled"); switch-hop sensitivity; ACK1 window; sequencer managed
  single point + failover MTTR; GC pressure; client-lib complexity.

- **`Paper/Text/UseCases.tex` — NEW (I.7 + I.8).** `\label{sec:usecases}`. Four use cases
  (in-rack SMR/consensus; in-rack DB WAL; disaggregated-DB message bus; in-pod
  event/feature pipelines) + explicit out-of-scope (no multi-sequencer/partitioned GOI;
  no geo-rep; no CXL 3.0 coherence; in-fabric direct-write clients = future sketch).

- **`Paper/bibliography.bib` — 12 new entries appended** (marked with a Track-06 header):
  `farm, drtm, clover, clio, legoos, aifm, hydrarpc, polardbmp, onepipe, eris, aurora,
  socrates`. All resolve under BibTeX (verified).

## Verified
- Local `pdflatex + bibtex` sanity-compile of a throwaway wrapper over the four Track-06
  `.tex` files: **exit 0, no errors, 0 undefined citations**, `tab:sys-compare` renders.
  (The C++ system can't build locally, but LaTeX can; wrapper + artifacts removed after.)
- All `\cref`/`\ref` targets I used (`sec:eval`, `sec:design`, `sec:coord-model`,
  `sec:related`, `sec:limitations`, `sec:usecases`, etc.) correspond to real `\label`s in
  the manuscript.

## Needs the human / other tracks

1. **`Main.tex` include wiring (I did NOT edit `Main.tex` — track rule).** The two new
   files need includes. Suggested placement (matches V.1 structure):
   ```latex
   \input{Text/RelatedWork}
   \input{Text/UseCases}        % new
   \input{Text/Limitations}     % new
   \input{Text/Conclusion}
   ```
   `Main.tex` currently sets `\mode` for anonymous acmart vs usenix; either works.
2. **Repo layout smell:** duplicate section files exist at both `Paper/*.tex` and
   `Paper/Text/*.tex` (e.g. `Sec6_Implementation.tex`, `Sec7_Evaluation.tex`). I edited
   only the `Paper/Text/` copies (the ones `Main.tex` `\input`s). Confirm `Paper/Text/` is
   canonical.
3. **Branch:** the shared checkout was on `session/03-baseline-ports` when I started
   (another track switched it). I did **not** switch branches (would disrupt the shared
   tree) and did **not** commit. Intended commit when you're ready:
   - Branch `session/06-related-work` off the integration branch.
   - `git add Paper/Text/RelatedWork.tex Paper/Text/Sec1_Introduction.tex
     Paper/Text/Limitations.tex Paper/Text/UseCases.tex Paper/Text/Main.tex
     Paper/bibliography.bib sessions/handoff-06.md`
   - **Cross-track (needs sign-off before staging):** `Paper/Text/Abstract2.tex` — I
     rewrote it for M6 consistency (v1 anomaly language → session-fencing; strawman
     multiples removed). Confirm ownership; stage separately if another track owns it.
   - Msg: `paper(related-work): conjunction-thesis re-spine, RDMA-disagg engagement,
     Kafka rebuttal, limitations+use-cases; reviewer-lens revision (Track 06)`
   - Docs/paper commit → use `--no-verify` per BACKGROUND.

## Round-2 revision (SOSP/OSDI reviewer-lens pass — applied)

A hostile PC-style re-review found conceptual soft spots beyond the mechanical ones;
all in-scope fixes are applied and compile clean.

- **M1 (conjunction was defeatable):** the leg-2 "funnel" argument now explicitly
  targets the **committed-payload path** (full-bandwidth traffic that must survive
  host death so consumers/replicas read without re-ingestion), not the ~5 MB/s
  metadata poll. Pre-empts the "just put metadata on an RDMA memory server" rebuttal.
  `Sec1` + `RelatedWork` RDMA paragraph + `tab:sys-compare` legs re-scoped to the
  payload path (caption states this). **Action for Track 05:** W5-B must hold
  committed *payloads* (best-effort memory server), or the contrast is a strawman.
- **M2 (memory-server rebuttal was beatable by sharding/multi-NIC):** re-anchored on
  **passive-vs-active** — sharding only rebuilds distributed disagg memory whose every
  shard is still an active host that fails/contends/charges CPU; CXL's irreducible
  property is passivity. (`Sec1:19`, `RelatedWork` RDMA paragraph.)
- **M3 (CXL positive never measured):** the "holds both legs" claim is no longer stated
  as flat fact in the intro; the hedge (contrast measured on RDMA; CXL side rests on
  passive-memory first principles + single-host substrate numbers; multi-host = residual
  gap) is now **in the claim itself** (`Sec1:25–26`).
- **M4 (contribution 2 contradicted the related-work concession):** Contribution 2
  rescoped from "the discipline" to **the *extension* of the single-writer/versioning
  discipline to fencing + total-order sequencing** (built on, but distinct from,
  FaRM/DrTM/Clover). `Sec1:80`, `RelatedWork` RDMA paragraph aligned.
- **M5 ("same hazard family" overclaim):** RDMA exercises **two of three** CXL hazards
  (torn reads, no cheap atomics) but **not** stale-cached-copy (one-sided reads bypass
  the reader cache); the `clwb`+`sfence` flush-visibility rule is validated **separately
  on single-host CXL**. Fixed in `Sec1:80` and `Limitations`.
- **M6 (reject-recidivism: strawman throughput headline):** removed the "2.2×/2.3×/2.8×"
  multiples and "advantages persist even on CXL primitives" from the intro; headline is
  now the decomposition + SLO curve + failure suite. **Kept all real absolute numbers**
  (18.2 GB/s, 745 µs P50, 1.6 ms P99) — did **not** fabricate the predicted 1.2–1.6×
  (unmeasured). **`Abstract2.tex` rewritten (CROSS-TRACK — needs your sign-off):** removed
  v1 anomaly language ("hold window → failed append") → session-fencing/per-session
  prefix; removed the strawman "7–219×" multiplier; now states the conjunction + the
  failure-behavior contrast. It is not in my file-ownership; flagging explicitly.
- **M7:** added **Delos (OSDI'20)** and **FuzzyLog (OSDI'18)** — the two glaring
  shared-log omissions — cited in the OBW paragraph.
- **M8:** `tab:sys-compare` repair-cost cell fixed (`δ`, per-session, no global stall —
  not "memory-speed repair"); caption clarifies detection is memory-speed, repair is a
  network round. Also fixed the same overclaim in Contribution 1.
- **M9:** table legs stated **per committed-payload path** (caption).
- **M10:** client-reorder / dual-write alternatives named and dismissed in the WBO
  paragraph (throughput cost), guarding the "first" claim; `Sec1:20` false claim
  ("no prior multi-broker log offers unconditional per-client FIFO" — Corfu does at low
  throughput) fixed with the striping/throughput qualifier.
- **Minors:** 1Pipe/Eris split from the switch-ASIC critique; Kafka-transactions clause
  (atomicity not order) added; intro redundancy removed (single latency-demotion, single
  conjunction statement); bullet lead-in softened.
- **Bib hygiene:** stripped page numbers reconstructed from memory (kept
  title/author/venue/year); **verified `polardbmp` via dblp** (SIGMOD Companion '24,
  pp. 295–308, corrected author list) and `hydrarpc` (ATC'24).

Re-verified: `pdflatex + bibtex` over {Abstract2, Sec1, RelatedWork, UseCases,
Limitations} — exit 0, 0 undefined citations, no oversized hboxes.

## Coordinate with other tracks
- **Track 07** owns the *substrate/hardware* comparison table with leg-1/leg-2 columns
  (V.2) — **distinct** from my *systems* table `tab:sys-compare`. Keep the leg-1/leg-2
  **definitions identical** across both (mine, in the `tab:sys-compare` caption:
  *leg 1 = log state survives any single host failure; leg 2 = every host reaches log
  state without funneling through a shared network endpoint*). Track 07 also owns the
  boxed theorem; my intro references the guarantee in prose only, no theorem box.
- **Track 07 confirmed** Track 06 owns intro edits — no contention this cycle.

## Open questions / TODO left in text
- `\todo` in RelatedWork for **Rowan (OSDI'23), Swarm (SOSP'24), LogStore** — the plan's
  I.5 list names these but dblp did not resolve them under those names, and I would not
  fabricate bib entries. Please supply exact citations (or confirm they're shorthand for
  other papers) and I'll add them + one sentence each.
- `\todo` in Limitations for the **XConn Apollo switch latency** number (used PolarDB-MP
  switch figures as the placeholder source).
- Eval-dependent numbers (failover MTTR, T/R margins, switch-hop model) are referenced by
  `\cref{sec:eval}`, filled by the evaluation tracks.
