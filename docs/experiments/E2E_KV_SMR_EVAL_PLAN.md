# End-to-End KV/SMR Evaluation Plan — Q3 and the Application-Visible Contract

**Status:** planning + partially executed (E1 smoke complete, 2026-07-16).
**Companion runbook:** `benchmarks/kv_store/README_SMR_FIFO.md` (Q3 harness mechanics).
**Owner intent:** this document is the execution contract for the remaining
e2e work and the source for the corresponding paper updates
(`Paper/Text/Sec7_Evaluation.tex` §`sec:eval:kv`, `tab:kv-pipelined`).

---

## 1. Why e2e benchmarks matter for THIS paper

The paper's thesis instrument is layered (`Sec7:4-9`):

- **Q1 (append micro):** the ordering path — not I/O — decides the knee.
- **Q2 (failure micro):** per-session hold/repair/fence, no global seal.
- **Q3 (e2e SMR/KV):** *"does native prefix-safe FIFO survive the full
  publish→order→deliver→apply path, where client-side serialization cannot?"*

Q1/Q2 show the **mechanism** (epochs, holds, CXL polling). Only Q3 shows the
**consequence an application can observe**: correct replicated state at full
pipeline depth. The intro's motivating example (`Sec1:18-23` — b₁ to server A,
b₂ to server B, arrival wins, "a state machine that issued them in order
observes them reversed") is not testable by any GB/s or latency microbench.
The SMR overwrite workload makes that exact sentence measurable: two versions
of one key on different brokers; the final KV value is wrong iff the log
permuted the session. That is what "end-to-end meaning" buys: it converts the
ordering contract from a protocol property into application state.

Secondary value, already demonstrated: the byte-level Valid check is a
**bug-finding instrument**. In one week it exposed (i) the subscriber
ordered-consume feed gate that silently starved every KV run on main, (ii) a
10-month-old payload-tail corruption in the v1 wire path invisible to
count-based validation, and (iii) the Corfu cross-broker token race that
violated the port's own frozen contract. An eval that can fail is an eval
whose passes mean something — this belongs in the paper's methodology voice.

## 2. Claim ledger (what the KV/SMR suite must carry)

Panel-corrected wording — the taxonomy is *where each system pays its
serialization*, not "only Embar is correct":

| # | Claim (paper wording target) | Instrument | Status |
|---|------------------------------|------------|--------|
| C1 | Under full striping and pipelined same-session overwrites, Embar applies correctly with **no serialization on the client path** (hold at sequencer, R = epoch) | E1 Pipe row | smoke ✅, paper-scale pending |
| C2 | Corfu is also Valid under Pipe — by serializing **the client's write path** (token RTT per batch); it pays ~35–40% of Embar's pipe rate for it | E1 Pipe row (post `[[CORFU_FIFO_FIX]]`) | smoke ✅ |
| C3 | Scalog/LazyLog global order **need not respect per-publisher submission order under striping** (`app:scalog-fifo`) — Pipe is Valid=NO with measured same-key inversions | E1 Pipe + reorder audit | smoke ✅ |
| C4 | Restoring FIFO **client-side by stop-and-wait** costs ~10³× (policy lower bound, not a universal tax) | E1 Serialize row | smoke ✅ |
| C5 | Restoring FIFO **by sticky routing** is free at small scale — and re-creates the single-ingress bottleneck striping exists to remove | E2 (control) + E3 (necessity) | not run |
| C6 | Reorder pressure was real: same seed/striping produced ~10⁴ inversions on WBO systems; Embar absorbed the identical interleave in holds (0 reorders) | E1 audit counters | smoke ✅ |
| C7 | The harness is a real SMR (independent subscriber replicas apply the log), and the Valid checker is itself cross-checked — **methodology hygiene, NOT a differentiator**: every totally-ordered log converges, including incorrect runs (uniformly wrong state) | E4 | harness done ✅ |
| C8 | Pipelining is real depth, not batching luck: publish→apply lag distribution bounded under Pipe | E5 | hooks exist |
| C9 | Per-client FIFO holds for **every** concurrent session, not just one | E6 | not run |
| C10 | Ordering decoupled from replica lag is application-visible: RF=2 + slow replica leaves Embar apply latency epoch-bounded while cut/binding systems stall | E7 | not built |
| C11 | Standard YCSB mixes run at competitive rates on the same log (appendix optics only — YCSB cannot exercise FIFO) | E8 | binary ready |

