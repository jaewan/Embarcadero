# YCSB Distributed KV Evaluation — Phase 0 Audit and Preregistered Plan

**Status:** Phase 0 complete (2026-07-23). No experiment code changed yet.
**Commit audited:** `eb6058b9` (clean tree; audit code-read spot-checked directly
against this commit; two research passes additionally read as far back as
`3eaadffb`, an ancestor with no relevant changes to the files in question).
**Supersedes for KV/YCSB purposes:** `docs/agent-prompts/DISTRIBUTED_KV_AGENT_PROMPT.md`
(obsolete `bench/kv_store` paths, SOSP target, over-broad matrix).
**Governing handoff:** `docs/agent-prompts/YCSB_DISTRIBUTED_KV_HANDOFF.md`.

This document answers the Phase 0 reconciliation questions from the handoff,
then freezes the preregistered matrix decisions that follow from the answers.
Every claim below is traceable to a file:line citation, verified directly
against the current tree (not inferred from comments/README prose alone).

---

## 1. Claim boundary (restated, so this doc is self-contained)

YCSB is an **appendix/optics macrobenchmark**, not a FIFO experiment. This is
already codified in `docs/experiments/E2E_KV_SMR_EVAL_PLAN.md:56,173-176` as
claim **C11 / experiment E8**:

> "Standard YCSB mixes run at competitive rates on the same log (appendix
> optics only — YCSB cannot exercise FIFO)." ... "Report ops/s + read/write
> p50/p99, Valid (counts-level) still checked. **Never cited as FIFO
> evidence.** Appendix only; include fidelity labels if comparative."

This is distinct from **E1** (`E2E_KV_SMR_EVAL_PLAN.md:70-88`), the
FIFO-critical versioned-overwrite experiment that already backs
`tab:kv-pipelined` in `Sec7_Evaluation.tex`. YCSB must not be presented as
substituting for E1, and the main paper's Q3 FIFO claim continues to rest
entirely on E1.

**Naming collision to avoid:** `PaperScripts/run_overnight_eval.sh:688-732`
already uses the bare label "E8" for an unrelated broker-CPU `pidstat`
overhead probe. This document and all YCSB artifacts refer to the YCSB
experiment as **YCSB-E8** (or "the YCSB appendix table") to avoid conflating
the two E8s across documents.

---

## 2. Phase 0 questions and answers

### Q1 — Are A, B, F canonical? Is F's RMW atomic?

A and B are simple write-ratio presets (`kv_bench_main.cc:190-197`, A:
write_ratio=0.5 uniform; B: write_ratio=0.05 uniform) dispatched through the
generic write/read path (`kv_bench_main.cc:776-843`): writes via
`store.publishBatch()`, reads via `store.get(key)`. This is standard-shape
YCSB A/B.

**F's RMW is *not* atomic and is *not* a single logged operation.** It is a
local read (through the read-your-writes barrier, see Q4) followed by an
independent `put()` that appends a new, separate log entry
(`distributed_kv_store.cc:784-798`):

```cpp
size_t DistributedKVStore::readModifyWrite(const std::string& key, size_t value_size) {
    std::string current = this->get(key);
    static const std::string kSuffix = "|rmw";
    std::string new_val = current + kSuffix;
    if (value_size > 0 && new_val.size() > value_size) new_val.resize(value_size);
    return put(key, new_val);
}
```

There is no CAS, no version check on write-back, and no single log record
capturing read+write as one unit. Two concurrent RMWs on the same key from
different sessions can both read the same pre-image and independently append
non-conflicting-looking writes — a classic lost update — with nothing in this
path to detect or prevent it. The in-code comment
(`distributed_kv_store.cc:786-788`) documents *why* the read goes through the
barrier (to avoid reading stale pre-log-apply state), but does not address
the separate cross-session race.

**Disposition for this matrix:** the preregistered placement (Section 4) runs
exactly one client process per host with disjoint key ranges (`--key_offset`,
see Q3), so no two processes ever RMW the same key concurrently in this
specific experiment — the race is real in the implementation but not exercised
by this matrix's design. **The paper must describe workload F as "local
read-modify-write, not an atomic/CAS operation, measured under
single-writer-per-key placement"** — never as "atomic RMW" or "linearizable
compare-and-swap."

### Q2 — Zipfian generator: correctness, determinism, shared draws

`ZipfDistribution` (`kv_bench_main.cc:49-110`) builds a standard `1/k^theta`
PMF, normalizes to a CDF, and samples by inverse-CDF binary search — a
correct, non-naive implementation. For keyspaces above 1,000,000 entries the
table is built over 1M buckets and linearly interpolated back to the full
range (`kv_bench_main.cc:78-81`) — an approximation, but out of scope at this
matrix's record counts (≥1M is the *floor* per the handoff, so this must be
checked against the actual record count chosen; see Section 6 open item).

**Determinism:** the whole benchmark shares one `std::mt19937_64 rng(42)`
(`kv_bench_main.cc:505`, confirmed by direct read — hardcoded literal, no
variable). There is **no `--seed` CLI flag anywhere in the file** (confirmed
by grep: zero matches for "seed" in `kv_bench_main.cc`). Because op/key/value
generation depends only on CLI parameters and this fixed seed, not on runtime
log feedback, **every system sees the identical op/key/value sequence** for a
given flag set — this is the property `README_SMR_FIFO.md:118-119` documents
and it is what makes cross-system comparison valid.

**Consequence that must be stated in the paper/plan, not glossed over:**
because the seed is fixed and unparameterized, **repeating the same trial N
times draws the identical sample sequence every time.** Trial-to-trial
variance in the reported numbers reflects only system/scheduling timing
noise, not independent sampling variance. This satisfies the handoff's "at
least three independent successful trials, median and min-max" requirement
(these are independent *executions*), but reviewers should not read the
across-trial spread as sampling-distribution evidence. Recommended harness
hardening (non-blocking for the first matrix): add an optional `--seed` flag
so a future sensitivity check can vary the draw; not required to ship the
preregistered matrix below.

### Q3 — Preload/load phase: single owner + barrier, or duplicate writes?

