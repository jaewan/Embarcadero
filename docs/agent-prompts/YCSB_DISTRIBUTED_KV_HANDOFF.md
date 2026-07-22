# Agent Prompt: Paper-Grade Distributed YCSB Evaluation

You are taking over the YCSB/distributed-KV evaluation for the Embarcadero
VLDB submission in `/home/domin/Embarcadero`. Treat this as a publication
experiment, not a demo. Design the experiment, harden the shared harness, run
smokes, then run the preregistered matrix. Do not put a number in the paper
unless the run is valid, comparable, and traceable to a clean commit.

## Objective and claim boundary

Build a distributed, log-backed KV macrobenchmark using the existing
`kv_ycsb_bench` and the same `Publisher`/`Subscriber` path for Embarcadero,
CXL-Corfu, and CXL-Scalog. LazyLog is optional and must be omitted or explicitly
labeled if its in-tree binding-gated path does not implement the faithful
pre-binding contract.

The useful YCSB claim is application-level throughput and latency under
standard mixes. YCSB does **not** make per-session FIFO load-bearing and must
not replace Q3's versioned-overwrite experiment. The main paper already uses
that experiment for FIFO; YCSB is a macrobenchmark/appendix result unless its
design exposes a new, independently justified application claim.

Start with YCSB A (50/50 read/update) and F (read/modify/write). Add B only if
space and runtime permit. Do not mechanically run A--F merely to make a large
matrix. Prefer a small, well-controlled comparison over broad but ambiguous
coverage.

## Read before changing code

Read these files completely, in this order:

1. `Paper/Text/Sec7_Evaluation.tex` — current Q1--Q3 claims and reporting tone.
2. `Paper/Text/Appendix.tex` — baseline fidelity and Scalog FIFO discussion.
3. `benchmarks/README.md` — current KV benchmark status and known limitations.
4. `docs/experiments/E2E_KV_SMR_EVAL_PLAN.md` — claim ledger; YCSB is E8,
   distinct from the FIFO-critical E1 experiment.
5. `benchmarks/kv_store/README_SMR_FIFO.md` — validated workload, validity,
   batching, and cluster-lifecycle mechanics that should be reused.
6. `benchmarks/kv_store/kv_bench_main.cc` and
   `benchmarks/kv_store/distributed_kv_store.{h,cc}` — actual workload and apply
   semantics. Do not trust stale documentation over code.
7. `benchmarks/kv_store/run_kv_baseline_compare.sh` and
   `benchmarks/kv_store/run_smr_fifo_eval.sh` — current common-binary baseline
   driver and fail-closed result handling.
8. `scripts/run_ycsb_eval.sh` and `scripts/aggregate_ycsb.py` — historical
   starting points only. Audit before reuse: the current driver is local-only,
   uses RF0, performs host-wide process cleanup, and does not provide the
   synchronized distributed-client launch required here.
9. `docs/contracts/ACK_RF_CONTRACT.md` — RF includes the primary; ACK2 is valid
   only for RF>=2 and means media-durable completion under the normative
   contract. A DRAM-copy sink must be labeled DRAM replica completion, never
   durable.
10. `docs/baselines/porting_rule.md`,
    `docs/contracts/CORFU_INVARIANT_LEDGER.md`,
    `docs/design/scalog_canonical_progression_contract.md`, and
    `docs/contracts/lazylog_metadata_replica_contract.md` — baseline fidelity.
11. `scripts/cluster_setup.sh`, `scripts/run_multiclient.sh`,
    `PaperScripts/run_overnight_eval.sh`, and `PaperScripts/README.md` — cluster
    synchronization, client placement, barrier launch, locking, and provenance.

The older `docs/agent-prompts/DISTRIBUTED_KV_AGENT_PROMPT.md` is historical.
It contains obsolete `bench/kv_store` paths, an SOSP target, and an over-broad
matrix; this prompt supersedes it.

## Phase 0: reconcile the current implementation

Before running anything, write a short audit in
`docs/experiments/YCSB_DISTRIBUTED_KV_PLAN.md` that answers:

- Are A, B, and F implemented with canonical operation semantics? In
  particular, verify whether F's read-modify-write is an atomic/logged RMW or
  merely a local read followed by a write, and state what is measured.
- Is the Zipfian generator correct, deterministic, and shared across systems?
- Does each process preload the same keyspace, or is there exactly one load
  owner followed by a barrier? Prevent duplicate preload from contaminating
  the timed log.
- Are reads local materialized-view reads? What consistency barrier precedes
  them, and is read-your-writes actually enforced?
- Does `throughput_ops_sec` include final apply drain? For writes, report
  publish-to-apply latency, not only enqueue latency.
- Can multiple remote processes use distinct client/session IDs, disjoint
  result directories, and one shared topic without one process terminating the
  cluster?
- Is the single apply thread the throughput ceiling? If it is, either implement
  deterministic partitioned apply with a clearly stated ordering rule or frame
  the experiment explicitly as a common apply-path ceiling. Never attribute
  that ceiling to the log.
- Reconcile the contradictory documentation: `kv_bench_main.cc` currently
  advertises A--F support while `benchmarks/README.md` still says the workload
  generator is not wired in. Correct the documentation only after validating
  semantics with tests.

Do not import Embarcadero hold/repair behavior into a baseline. Any change in a
baseline must preserve the frozen protocol contracts above.

## Preregistered experiment

Use a common binary and identical application path for every system.
Recommended first publication matrix:

- Systems: Embarcadero ORDER5, CXL-Corfu, CXL-Scalog.
- Workloads: YCSB A and F; Zipfian theta 0.99.
- Placement: 1, 2, and 3 **remote** client processes on c4, c3, and c1. Keep a
  co-located client out of the primary scaling curve.
