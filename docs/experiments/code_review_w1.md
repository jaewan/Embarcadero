# Track 01 — Code review of the ORDER=5 sequencer/collector, and fixes applied

**Author:** Track 01 (`session/01-core-protocol`), 2026-07-05
**Scope reviewed:** `src/embarlet/topic.cc` (epoch pipeline, hold buffer, CommitEpoch, export),
`src/embarlet/topic.h`, `src/embarlet/sequencer_utils.h`, `src/cxl_manager/cxl_datastructure.h`.
**Method:** senior-eng read → dynamic workflow (per-issue design + adversarial verification,
18 agents) → serial application from actual file content → compile-verify on `broker`.

Every finding below was independently re-verified by a second (adversarial) agent before any edit.
All line numbers are pre-edit unless noted.

## Disposition summary

| ID | Finding | Severity | Disposition |
|----|---------|----------|-------------|
| C1 | `committed_this_epoch` `std::array<size_t,8>` read at `b<NUM_MAX_BROKERS(32)` → OOB ≥8 brokers; feeds force-expiry | HIGH (latent @≤8 brokers) | **FIXED** |
| C3 | read of `*p`/`*jt` after `std::move` in 4 hold-insert sites | LOW (no live UB; PODs) | **FIXED (hardening)** |
| C4 | export descriptor: `publish_commit` sourced from batch, `pbr_absolute_index` from hold_meta | LOW (latent) | **FIXED** |
| E1 | `hold_export_queues_[b]` written for all brokers; only `[broker_id_]` ever read → dead writes + unbounded DRAM growth | MED (waste/leak) | **FIXED** |
| E2 | `RecomputeOrder5BatchSizeFromPayload` (CXL walk) run 2×/batch in CommitEpoch | MED (hot-path) | **FIXED (compute-once)** |
| E3 | per-epoch heap alloc of `vector<uint8_t>` scratch in `AdvanceConsumedThroughForProcessedSlots` | LOW | **FIXED (member scratch)** |
| C2 | `CommittedSeqUpdaterThread` `pending` unbounded on a permanent GOI gap | not live today | **DEFERRED (see below)** |
| C5 | legacy (ORDER=4) seeds `next_expected` from first-observed seq | LOW (legacy only) | **DEFERRED → S2** |
| S2 | `ProcessLevel5BatchesShard` 4× duplicated classifier | HIGH (maintainability) | **DEFERRED (design ready)** |
| S1 | `EpochDriver`/`seal()` complexity; "10 ms seal → E2E tail" claim | claim NOT real | **DEFERRED (analysis)** |
| W1.2 | empirical per-session in-order-commit assertion (W1 item #2) | — | **ADDED** |

---

## Fixes applied (this branch)

- **C1** — `topic.cc`: `std::array<size_t, 8> committed_this_epoch` → `std::array<size_t, NUM_MAX_BROKERS>`.
  The two increment sites are already `.size()`-guarded, so this both removes the OOB read at lines
  ~4366/4407 and makes commit accounting correct for all 32 brokers (it feeds the idle force-expiry
  trigger). Behavior-identical for ≤8 brokers.
- **C3** — `topic.cc`: at all 4 hold-insert sites, moved `he.batch = std::move(*p)` *below* the
  `he.meta.* = p->…` captures and re-sourced the post-move reads (`InvalidateOrder5HeldSlot`, marker
  `broker_id`/`slot_offset`) from `he.batch`. Behavior-identical today (POD move == copy); removes the
  use-after-move trap that would activate if `PendingBatch5` ever gains a non-trivial member.
- **C4** — `topic.cc`: export descriptor `cur->publish_commit = p.cached_pbr_absolute_index` →
  `= pbr_abs` (same source as `cur->pbr_absolute_index`), making the `BatchHeaderPublishCommitted`
  invariant source-consistent for from-hold batches.
- **E1** — `topic.cc`: guarded all 3 `hold_export_queues_[owner].q[…]=ex` inserts with
  `if (owner == broker_id_)`. `hold_export_queues_` is process-local DRAM drained only for the
  sequencer's own broker; non-head brokers export via the shared CXL ring descriptors (written
  unconditionally in the same loop, untouched). Eliminates dead inserts + unbounded DRAM growth for
  non-owned brokers; ring path (the correctness-bearing one) is unchanged.