**For standard A–F workloads there is no load-owner or barrier mechanism at
all.** The load phase (`kv_bench_main.cc:508-566`) runs unconditionally, once
per process, unconditionally writing keys `k0..k(record_count-1)`. There is
no `--skip_load`, no readiness check, no environment-based owner gate. If two
plain processes were pointed at one shared topic today, both would
independently write the full keyspace into the same log — duplicate load
writes contaminating the timed run.

The only existing disjoint-keyspace/ownership primitives are gated behind
`--fifo_valid` and were built for the Q3 FIFO harness, not YCSB:
- `--key_offset` (rejected unless `--fifo_valid` is set,
  `kv_bench_main.cc:413-418`, confirmed by direct read) gives a session a
  disjoint key range.
- `--manage_cluster` designates exactly one process to create/tear down the
  topic (`kv_bench_main.cc:147-148`; destructor `distributed_kv_store.cc:203-209`).
- The shell-level pattern in `run_smr_fifo_eval.sh:320-380` assigns session 0
  `--manage_cluster=1` and gives every session `--key_offset = s *
  record_count`, then gives session 0 a **fixed `sleep 3` head start**
  (`run_smr_fifo_eval.sh:379`) before other sessions launch — this is a delay,
  not a polled readiness barrier. It works there because keyspaces are
  disjoint per session (no dependency on load-order between sessions), not
  because of the sleep.

**Decision for this matrix:** reuse exactly this pattern — `--key_offset`
gives every remote client process (whether n=1, 2, or 3) a disjoint key range
sized to `record_count`, and `--manage_cluster=1` is set on exactly one
process (the first-launched client). This sidesteps the duplicate-load
problem without needing new code: it is "disjoint owners load concurrently,"
not "one owner loads, others wait," and that is sufficient because no process
ever touches another's key range. **Do not use a fixed sleep as the
readiness gate for the *timed* window** — the harness's existing
push-ready/go barrier in `run_multiclient.sh:2115-2192` (see Q6/Section 5)
already provides a polled, synchronized start; the new driver must use that
for the timing barrier and use `--key_offset`/`--manage_cluster` only for
load-phase safety.

### Q4 — Read consistency: local view + read-your-writes barrier

Reads are served from a purely local, per-process `ShardedKVStore`
(`distributed_kv_store.h:76-247`), mutated only by the single apply thread
(Q7). `get()` (`distributed_kv_store.cc:743-749`) calls
`waitForSyncWithLog()` first, which flushes the publisher, captures
`target = publisher_->GetNextPublishOrder()` (this client's own publish
count at that instant), and spins until `applied_local_ops_ >= target`
(30s no-progress timeout, `distributed_kv_store.cc:652-684`). This is a
genuine **read-your-own-writes** barrier: a `get()` can never return state
older than this same process's own prior writes. It says nothing about
cross-client visibility latency (not needed for YCSB's single-writer-per-key
placement) and nothing about global linearizability (not claimed).

**Performance caveat to carry into the report:** because `target` is
recomputed fresh at the moment of the call, a read immediately following a
write always blocks on that specific write's apply — for read-heavy mixes
(B: 95% read) interleaved with occasional writes, each write forces the next
read to stall on apply-catchup rather than proceeding purely pipelined. This
is a property of the harness's synchronization design, not of the log
backend, and should be described as such if read-latency tails are reported.

### Q5 — Does the metric include apply-drain? Is publish→apply latency measured separately?

**Yes to both.** The reported throughput stops the clock only after a final
`store.waitForSyncWithLog(pending_opid)` drains every published op to applied
(`kv_bench_main.cc:857-867`), matching `README_SMR_FIFO.md:66-67`'s documented
design ("clock stops only after the final apply barrier"). The validity
checker separately cross-checks `applied_entries == expected_applied_entries`
(`kv_bench_main.cc:872-901`), so a premature clock-stop would also fail
validity — the drain requirement is enforced twice, not just documented.

Two distinct latency vectors exist: `write_latencies_us` measures only the
publish-call duration (client-side enqueue cost), while `apply_lats`
(`distributed_kv_store.cc:770-775`, fed by `completePendingLocalOp()` at
`:525-540`) measures **publish-to-apply** latency specifically, gated behind
`--latency`/`track_latency_`. The plan in Section 5 reports the
publish-to-apply figure as the primary write-latency number, per
`benchmarks/README.md:43`'s own recommendation for cross-system comparison.

### Q6 — Multi-process isolation: disjoint IDs/dirs OK; cluster teardown is mixed

Per-process identity (`server_id_` from `publisher_->GetClientId()`,
`distributed_kv_store.cc:159`), result directories, and `--shared_topic`
(waives the exact-store-size check, `kv_bench_main.cc:891`) already support
multiple processes coexisting on one topic. **Cluster lifecycle is where the
three existing shell drivers diverge sharply:**

- `run_smr_fifo_eval.sh` and `run_kv_baseline_compare.sh` both **guard**
  `broker_local_cleanup()` with a live-process check
  (`run_kv_baseline_compare.sh:100-112`, `run_smr_fifo_eval.sh:103-129`) and
  scope SHM cleanup to an exact randomized name, never a UID-wide wildcard
  (`run_smr_fifo_eval.sh:87-99`, matching the documented rule in
  `README_SMR_FIFO.md:262-267` and the [[moscxl-shared-host-hazard]] memory
  note).
- **`scripts/run_ycsb_eval.sh` is genuinely unscoped and unguarded**
  (`run_ycsb_eval.sh:29-45`, verified by direct read): its `cleanup_kv_cluster()`
  runs unqualified `pkill -x` against `kv_ycsb_bench`, `embarlet`, and all
  three sequencer binaries with **no live-cluster pre-check**, on both the
  `EXIT` trap and between every cell, plus a wildcard
  `rm -f /dev/shm/CXL_SHARED_FILE*`. This script will tear down any other
  session's cluster on the same host. **It must not be extended or reused for
  the distributed driver.**
- `scripts/lib/broker_lifecycle.sh`'s `broker_local_cleanup()`
  (`:752-785`) itself kills **by exact process name, host-wide within that
  name set** (`embarlet throughput_test corfu_global_sequencer
  lazylog_global_sequencer scalog_global_sequencer` plus mailbox variants,
  `:776-782`) — it does not include `kv_ycsb_bench` today, and it has no
  concept of "my campaign's PIDs" vs. anyone else's. Adding `kv_ycsb_bench`
  to this allowlist would inherit the same host-wide-by-name hazard for any
  co-tenant running the same binary.
