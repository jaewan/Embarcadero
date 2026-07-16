# Agent Prompt: Plan + Implement E2E Replicated State Machine (SMR) Eval

## Role

You are a senior systems researcher / coding agent working in `/home/domin/Embarcadero`.

**Goal:** Read the paper draft and the existing shared-log KV/SMR harness, then **plan and implement** (or harden) an **end-to-end replicated state machine** evaluation on:

- Embarcadero (`SEQUENCER=EMBARCADERO`, `ORDER=5`)
- CXL-adapted baselines already in-tree: Scalog, LazyLog, Corfu

This eval is **paper Q3** (“end-to-end meaning”), not another bulk append GB/s microbench. Bulk RF0 throughput where Scalog/LazyLog can beat Embar ORDER=5 is **out of scope as the headline** for this task.

**Deliverables (in order):**

1. A short **design/plan doc** (paths, configs, success criteria, plots).
2. Code + scripts so the experiment is **reproducible**.
3. A **result schema** (CSV/JSON) that directly fills the paper table/figure.
4. Do **not** claim publication numbers until Valid/FIFO contracts are honest for every system.

Do **not** invent a new shared-log API. Build on `Publisher` / `Subscriber` and the existing KV store.

---

## Phase 0 — Read first (mandatory)

Read these **before** writing code. Quote the paper’s Q3 claim in your plan.

### Paper (what Q3 must show)

| File | Why |
|------|-----|
| `Paper/Text/Sec7_Evaluation.tex` | Q1/Q2/Q3 framing; §`sec:eval:kv` SMR KV table `tab:kv-pipelined`; Valid = applied==published |
| `Paper/Text/Appendix.tex` | §`app:scalog-fifo`: Scalog can violate **per-publisher FIFO under striping** |
| `Paper/Text/RelatedWork.tex` | Write-before-order scales TP; **per-client FIFO under striping does not**; Kafka FIFO is per-partition/single-leader |
| `Paper/Text/Sec1_Introduction.tex` / thesis paras | Embar = total order **+** prefix-safe session FIFO under stripe; order decoupled from replica lag |

### Existing code (reuse, do not fork a second KV)

| Path | Role |
|------|------|
| `benchmarks/kv_store/distributed_kv_store.{h,cc}` | Log-backed SMR KV: publish ops → subscribe → apply in `total_order` |
| `benchmarks/kv_store/kv_bench_main.cc` | Pipelined writes, `sync_interval` (0 = max pipeline), Valid checks |
| `benchmarks/kv_store/run_kv_baseline_compare.sh` | Multi-sequencer driver |
| `scripts/run_ycsb_eval.sh` / YCSB paths | Related but **not** a substitute for FIFO-critical SMR |
| `docs/baselines/porting_rule.md` | Baselines must keep protocol; do not “fix” Scalog FIFO by importing Embar holds |
| `docs/baselines/fairness_appendix.md` | How to label CXL vs gRPC control; co-located sequencers |
| `docs/agent-prompts/DISTRIBUTED_KV_AGENT_PROMPT.md` | Older broader KV prompt — **supersede** conflicting paths (`bench/kv_store` → use `benchmarks/kv_store`) |

Also skim: client `Publish`/`Poll`/`Consume`, ACK1 vs ACK2, how Scalog/LazyLog/Corfu are selected in `run_multiclient.sh` / KV scripts.

---

## Scientific claim this eval must support

**Claim (Q3):** Native prefix-safe **session FIFO** on Embar lets an SMR apply path **pipeline** appends across **striped brokers** and still apply overwrites correctly. Systems without that contract must either:

- **accept reorder** → `Valid=NO` under pipelined same-key / same-session overwrites, or
- **serialize** (stop-and-wait / sticky single broker / `sync_interval=1`) → `Valid=YES` but large **slowdown**.

**Non-claims (do not sell these as Q3 wins):**

- Embar highest RF0 append GB/s vs Scalog/LazyLog.
- YCSB A/B/C alone “proves FIFO” (YCSB does not require session FIFO under stripe).
- LazyLog “pre-binding append” numbers unless the harness actually matches LazyLog’s append-complete-before-binding contract (`Sec7` already withholds bad LazyLog rows).

---

## What the E2E SMR must show — and how

### Architecture (standard shared-log SMR)

