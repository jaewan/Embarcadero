# Agent prompt: Scrutinize Fig2 CXL-Scalog baseline (fidelity / fairness)

Copy everything below the line into a coding agent (laptop or cluster). Prefer **read-only analysis** first; only change code/scripts if you find a concrete harness bug or mis-claim.

---

## TL;DR

Fig2 **Scalog cells already succeeded** (`fig2_scalog_o1_ack2_rf2_mem`, status `ok`). This is **not** a crash-fix mission like Corfu. Your job is a **hostile third-eye review**: is CXL-Scalog too favorable to Scalog, unfair vs Embar, misconfigured, or paper-honest? Quantify what the ~4–6× ACK gap vs Embar actually means, and what (if anything) should change in harness, paper language, or follow-up experiments.

**No git / no commits.** Do not start a full Fig2 campaign unless the user explicitly asks after your memo.

## Environment

| Fact | Detail |
|------|--------|
| Cluster | `ssh broker` → moscxl; repo often `/home/domin/Embarcadero` or `~/Embarcadero` |
| Client | Fig2 publisher **`c4`** |
| Git | **Do not use** (no commit/push/checkout) |
| Parallelism | **Never** overlap another Fig2/overnight/multiclient run. Check `/tmp/embarcadero_paper_fig2.lock` and `embarlet`/`throughput_test` before any broker launch |

## Mission

Produce a written **fairness & fidelity memo** that answers:

1. **What does Scalog ACK measure here?** Critical path from submit → ACK for `SEQUENCER=SCALOG` ORDER=1 ACK=2 RF=2 memory-copy under Fig2/`run_latency.sh`.
2. **What did full Scalog do that this harness omits?** Map paper’s “CXL-Scalog = non-FT data-plane, omits Paxos ordering + failure/reconfig” claim to **actual code paths** (prove or refute).
3. **Is the Embar↔Scalog comparison fair on axes that matter for Fig2?** Same sink, publisher, pacing, message size, RF/ACK? Any hidden favoritism to Scalog *or* Embar?
4. **Why is the gap “only” ~4–6×?** Attribute to: (a) stripped Scalog, (b) real Embar strength, (c) Embar not fully optimized, (d) Scalog misconfig making it artificially fast/slow, (e) metric mismatch. Rank hypotheses with evidence.
5. **Recommendations:** paper wording only / harness fix / optional microbench / do-not-touch.

## Non-goals

- Do not redesign Scalog cut/Paxos.
- Do not “optimize Embar to widen the gap.”
- Do not start subscriber-delivery work.
- Do not kill or race an in-progress Corfu/Fig2 run.
- Do not treat successful Scalog rows as failures.
- Do not commit.

## Known numbers (verify in CSV; do not invent)

Campaign: `data/paper_eval/fig2/fig2_append_latency/results.csv`

Embar primary `fig2_embar_o5_ack2_rf2_mem` (ACK p50 µs, approx):  
100→224, 250→256, 500→280, 1000→314, 2000→861

Scalog `fig2_scalog_o1_ack2_rf2_mem` (ACK p50 µs):  
100→858, 250→1010, 500→1279, 1000→1798, 2000→4449  
→ ratio Scalog/Embar ≈ **3.8–5.7×** depending on load.

Paper (`Paper/Text/Sec7_Evaluation.tex`, Baselines): explicitly says CXL-Scalog **omits Paxos-replicated ordering** and is **favorable to Scalog**. Legacy `tab:latency` is marked **not comparable / pending refresh**.

Fig2 launch knobs (verify in `PaperScripts/run_fig2_latency_vs_load.sh`):

```text
fig2_scalog_o1_ack2_rf2_mem
  order=1 sequencer=SCALOG rf=2 ack=2 sink=mem
  SKIP_REMOTE_SCALOG_SEQUENCER=1
  EMBARCADERO_SCALOG_SEQ_IP=$BROKER_IP   # typically 10.10.10.10
```

Embar counterpart: ORDER=5, same mem RF2 ACK2, same remote `c4`, `PACING_MODE=steady`.

## Investigation checklist (do in order)

### A. Artifact audit (read-only)

1. Pick one mid-load Scalog trial artifact, e.g. under  
   `data/paper_eval/fig2/fig2_append_latency/latency/20260716T211407Z/fig2_scalog_o1_ack2_rf2_mem/...`  
   Read `run_metadata.txt`, `client_command.txt`, `run.log`, and `brokers/broker_0.log` (and one follower).
2. Confirm: `ack_claim_label`, `chain_replication_sink=memory-copy`, `order=1`, `replication_factor=2`, sequencer process (`scalog_global_sequencer` local vs remote).
3. Confirm pub ACK metric name used in percentiles matches Embar’s append→ACK (batch vs per-message). Flag any mismatch.

### B. Critical-path code map

Trace Scalog append→ACK2 for ORDER=1:

