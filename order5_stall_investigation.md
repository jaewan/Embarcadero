# ORDER=5 non-head ACK stall — investigation report (moscxl agent)

**Date:** 2026-07-06. **Brief:** `docs/experiments/ORDER5_STALL_INVESTIGATION_BRIEF.md`.
**Status:** IN PROGRESS — A/B running.

## Corrections to the brief's symptom model (established from code + failed-run logs)

1. **"B1/B2/B3 return 0 ACKs" is by design, not the failure.** For ORDER=5 + EMBARCADERO +
   ACK1/2, the ACK stream is *head-owned*: only broker 0 relays a single per-client frontier
   (`GetClientOrdered(client_id)` — `network_manager.cc:2333-2342`), and the client normalizes
   progress as `broker_stats_[0].acked_messages` alone (`publisher.cc:1103-1121`). Non-head
   brokers return `-1` and never send ACKs in this mode. The per-broker printout showing
   `B1=0(short=all)` is cosmetically alarming but expected.
2. **The real failure: the head-relayed per-client frontier stops ~1.4M messages short.**
   Failed trials end at e.g. 9,040,510 / 10,485,760 with `sent_all=yes` (5442/5442 batches
   sent). The sequencer ends *idle-clean*: `hold=0 deferred=0`, no held work, flight recorder
   shows only idle epoch-driver events. The missing ~1.44M messages (≈705 batch-equivalents)
   were dropped, not delayed.
3. **Drop mechanism (hypothesis, consistent with all evidence so far):** under 4-thread load a
   broker's ingest/scan path lags ≳750 ms → the idle force-expiry
   (`GetOrder5IdleForceExpireTriggerNs`, default 750 ms / window 250 ms) arms and **gap-skips**
   (`order5_hold_timeout_skips_`, topic.cc:5981-5989) → the lagging batches arrive after
   `next_expected` has advanced and are **terminally dropped** (`L5_EXPIRED_LATE_DROP`,
   topic.cc:5950-5961) → their messages are never committed → the publisher's ACK wait can
   never complete → 120 s timeout. Prior graceful-run CSV shows `SkippedBatches=724,
   HoldTimeoutSkips=724` (× ~2048 msg/batch ≈ 1.48M ≈ the observed shortfall).
   This is exactly the `Hole` / hold-timeout anomaly semantics that improvement_plan G1/D1
   proposes to eliminate (gap-skip instead of session fencing).

## Environment facts confirmed

- moscxl kernel buffers already tuned (wmem_max=rmem_max=256MB).
- Baseline tree built at `~/Embarcadero-sessions/01-core-protocol-baseline` (tagged hunks
  C1/C3/C4/E1/E2/E3/W1.2 reverse-applied; untagged reorg edits kept; verified by diff).
  Same cmake flags (COLLECT_LATENCY_STATS=ON, same gRPC cache, empty CMAKE_BUILD_TYPE).
- Trace hooks for diagnosis runs: `EMBARCADERO_ORDER5_TRACE=1` (strict bool),
  `EMBARCADERO_ORDER5_PHASE_DIAG=1`, `EMBAR_FRONTIER_TRACE=1`, `GLOG_v=1` (late-drop/skip
  events are VLOG(1)).
- Head broker does not exit gracefully after a stalled run (killed -9), so its anomaly CSV row
  is missing for failed trials; non-heads write theirs.

## Phase A — A/B attribution (loopback repro, 5 trials each, TRIAL_MAX_ATTEMPTS=1)

Repro: `NUM_BROKERS=4 TEST_TYPE=5 ORDER=5 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024
THREADS_PER_BROKER=4 SEQUENCER=EMBARCADERO` via `scripts/singlenode_run_throughput.sh`.

| Trial | modified | baseline |
|-------|----------|----------|
| 1 | FAIL (9,080,977/10,485,760) | FAIL (5,467,852/10,485,760) |
| 2 | FAIL (9,040,510/10,485,760) | FAIL (5,272,272/10,485,760) |
| 3 | FAIL (6,377,396/10,485,760) | FAIL (6,948,762/10,485,760) |
| 4 | PASS (12,210 MB/s) | FAIL (5,545,906/10,485,760) |
| 5 | FAIL (2,699,727/10,485,760) | FAIL (8,466,264/10,485,760) |

Modified: **1/5 pass**. Baseline: **0/5 pass**.