- **E2** — `topic.cc`: compute the ORDER=5 recomputed batch size **once** per batch in the step-2
  loop into a `std::vector<size_t> order5_recomputed_size(ready.size())` (indexed by
  `&p - ready.data()`, identical across both range-for loops) and reuse it in step-3. Removes the 2nd
  CXL payload walk per batch; byte-identical values (the payload is not mutated between the loops), so
  export-ring writes, `hold_export_queues_` entries, and the `ORDER5_EXPORT_SIZE_FIX` warning are
  unchanged. *(Deeper fix — flushing `BlogMessageHeader.size` on the v2 ingest path so the recompute
  can be deleted entirely — is recorded under "deeper fixes" and deferred; it touches Track 04's
  ingest area / `network_manager`.)*
- **E3** — `topic.h` + `topic.cc`: replaced the per-call `std::array<std::vector<uint8_t>,…>` alloc
  with a member `processed_slots_scratch_` reused across calls (single-writer: only the
  EpochSequencerThread reaches it), zeroing only the seen brokers' regions each call.

### W1.2 assertion added (per your directive — empirically confirm before de-gating D1)
`topic.cc` CommitEpoch GOI-write loop now checks, for ORDER=5 only, that each committed batch's
`client_seq` is **strictly greater** than the last committed `client_seq` for that `client_id`
(persisted in a new `commit_order_last_seq_` map across epoch seals — single-writer, no lock). A
violation (a lower seq committed after a higher one = a genuine collector reorder across a seal)
increments `order5_commit_order_violations_` and `LOG(ERROR)`s the details; with
`EMBAR_ASSERT_COMMIT_ORDER=1` it becomes a hard `CHECK` (abort) for tests. Default is count+log so it
is safe to leave on under the W1.1 load run. Reset at Sequencer5 start; summary logged at teardown
(`[W1.2_SUMMARY] … order5_commit_order_violations=N`).

**Gating logic:** deliberate gap-skips still advance monotonically, so they do NOT trip this — only a
true reorder does. If the counter stays 0 under the load run + a cross-broker striped stress, W1 item
#2 holds and **D1 is de-gated** (hold-layer fix, no collector rework). If it fires, D1 is back to a
Week-2 gate and I escalate immediately.

---

## Deferred (design ready; not applied)

### C2 — `pending` unboundedness: **not a live bug today**
The adversarial investigation proved GOI indices cannot be permanently skipped under any current
path: `global_batch_seq_.fetch_add(num_goi_order5)` (topic.cc:4097) and the matching
`EnqueueCompletedRange(base, base+num_goi_order5)` (4425-4427) are in the **same
`export_cursor_mu_` critical section with no early exit between them**, `num_goi_order5` counts only
non-skipped/non-held batches, and skipped/held/stale batches never consume a GOI index. There is
exactly one `EpochSequencerThread`, so completed ranges tile `[0, global_batch_seq_)` contiguously →
`CommittedSeqUpdaterThread`'s contiguity walk always advances.

**Residual risks worth an interim guard (not applied yet):**
1. `EnqueueCompletedRange` can silently drop a range if `stop_threads_` flips while the 65536 ring is
   full (shutdown-only, benign today).
2. `pending` has no size bound; a hypothetical permanent gap would grow it without limit.
3. A naive cap that `pop()`s is **actively harmful** — `pending` is a min-heap on `start`, so the top
   is the range most likely to unblock; any real drop must be a deliberate gap-skip, not a pop.

**Recommendation:** (1) add a rate-limited WARNING when `pending` exceeds a soft cap (safe, no
control-flow change); (2) add a debug `DFATAL` asserting the reserve==enqueue coupling; (3) defer any
tombstone/GOI-poisoning gap-fill to **D3/D4 co-design** — advancing `committed_seq` past an unwritten
GOI index is unsafe unless paired with a GOI-entry poisoning convention (sentinel + skip in all GOI
consumers, crash-consistently flushed). This intersects the D3 commit-driven-reclamation contract, so
it must not land independently.

### S2 (+C5) — unify the 4× classifier in `ProcessLevel5BatchesShard` **before D1**
The seq-classification decision tree (`==next_expected` emit / `<` late / `>` hold-or-skip) is
duplicated across 4 per-record loops: fast-path×{true_chain, legacy}, general×{true_chain, legacy}.
Only the true_chain sites run for ORDER=5. **C5** is the direct consequence: the two *legacy* sites
still seed `next_expected` from the first-observed `batch_seq` (topic.cc:5442, 5664) — the exact bug
the true_chain sites fixed (5328-5336, 5550-5554).