- `run_multiclient.sh`'s own remote-client teardown avoids this entirely: it
  tracks the **exact remote PID** per client in a PID file
  (`run_multiclient.sh:2010,2100`, `CLIENT_REMOTE_PID_HOSTS`/`_FILES`) and
  kills only that PID (`:1329-1335`), explicitly commented "Never use a broad
  remote `pkill -f throughput_test`: remote machines are shared by
  independent experimenters" (`:1289-1290`).
- Counter-example already in-tree to avoid copying: `run_overnight_eval.sh`'s
  `cleanup_remote_stray_procs()` (`:283-291`) does `ssh $host 'pkill -x
  throughput_test'` as an outer safety net — this is exactly the host-wide
  pattern the handoff forbids in a new driver.

**Decision:** the new distributed YCSB driver launches `kv_ycsb_bench` through
`run_multiclient.sh`'s exact-PID-file remote-launch/teardown mechanism (or an
equivalent extension of it), never through `run_ycsb_eval.sh`'s pkill-based
lifecycle, and never by adding `kv_ycsb_bench` to
`broker_lifecycle.sh`'s name-based allowlist.

### Q7 — Is there a single apply-thread ceiling?

Confirmed: exactly one thread is ever pushed into `log_consumer_threads_`
(`distributed_kv_store.cc:168`, `distributed_kv_store.h:288`) — no other call
site adds a second. `logConsumer()` (`:544-596`) applies every entry serially
in log order; the `ShardedKVStore`'s 64 shards
(`distributed_kv_store.h:76-247`) parallelize concurrent *readers* only, not
the single writer. **This one thread is the ceiling for every sequencer
backend measured through this harness.** Any KV-level throughput difference
across Embarcadero/Corfu/Scalog measured via `kv_ycsb_bench` is bounded by
this thread's serial decode+mutate rate and must never be attributed to the
log backend's own ingest capability without first checking whether this
ceiling was reached. **Reporting requirement:** the campaign must record
apply-thread CPU utilization per cell precisely so this can be checked before
any comparative claim ("Corfu is faster than Scalog at KV writes") is made;
if all systems hit the same apply ceiling, the plan reports that explicitly
as a shared harness bound, not a log-ordering result (per the handoff's
"Interpret the result scientifically" section).

### Q8 — Reconcile `kv_bench_main.cc` vs. `benchmarks/README.md`