Artifacts: `/tmp/ab_o5/{modified,baseline}/run_N.log`, broker-log snapshots in
`/tmp/ab_o5/brokerlogs/`.

## Phase C — real-wire numbers from c3

**Network correction to the brief:** the documented c3 dataplane (192.168.60.181 → moscxl
192.168.60.8) is **1 GbE on both ends** — a first Phase C run measured exactly line rate
(TOTAL 105.6 MB/s for both O0 and O5, 3/3 trials each; O5 completed with zero stalls even
there). c3 has an unaddressed **100G NIC** (`ens801f0np0`) on moscxl's 10.10.10.x fabric;
assigned `10.10.10.181/24` (non-persistent) and reran over 100G. c3 hugepages are
pre-provisioned (34288); the client's MAP_HUGETLB warning in run 1 was transient.

Results (fixed broker build, 4 brokers, 16 GiB, 1 KiB msgs, THREADS_PER_BROKER=4, ACK=1):
TBD (100G rerun in progress).

## Verdict (Phase A)

**PRE-EXISTING — not a Track-01 regression.** The unmodified baseline (all tagged hunks
C1/C3/C4/E1/E2/E3/W1.2 reverse-applied, same build flags, same config, same tuned kernel
buffers) fails 5/5 with the identical signature; the modified build fails 4/5. The stall is a
design-level behavior of the ORDER=5 pipeline under multi-threaded striping (idle force-expiry
gap-skip → late-drop), consistent with the baseline failure documented in
`LATENCY_EXPERIMENT_CHECKLIST.md` Step 27 ("8/10 pass, 2/10 fail ... lagging broker tail").
The higher failure rate here vs the checklist's 2/10 is likely config-dependent (10 GiB,
4 threads/broker, loopback).

## Phase B — root cause (instrumented capture)

**Captured failure (diag attempt 3, `/tmp/ab_o5/diag/attempt_3*`):** frontier ended at
9,553,092/10,485,760 (missing 932,668). End-state PHASE_DIAG:
- B0 ordered=2,623,600 (full), B1 ordered=2,620,720 (full), B2 ordered=2,620,720 (full)
- **B3 ordered=1,688,052; scan_push_b frozen at 933 (of ~1360 batches)** — the missing
  932,668 messages are exactly broker 3's unscanned tail.

Revisions to the initial hypothesis:
1. **Not a force-expiry late-drop event** in this capture: `L5_EXPIRED_LATE_DROP=0`; hold and
   deferred stayed 0; the sequencer was idle-clean the whole run.
2. **No cross-broker gap forms** because ORDER=5's hold contract is per-(client,broker) stream
   (`MakeClientBrokerStreamKey`): B0-2's streams complete independently; B3's stream simply
   ends early. Nothing is ever held, so idle force-expiry never fires. The per-client frontier
   = sum of committed messages = exactly total minus B3's unscanned tail.
3. The uniform "Connection closed by publisher" lines at send-complete are innocuous: all 16
   data sockets close at once when PublishThreads return (`ScopedFd` RAII closes them despite
   the stale "kept open" comment, publisher.cc:2360-2366); the kernel flushes buffered data
   before EOF, and recv() drains fully first.
4. So the failure is a **head-side per-broker scanner freeze** (`BrokerScannerWorker5` for one
   broker stops advancing mid-run and never recovers). Since a reserved-but-unpublished PBR
   slot is CLAIMED (reserve writes a claimed descriptor immediately, topic.cc:2211-2225) and
   CLAIMED slots get a bounded 10 s wait then skip, a permanent freeze narrows to:
   (a) scanner parked on a slot it classifies as **empty tail** (`!claimed && !publish_ready` →
   infinite wait) despite later slots being published, or
   (b) the **push-retry loop** wedging (`enter_collection` failing indefinitely;
   `[PUSH_FAILURE]` logging is diagnostics-gated so round 1 couldn't see it).

**Diag round 2 (`/tmp/ab_o5/diag2/attempt_1*`, `EMBAR_TOPIC_DIAGNOSTICS=1 GLOG_v=1`) —
ROOT CAUSE CONFIRMED.** This capture froze B0's own scanner (any broker's can freeze):
push_b frozen at 213, B0 ordered=400,816, B1-3 complete; 400,816+3×2,620,720=8,262,976 =
exactly the reported ACK count.