```
Client(s)  --Publish(op)-->  Shared log (Embar | Scalog | LazyLog | Corfu)
                                |
                           total_order
                                v
Replica apply thread(s)  --Subscribe/Consume-->  deterministic KV state
```

- **Write path:** serialize `Put`/`Delete` (and optional `MultiPut`) → `Publish` → wait per policy (`sync_interval`).
- **Apply path:** consume in **log total order**; apply to local map; never apply past a gap if the contract forbids it.
- **Read path (optional for Q3):** local read after apply barrier; primary Q3 metric is **write pipeline + Valid**, not read-heavy YCSB.

### Workload that makes FIFO load-bearing

Use a workload where **order of apply changes final state**:

1. **Warmup:** insert `K` distinct keys (e.g. 500K) so store size is known.
2. **Pipelined overwrite phase:** for each of `M` ops (e.g. 500K), pick key `k` and write a **monotone version** `v` (or embed `(client_id, client_order)` in value).
3. Prefer a **single logical session / client_id** that **stripes across all brokers** (full striping — Embar’s setting). Do **not** sticky-route to one broker unless that is an explicit Scalog “serialized for correctness” mode.
4. Finish only when **every published op is applied** (apply == publish count).
5. **Valid iff:**
   - `applied_count == published_count`
   - store size stays `K` (overwrite-only phase)
   - for every key, final value equals the **last published version for that key in client submission order** (session FIFO), **or** document that the system only promises global total order without session FIFO and then Valid must use **log order** as ground truth — but then you **cannot** claim Embar’s FIFO advantage; prefer session-FIFO Valid for the main table.

**Critical:** For Scalog under striping, session-FIFO Valid may fail under `sync_interval=0`. That failure is a **result**, not a harness bug — record it. Offer an alternate row: Scalog with `sync_interval=1` or sticky broker → Valid YES + Slowdown.

### Modes to run (same binary, different config)

| Mode | `sync_interval` | Striping | Purpose |
|------|-----------------|----------|---------|
| **Pipe** | `0` (ACK only at end / large window) | Full (all brokers) | Headline: Embar Valid=YES + high ops/s |
| **Serialize** | `1` (or wait-for-apply each op) | Full | Fairness row for Scalog/LazyLog when Pipe breaks FIFO |
| **Sticky** (optional) | `0` | Single broker / partition | Shows “Kafka-style” escape hatch; not Embar’s claimed setting |

Run for: `EMBARCADERO/ORDER=5`, `CORFU`, `SCALOG`, `LAZYLOG` with **matched** RF/ACK (start RF=1 ACK1; optional RF=2 ACK2 later). Same `THREADS`, msg encoding, `NUM_BROKERS`, record/op counts.

### Metrics (must export)

Per system × mode × trial:

- `pipe_ops_per_sec` (overwrite phase wall time)
- `fifo_mode`: `native` | `token_order` | `stop_and_wait` | `sticky` | `none`
- `slowdown_vs_embar_pipe` (if Embar Pipe is reference)
- `valid`: YES/NO + failing check name
- `published`, `applied`, `final_mismatch_keys` (count)
- `reorder_detected` (if you can detect session-order vs apply-order divergence)
- commit, hostname, sequencer, ORDER, ACK, RF, `pub_threads`, striping flag

Median of trials 2–4 (or 3 trials after 1 warmup) — match overnight eval style.

---

## How to show it — plots / tables for the paper

Q3 is **not** a GB/s scaling plot. Prefer:

### Table 1 (primary) — fill `tab:kv-pipelined`

Columns (match paper):

`System | Pipe (ops/s) | FIFO | Slowdown | Valid`

Rows:

- Embarcadero — Pipe — `native` — — — YES  
- CXL-Corfu — Pipe — `token order` — — — YES (if true)  
- CXL-Scalog — Pipe — `none`/striped — — — YES **or** NO  
- CXL-Scalog — Serialize — `stop-and-wait` — `>N×` — YES  
- CXL-LazyLog — same pattern; **do not** report Pipe as faithful LazyLog if append waits on binding  

Caption must say: RF, key count, overwrite count, striping on, Valid definition.

### Figure 1 (recommended) — grouped bar: Pipe ops/s

- X: system  
- Y: ops/s (log scale if Scalog serialize is tiny)  
- Hatch or color: Valid YES vs NO  
- Optional second panel: Slowdown vs Embar Pipe  