- Brokers: four on moscxl; broker address `10.10.10.10` unless the current
  testbed configuration says otherwise.
- Records: at least 1M after smoke; enough measured operations to run for tens
  of seconds at the achieved rate. Use 100-byte values unless a justified
  sensitivity changes it.
- Replication: choose one primary regime and name it exactly. For the most
  comparable macrobenchmark, use RF2/ACK2 DRAM replica completion if all three
  ports support it faithfully. If using normative media durability, use the
  disk-durable sink for every system. Never call RF1 durable.
- Trials: at least three independent successful trials per cell; report median
  and min--max or a confidence interval. Retries are allowed only for
  preregistered infrastructure failures and must remain in the artifact log.
- Batch size, publisher threads, warmup, record count, operation count, seed,
  broker count, sink, RF/ACK, and client placement must be identical.

Before the full matrix, run this gate sequence:

1. Unit/standalone deterministic workload tests.
2. One-process local smoke for each system.
3. One remote process on c4 for each system.
4. Two remote processes with shared-topic/load coordination.
5. Only then launch the full matrix.

## Validity and fail-closed acceptance

A trial is publishable only if all applicable checks pass:

- every issued write is published and applied before timing completes;
- expected reads/writes/RMWs sum to total operations;
- preload record count and post-run store cardinality match workload semantics;
- all replicas/processes that materialize the full state end with the same
  digest and applied count;
- no parse, apply, ACK, subscriber, topic-creation, or cluster-lifecycle error;
- the achieved-load/runtime window is nonzero and all required client windows
  overlap;
- the run records clean git commit, host roster, NUMA placement, full knobs,
  per-process logs, and raw latency samples.

Do not accept a copied CSV, a process exit alone, or a correct final digest
alone as success. For mixed workloads, add deterministic oracle tests for read
and RMW semantics. Keep failed attempts in the campaign manifest; do not
performance-filter trials.

## Distributed launch and remote synchronization

Never develop or benchmark from an uncommitted overlay. `cluster_setup.sh`
exports a clean `git archive HEAD`, so uncommitted changes are intentionally
absent on clients.

On moscxl:

```bash
cd /home/domin/Embarcadero
git status --short
git rev-parse HEAD
cmake --build build -j --target embarlet kv_ycsb_bench \
  corfu_global_sequencer scalog_global_sequencer
CLIENT_NODES_CSV=c4,c3,c1 bash scripts/cluster_setup.sh
```

Important: `scripts/cluster_setup.sh` currently natively rebuilds and verifies
only `throughput_test` on clients. As the first harness change, extend its
explicit client target list and executable verification to include
`kv_ycsb_bench`; do not fall back to copying a broker-host binary unless it is
proven ABI-compatible on that client. Preserve the clean-HEAD archive model.
Remote roots default to `/home/domin/Embarcadero` through
`REMOTE_PROJECT_ROOT`.

Use the topology already encoded by the throughput harness:

| client | data NIC | NUMA |
|---|---|---:|
| c4 | 100 GbE | 1 |
| c3 | 100 GbE | 1 |
| c1 | 100 GbE | 0 |

Reuse `scripts/run_multiclient.sh`'s orchestration patterns rather than
embedding ad-hoc SSH: one campaign lock at
`/tmp/embarcadero_run_multiclient.lock`, future-timestamp/NTP barrier start,
per-host NUMA binding, bounded process waits, exact PID ownership, and scoped
cleanup. Do not wrap this runner in a second `flock`. Do not use host-wide
`pkill` or broad `/dev/shm` deletion from a new driver; reuse
`scripts/lib/broker_lifecycle.sh` and delete only the campaign-owned CXL segment
after resolving its exact name.

Run `bash scripts/cluster_setup.sh --check` immediately before a campaign.
If clocks are not synchronized, use `scripts/setup/sync_clocks.sh` and record
the check. Verify that no other benchmark owns the cluster; never clear a live
lock merely because a run is slow.

## Artifact layout and reporting

Put new publication data under:

```text
data/paper_eval/ycsb/<campaign_id>/
  campaign_manifest.json
  results.csv
  trials/<cell>/<trial>/<host>/...
```

The manifest must include the exact commit and dirty flag, command/config,
host roster and NUMA nodes, binary hashes, input seeds, start/end timestamps,
trial acceptance verdicts, and hashes of raw inputs. Add a deterministic
aggregator/plotter under `PaperScripts/`; figures must be generated from the
selected clean campaign, not hand-entered values.

Primary outputs:

- aggregate completed ops/s versus remote client count;
- publish-to-apply write P50/P99 and local-read P50/P99;
- validity and applied/drained counts alongside performance;
- CPU/apply utilization if needed to identify a shared application ceiling.

Interpret the result scientifically. If all systems meet the same apply-path
ceiling, say so; that is not evidence that their ordering paths are equal. If
Corfu or Scalog wins a workload, report it. Update the paper only after the
campaign and generated manifest pass the fail-closed checker.

## Definition of done

- The plan freezes claims, workload semantics, and matrix before full runs.
- Tests validate A/F operation counts, deterministic keys, RMW semantics,
  load coordination, and final drain.
- The remote sync path builds and verifies `kv_ycsb_bench` natively on c4/c3/c1.
- One driver owns cluster lifecycle and synchronized multi-host launch.
- Every reported cell has at least three valid trials from a clean commit.
- Raw data, manifest, deterministic aggregation, and figure/table source are
  committed.
- Paper wording distinguishes application throughput from FIFO evidence and
  attributes observed ceilings to the measured component.