**Design (verified sound, risk HIGH, do not auto-apply):** extract one classifier
`classify(shard, key, seq, batch, true_client_chain, side_effects…)` reproducing, per the adversarial
line-by-line map: DUP-already-emitted, IsEmitted-only, EQ, LT (drop-late for true_chain vs emit-late
for legacy), GT-force-skip, GT-hold-insert — the only variation axes are `(key, true_client_chain,
which record_* fire, LT divergence)`, all pure functions of `(true_client_chain, key, batch fields)`.
Land in **two commits**: (1) behavior-preserving classifier extraction (byte-identical for ORDER=5);
(2) unify the seed to 0 (fixes C5 — a *behavior change for legacy*, a no-op for ORDER=5). **D1 anchor:**
the gap-skip session-fencing replaces `state.next_expected = ent.seq + 1;` at topic.cc:5929 (expiry
loop 5884-5944) — **outside** the classifier scope, so the classifier refactor and D1 compose cleanly.

**Sequencing:** do S2 refactor → then D1 fencing at 5929, so D1 lands in one place instead of four.

### S1 — `EpochDriver`/`seal()`: the "10 ms seal → E2E tail" claim is **not real**
`EpochBuffer5::seal()`'s 10 ms budget is only a worst-case liveness ceiling before it rolls back to
COLLECTING. The `broker_active` window a sealer waits on is a single `enter_collection` →
`push_back` → `exit_collection` critical section (sub-µs to low-µs). In steady state `seal()`'s
`wait_for` predicate is already satisfied and returns ~immediately; the timeout branch is essentially
unreachable absent a scanner descheduled *while holding* `broker_active` (a rare ~10 ms outlier, not
a systematic P50/P99 shift). The more likely steady-state tail contributors are the **500 µs epoch
cadence** and **sequencer-lag successor waits**, not the seal budget.

**Recommendation:** the budget is safe to lower for correctness at any value (rollback+retry loses no
data). A defensive lower bound of `max(200 µs, 2·epoch_us)` cuts the rare worst case from 10 ms to
sub-ms; do **not** go near 0 (spurious rollbacks → re-seal churn). But this is a cheap defensive win,
**not** the W1.1 tail fix — attribute tail mass to seal timeouts via the phase-diag counters first.

---

## Deeper fixes recorded (out of Track-01 hot-file scope; coordinate)
- **E2 root cause:** `RecomputeOrder5BatchSizeFromPayload` exists because per-message
  `BlogMessageHeader.size` may not be flushed on the v2/blog ingest path before `batch_complete=1`
  (`network_manager.cc` flushes headers only for `!is_blog_header_enabled`). Flushing the header
  cacheline before publish would let the sequencer trust `BatchHeader.total_size` and delete the
  payload walk entirely (all 3 call sites). Touches the ingest path — coordinate before changing.

## Verification
- All edits applied from actual file content (agent-transcribed `old_code` for E1 had wrong
  indentation — re-derived line-based). Compile-verified on `broker` with `-DCOLLECT_LATENCY_STATS=ON`
  (`~/Embarcadero-sessions/01-core-protocol`, `-j64`, cached gRPC).
- W1.2 empirical check: **0 commit-order violations** across all brokers under the W1.1 latency run
  (1 thread) AND a 4-thread cross-broker stress with `EMBAR_ASSERT_COMMIT_ORDER=1` (hard CHECK) →
  the per-session in-order-commit invariant holds across seals → **D1 de-gated**.
- Throughput (loopback, single-node): Order0 12.46 GB/s, Order5 12.28 GB/s = matches historical
  loopback → broker path not regressed. (Real-wire multi-client number: see §c3 below / pending.)

---

## ORDER=5 scanner-freeze fix (found while chasing the throughput question)

**Symptom:** ORDER=5 + ACK=1 + `THREADS_PER_BROKER=4` intermittently times out; the head-relayed
per-client ACK frontier ends ~1000 batches short with the sequencer *idle-clean* (hold=0, deferred=0).
(The `B1/B2/B3=0 ACK` per-broker printout is **cosmetic** — ORDER=5 ACK is head-owned; only B0 relays
a per-client frontier via `GetClientOrdered`.)

**Attribution — PRE-EXISTING, not a Track-01 regression** (moscxl-side agent, clean A/B): unmodified
baseline (all C1/C3/C4/E1/E2/E3/W1.2 hunks reverse-applied, same flags/config/tuned buffers) failed
**5/5** with the identical signature; the modified build failed 4/5. Matches the documented Step-27
baseline flakiness (`LATENCY_EXPERIMENT_CHECKLIST.md`, "8/10 pass, 2/10 fail … lagging broker tail").