| Area | Likely paths (confirm/correct) |
|------|--------------------------------|
| Sequencer | `scalog_global_sequencer`; how cuts are decided; local on broker host in Fig2 |
| Broker / topic | Scalog ordering / cut wait before ACK |
| Replication | Scalog RF2 + `memory-copy` (see `scalog_replication_manager.cc` and disk_manager wiring) |
| Client | `publisher.cc` ACK2 wait; same `pub_ack_*` stats as Embar? |

Also map Embar ORDER=5 ACK2 mem path at the same altitude (epoch/linger vs Scalog cut) so the comparison is apples-to-apples for **coordination**, not buzzwords.

### C. Fidelity vs paper Scalog

Answer with citations (file:line):

- What **is** preserved: shard reports, element-wise-min global cut, FIFO in-shard, RF2 replica wait?
- What **is omitted**: Paxos / replicated sequencers / reconfiguration / seals?
- Is Fig2’s `SKIP_REMOTE_SCALOG_SEQUENCER=1` + co-located sequencer on `BROKER_IP` more favorable than a remote sequencer would be? Estimate impact if possible (or say unknown).
- Is ORDER=1 the right Scalog counterpart to Embar ORDER=5 for the paper claim?

### D. Fairness vs Embar Fig2 primary

Check for hidden asymmetries:

- Epoch / linger / runtime_mode (`latency` vs `throughput`)
- Threads per broker, NUMA binds (Corfu sometimes disables NUMA bind — does Scalog?)
- ACK2 credit / offered-rate caps that might throttle one system
- Whether Scalog mem RF2 truly waits on replica DRAM copy (same claim class as Embar `replicated_ack_emulated` / memory medium)
- Steady pacing both sides

### E. Gap attribution (required table)

Fill a table:

| Hypothesis | Supports gap compression (Scalog looks better) | Supports Embar win | Evidence | Severity |
|------------|-----------------------------------------------|--------------------|----------|----------|
| Stripped Paxos / non-FT harness | ✓ | | | |
| Co-located sequencer on CXL host | ? | | | |
| Embar epoch-bounded design | | ✓ | | |
| Embar not fully tuned | | ? | | |
| Metric / batching mismatch | ? | ? | | |
| Scalog bug making it too fast | ? | | | |
| Scalog bug making it too slow | | ? | | |

Rank the top 3 explanations for the observed ~4–6× ratio.

### F. Optional micro-check (only if cluster idle and user would want data)

**Default: skip running brokers.** If you must validate a suspicion:

- One Scalog + one Embar point at **250 MB/s**, 1 GiB, `NUM_TRIALS=1`, same mem RF2 — only to confirm instrumentation, not to replace the campaign.
- Or a config A/B that isolates one fairness knob (e.g. remote vs local Scalog sequencer) — document exactly what changed.

If lock held / brokers live: **do not run**; memo from logs+code only.

## Code / doc map

| Path | Role |
|------|------|
| `PaperScripts/run_fig2_latency_vs_load.sh` | Scalog series knobs |
| `PaperScripts/FIG2.md` | Campaign contract |
| `scripts/run_latency.sh` | Starts local `scalog_global_sequencer` when remote host unset |
| `scripts/run_multiclient.sh` | Reference Scalog RF2 / sink handling |
| `src/disk_manager/scalog_replication_manager.cc` | memory-copy vs disk for Scalog |
| `src/client/publisher.cc` | ACK2 / sink behavior |
| `Paper/Text/Sec7_Evaluation.tex` | CXL-Scalog favorable caveat; latency tables |
| `data/paper_eval/fig2/fig2_append_latency/results.csv` | Measured Fig2 numbers |

## Deliverables

Write on the cluster (or return in chat) a short memo, e.g.  
`data/paper_eval/fig2/fig2_append_latency/SCALOG_FIDELITY_MEMO.md`  
with:

1. **Verdict in 5 lines** (fair / too kind to Scalog / broken / needs paper edit only).
2. **Critical-path summary** (bullets + file:line).
3. **Gap attribution table** (above).
4. **Paper language recommendation** (exact sentence patches if any).
5. **Harness changes** — only if you found a real bug or unfair knob; otherwise “no change.”
6. **What not to do** (e.g. don’t retune Embar epochs to inflate ratios).

## Stop conditions

- Memo complete with ranked hypotheses and citations → **done**.
- If you discover Scalog ACK is wrong/misleading (e.g. ACK before replica wait): stop and flag as **harness bug** with log+code proof; propose a minimal fix but do not expand scope to Embar/Corfu.
- If cluster is busy: finish memo without experiments.

## Tone

Be a skeptical SOSP/OSDI reviewer. Prefer “this comparison overclaims X” over “Scalog is broken.” Embar’s flat ACK curve can be strong even if Scalog is only 4–6× slower under a favorable harness — say that clearly if true.
