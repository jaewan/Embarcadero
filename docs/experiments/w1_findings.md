# W1 Verification Findings — Track 01 (core protocol)

**Author:** Track 01 (`session/01-core-protocol`)
**Date:** 2026-07-05
**Base:** `chore/repo-reorg` @ HEAD
**Plan ref:** `Paper/improvement_plan.md` §W1 (items 1–4), D1–D4.
**Scope:** Ground-truth reading of the epoch collector / hold buffer / export path in
`src/embarlet/topic.cc` + `topic.h`, `cxl_datastructure.h`, the broker export loop in
`src/network_manager/network_manager.cc` (read-only; not owned), and the subscriber receive path
in `src/client/subscriber.cc`. No code changed. Line numbers are against base HEAD.

**TL;DR for the human**
- **W1.2 (in-order ACK):** The in-order *commit* and *ACK-only-on-commit* invariants **hold** on
  the happy path **and across epoch-collector seals**. The known degradation ("prefix-FIFO with
  causal exceptions") is a **hold-expiry / forced-skip policy** that does **gap-skip**
  (`topic.cc:5928-5943`), which is exactly what D1 replaces with session fencing. **This is NOT a
  surprise collector-layer break → NO Week-2 escalation.** D1 is implementable at the hold-buffer
  layer without collector rework.
- **W1.3 (assign-at-release):** **Already substantially implemented.** Held batches are excluded
  from the per-epoch `global_seq_/global_batch_seq_` `fetch_add` and receive their GOI seq only when
  drained from hold (`from_hold`). D4 is *smaller* than the plan assumes; the residual work is the
  spatial-guard duplicate case (TLA⁺ scenario a).
- **W1.4 (PBR frontier):** **Scan-driven today**, not commit-driven. Held slots' `consumed_through`
  is advanced as soon as the scanner pulls them into an epoch, so the hold buffer must *copy* batch
  metadata into DRAM (`HoldBatchMetadata`) because "the ring slot may be reused." **D3 (commit-driven
  reclamation) is a real, substantial change** — held batches are currently *not* reconstructible
  from PBRs after failover.
- **W1.1 (δ / E2E tail):** Broker-side visibility is **not** the bottleneck (export loop busy-spins;
  epoch cadence 500 µs; committed-seq updater 200 µs). Prior in-repo latency work already (a) built a
  **stage decomposition** (`append_send_to_ordered/ack/deliver`) and (b) **measured the exact tail**
  cited in the plan. Full quantitative localization needs one testbed run reading
  `stage_latency_summary.csv` — **not yet run** (needs the `flock` lock). Ranked hypotheses below.

---

## W1.4 — PBR frontier semantics: **SCAN-DRIVEN** (sizes D3)

**Answer: PBR reclamation is scan-driven.** The per-broker reclamation frontier is
`TInode.offsets[b].batch_headers_consumed_through` (`cxl_datastructure.h:316`), consumed by the
receive/producer path to know which ring slots are free.

**Evidence:**
- `consumed_through` is advanced from the **sealed epoch's `batch_list`** — i.e. every slot the
  scanner pushed into the epoch — regardless of whether the batch committed to GOI:
  - `EpochSequencerThread` seeds `contiguous_consumed_per_broker[b]` from the current
    `consumed_through` for every broker seen in `batch_list` (`topic.cc:4741-4759`).
  - `CommitEpoch` advances it for committed slots inline (`topic.cc:4350-4354`) and then calls
    `AdvanceConsumedThroughForProcessedSlots(batch_list, …)` for **all** processed slots
    (`topic.cc:4423`), including skipped/held ones.
  - `AdvanceConsumedThroughForProcessedSlots` (`topic.cc:6841-6899`) marks a slot processed purely
    by presence in `batch_list` (`processed_slots[b][slot] = 1`, line 6868) and advances the
    contiguous frontier over them — no commit check.
- **Held batches free their slot before commit.** When a batch is moved to the hold buffer, the
  shard pushes an `is_held_marker` placeholder carrying only `broker_id`+`slot_offset` into `ready`
  (e.g. `topic.cc:5411-5417`, `5748-5754`), and the real slot is invalidated
  (`InvalidateOrder5HeldSlot(p->hdr)`). The explicit comment at `topic.cc:4737-4739`: *"scanner
  pushed them to epoch buffer, so sequencer must advance consumed_through to allow ring to drain and
  prevent deadlock."*
- Because the slot is reclaimed before commit, the hold buffer **copies** the batch metadata out of
  CXL into DRAM (`HoldBatchMetadata`, `topic.h:50-61`; filled at hold time `topic.cc:5392-5405`)
  precisely because *"ring slot may be reused so we never read p.hdr when from_hold"* (`topic.h:49`).

**Consequence for D3:** Today a held batch is **not reconstructible from the PBR after a failover**
— its slot may already be overwritten, and the only copy lives in the head sequencer's process-local
DRAM hold buffer. D3 ("slot freed at commit, not scan; held batches reconstructible from PBRs")
requires keeping the PBR slot occupied until GOI commit, which means:
1. `consumed_through` must advance only over *committed* (or dedup-dropped) slots, **not** held ones.
2. Backpressure then emerges from ring fill (plan: "delete per-session credit windows"), so the
   provisioning inequality **session-lease < PBR-fill-time** must be enforced or the ring wedges.
3. The DRAM `HoldBatchMetadata` copy can then be dropped (or demoted to an L1 cache) since the PBR
   slot itself is the durable record.

This is the largest of the D-changes and touches the hottest interaction (ring drain vs. hold).

---

## W1.2 — In-order ACK invariant: **HOLDS on commit path; violated only by the expiry/skip policy (= D1 target)**

**Answer:** The sequencer **does** commit per-session batches strictly in order and **does** ACK only
on GOI commit, **including across epoch-collector seals**. The only ordering violations are the
**deliberate gap-skips** on hold-expiry / hold-buffer-full / scanner-timeout — which is exactly the
semantic D1 fixes. **No Week-2 gate escalation is warranted** (see caveat at end).

**Per-session expected_seq exists and persists across seals.**
- Per-session state is `ClientState5` keyed by `client_id` (true client-chain ordering) or by
  `MakeClientBrokerStreamKey(client_id, broker)` (per-(client,broker) stream), living in
  `Level5ShardState::client_state` (`topic.h:880`). It survives across epochs (the shard is
  long-lived), so `next_expected` is not reset at a seal.
- Commit-in-order: a batch reaches `ready` only when `seq == state.next_expected`
  (`topic.cc:5354-5360`, `5575-5581`, `5687-5693`); `seq > next_expected` → hold buffer
  (`topic.cc:5372-5418` and the mirrored blocks); `seq < next_expected` → dedup/terminalize, never a
  fresh out-of-order commit on the happy path.
- In-order drain from hold: the drain loop only emits `cmap.find(state.next_expected)`
  (`topic.cc:5777-5814`), advancing `next_expected` one at a time.

**ACK is driven by GOI commit.**
- Per-client ACK1 frontier `per_client_ordered_` is advanced **only inside `CommitEpoch`**
  (`topic.cc:4374-4379`), over the `ready` set being written to GOI that epoch. The CV
  `sequencer_logical_offset` is likewise written from `CommitEpoch`'s accumulation
  (`FlushAccumulatedCV`, `topic.cc:4420`). The ACK path (`network_manager.cc` `GetOffsetToAck`,
  ORDER=5 ACK1) reads `max(CV.completed_logical_offset, tinode.ordered)` — both sequencer-ordered
  cumulative counts. So an ACK cannot precede the GOI write.
- Across seals: `CommitEpoch` runs per sealed epoch; `global_seq_`/`global_batch_seq_` are process-
  global monotonic atomics (`topic.h:817-819`), so ordering is continuous across the triple-buffered
  epoch boundary.

**The violation (D1 target).** On **hold expiry** with `seq > next_expected`, the code performs a
**gap-skip**: `state.next_expected = ent.seq + 1` and emits the out-of-order batch
(`topic.cc:5928-5943`, counters `order5_hold_timeout_skips_`, `order5_fifo_violations_`). The same
gap-skip happens on **hold-buffer-full forced skip** (`topic.cc:5373-5386`, `5709-5722`) and on
**scanner targeted-skip** (`topic.cc:5286-5289`). Note also `GetOrder5HoldTimeoutNs` returns **0 by
default** (`topic.cc:77-94`) — steady-state age-expiry is *disabled*; expiry only fires inside
force-expire windows (idle-trigger 750 ms non-replicated / 2000 ms replicated, `topic.cc:96-108`).

**D1 implication:** the fix is local to the hold-buffer layer — replace the three gap-skip branches
with **session fencing** (emit `SESSION_FENCED` carrying the committed HWM; never advance
`next_expected` past a gap). No epoch-collector rework is required because the in-order commit
machinery underneath is already correct.

**Caveat / escalation note (plan item W1.2):** The plan says "If violated, D1's fencing needs rework
at the collector layer." Strictly, the invariant *as stated* (in-order commit + ACK-on-commit) is
**not** violated on the commit path; what's "violated" is FIFO delivery, via an intentional skip
policy at the hold layer. So the D1 rework stays at the **hold-buffer** layer, not the collector. I'm
flagging this to the human as a **downgrade from gate to normal work**, not an alarm — but confirm
you agree with that reading before I build on it.

---

## W1.3 — GOI assign-at-release feasibility: **already largely implemented; D4 is small**

**Answer:** Assign-at-release is already the behavior in the triple-buffered collector. Held batches
are excluded from the per-epoch order reservation and get their GOI index only on release.

**Evidence:**
- Per-epoch reservation counts only the `ready` set: `total_msg` sums `p.num_msg` over `ready`
  (`topic.cc:4076-4078`), then `global_seq_.fetch_add(total_msg)`; `num_goi_order5` counts only
  non-skipped, non-held ready batches (`topic.cc:4092-4097`), then
  `global_batch_seq_.fetch_add(num_goi_order5)`. Held batches are represented in `ready` as
  `is_held_marker` (num_msg=0) and are explicitly skipped in the GOI loop
  (`if (p.skipped || p.is_held_marker) continue;`, `topic.cc:4093-4095`, `4129-4130`). So a held
  batch consumes **no** order/GOI index in the epoch it is held.
- On release, the drained batch enters a *later* epoch's `ready` set with `from_hold=true`
  (`topic.cc:5803-5811`) and is assigned `total_order`/GOI index in that `CommitEpoch`
  (`topic.cc:4134-4156`) — i.e. **at release**.
- `committed_seq` (spatial guard input) is advanced monotonically by `CommittedSeqUpdaterThread`
  from the completed-range ring (`topic.cc:3116-3140`); `tinode.ordered` is monotonic.

**Residual D4 work:** the mechanism is present; what remains is the **spatial-guard interaction with
a retransmitted duplicate arriving at `S_new`** (plan TLA⁺ scenario a) — ensure `committed_seq`
monotonicity + wrap-fence reject a duplicate inside the guard window. This is a correctness-audit +
targeted guard, not a collector rewrite. **Feasibility: HIGH.**

---

## W1.1 — Coupled δ / E2E tail: broker visibility ruled out; tail is delivery-side or hold-induced (measurement pending)

**Reference numbers (from prior in-repo work, `LATENCY_EXPERIMENT_CHECKLIST.md` Step 8 J2):**
offered-load sweep, ORDER=5 ACK=1, 4 GiB, 5 trials — CI-mean e2e (µs) p50 / p99 / p99.9:
`1000 MB/s → 6007.8 / 82888.8 / 127539.8`. The plan's "E2E P99 86.7 ms vs append P50 745 µs"
matches this p99≈82.9 ms point. Note the **e2e p50 is ~6 ms**, already 8× the append p50 — so the
gap is not purely a 1% tail; the whole e2e distribution sits well above append.

**What is ruled out (broker visibility is fast):**
- **Broker export loop busy-spins** — no fixed sleep on the no-data path; it `std::this_thread::yield()`s
  and retries `GetBatchToExport{,WithMetadata}` (`network_manager.cc:1505-1507`, `1534-1536`). The
  `kPollIntervalMs=100` nearby (`network_manager.cc:1415`) is only the *startup* topic-wait, not the
  hot loop.
- **Epoch cadence** is 500 µs (`kEpochUs`, `topic.h:835`); a batch waits ≤ ~1 epoch to be sequenced.
- **committed_seq updater** waits at most 200 µs (`topic.cc:3193`).
- Export becomes visible immediately at commit (descriptor written in `CommitEpoch`/`AssignOrder5`).

**Ranked hypotheses for the tail (to confirm by measurement):**
1. **Subscriber dual-buffer swap granularity (top suspect).** Each subscriber connection uses **two
   16 MB buffers** (`subscriber.cc` `buffer_size_per_buffer_ = 16<<20`); the receiver blocks on
   `receiver_can_write_cv` with **no timeout** when the write buffer fills before the consumer
   releases the read buffer (`subscriber.cc:1282-1290`). A single consumer-scheduling hiccup can
   stall an entire 16 MB buffer's worth of already-received messages — at multi-GB/s this is tens of
   ms, matching the ~83 ms tail and the elevated ~6 ms median. This is the "poll-scheduling/batching
   artifact" the plan suspects.
2. **Consumer poll sleeps.** `Consume()` sleeps **1 ms** when no buffer is ready
   (`subscriber.cc:~2153`); `ConsumeOrdered()` polls at **100 µs** (`subscriber.cc:~2336`); `Poll()`
   adaptive 10 µs–1 ms (`subscriber.cc:~1031`). These bound per-iteration latency and interact with (1).
3. **Hold-buffer wait (load/striping-dependent).** With steady-state age-expiry disabled
   (timeout=0), a session's batch waits for its predecessor. At `pub_threads=1` (current KV default)
   arrivals are in-order → negligible; under multi-connection striping this can add unbounded tail
   until the gap fills. Relevant to the *loaded* sweep, less so to the 1 Mops/s single-thread KV.

**Why 1.0 Mops/s KV can still be "fast":** that path is pipelined (batched), so amortized throughput
hides the per-message delivery tail that the E2E latency harness exposes.

**Diagnosis plan (one testbed run; needs `flock`):** the stage decomposition already exists —
`stage_latency_summary.csv` with `append_send_to_ordered`, `append_send_to_ack`,
`append_send_to_deliver` and a monotonicity check (Checklist Step 5). Reading p99 of each stage
localizes the tail unambiguously:
- if `deliver − ack` dominates → subscriber (hypotheses 1/2) → fix delivery batching (smaller /
  time-bounded buffer swap, bounded CV wait); this directly bounds the E2E tail and lets δ be set.
- if `ack − ordered` dominates → hold/ACK path (hypothesis 3).
- if `ordered − append_send` dominates → sequencer (unexpected given above).

**Blocking dependency for δ:** the plan is explicit — δ (adaptive retransmit constant, D1) cannot be
set until **append P99.9-under-load** is known, else steady-state spurious retransmits. So this
measurement gates the D1 client-side retransmit constants. Recommend running it early.

---

## Cross-track notes
- **Track 02 (TLA⁺):** the two scenarios where D1/D4 bugs will live — (a) retransmitted duplicate at
  `S_new` inside the spatial-guard window; (b) stale-CV ACK relay after election — both touch code I
  own (`CommitEpoch` guard, ACK-relay epoch check). W1.3 shows assign-at-release is already in place,
  so scenario (a) should model `committed_seq` monotonicity + wrap-fence as the guard.
- **Track 05 (RDMA variants):** the frozen session/lease design they depend on is not yet written;
  it will land in the Track 01 handoff after D1/D2 are specified.

## Open questions for the human
1. Confirm the **W1.2 reading**: gap-skip is a hold-layer policy, not a collector-invariant break →
   D1 stays at the hold-buffer layer, no Week-2 gate. (I believe this is correct and low-risk.)
2. **W1.1 measurement — DONE (2026-07-05).** ORDER=5 latency-vs-load run (4 brokers, 4 GiB, ACK1,
   steady). Stage p99 decomposition: append→**ordered** = 1.36 ms; append→**ack** = 1.36 ms;
   append→**deliver** = **733 ms** (p50 3.6 ms). **deliver − ack ≈ 731 ms → the entire tail is
   subscriber-side delivery, not the sequencer/collector** (which orders+ACKs in 1.36 ms p99).
   Localized to the subscriber's coarse 16 MB dual-buffer swap + poll-scheduling (hypothesis #1).
   **δ is unblocked:** append/ack **p99.9 = 1.68 ms** governs publisher retransmit (matches paper's
   P99≤1.68 ms claim); the 733 ms is subscriber-read and does NOT gate δ → set δ ≈ single-digit ms.
   *(Note: this run used steady pacing; not comparable to the paper's open-loop 83 ms e2e figure.)*
3. **D3 scope confirmation:** commit-driven reclamation + delete per-session credit windows is the
   biggest change and reworks the ring-drain/hold interaction. Confirm we want it now vs. staged
   after D1/D2 (which are independently shippable).