A–F **are** implemented in `kv_bench_main.cc` (preset dispatch
`:184-217`, run-phase dispatch `:700-855`, including D's synthetic-insert
"latest" logic and E's scan-length-in-[1,100] logic). `benchmarks/README.md`
currently states (lines 7, 15, 57) that this is "not yet a full canonical
YCSB implementation" and that a separate `bench/kv_store/ycsb_workload.h`
"exists... but is not wired in." **That file does not exist anywhere in the
tree or git history** (checked both `bench/` and `benchmarks/` paths). Git
history resolves the contradiction: `benchmarks/README.md` was last edited by
`f1eb6221` (2026-07-06); A–F support was added four days later by `ae905ee3`
"feat(ycsb): implement workloads A-F, Zipf dist, scan, RMW; fix Scalog RF=1
stall" (2026-07-10). **The README is stale, not the code — it describes a
pre-`ae905ee3` snapshot.** `benchmarks/README.md` will be corrected in
Section 7 below (after this document's semantics are validated by the tests
in Section 3, per the handoff's instruction not to fix docs before validating
semantics).

Separately, `scripts/run_ycsb_eval.sh` is confirmed **local-only** (hardcoded
`EMBARCADERO_HEAD_ADDR=127.0.0.1`, `:63`), **RF0**
(`EMBARCADERO_REPLICATION_FACTOR=0`, `--rf 0`, `:65,226`), performs the
host-wide cleanup described in Q6, and launches exactly one `kv_ycsb_bench`
process per trial with **no multi-host synchronization capability of any
kind** — it is not a candidate to extend into a distributed driver; it should
remain (with its cleanup left as-is, out of scope) as the existing local-only
sweep tool it already is.

---

## 3. Baseline-fidelity and contract constraints (binding on the new driver)

- **RF/ACK vocabulary** (`docs/contracts/ACK_RF_CONTRACT.md:5-32`): RF
  includes the primary; ACK2 is valid only for RF≥2 and requires every RF−1
  disk replica media-durable. A DRAM-copy replica sink is a *contractually
  valid* ACK2 implementation but must be labeled **"DRAM replica completion"**
  in every table/figure, never "durable" (`Paper/Text/Sec7_Evaluation.tex:53-60`).
- **Sink parity across systems is not uniform.** Embarcadero and Scalog both
  have working DRAM-copy and disk-durable sinks. Corfu has a disk-durable
  chain plus a memory-copy-sink knob (`corfu_uses_memory_copy_sink()`,
  `run_multiclient.sh:352-373`). **LazyLog has no faithful DRAM-only sink at
  all** — durability is intrinsic to its append-ACK contract
  (`docs/contracts/lazylog_metadata_replica_contract.md`,
  `Sec7_Evaluation.tex:99-101`).
- **LazyLog decision: omit from the YCSB read/write matrix.** Its pre-binding
  append path is faithful for append-ACK only; reads require the binding
  round, and both `Sec7_Evaluation.tex:33-36,96-101` and
  `Paper/Text/Appendix.tex:106-151` state its paced delivery run fails the
  common end-to-end checker for exactly that reason — the same reason
  `E2E_KV_SMR_EVAL_PLAN.md` already withholds LazyLog's Pipe/read-dependent
  row while keeping only its append number. Since YCSB needs both reads and
  writes, LazyLog is excluded, matching the handoff's own "omit or explicitly
  label" instruction.
- **Porting-rule invariant** (`docs/baselines/porting_rule.md:53-73`): no
  baseline change may decouple ordering from durability or otherwise import
  Embarcadero's architectural move into a baseline. The KV harness must not
  "fix" a baseline's serialization behavior to make its YCSB numbers cleaner —
  that would be a forbidden redesign, not a harness improvement. Any baseline
  touch during this work goes through the 5-question decision test at
  `porting_rule.md:213-226` first.
- **Scalog read frontier** (`docs/design/scalog_canonical_progression_contract.md:15-32`):
  export/read must never return past the `ordered` frontier (post-global-cut),
  not `written`. A YCSB read against Scalog is only a valid materialized-view
  read if it respects this — confirmed already enforced inside the
  subscriber/apply path the KV bench uses (same path SCALOG ORDER=1
  throughput/latency runs already validate per project memory), not
  reimplemented by the KV bench itself.
- **Corfu provenance** (`docs/contracts/CORFU_INVARIANT_LEDGER.md:78-81`,
  invariant C10): every paper-visible Corfu row must record transport
  (grpc/mailbox), token policy, RF, ACK, and batching policy. The YCSB
  campaign manifest carries the same fields for Corfu cells as the existing
  throughput harness does.

---

## 4. Preregistered matrix (frozen)

| Axis | Value |
|---|---|
| Systems | Embarcadero ORDER5, CXL-Corfu, CXL-Scalog. LazyLog omitted (Section 3). |
| Workloads | YCSB A (50/50 read/update), YCSB F (read-modify-write). B added only if A/F complete with runway remaining. |
| Key distribution | Zipfian, theta=0.99, fixed seed=42 (shared across all systems by construction, Section 2/Q2). |
| Record count | ≥1,000,000 keys total across all client processes (per-client disjoint range via `--key_offset`, sized `record_count / num_clients` per process so the aggregate keyspace is constant across the n=1/2/3 scaling points — **not** `record_count` per process, to keep total loaded data comparable across placements). |
| Value size | 100 bytes. |
| Operation count | Sized so the timed window runs tens of seconds at the achieved rate per cell (determined during the smoke gate, Section 5, before the full matrix). |
| Replication | RF2/ACK2, DRAM replica completion, labeled as such everywhere (never "durable"). |
| Placement | 1, 2, 3 remote client processes on c4, c3, c1 respectively (co-located client excluded from the primary scaling curve, per handoff). |
| Brokers | 4 on moscxl, broker address `10.10.10.10`. |
| Trials | ≥3 independent executions per cell; report median and min–max. Retries only for preregistered infra failures, logged in the manifest regardless of outcome. |
| Fixed across all cells | batch size, publisher threads, warmup, record count, operation count, seed, broker count, sink, RF/ACK, client placement — identical for every system/workload/placement combination. |

**Open item carried forward, not blocking:** exact operation count and batch
size are determined empirically during the Section 5 smoke gate (the
handoff requires "tens of seconds at the achieved rate," which depends on
the achieved rate discovered in gate step 2/3) and recorded back into this
document once fixed, before the full matrix (gate step 5) is launched.

---

## 5. Gate sequence (must pass in order, matching the handoff)

1. Unit/standalone deterministic workload tests (Section 6: A/F op-count
   accounting, deterministic keys given seed=42, RMW semantics, load/offset
   coordination, drain-inclusive timing).
2. One-process local smoke, each system.
3. One remote process on c4, each system.
4. Two remote processes (shared topic, `--key_offset` disjoint ranges,
   `--manage_cluster` on exactly one), each system.
5. Full matrix (Section 4), only after 1–4 pass with fail-closed acceptance
   (handoff §"Validity and fail-closed acceptance") on every trial.

---

## 6. Harness hardening required before gate step 2 (this document's action items)

1. **`scripts/cluster_setup.sh`** currently only natively rebuilds/verifies
   `throughput_test` on clients — `REQUIRED_BINS=(embarlet throughput_test
   corfu_global_sequencer)` (`:52`, confirmed by direct read) omits both
   `scalog_global_sequencer` and `kv_ycsb_bench`, even though the script's own
   die-message (`:74`) already suggests building `kv_ycsb_bench`, and the
   `kv_ycsb_bench` CMake target already exists
   (`benchmarks/kv_store/CMakeLists.txt:61-69`). Required change: add
   `kv_ycsb_bench` to `REQUIRED_BINS`, duplicate the native-rebuild block
   (currently `:130-156`) and the `verify_client_binary_runs()` bad-option/grep
   pattern (currently `:159-174`) for `kv_ycsb_bench`. The verification
   mechanism (uncaught `cxxopts` exception on an unrecognized flag, grepped
   for `"no_such_option\|does not exist"`) transfers unmodified since
   `kv_ycsb_bench` parses options the same way
   (`benchmarks/kv_store/kv_bench_main.cc:1178`, no try/catch around
   `options.parse`). Do not fall back to copying the broker-host binary
   unless proven ABI-compatible, matching the existing rule for
   `throughput_test`.
2. **Clock sync is not gated by `cluster_setup.sh --check`.** No clock/NTP
   reference exists anywhere in `cluster_setup.sh` (confirmed by grep — zero
   matches). `scripts/setup/sync_clocks.sh` is a separate, manual,
   password-prompting script never invoked by `cluster_setup.sh`. The only
   in-band skew detector is reactive — a post-barrier >2000ms-miss warning
   inside `run_multiclient.sh:2091-2099` — which fires *after* a trial has
   already launched. **Action:** run `sync_clocks.sh` (or a lighter read-only
   `chronyc tracking` offset check) as an explicit, separate preflight step
   before the campaign, immediately after `cluster_setup.sh --check`; do not
   rely on `--check` to catch skew.
3. **New driver location and reuse contract:** per `PaperScripts/README.md:16-24,74-78`,
   the new distributed YCSB driver belongs under `PaperScripts/` (paper-honest
   defaults, `data/paper_eval/` output) and must call into
   `scripts/run_multiclient.sh` / `scripts/lib/broker_lifecycle.sh` /
   `scripts/cluster_setup.sh` rather than reimplement their orchestration.
   Concretely it reuses: the `/tmp/embarcadero_run_multiclient.lock`
   `flock -n` campaign lock (`run_multiclient.sh:37-43` — do not nest a second
   `flock`), the millisecond barrier-start mechanism (`:1944-1954,2091-2099`),
   the push-ready/go secondary barrier (`:2115-2192`) for the load-phase-done
   signal needed in gate step 4 (replacing `run_smr_fifo_eval.sh`'s fixed
   `sleep 3` with this polled mechanism), per-host NUMA binding
   (`resolve_client_numa()`, `:774-792`), exact-PID-file remote process
   tracking/teardown (`:2010,1289-1335`), scoped CXL shm cleanup
   (`shm_cleanup()`, `:1247-1265`), the dirty-commit guard
   (`:1867-1876`), and the existing `run_contract.csv`/`attempt_summary.csv`
   provenance rows as the basis the new `campaign_manifest.json` wraps rather
   than re-derives. Since `run_multiclient.sh` currently only launches
   `throughput_test` (confirmed by grep — zero mentions of `kv_ycsb_bench` in
   the file), the new driver either (a) adds a `kv_ycsb_bench` launch mode to
   `run_multiclient.sh` behind an explicit flag, reusing all the above
   primitives, or (b) is a new script under `PaperScripts/` that sources
   `scripts/lib/broker_lifecycle.sh` directly and re-implements only the
   client-launch loop against the same primitives. Given the size of
   `run_multiclient.sh` (2411 lines) and the risk of destabilizing the
   throughput-test paper figures that already depend on it, **(b) is the
   lower-risk choice** and is what this plan adopts; it must still acquire the
   same lock file rather than double-`flock`.

---

## 6a. Section 6 item 1 — resolved (2026-07-23)

`cluster_setup.sh` now builds and ABI-verifies `kv_ycsb_bench` on c4/c3/c1
exactly as it does `throughput_test` (`REQUIRED_BINS`/`CLIENT_LAUNCH_BINS`,
`scripts/cluster_setup.sh:52-57`). Getting there required two real code
fixes, not just a script extension — both landed and validated live against
the (idle, confirmed via lock-file check) c4/c3/c1 cluster:

1. **`kv_ycsb_bench` was missing the GLIBC compatibility shim.** Unlike
   `throughput_test` (`src/CMakeLists.txt:152`), `kv_ycsb_bench` never linked
   `common/compat_isoc23.cpp`, so a moscxl-built binary required GLIBC 2.38
   (`__isoc23_strtoul` and friends) that none of c4/c3/c1 (Ubuntu 22.04,
   GLIBC 2.35) have. Fixed in `benchmarks/kv_store/CMakeLists.txt` by adding
   `../../src/common/compat_isoc23.cpp` to `KV_CLIENT_SOURCES` and mirroring
   `throughput_test`'s `BUILD_RPATH_USE_ORIGIN`/`INSTALL_RPATH="$ORIGIN"`
   target properties. Confirmed via `objdump -T` before/after: max required
   `GLIBC_2.38` → `GLIBC_2.34`, matching `throughput_test`'s own ceiling.
2. **`cluster_setup.sh`'s native-rebuild step unconditionally wipes
   `CMakeCache.txt`** before every client rebuild, and without a
   `CMAKE_PREFIX_PATH` hint this lets `find_package(glog)`/`find_package(yaml-cpp)`
   resolve to whatever system package happens to be installed. On c4 the
   system glog is 0.4.0 (lacks the VLOG3 symbols the codebase links against)
   while a compatible vendored copy already sits at `third_party/glog-0.6`
   (unused because it isn't on CMake's default search path) — this most
   likely already silently broke `throughput_test`'s native rebuild too,
   masked by its working fallback-to-broker-binary path. Fixed by adding a
   conditional `CMAKE_PREFIX_PATH` hint pointing at
   `third_party/glog-0.6`/`third_party/yaml-cpp-0.8` when present
   (`scripts/cluster_setup.sh`, inside the native-rebuild heredoc) — a no-op
   on hosts (c1, c3) that lack those directories and already resolve
   correctly.

**Live validation result:** c4 now builds both `throughput_test` and
`kv_ycsb_bench` fully natively. c1's native rebuild fails on a *different*,
separate issue: its system `libyaml-cpp.so` itself requires `GLIBCXX_3.4.31`
that c1's own GCC 11 toolchain can't satisfy when linking natively (a
pre-built dependency problem, not a `find_package` resolution problem —
`third_party/` doesn't exist on c1 at all, so there was no vendored copy to
prefer). In both fallback cases the broker-binary copy now passes ABI
verification for both binaries, because of fix #1 above.

**c3's fast (~3s) native-rebuild failure — root-caused 2026-07-23, one layer
fixed, one left open.** `/tmp/embar_client_cmake.log` on c3 showed the
reconfigure itself failing immediately: c3's CMake is new enough to reject
grpc's vendored `c-ares` (`cmake_minimum_required` below 3.5, which modern
CMake refuses outright). Fixed by adding `-DCMAKE_POLICY_VERSION_MINIMUM=3.5`
to the reconfigure line in `scripts/cluster_setup.sh`. Re-validated live:
this error is completely gone and configure now progresses much further
(past libsystemd/Unwind into folly's own config), but hits a **different,
unrelated** fourth issue — c3's Boost 1.90.0 install doesn't ship a
`boost_systemConfig.cmake` (modern Boost made `system` header-only, so
folly's `find_package(Boost COMPONENTS system)` config-mode lookup fails).
Not fixed; left as a follow-up, since the fallback path is fully verified
working for c3 regardless and chasing per-host Boost/folly toolchain drift
further is open-ended, unbounded work relative to what this section needs.

Net result: `kv_ycsb_bench` is now exactly as reliably deployable across
c4/c3/c1 as `throughput_test` already was — native build on c4, verified
working fallback on c3/c1.

**Q3's key_offset gap — found and fixed 2026-07-23.** While building the
Section 6 semantic tests below, discovered that `--key_offset` only offset
the *load* phase (`kv_bench_main.cc:546,574`) for standard workloads —
the actual read/write/RMW dispatch for A/B/C/F never added it, so this
document's own Q3 "decision" (give each client a disjoint `--key_offset`
range) would have silently broken: sessions would load into disjoint ranges
but then read from `[0, record_count)`, missing their own data. Fixed by
adding `cfg.key_offset +` at the three `makeKey()` call sites following
`sampleKey()` in the standard-workload dispatch (F's read/RMW, A/B/C's write
batch, A/B/C's read), relaxing the CLI gate to allow `--key_offset` without
`--fifo_valid` for workloads A/B/C/F while continuing to reject it for D/E
(whose `scan()`/`latest_insert_idx` interaction with a nonzero offset is
still unverified and out of scope for this matrix). The `fifo_valid` branch
is structurally untouched and mutually exclusive with the modified code
path, so this cannot regress the E1 FIFO experiment.

**Live validation:** `benchmarks/kv_store/test_ycsb_key_offset.sh` (new,
committed) runs two concurrent `kv_ycsb_bench` processes on a 4-broker local
cluster, disjoint `--key_offset` ranges (0 and 2000), `--shared_topic`,
`--manage_cluster=1`/`0`. Confirmed passing: both sessions report
`valid=YES` with correct op-count accounting (`1969 writes + 2031 reads =
4000` each), and session 1's `store_size=4000` correctly reflects it
materializing both sessions' data via the shared topic. This is the exact
load-coordination model the Task 4 distributed driver depends on, and it is
now proven rather than assumed. Note for that driver: the second-launched
session consistently incurs a ~60s stall on a secondary "cluster status
stream" connection (`Connection refused` on a fixed port) before proceeding
anyway — non-fatal (the run still reaches `valid=YES`), but adds real
latency per additional concurrent client and should be accounted for in the
gate-4 and full-matrix timing budget.

**Determinism — proven via content digest, not just op-counts (2026-07-23).**
`benchmarks/kv_store/test_ycsb_determinism.sh` (new, committed) runs the
identical single-process workload twice, each against a fresh cluster/CXL
segment, and captures `DistributedKVStore::stateDigest()` (an
order-independent commutative hash over every key/value pair,
`distributed_kv_store.h:226-238`) via an independent `--replica` process
(`runReplica()` is not gated on `fifo_valid`, unlike the writer's own digest
path, so it works for any workload). Result: **identical digest
(`cca80212f1fc671c`) and identical op-count breakdown across two fully
independent process/cluster instances**, confirming not just that op-count
statistics match but that every key resolved to the same final value in both
runs — direct empirical confirmation of the Section 2/Q2 fixed-seed
determinism claim.

**RMW/workload F — validated under the matrix's actual placement model.**
`test_ycsb_key_offset.sh` was parameterized with `WORKLOAD=A|F`; re-run with
`WORKLOAD=F` under the same two-session disjoint-`--key_offset` model:
`PASS`, both sessions `valid=YES`. This confirms F's read/RMW dispatch works
correctly under single-writer-per-key placement (the only placement this
matrix uses), without needing to exercise the documented cross-session RMW
race (Q1) — which stays out of scope, matching the matrix's own design.

Section 6 semantic testing is now complete: op-count accounting,
deterministic keys, RMW semantics, load coordination, and drain-inclusive
timing are all validated live, not just reasoned from code.

## 6b. Section 6 item 3 — distributed driver built (2026-07-23)

`PaperScripts/run_ycsb_distributed.sh` (new, committed): launches
`kv_ycsb_bench` across 1-3 remote clients (c4, c3, c1 in order) against a
local moscxl broker cluster. Supports EMBARCADERO/CORFU/SCALOG (LazyLog
excluded per Section 3); gives each client a disjoint `--key_offset` range
(`RECORD_COUNT_TOTAL / NUM_CLIENTS`), `--shared_topic` when `NUM_CLIENTS>1`,
`--manage_cluster=1` on client 0 only. Reuses the exact primitives audited in
Section 6 item 3 rather than reinventing them: the same
`/tmp/embarcadero_run_multiclient.lock` campaign lock `run_multiclient.sh`
uses (no nested flock), its NUMA-pinning convention (c4/c3→1, c1→0), its
millisecond spin-wait barrier construction (`%s%N/1e6`, not `%3N` — the c3
uutils-`date` bug that silently degenerated N>=2 overlap for months), its
exact-PID-file remote teardown (never a broad remote `pkill`), and a
dirty-commit guard. Self-reviewed before running against real hardware and
fixed two issues found: dead/unused code, and a real gap where the normal
(successful) exit path skipped `remote_teardown()` — only the abnormal-exit
trap called it — which would have left stale PID files on every remote
client host after every clean run. Fixed by routing both paths through the
same `cleanup()` function, matching `run_multiclient.sh`'s own convention of
one idempotent cleanup function called on any exit.

## 6c. Gate step 3 — first real remote run, and a provenance gap it exposed (2026-07-23)

First live run: `NUM_CLIENTS=1 SYSTEM=EMBARCADERO WORKLOAD=A CLIENT_NODES_CSV=c4`,
RF=1/ACK=1 for speed. Result: **fully successful** — c4 connected to the
broker over the real network fabric (`10.10.10.10`), completed load+run with
correct op-count accounting (`1969 writes + 2031 reads = 4000`) and
`applied_entries=5969/5969 valid=YES`. This is the first proof the driver's
actual mechanics (NUMA binding, millisecond barrier, real-network broker
address, remote SSH launch) work end-to-end on real hardware, not just
locally.

Two more bugs found while checking the result, both fixed:

1. **The driver's own per-client summary loop silently aborted** without
   printing anything, due to `set -e -o pipefail` plus a `grep` that found no
   match: the pattern hardcoded a specific source line number
   (`kv_bench_main.cc:1014`) from glog's `__FILE__:__LINE__` output, which
   shifts every time earlier code in the file changes — exactly what the
   key_offset fix (Section 6/6a) did. Fixed by matching `kv_bench_main\.cc:\d+`
   (any line number) plus `|| true` after every such grep, in both
   `run_ycsb_distributed.sh` and `test_ycsb_determinism.sh`.
2. **`test_ycsb_determinism.sh`'s core comparison had a latent vacuous-pass
   risk**: the same brittle pattern fed `ops1`/`ops2`, and if grep ever
   matched nothing on *both* sides, `"" != ""` is false — the test would
   report PASS without having compared anything. (It did not actually
   misfire in the runs already reported in Section 6a, since line 1014 was
   still correct for the local binary at the time, but the risk was real and
   is now closed.) Fixed by explicitly failing if either side is empty
   rather than letting empty-equals-empty read as success.

**A more consequential finding, caught by checking rather than assuming:**
verifying gate step 3 actually exercised the fixed code required checking
c4's *synced* source tree directly — and it did not have the fix. Per
`cluster_setup.sh`'s own documented design (Section 6/6a; also the handoff's
"never develop from an uncommitted overlay" rule), the source-tree sync uses
`git archive HEAD`, which only exports **committed** content. Nothing from
this session has been committed, so c4's natively-rebuilt `kv_ycsb_bench`
still ran the pre-key_offset-fix code — gate step 3 didn't catch this because
`--key_offset=0` (the only value a single-client run ever uses) is a no-op in
both the old and new code paths, and `valid=YES` does not distinguish "read
the right keys" from "read the wrong keys that happened to still be
present" (Q1's own point about the RMW race applies structurally here too:
count-based validity does not imply key-level correctness).

