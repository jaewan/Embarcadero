# SMR-FIFO End-to-End Eval (Paper Q3 → `tab:kv-pipelined`)

Design + runbook for the end-to-end replicated-state-machine eval over the
shared log: Embarcadero (`ORDER=5`) vs the in-tree CXL baselines (Scalog,
Corfu, LazyLog). This is **not** a bulk-append GB/s benchmark; it exists to
answer Q3 only.

## 1. What the paper claims (Q3)

`Paper/Text/Sec7_Evaluation.tex:9`:

> (Q3) *End-to-end meaning:* does native prefix-safe FIFO survive the full
> publish→order→deliver→apply path, where client-side serialization cannot?

Supporting thesis (`Sec1_Introduction.tex:106-112`): Embarcadero realizes
"prefix-safe per-session FIFO under full multi-server striping … native FIFO
that preserves SMR pipelining where client serialization cannot."
`Appendix.tex` §`app:scalog-fifo`: Scalog's disseminate-before-order model can
yield a total order that violates per-publisher FIFO when one publisher spans
multiple shards. `RelatedWork.tex:23`: "Throughput scales; per-client FIFO
under striping does not."

**Claim under test:** with one client session striping across all brokers and
pipelined same-key overwrites, Embarcadero applies correctly at full pipeline
depth (Valid=YES, high ops/s). Write-before-order systems must either accept
reorder (Valid=NO in Pipe) or serialize (Valid=YES with a large Slowdown).