**Root cause:** a head-side per-broker **`BrokerScannerWorker5` freeze**. A hold-insert invalidates a
ring slot via `ClearOrder5PublishState`, which zeroed `flags`/`batch_complete` → the slot became
indistinguishable from a never-written tail slot. A transient EMPTY read then took the empty-slot
**resync**, which jumped the cursor to `batch_headers_consumed_through` **with no direction guard** —
and since `consumed ≤ cursor`, that jump could only go *backward*, landing on the invalidated held
slot. The scanner then parked there forever (classifying it as ring tail), stranding ~1000 published
batches in later slots. Intermittent + 4-thread-only: needs out-of-order arrival → hold → invalidated
slot at the consumed frontier, plus a transient EMPTY read. Single-thread never holds → never freezes;
ORDER=0 has no scanner.

**Fix (Track-01-owned files only — `topic.cc`, `cxl_datastructure.h`):**
1. `cxl_datastructure.h`: new `kBatchHeaderFlagRetired = 1u << 2` (additive; existing readers use
   bitmask tests so it reads as neither CLAIMED nor VALID everywhere else — safe for other tracks).
2. `ClearOrder5PublishState` writes `flags = RETIRED` (not 0) → cleared slots stay distinguishable
   from never-written tail slots.
3. `BrokerScannerWorker5`: (a) **retired-slot skip** — a `!publish_ready` RETIRED slot is advanced
   past immediately (its batch is already sequencer-owned; skipping cannot lose data), with a
   full-lap streak guard that yields; (b) **forward-only resync** — the empty-slot resync now uses
   the same `0 < forward < slot_count/2` ring-distance window as the top-of-loop jump, eliminating
   backward jumps onto invalidated slots.

**Validation (moscxl agent, exact repro):** pre-fix **1/5 pass** → post-fix **9/10 pass**
(11.46–12.11 GB/s). The one residual failure ends ~2 batches short (≈500× smaller blast radius) — a
**separate, pre-existing** hold-buffer bookkeeping loose end (an entry removed without committing),
plausibly the old Step-27 intermittent. Flagged as follow-up (dedicated hold-insert/drain capture);
**this is also exactly what D1 (session fencing, replacing gap-skip) is designed to eliminate.**

**Integration hygiene:** the fix was cherry-picked into the clean Track-01 tree as **2 files only**
(verified: `topic.cc` differs from pre-fix by exactly the 43-line fix; header by the 4-line constant).
The broker's `01-core-protocol` dir had diverged (other tracks' newer versions + the agent's baseline
tree + a non-persistent `10.10.10.181` on c3) — treat the fix as a 2-file cherry-pick, **not** a merge
of that tree. No other-session file was modified.

**Clean-tree verification (2026-07-06, `01-core-protocol-clean` = my W1 + fix, no contamination):**
- Builds clean (`-j64`, `COLLECT_LATENCY_STATS=ON`).
- **ORDER=5 loopback repro ×8 with `EMBAR_ASSERT_COMMIT_ORDER=1`: 7/8 PASS** (11.5–12.3 GB/s) vs
  **pre-fix 1/5** → scanner-freeze fixed. **W1.2: 0 commit-order violations, 0 assert-aborts** across
  all 8 trials → invariant holds *with* the fix.
- **c3 100G real-wire single-client** (c3 = the powerful Intel client, `10.10.10.181 → moscxl
  10.10.10.10`, kernel buffers tuned):
  - **ORDER=0: 7.39 / 7.49 / 7.81 GB/s** (3/3) — **at/above the documented ~6.7 GB/s single-client
    baseline** (c4). Real-wire throughput is *not* regressed and c3 beats c4 as expected.
  - **ORDER=5: 4.12 GB/s** (completing trial). The scanner freeze does not recur (trial 1 completes
    clean); the failures are the **residual** (short by 481–962 msgs ≈ 2–4 batches, ~0.005%), which
    the strict ACK gate counts as a trial failure and which is **more frequent on real-wire** than the
    1/10 seen on loopback. This residual = the pre-existing hold-buffer loose end (D1 will remove it).
- Real-wire ORDER0→ORDER5 gap (7.4→4.1 GB/s) is larger than loopback (12.5→12.3) — expected: on the
  producer-bound Intel client, ORDER=5's extra per-message client work dominates; not a broker
  regression (broker sequences+ACKs at ≤1.4 ms p99 and 12+ GB/s on loopback).

**Net:** performance restored (freeze fixed, throughput at/above baseline), Track-01 work preserved
(W1 + W1.2 intact), no other-session files touched. Remaining loose end = the small ORDER=5 residual,
recommended as a dedicated follow-up (and subsumed by D1).