By contrast, the **binary fallback path** (`rsync $LOCAL_BIN/$bin` to a
client's `build/bin/`, used automatically when a native rebuild fails, e.g.
on c3/c1) copies the actual local build *artifact*, not source — so it
reflects whatever is currently built locally regardless of commit status.
This is a real, useful distinction: source-tree provenance is deliberately
commit-gated (correct, for the eventual publication campaign); build-artifact
copies are not.

**Resolution for gate-testing (not yet the real campaign):** manually
`rsync --checksum`'d the current local `build/bin/kv_ycsb_bench` (confirmed
via `strings | grep -c "scan-based workloads are not offset-safe"` to
contain the fix) directly to c4's and c3's `build/bin/`, verified with the
same bad-option ABI smoke check `cluster_setup.sh` uses. This is the same
class of action `cluster_setup.sh`'s own fallback already performs
automatically — not a new risk, just done manually here — and is a
deliberate, temporary shortcut for gate smokes only. **Before Task 7 (the
real preregistered matrix), this session's changes must be committed** so
`cluster_setup.sh`'s normal native-build+`git archive` path naturally carries
the fix with correct, reproducible provenance; running the actual campaign
against a manually-copied binary would violate the handoff's clean-commit
requirement and must not happen.

## 6d. Gate step 4 — two remote clients, shared-topic, real hardware (2026-07-23)