Timeline from the log:
1. 12:43:09.5 — `SEQ5_IDLE_DIAG hold=1`: one batch entered the hold buffer (out-of-order
   arrival across the 4 publisher threads). Hold insertion calls `InvalidateOrder5HeldSlot` →
   `ClearOrder5PublishState` (topic.cc:166-183), which zeroes the slot's first cacheline
   (flags=0, batch_complete=0) and sets publish_commit=UNCOMMITTED — making the slot
   indistinguishable from a never-written tail slot. (`CommitEpoch` also clears every
   committed batch's slot the same way, topic.cc:4278.)
2. 12:43:10.489-.633 — transient epoch-pipeline wedge: `[PUSH_FAILURE] ×19` with
   `states=[SEALED,SEALED,SEALED]`, sequencer 3 epochs behind (batch_seq=818, slot ~213).
   The pipeline itself recovered (final `epoch_index == last_sequenced`).
3. During/after the wedge the scanner landed on a slot reading EMPTY and took the
   **empty-slot resync** branch (topic.cc:6386-6406), which jumps the cursor to
   `batch_headers_consumed_through` with no direction guard — since consumed ≤ cursor
   always, this jump can only go backward. consumed pointed at slot 208: an
   **invalidated held slot** (heartbeat shows flags=0x0, publish_commit=UNCOMMITTED,
   num_msg=0, but pbr_abs=208 still intact in the second cacheline — the exact
   ClearOrder5PublishState signature; consumed_through=26624=slot 208×128).
4. Slot 208 never becomes publishable again, and once cursor==consumed the resync can't
   re-fire → the scanner waits at slot 208 forever, classifying it as ring tail, while
   ~1,150 published batches sit unscanned in slots 214..1360. Scanner heartbeats at
   12:45:09-15 show it parked there permanently.

**Why intermittent & 4-thread-only:** requires (a) a hold insertion (only possible with
out-of-order arrival within one (client,broker) stream, i.e., multiple publisher threads) whose
invalidated slot sits at the consumed frontier, and (b) a transient EMPTY read knocking the
scanner into the backward resync. Single-thread never holds → never invalidates mid-stream →
never freezes. ORDER=0 has no scanner → unaffected.

## Fix (applied to the session tree; NOT committed per brief)

Three changes, all in the modified tree:
1. `src/cxl_manager/cxl_datastructure.h`: new `kBatchHeaderFlagRetired = 1u << 2`.
2. `topic.cc ClearOrder5PublishState`: writes `flags=RETIRED` instead of 0, so invalidated
   slots are positively distinguishable from never-written tail slots. (All existing readers
   use bitmask tests for CLAIMED/VALID, so RETIRED reads as "neither" everywhere else.)
3. `topic.cc BrokerScannerWorker5`:
   - **Retired-slot skip**: a `!publish_ready` slot with RETIRED set is advanced past
     immediately (its batch is already owned by the sequencer — committed or in hold; skipping
     can never lose data). A full-lap streak guard yields the CPU if the ring ever reads fully
     retired (post-wrap idle).
   - **Forward-only resync**: the empty-slot resync now uses the same
     `0 < forward < slot_count/2` ring-distance window as the top-of-loop jump, eliminating
     backward jumps onto invalidated slots.

## Fix validation

10 loopback trials of the exact repro on the fixed build (pre-fix: **1/5 pass**):

| Trial | Result |
|---|---|
| 1-8, 10 | PASS — 11,460-12,112 MB/s |
| 9 | FAIL — 10,481,906/10,485,760 (short **3,854 msgs ≈ 2 batches**) |

**9/10 pass.** The one failure is a *different, much smaller* residual: mid-run `hold=65`
(a hold wave) that later drained with the frontier ending exactly ~2 batches short —
i.e., a hold-buffer bookkeeping path that removes entries without committing them (the same
loose end seen in diag2 where `hold=1` drained without `commit_b` advancing). Notably the
post-fix failure rate (1/10) matches the baseline failure rate documented in
`LATENCY_EXPERIMENT_CHECKLIST.md` Step 27 ("8/10 pass, 2/10 fail"), so this residual is
plausibly the *old* known intermittent, while the 80%-frequency scanner freeze (this
investigation's subject) is fixed. The residual deserves its own instrumented capture
(hold-insert/drain VLOG) as follow-up; its blast radius is ~2 batches per affected run
instead of ~1000.