**Non-claims:** Embar winning RF0 append GB/s (that's Q1); YCSB A–F throughput
(YCSB never makes session FIFO load-bearing); LazyLog "Pipe" as a faithful
LazyLog number while the harness gates appends on binding — `Sec7` already
withholds that row, and this eval keeps the same labeling discipline.

## 2. What "Valid" means here

A trial is Valid iff **all** of:

1. `applied == published` (every issued op applied before the clock stops);
2. store size stays exactly `record_count` (overwrite-only phase);
3. **session-FIFO final state**: for every key, the final value is
   byte-for-byte the *last version this client session submitted for that
   key*; untouched keys still hold the load-phase template value.

Check 3 is the new, load-bearing one: a system that applies a permutation of
submission order leaves a stale version as the final value even though
`applied == published` passes. Ground truth is *client submission order* (the
bench records the last version it issued per key), **not** log order — using
log order as truth would define the violation away and forfeit the FIFO claim.

Diagnostics (not validity gates): the apply path also counts
`session_reorders` (an applied version lower than any previously applied
version — total order permuted submission order somewhere) and `key_reorders`
(same, per key — the state-changing case). A run can have `session_reorders>0`
with Valid=YES if no same-key pair was inverted; that is correct and worth
reporting as "reorder pressure absorbed."

## 3. Workload

Same binary, all systems (`kv_ycsb_bench --fifo_valid`):

1. **Load**: `record_count` (default 500K) distinct keys, template values.
2. **Warmup**: `warmup_ops` (default 50K) discarded writes.
3. **Overwrite phase (timed)**: `operation_count` (default 500K) pipelined
   `Put`s from **one client session**; key uniform over the keyspace; value =
   `F|<key:12>|<version:16>|pad` where `version` is a single session-wide
   monotone counter. Clock stops only after the final apply barrier (drain
   included in ops/s).
4. **Validation sweep (untimed)**: read back all keys locally, compare against
   recorded last-submitted versions.

Striping: the client publisher round-robins *sealed batches* across all
brokers' queues regardless of `pub_threads`
(`src/client/queue_buffer.cc` `SealCurrentAndAdvance`; queues are per-broker,
`src/client/publisher.cc` `PublishThread`). So even `pub_threads=1` stripes a
single session across all `NUM_BROKERS` — the Q3 setting. The striping unit is
a batch (~64KB ≈ ~340 ops at 100B values), so reorder pressure appears at
batch granularity; consecutive-batch inversions across brokers are exactly
Scalog's `app:scalog-fifo` case. `EMBARCADERO_ORDER5_HOME_BROKERS=1` exists as
a sticky single-broker escape hatch **for Embar ORDER=5 only** — baselines
have no equivalent, so the optional "Sticky" row is out of the main matrix.

Delivery/apply order: `Subscriber::Consume()` delivers strictly by
`total_order` for every order level ≥1 (gap-filling reorder buffer,
`src/client/subscriber.cc` `ConsumeOrdered`), and the KV store applies in
delivery order with a single consumer thread. Apply order == log total order
for all four systems, so Valid measures the *log's* ordering contract, not
subscriber luck.

## 4. Modes

| Mode | `sync_interval` | Barrier | Purpose |
|------|-----------------|---------|---------|
| **Pipe** | 0 (sync at end) | apply | Headline: max pipeline depth |
| **Serialize** | 1 (stop-and-wait) | ack (ACK1 = ordered frontier) | Restore FIFO without native holds; the fairness row |

Serialize gates op N+1 on op N clearing the ordering pipeline (ACK1 frontier is
the *ordered* frontier for all ordered modes), so submission order is forced
into the log one op at a time. Serialize runs use fewer ops
(`SMR_FIFO_SERIALIZE_OPS`, default 20K) — rates, not wall times, are compared.

## 5. Matrix

Systems: `EMBARCADERO/ORDER=5`, `CORFU/ORDER=2`, `SCALOG/ORDER=1`,
`LAZYLOG/ORDER=2` × {Pipe, Serialize} × 3 trials (medians). Matched knobs:
RF=1, ACK=1, `value_size=100`, `batch_size=1`, `pub_threads=1`,
`NUM_BROKERS=4`, fixed seed (42) so every system sees the identical
key/version sequence.

Expected shape (to be replaced by measured rows):

```
System        | Pipe (ops/s) | FIFO           | Slowdown | Valid
Embarcadero   | high         | native         | ---      | YES
CXL-Corfu     | mid          | token order    | ---      | YES (expected)
CXL-Scalog    | high         | none (striped) | ---      | NO  ← the result
CXL-Scalog    | serialize    | stop-and-wait  | >>1x     | YES
CXL-LazyLog   | (labeled)    | none (striped) | ---      | see below
```

**LazyLog labeling:** the default in-tree LazyLog path gates appends on
binding (`Appendix.tex` `app:lazylog-cxl`), which is not LazyLog's own
append contract. Its rows are emitted by the harness but must be labeled
non-faithful for Pipe claims (or withheld), matching `Sec7`'s current
footnote. Do not present harness-LazyLog Pipe as LazyLog.

**Corfu labeling (measured 2026-07-15):** the harness's Corfu Pipe row shows
rare cross-broker inversions (600 session reorders / 11 same-key / 8 final
mismatches per 20K ops) — but this is a **port-fidelity gap, not a Corfu
property**. The shared-publisher token path assigns `batch_seq` per broker at
send time from parallel per-broker threads
(`src/client/publisher.cc` `corfu_batch_seq_per_broker_`), and the
sequencer's `expected_batch_seq` gate is keyed per `(client_id, broker_id)`
(`src/cxl_manager/corfu_sequencer_service.cc` `AssignToken`); the client's
monotone submit order is never transmitted. Real Corfu derives per-client
FIFO from token *acquisition* order, which requires the client to request
tokens in submission order — the deferred Track-03 forked-client work
(`docs/baselines/fairness_appendix.md`). Until that lands, label the Corfu
Pipe row non-faithful (or withhold it) exactly as with LazyLog; do NOT
report "Corfu violates FIFO." Its Serialize row (stop-and-wait) is faithful
and Valid=YES.

**Embar ORDER=0 negative control (optional):** same binary with
`--sequencer=EMBARCADERO --order=0`; session-FIFO Valid should fail or be
meaningless — evidence the check has teeth.

## 6. Metrics exported (per trial → `summary.csv`)

Existing columns plus: `pub_threads`, `num_brokers`, `fifo_valid`,
`fifo_mode` (`native|token_order|stop_and_wait|sticky|none`),
`overwritten_keys`, `final_mismatch_keys`, `untouched_mismatch_keys`,
`session_reorders`, `key_reorders`, `failed_checks`. `write_throughput_ops_sec`
is the Pipe ops/s (drain-inclusive). Slowdown is computed at aggregation time
vs the Embar Pipe median.

## 7. Figures / tables

- **Table (primary)** — fills `tab:kv-pipelined`:
  `System | Pipe (ops/s) | FIFO | Slowdown | Valid`; caption states RF=1,
  500K keys, 50K warmup, 500K overwrites, full striping, and the Valid
  definition from §2. Generated as a markdown snippet by the plotter.
- **Fig 1** `smr_kv_pipe_ops.pdf`: Pipe ops/s per system; Valid=NO bars
  hatched/outlined.
- **Fig 2** `smr_fifo_tax.pdf`: Pipe vs Serialize per system, log-y —
  "cost of restoring session FIFO without native holds."
- **Optional appendix**: publish→apply latency CDF under Pipe
  (`apply_latency_us.csv` is already emitted per run).

Not used as Q3 headline: append GB/s scaling (Q1), broker-kill MTTR (Q2),
YCSB throughput.

## 8. Implementation map

| Piece | Where |
|-------|-------|
| Versioned overwrite workload + session-FIFO validation | `benchmarks/kv_store/kv_bench_main.cc` (`--fifo_valid`, `--fifo_mode`) |
| Apply-side reorder audit (session/key regressions) | `benchmarks/kv_store/distributed_kv_store.{h,cc}` (`Config::fifo_audit`, `auditFifoValue`) |
| Driver (matrix, cluster lifecycle, aggregation) | `benchmarks/kv_store/run_smr_fifo_eval.sh` |
| Plots + markdown table | `scripts/plot_smr_fifo.py` |

The audit lives in the *shared* SMR harness and runs identically for every
system; no baseline protocol is touched (porting rule respected — in
particular, no Embar-style holds are added to Scalog to make Valid pass).

**Payload-corruption bug exposed by byte-level Valid (FIXED, v1 wire path):**
since commit `6c75b741` (2025-09), the DelegationThread's per-message loop
(`src/embarlet/topic.cc` ~1005-1030, non-Blog path used by SCALOG/LAZYLOG)
wrote an 8-byte segment-header backlink to `msg_ptr - 64` — which, for
contiguously packed v1 batch messages, is the *previous message's payload
tail* (8 bytes at stride−64, value `0x40`). Every non-last message in a
multi-message batch had its value tail clobbered; single-message
(stop-and-wait) batches were unaffected. The legacy Valid check (counts +
store size) could never detect it; the byte-for-byte final-state check did,
and the offset moved exactly as predicted when `value_size` changed (100→94
moved the clobber from payload offset 128 to 64). The only readers of that
backlink word are dead `#ifdef MULTISEGMENT` blocks, so the store was
removed. Reorder metrics (`session_reorders`, `key_reorders`) parse only the
first 32 value bytes and were never affected — the Pipe-mode FIFO-violation
signal stands independently of this bug.

**Known trap (fixed here):** `Subscriber::Consume()`'s ordered stream is only
fed when `feed_ordered_consume_stream` is true (`src/client/subscriber.cc`).
Under `EMBARCADERO_RUNTIME_MODE=latency|throughput` with
`measure_latency=false` the feed is disabled and every sequencer's KV run
stalls at `applied=0` (bytes arrive; nothing is staged for `ConsumeOrdered`).
`throughput_test` is unaffected because it drains via `Poll()`.
`DistributedKVStore` now sets `EMBARCADERO_ENABLE_ORDERED_CONSUME_STREAM=1`
(non-overriding) before constructing its Subscriber.

## 9. Runbook

```bash
make -C build -j64 kv_ycsb_bench

# Full matrix (local 4-broker bring-up, ~all four systems x pipe/serialize x 3)
bash benchmarks/kv_store/run_smr_fifo_eval.sh

# Quick smoke
SMR_FIFO_SEQUENCERS="EMBARCADERO SCALOG" SMR_FIFO_NUM_TRIALS=1 \
SMR_FIFO_RECORD_COUNT=20000 SMR_FIFO_OPERATION_COUNT=20000 \
SMR_FIFO_WARMUP_OPS=2000 bash benchmarks/kv_store/run_smr_fifo_eval.sh
```

Outputs land in `build/results/smr_fifo_<ts>/`: per-run dirs + logs,
aggregated `summary.csv`, `paper_snippet.md`, and the two PDFs.

A bench process exits nonzero when Valid=NO **by design**; the driver records
it as a result row (`VALID=NO RUNS`) and only treats a missing `summary.csv`
as a broken run.

## 10. Fairness / anti-footguns

1. Same striping intent everywhere: no sticky routing for any system in the
   main table (the knob doesn't even exist for baselines).
2. Same durability: RF=1 ACK=1; ACK2/disk stays out of Q3.
3. Scalog Pipe Valid=NO is recorded and explained (`app:scalog-fifo`), never
   patched around; its Serialize row is the honest recovery path.
4. Corfu slower-but-Valid under Pipe is expected (token order) and fine.
5. Medians over 3 trials; commit hash + hostname recorded in each
   `metadata.txt`; warmup discarded.
6. Single-broker runs cannot exhibit the violation — Q3 requires
   `NUM_BROKERS>1` (driver default 4).
7. Publisher co-location: local bring-up puts client and brokers on one host —
   fine for Valid/FIFO semantics and mode-relative comparisons; a
   publication-quality ops/s table should re-run with the client on a remote
   host per the standard campaign layout, unlabeled numbers stay internal.

## 11. Definition of done / status

- [x] Plan doc (this file)
- [x] `--fifo_valid` workload + session-FIFO Valid + reorder audit
- [x] Driver + aggregation + markdown/plots
- [x] Pipe + Serialize matrix runs locally: all four systems, 20K/20K smoke
      (`build/results/smr_fifo_matrix1`): Embar Pipe Valid=YES 782K ops/s,
      0 reorders; Scalog/LazyLog Pipe Valid=NO (10.2K/9.2K session reorders);
      all Serialize rows Valid=YES (Scalog 227 ops/s ≈ 3446×, LazyLog 280 ≈
      2794×, Corfu 1149 ≈ 681×, Embar 2001 ≈ 391×)
- [x] Striped-Scalog Pipe outcome explained against `app:scalog-fifo`
      (session reorders + same-key inversions measured by the apply audit)
- [ ] Corfu client token-order fidelity (Track-03 forked client) before a
      faithful Corfu Pipe row can be claimed
- [ ] Paper-scale run (500K/500K, 3 trials) → numbers into `tab:kv-pipelined`
      (only after Valid/FIFO contracts are honest for every system: LazyLog
      binding-gated Pipe + Corfu token-race Pipe both labeled/withheld)