`NUM_CLIENTS=2 SYSTEM=EMBARCADERO WORKLOAD=A CLIENT_NODES_CSV=c4,c3` (RF1/ACK1
smoke), after copying the fixed local binary to c3 as well (same procedure as
Section 6c). Result: **PASS**. Directly inspected both client logs rather
than trusting only the driver's own summary line:

- client 0 (c4, `key_offset=0`, `manage_cluster=1`): `Ops: 1969 writes + 2031
  reads = 4000`, `Store size: 4000 applied_entries=3969/3969 valid=YES`
- client 1 (c3, `key_offset=2000`, `manage_cluster=0`): identical op-count
  breakdown, `Store size: 2000 applied_entries=3969/3969 valid=YES`

This exactly reproduces the local (single-host) key_offset test's pattern
(Section 6a/6b) — client 0's store size reflects both sessions' combined data
via the shared topic while client 1's reflects its own view at print time —
now confirmed on two independent real remote hosts using the actual fixed
binary, not a stale one. The disjoint `--key_offset` load-coordination model
Task 4's driver depends on is proven correct end-to-end, not just locally.

**One more bug found and fixed while checking this run**: the driver's
per-client summary printed `<no Ops line found>` for both clients despite the
run genuinely succeeding (`valid=YES`, confirmed by direct log inspection).
Root cause: this host's `grep` is `ugrep`, which treats a *variable-length*
lookbehind assertion (`(?<=kv_bench_main\.cc:\d+\] )` — variable because
`\d+` isn't a fixed count) as a hard regex error, not "no match" — silently
swallowed by the `|| true` guard added in Section 6c, which fixed the
abort-on-no-match problem but not this one. Fixed by replacing the lookbehind
with `\K` (`kv_bench_main\.cc:\d+\] \KOps:.*`), which has no such
fixed-length restriction; verified directly against ugrep before reapplying.
This did not affect either gate step's actual pass/fail verdict, since that
gate is `valid=YES`/exit-code based, not the cosmetic `ops_line` — but it
underscores why every result in this document has been cross-checked against
raw log content rather than trusted from a driver's own summary output alone.

**Status: gate steps 1-4 complete, and all Task 7 prerequisites now closed
(2026-07-23).**

- This session's changes committed (`88a073d5`, `c18fdd67`); re-ran
  `cluster_setup.sh` against the clean commit and directly verified (not just
  trusted the log) that c4's natively-rebuilt `kv_ycsb_bench` now contains
  the key_offset fix via the committed `git archive` path — the manual
  binary-copy workaround from Section 6c/6d is no longer needed or used.
- `scripts/setup/check_clock_sync.sh` (new, committed): read-only
  `chronyc tracking` check, no sudo/reconfiguration. c4/c3/c1 all under 3ms
  of the NTP source — well within the 50ms default threshold.
- CORFU and SCALOG gate smokes (`NUM_CLIENTS=1`, c4, RF1/ACK1): both **PASS**,
  matching EMBARCADERO's result exactly (`1969 writes + 2031 reads = 4000`,
  `valid=YES`). All three systems now confirmed working through the
  distributed driver on real hardware, not just EMBARCADERO.