### Figure 2 (strong thesis, optional but high value) — FIFO necessity

Two bars per write-before-order system:

- Pipe (may be invalid or omitted if invalid)
- Serialize-for-correctness  

Annotation: “cost of restoring session FIFO without native holds.”

### Figure 3 (optional appendix) — apply lag CDF

Time from publish to apply under Pipe for Embar vs Corfu; shows pipeline depth is real.

### Do **not** use as Q3 headline

- Aggregate append GB/s vs N publishers (that is Q1 / Fig throughput_scaling).
- Broker-kill MTTR (that is Q2).
- YCSB throughput without Valid/FIFO story.

---

## Implementation plan (agent checklist)

### A. Audit existing KV SMR

1. Confirm `distributed_kv_store` applies strictly by `total_order`.
2. Confirm `kv_bench_main` can set `sync_interval=0` and waits for full apply at end.
3. Add or harden **session-FIFO Valid** (final value == last client submission per key), not only `applied==published`.
4. Ensure one client session can **stripe** (`NUM_BROKERS>1`, publisher threads/brokers as in TP cells). Document if current default `KV_BENCH_PUB_THREADS=1` hides striping reorder — for Q3 you likely need striping pressure (multi-broker publish) while keeping **one client_id / session**.

### B. Baseline honesty

1. Wire Scalog/LazyLog/Corfu through the **same** KV binary and cluster bring-up as `run_kv_baseline_compare.sh`.
2. Label LazyLog: if client waits for binding before append completes, mark row **non-faithful** and either fix contract or withhold Pipe claim (per `Sec7`).
3. Do **not** add Embar-style hold buffers to Scalog to “make Valid pass.”

### C. Driver script

Extend or add `benchmarks/kv_store/run_smr_fifo_eval.sh`:

- Matrix: systems × {pipe, serialize} × trials  
- Fixed seeds, matched knobs  
- Writes `results/<tag>/summary.csv` + per-trial logs  
- Emits a markdown snippet suitable for pasting into paper notes  

### D. Plotting

Add a small Python plotter (e.g. `scripts/plot_smr_fifo.py`) that reads `summary.csv` and writes:

- `Figures/smr_kv_pipe_ops.pdf` (Fig 1)  
- `Figures/smr_fifo_tax.pdf` (Fig 2)  

Use paper-friendly fonts/sizes; no dashboard clutter.

### E. Definition of done

- [ ] Plan doc checked into `docs/` or `benchmarks/kv_store/README_SMR_FIFO.md`
- [ ] Pipe + Serialize matrix runs on at least Embar + one write-before-order baseline locally
- [ ] Valid failures for striped Scalog Pipe are **explained**, not papered over
- [ ] Table schema matches `tab:kv-pipelined`
- [ ] Plot PDFs generated from CSV
- [ ] Explicit note linking results to paper Q3 text (FIFO under striping), not Q1 TP

---

## Fairness / anti-footguns

1. **Same striping intent** across systems when comparing Pipe.
2. **Same durability:** start RF=1 ACK1; do not mix ACK2 disk path into Q3.
3. Embar `ORDER=5` only for headline; optional Embar `ORDER=0` row as negative control (Pipe Valid should fail or be meaningless for session FIFO).
4. Corfu token-before-write may be slower but Valid=YES — that is expected and fine.
5. Do not co-locate publisher on broker host for Q3 unless labeled ceiling.
6. Warmup discard; report medians; record commit hash.
7. If N=1 broker, Scalog FIFO violation may not appear — **Q3 requires multi-broker striping.**

---

## Output format for your first response

1. **Paper Q3 quote** (2–4 sentences) + what Valid means.  
2. **Gap analysis:** what `benchmarks/kv_store` already has vs missing (Valid, striping, drivers, plots).  
3. **Implementation plan** with file-level tasks (ordered).  
4. **Exact experiment matrix** (systems × modes × knobs).  
5. **Figure/table mock** (ASCII is fine).  
6. Then implement; do not stop at the plan unless asked.

---

## One-line success criterion

> Under full striping and pipelined same-session overwrites, Embar shows high Pipe ops/s with Valid=YES via native FIFO; Scalog/LazyLog either fail Valid in Pipe or only pass Valid under serialize/sticky with large Slowdown — presented as Table `tab:kv-pipelined` + a Pipe ops/s bar chart (and optional FIFO-tax figure).