Explicitly **not** claimed from KV: append throughput scaling (Q1's job),
broker-kill MTTR (Q2's job), and any end-to-end *latency-vs-load* curve —
delivery above ~270 MB/s/connection is a documented client-library ceiling
(`Sec7:215-233`); a KV latency curve would confound the paper's deliberate
append-path scoping.

## 3. Experiment matrix

Common fixed knobs unless stated: 4 brokers, RF=1 ACK=1, `value_size=100`,
`batch_size=1`, one client session, seed 42, medians of 3 trials, commit +
host recorded. Driver: `benchmarks/kv_store/run_smr_fifo_eval.sh`.

### E1 — SMR session-FIFO matrix (Pipe / Serialize) — **P0, harness done**
- **Design:** 500K keys load, 50K warmup, 500K pipelined versioned overwrites
  (`--fifo_valid`); Serialize = `sync_interval=1`, ACK barrier, 2–5K ops
  (rates compared, caption states policy).
- **Expected:** Embar Pipe Valid=YES ~0.7–1.0M ops/s, 0 reorders (legacy table
  point: 1.00M). Corfu Pipe Valid=YES ~0.5M. Scalog/LazyLog Pipe Valid=NO,
  session reorders ~10³–10⁴, same-key inversions surviving to final state.
  Serialize: all Valid=YES at ~10²–10³ ops/s.
- **Falsifier:** any Embar reorder count > 0, or a WBO Pipe run that passes
  session-FIFO Valid at N=4 (would contradict `app:scalog-fifo`).
- **Paper:** fills `tab:kv-pipelined` (add Scalog row + Mode column or split
  table); rewrites §`sec:eval:kv` prose from "pending refresh" to results;
  Fig `smr_kv_pipe_ops.pdf` + `smr_fifo_tax.pdf` (log-y, hatched Valid=NO).
- **Caption obligations:** RF/ACK, counts, striping on, Valid definition,
  "stop-and-wait" labeled as policy lower bound (C4), LazyLog row labeled
  non-faithful (binding-gated append) per `Sec7:42-45`.

### E2 — Sticky control (`NUM_BROKERS=1`) — **new, run-only**
- **Design:** same workload, one broker. Sticky routing by construction, all
  systems.
- **Expected:** every system Valid=YES at Pipe; Scalog N=1 ≈ its N=4 pipe rate
  *at this payload size* — which is precisely the reviewer attack E3 answers.
- **Value:** (a) negative control — proves violations require multi-broker
  striping (eval-spec anti-footgun #7); (b) the honest "Kafka corner" row the
  red-team demanded; (c) frames E3.

### E3 — Striping-necessity variant — **new, the load-bearing defense**
- **Why:** the sharpest remaining attack: *"your 100-B single-session workload
  never needed striping — sticky is free."* The paper's answer (single-link
  ingress cap, `Sec1:24-26`) must be visible **inside Q3**, or Q3 must
  explicitly lean on Q1 for it.
- **Design:** raise per-op payload (4–16KB values) and/or run N client
  sessions until one broker's ingress saturates. Three bars per system:
  Sticky (Valid=YES, capped), Striped Pipe (fast; Valid=NO for WBO), Embar
  Striped Pipe (fast AND Valid=YES).
- **Expected:** sticky ops/s plateaus at single-broker ingest; striped Embar
  scales ~N×; Scalog striped remains Valid=NO. One figure then shows the
  full trade: correct-but-capped / fast-but-wrong / fast-and-correct.
- **Falsifier:** sticky matching striped throughput at saturating load.
- **Cost:** driver knobs exist (`SMR_FIFO_VALUE_SIZE`, `NUM_BROKERS`); N>1
  sessions needs E6's multi-process runner. Start with value-size scaling.

### E4 — Replica convergence — **done + validated; scope per reviewer-2 pass**
- **Why (corrected framing):** two defensive jobs, no offensive one.
  (a) The paper's most aggressive claim is *negative* — a baseline produces
  wrong final state. The burden of proof falls on the harness ("how do I
  know this isn't your bug?" — and the harness DID once have a payload bug).
  Digest agreement across systems/modes/replicas is the sentence that says
  the checker is itself cross-checked. (b) It makes the term "SMR KV store"
  honest — without replicas the harness is one process reading back its own
  writes.
- **Explicitly NOT:** a result, a contribution, or a differentiator.
  Convergence is table stakes for any totally-ordered log; incorrect runs
  also converge (uniformly wrong). Never phrase as "our replicas converge."
  The digest is a multiset-sum tripwire, not collision-resistant; publish no
  digest values; the per-key byte-level check remains the Valid criterion.
- **Design (implemented 2026-07-16):** `--replica` subscriber-only mode
  (`--expected_entries`, `--digest_out`), order-independent `stateDigest()`,
  driver `SMR_FIFO_REPLICAS=N` with automatic convergence verdict.
  Validated: 2 replicas + publisher digest-identical (`smr_fifo_v2`);
  cross-system digest identity for all Valid runs, divergence only for
  reordered Scalog pipe (`smr_fifo_v1`).
- **Paper treatment (two sentences max, methodology voice, §`sec:eval:kv`):**
  see wording bank §8. Also ideal for the artifact-evaluation appendix — a
  one-number reproducibility check for the AE committee.

### E5 — Apply-lag CDF (pipeline depth) — **run-only**
- **Design:** `--latency` in fifo mode; per-op publish→apply samples already
  exported (`apply_latency_us.csv`). Embar vs Corfu under Pipe, RF=1.
- **Expected:** both bounded (no runaway backlog at this load); Embar median
  in the low-ms class (epoch + delivery), Corfu comparable or wider.
- **Value:** shows Pipe ops/s is sustained pipeline, not end-spike; the eval
  prompt's optional Fig 3. Keep observational — no latency-vs-load sweep.

### E6 — Multi-session FIFO (N=2–3 clients) — **moderate**
- **Design:** N kv_bench processes, disjoint keyspaces, each validating its
  own session; one shared cluster. Requires a small multi-process wrapper +
  per-process Valid aggregation.
- **Expected:** per-session Valid=YES on Embar for all N; WBO failures scale
  with N. **Risk:** ORDER=5 concurrent-session history (session-storm at
  N≥3, fixed 2026-07; validate N=2 first).
- **Value:** the paper says *per-client* FIFO; one client proves existence,
  not the quantifier.

### E7 — Durability-decoupling, application-visible (RF=2 + slow replica) — **largest, highest marginal value**
- **Why:** the architecture table's sharpest row (`tab:baseline-comparison`:
  "slow replica stalls ordering — Scalog/LazyLog yes, Embar no") currently
  has no e2e evidence.
- **Design:** RF=2, ACK2; inject periodic fsync delay (5–50ms) on one
  replica (cgroup io throttle or LD_PRELOAD shim on fdatasync). Measure KV
  apply-lag distribution + ops/s.
- **Expected:** Embar apply p99 stays epoch-bounded (replication off the
  ordering path; CV tracks durability separately); Scalog/LazyLog apply lag
  tracks injected jitter (global cut gated on min-replica progress,
  `Appendix:45-54`).
- **Falsifier:** Embar apply lag tracking the jitter → would contradict the
  decoupling claim; must be reported if seen.
- **Gates:** Scalog RF=2 end-to-end verification is itself flagged in-progress
  in the appendix (`Appendix:39-43`) — coordinate with that work; do not run
  this before E1–E4 are banked.

### E8 — YCSB A–F appendix table — **run-only, optics**
- Same binary (`--workload=A..F`). Report ops/s + read/write p50/p99, Valid
  (counts-level) still checked. **Never cited as FIFO evidence.** Appendix
  only; include fidelity labels if comparative.

## 4. Expected headline (paper-scale predictions to verify)

| System | Pipe (ops/s) | FIFO locus | Valid | Serialize (ops/s) |
|---|---|---|---|---|
| Embarcadero | ~0.7–1.0M | hold @ sequencer (off client path) | YES | ~2K (391× — even Embar pays if you force client serialization) |
| CXL-Corfu | ~0.5M | token RTT @ client write path | YES | ~1.1K |
| CXL-Scalog | ~0.6–0.7M | none under stripe | **NO** | ~230 (~3,000× stop-and-wait) |
| CXL-LazyLog | (labeled non-faithful) | none under stripe | **NO** | ~280 |

Smoke sources: `build/results/smr_fifo_matrix1`, `smr_fifo_corfufix1`.
These become table-ready only after: 3 trials at 500K scale, LazyLog labeling
decision, and (for final ops/s optics) the remote-client layout per the
testbed section — local co-located numbers stay internal.

## 5. Fidelity gates before any number enters the paper

1. Corfu: `[[CORFU_FIFO_FIX]]` in tree (`ae4a7651`) — Pipe row now faithful.
2. LazyLog: Pipe row withheld/labeled until pre-binding append path validated.
3. Scalog: harness labeled "non-fault-tolerant data-plane, favorable to
   Scalog" per `Sec7:34-41`.
4. Same binary, matched knobs, fixed seed; provenance (commit, host, config)
   in every run dir (harness already records).
5. Shared-host discipline: port pre-flight, scoped shm cleanup, no
   overlapping drivers (see `README_SMR_FIFO.md` known traps).
6. Medians over ≥3 trials; warmup discarded; drain-inclusive timing stated.

## 6. Execution order and cost

| Order | Item | Cost | Unblocks |
|---|---|---|---|
| 1 | E1 paper-scale (local) | ~2–2.5h wall | table draft, C1–C4, C6 |
| 2 | E2 sticky control | **harness done + smoke-validated 2026-07-16** | C5 control, E3 framing |
| 3 | E4 replica mode | **harness done + smoke-validated 2026-07-16** (2 replicas + publisher digest-identical, `smr_fifo_v2`) | C7 |
| 4 | E5 apply-lag CDF | run-only | C8 |
| 5 | E3 value-size scaling | ~1h runs; design sign-off first | C5 (necessity) |
| 6 | E6 multi-session | **mechanics validated at N=2 2026-07-16** (both sessions Valid=YES, 0 reorders, `smr_fifo_v3`); N=3 pending | C9 |
| 7 | E7 slow-replica RF=2 | ~1–2 days incl. jitter shim | C10 |
| 8 | E8 YCSB | ~1h | C11 |
| 9 | Remote-client rerun of banked winners | scheduling-dependent | publication ops/s |

Smoke evidence from the 2026-07-16 harness validation (`smr_fifo_v1..v3`,
10K/10K local): Embar pipe 747K Valid=YES; Scalog pipe Valid=NO (3,498/570
reorders); **Scalog sticky (N=1) Valid=YES at 514K** — confirming both the
negative control and the E3 premise that sticky is free at small payloads.
`state_digest` was identical across all Valid runs (Embar pipe/sticky,
Scalog sticky; replicas included) and divergent only for reordered Scalog
pipe — internal cross-check of the Valid checker (see E4 scope note: this is
methodology hygiene and an AE-appendix aid, never a paper result).

## 7. Paper update map (execute as results bank)

- `tab:kv-pipelined`: replace legacy 3-row table with System × Mode rows from
  E1 (+E2 sticky rows or footnote); caption gains Valid definition,
  stop-and-wait labeling, reorder-pressure sentence (C6), LazyLog label.
- §`sec:eval:kv` prose: from "Q3 remains pending" to the serialization-locus
  narrative (C1–C5); the two-sentence digest/replica methodology note (§8),
  one sentence for apply-lag (E5); cross-reference to Q1 for striping
  necessity or E3 figure. Digest detail goes to the AE appendix, not here.
- §`ssec:eval-discussion` "what these results establish": can strengthen the
  existing line "Native FIFO preserves pipelining on an SMR apply path where
  client serialization cannot" from claim to measured statement.
- Appendix: E8 YCSB table; E7 (if run) gets its own paragraph beside the
  durability-coupling discussion.
- Do NOT touch: throughput-scaling and failure sections (Q1/Q2 own those);
  no KV latency-vs-load anywhere.

## 8. Wording bank (panel-approved boasts)

- "Every system that preserves per-publisher FIFO serializes *somewhere*;
  they differ in where and at what cost. Corfu serializes the client's write
  path (a token round-trip per batch). Scalog and LazyLog leave submission
  order unprotected under striping — restoring it client-side costs three
  orders of magnitude (stop-and-wait) or forfeits striping (sticky). \sys
  serializes at the sequencer's hold, off the client path, bounded by the
  epoch — preserving both the pipeline and the contract."
- "The same striped interleave that produced ~10⁴ per-session inversions on
  write-before-order baselines was absorbed by \sys's holds with zero
  reorders and byte-identical final state."
- Digest / replicas (methodology voice, §`sec:eval:kv`, two sentences max):
  "Validity is checked byte-for-byte per key against session submission
  order and corroborated by an order-independent digest of the final state:
  all Valid runs — across systems and routing modes, independent subscriber
  replicas included — converge to the identical state; only runs with
  observed same-key inversions diverge."
- Avoid: "only \sys is Valid" (false — Corfu is Valid, slower); "Scalog
  orders by arrival" (imprecise); "3,000× FIFO tax" without the
  stop-and-wait qualifier; "replicas converge" as a comparative result
  (every totally-ordered log converges, including incorrect runs); the word
  "oracle" or any digest values in the paper.