Nothing outstanding blocks Task 7.

## 6e. EMBARCADERO ORDER=5 intermittent stall — characterized, not yet fully
     root-caused (2026-07-23)

While calibrating for the real ≥1M-record matrix (gate step 5), EMBARCADERO
(ORDER=5) runs intermittently (~2 hits in ~40 attempts across load-phase and
mixed-workload phases, at scales from 500K to 1M records) hang permanently in
`DistributedKVStore::waitForSyncWithLog()`: `applied_local_ops_` freezes at a
fixed value while `target` (`Publisher::GetNextPublishOrder()`) keeps
climbing as new writes keep succeeding. Not reproducible at will; not tied to
a specific record count. CORFU and SCALOG show no analogous stall.

**Ruled out** (each via direct evidence, not assumption):
- CXL non-coherent-memory flush/fence gaps — this hardware's CXL is coherent
  (explicit user correction); the code correctly gates flush calls off via
  `CXL::ExplicitFlushRequired()`.
- Session fencing / reconnect races (`IsOrder5SessionMode()` epoch-change
  path, `MakeClientBrokerStreamKey` epoch keying) — zero fence/reconnect log
  lines in any captured run, healthy or stalled.
- The previously-fixed backward-resync-onto-invalidated-slot scanner bug
  (`order5-ack-stall-preexisting` session memory, 2026-07-06) — its fix
  (`kBatchHeaderFlagRetired`, forward-only bounded resync) is already present
  and correct in `topic.cc` (~L329-348, ~L7690-7841); this is a different,
  still-open issue.
- `/dev/shm` exhaustion from repeated repro attempts (two incidents this
  session, see below) — the stall's arithmetic is too clean to be a resource
  artifact.

**Localized (high confidence, from two independent live captures with
`EMBARCADERO_ORDER5_PHASE_DIAG=1`)**: the bug is in the **client's**
multi-broker total-order reconstruction, not broker-side sequencing.
- In both captures, summing all 4 brokers' `ordered` counters exactly equals
  the client's stuck `target` (e.g. 286296+286264+286533+286215 =
  1,145,308 = target exactly) — every message was fully sequenced and
  exported by its owning broker.
- Live gdb at the moment of stall: brokers 1-3's `SubscribeNetworkThread` are
  idle in `sched_yield()` (nothing new to export); broker 0 (head) was mid-
  call in `GetBatchToExportWithMetadata`. Consistent with all brokers having
  already sent everything they have.
- `applied_local_ops_` sits at exactly `target − N` (N=64 during the load
  phase's sync-every-64 checkpoint, N=14 during the mixed-workload phase),
  and N only grows as new writes succeed — the signature of a permanent
  head-of-line block: `Subscriber::TryPopOrderedMessageLocked` refuses to
  advance once one specific `total_order` position is never filled in
  `pending_messages_`, wedging every later position behind it forever.

**Most likely mechanism (not yet confirmed with a client-side backtrace —
the client process had already exited by the time gdb could attach in both
captures)**: ORDER=5 deliberately does not stamp `total_order` into
individual message headers on the wire (disabled at `topic.cc:8532-8538`,
"was causing performance regression"); the client instead derives each
message's position purely by counting within a batch, seeded from
`BatchMetadata.batch_total_order` (`subscriber.cc` `StageOrderedMessages` /
`ParseAndStageOrderedBytes`). A single off-by-one in that bookkeeping under
rare timing (e.g. a batch boundary straddling two `recv()` calls) would
silently misplace or drop one message's contribution, matching everything
observed.

**Diagnostic added (this session, not yet exercised against a live
recurrence)**: `Subscriber::LogOrderGapIfStalled()` (`subscriber.cc`/`.h`,
called from `TryPopOrderedMessageLocked`) logs, once `next_expected_order_`
has been stuck for >5s and there is buffered data behind the gap:
`[ORDER_GAP_DIAG] next_expected_order=... stuck_ms=... buffered_slots=...
present=... missing=... first_present_order=... highest_buffered_order=...`.
This should pinpoint the exact stuck position (and confirm data exists
beyond it) the next time this fires naturally, without requiring another
manual gdb chase.

**Operational hazard found and worth flagging for any future automated
repro loop**: `scripts/lib/broker_lifecycle.sh`'s `broker_local_cleanup`
deliberately does not wait for a killed broker's ~128GB tmpfs-backed CXL
mapping to finish unmapping (documented tradeoff at L767-770: "making the
next experiment wait on it defeats bounded cleanup"). Firing many attempts
back-to-back with only ~0.1-0.2s gaps outruns the kernel's reclaim and
exhausts `/dev/shm`, killing the host session — happened twice this session
before a throttled retry loop (explicit `/dev/shm` drain-wait between
attempts, bounded ~90s with a warning) was introduced. Also: force-killing
`run_ycsb_distributed.sh` itself via `kill -9` (rather than SIGTERM) skips
its `trap cleanup EXIT`, leaking its `CXL_KVBASE_*` shm segment — must be
unlinked manually (scoped to `/CXL_KVBASE_${UID}_*`) after any such kill.

Gate step 5 (full matrix) remains blocked on this until either the gap
diagnostic captures a natural recurrence with the exact stuck position, or
the team decides the failure rate (~5%, intermittent, EMBARCADERO/ORDER=5
only) is acceptable to document as a known limitation and proceed.

## 7. `benchmarks/README.md` correction (deferred until Section 6 tests land)

Once the Section 3/Section 6 semantic tests validate A/F/Zipf/RMW behavior
as described above, `benchmarks/README.md` will be corrected to remove the
stale "not yet a full canonical YCSB implementation" / unwired
`ycsb_workload.h` claims (lines 7, 15, 57) and describe the actual
`kv_bench_main.cc`-inline A–F implementation, the fixed-seed determinism
property, the RMW non-atomicity caveat, and the single-apply-thread ceiling.
Not done in this commit, per the handoff's explicit ordering ("Correct the
documentation only after validating semantics with tests").

---

## 8. Definition of done for this document

- [x] Freezes claims, workload semantics, and matrix before full runs.
- [x] Tests validate A/F operation counts, deterministic keys, RMW semantics,
      load coordination, and final drain (Section 6/6a/6b, resolved
      2026-07-23 — op-count accounting, key_offset load coordination, and
      drain-inclusive timing live-validated; determinism proven via matching
      content digests across independent runs; RMW/F validated under the
      matrix's own single-writer-per-key placement).
- [x] `cluster_setup.sh` builds and verifies `kv_ycsb_bench` natively on
      c4/c3/c1 where possible, with a validated working fallback elsewhere
      (Section 6a, resolved 2026-07-23).
- [x] One driver owns cluster lifecycle and synchronized multi-host launch
      (`PaperScripts/run_ycsb_distributed.sh`, Section 6b-6d; gate steps 3-4
      (1 and 2 real remote clients, c4/c3) passed 2026-07-23, EMBARCADERO
      only — CORFU/SCALOG not yet smoke-tested).
- [ ] Every reported cell has ≥3 valid trials from a clean commit (blocked on
      committing this session's changes — Section 6c/6d).
- [ ] Raw data, manifest, deterministic aggregation, figure/table source
      committed under `data/paper_eval/ycsb/<campaign_id>/`.
- [ ] Paper wording distinguishes application throughput from FIFO evidence
      and attributes ceilings to the measured component (apply thread vs.
      log backend, per Q7).
