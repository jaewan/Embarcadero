# ORDER=5 real-wire ACK-completion stall — root cause & fix

**Date:** 2026-07-07. **Tree:** `~/Embarcadero-sessions/01-core-protocol` (uncommitted).
**Task brief:** fix the intermittent ORDER=5 real-wire (c3, 100G) ACK-completion stall.

## Root cause (two defects, one primary)

### Defect 1 (primary, data loss): epoch-buffer IDLE-rollback race wipes in-flight batches
`BrokerScannerWorker5`'s push-retry recovery (topic.cc ~6753) does:
`cand.reset_and_start()` (buffer → COLLECTING) → CAS `epoch_index_` → **on CAS failure,
rolls the buffer back with a raw `state.store(IDLE)`**. Between `reset_and_start()` and the
rollback, another scanner can legitimately pass `enter_collection()` (state==COLLECTING at
both checks) and push a batch into the candidate buffer. The rollback ignores
`broker_active[]` and the deque contents, so the buffer goes IDLE **containing batches**;
the next `reset_and_start()` — the only IDLE→COLLECTING path — clears the deques and the
batches vanish: not committed, not held, not deferred, no counter, no log.

Smoking-gun capture (run of 2026-07-07, `[ORDER5_HOLD_DIAG]` instrumentation, frozen trial):
```
client=267751 held=30666 min_held_seq=2033 max_held_seq=32699
next_expected=2032 highest_sequenced=2031 emitted_contig_max=2031 deferred=0
```
Seqs 0..2031 committed; **only batch 2032 is missing** (all 32,700 batches were pushed —
scanner counters complete); everything after it (30,666 batches) is stuck in the hold
buffer behind the gap. ORDER=5's true-client-chain contract sequences the client's global
batch_seq, so one vanished batch dams the entire remaining stream.

The same race at lower frequency explains the previously-documented "benign" residuals:
loopback validation failures short exactly 1-2 batches (481/962 msgs), and real-wire trials
completing 99.994% — those are runs where the vanished batch(es) sat near the stream tail
(little or nothing behind the dam) or where the storm drained the rest via force-expiry
gap-skip.

### Defect 2 (liveness): force-expiry arming starves while the sequencer is busy
The only code that arms the force-expiry window (`run_idle_hold_tick` → 750 ms
stalled-broker trigger) ran exclusively in the sequencer loop's *idle* branches. With a
~30k-entry hold buffer, every epoch pass costs ≈ the 500 µs seal cadence (the hold-age scan
is O(held) per epoch), so the sequencer trails `epoch_index_` by 2 forever, never idles,
never arms expiry, and never even prints `[SEQ5_IDLE_DIAG]`. Result: a gapped stream freezes
permanently (observed frozen at 569k/1.83M/... of 15.7M) instead of recovering via gap-skip.
When the sequencer *did* reach idle (send completed while the hold was moderate), expiry
armed and drained the hold as an "expiry storm" (20k-28k `[L5_EXPIRY_ENTRY]` lines) — the
intermittency between "storm + near-complete" and "frozen" runs is just whether the
sequencer ever idled.

Note: `[Publisher ACK Per-Broker] B1..B3=0(short=all)` is by design (ORDER=5 ACK is
head-owned; the client normalizes on B0 only). The stall was always the head's per-client
commit frontier, never the ACK relay/socket path. Hypotheses 1 and 3 from the brief were
ruled out by `GetClientOrdered`-sourced PHASE_DIAG matching the client's frozen normalized
count exactly; hypothesis 2 (startup race) was ruled out by mid-burst freeze points.

### Defect 1b (second data-loss window, same observable): drain races the seal quiesce
`EpochBuffer5::seal()` publishes `state=SEALED` **before** waiting for active collectors to
quiesce (topic.h:194-215). The sequencer consumes a buffer the moment it reads SEALED, so a
scanner that already passed `enter_collection()` (broker_active set, both state checks
passed pre-CAS) can complete its push **after** the drain — into an emptied buffer that then
sits IDLE and is wiped by the next `reset_and_start()`. Verified empirically: with Defect 1
fixed, an after-run trial still lost exactly one batch (seq 6075, B2 commit_b=8175/8176);
the liveness fix (Defect 2) then recovered the run via force-expiry storm to within 481 msgs
instead of freezing — proving a second, drain-side window existed.

## The fix (3 changes in src/embarlet/topic.cc)

1. **[[EPOCH_BUFFER_LOSS_FIX]]** (scanner recovery, ~topic.cc:6760): on `epoch_index_` CAS
   failure, do NOT roll the candidate buffer back to IDLE; leave it COLLECTING. Buffers are
   not epoch-tagged; the driver's rotation seals it and the sequencer consumes it within ≤2
   epochs, and late-landing batches are exactly the out-of-order case the hold buffer
   handles. This removes the only path that can mark a non-empty buffer IDLE.
2. **[[EXPIRY_ARMING_LIVENESS_FIX]]** (`EpochSequencerThread`): split `run_idle_hold_tick`
   into arming/tracking (runs from the busy loop too, internally 1 ms-gated) vs the inline
   empty-epoch drain (idle-only, as before). A busy sequencer can now still detect a
   750 ms-stalled broker and arm force-expiry; the drain itself already runs in the normal
   per-epoch pass.
3. **[[EPOCH_BUFFER_LOSS_FIX]] (consumer quiesce)**: both epoch-consume sites (main loop and
   shutdown drain) now require `state==SEALED && no broker_active[]` before draining. New
   collectors cannot enter a SEALED buffer, and stragglers hold `broker_active` until after
   their push completes, so waiting on the flags closes the drain-vs-push window.

Plus temporary diagnostics (`[ORDER5_HOLD_DIAG]`, 1/s, phase-diag-gated) used for the
capture — kept behind `EMBARCADERO_ORDER5_PHASE_DIAG=1`.

## Before/after — ORDER=5 real-wire single-shot (c3 → moscxl 100G, 5 trials, 16 GiB, 1 KiB)

Before (fix absent, two 5-trial runs — **1/10 completions overall**):
| Run | Completions | Per-trial ACKed (of 15,728,640) |
|---|---|---|
| 2026-07-07 run 1 | 1/5 | frozen 1,828,281; storm-short 15,727,678 (-962); PASS 7.47 GB/s; two more fails |
| 2026-07-07 run 2 (diag) | 0/5 | frozen 977,392; -962; **frozen 0**; -481; -481 |

Intermediate (fixes 1+2 only, one 5-trial run): 1/5 completions (6.51 GB/s), but **zero
freezes** — every failure reached ≥99.99% and was short exactly 1-2 batches
(-481/-481/-481/-962), isolating the remaining loss to the drain-race (Defect 1b).

After (all three fixes, 5-trial single-shot run, 2026-07-07):
| Trial | Result |
|---|---|
| 1 | **COMPLETE** 7.02 GB/s total / 6.80 overlap |
| 2 | **COMPLETE** 6.33 / 6.24 |
| 3 | **COMPLETE** 7.06 / 6.80 |
| 4 | **COMPLETE** 6.73 / 6.51 |
| 5 | **COMPLETE** 7.10 / 6.80 |

**5/5 completions, zero `ACK Timeout` lines in all five client logs (shortfall = 0).**
Success criterion met (≥5/5, ≤1k shortfall; achieved 0).

## No-regression evidence (all with the full fix, 2026-07-07)

| Check | Requirement | Result |
|---|---|---|
| ORDER=5 loopback ×5 (single-shot) | ≥4/5, ≥11.4 GB/s | **5/5**: 12.50 / 12.18 / 11.54 / 12.32 / 11.79 GB/s |
| ORDER=0 loopback ×5 | ≈12.5 GB/s | **5/5**: 12.37 / 13.07 / 12.53 / 12.41 / 12.51 GB/s |
| ORDER=0 real-wire ×3 (c3, 100G) | ≈7.4-7.65 GB/s | 7.39 / 7.19 / 7.78 GB/s |
| Correctness | counters zero | Non-head `order5_anomaly_counters_broker{1,2,3}.csv`: all-zero rows for every run. All 10 O5 runs (5 real-wire + 5 loopback) ran with `EMBAR_ASSERT_COMMIT_ORDER=1` (violation = fatal CHECK) and completed → `order5_commit_order_violations=0` by construction. Every completed run ACKed exactly all messages (0 shortfall). Head-broker CSV rows are absent after killed runs (pre-existing behavior — head teardown needs SIGKILL; noted, not caused by this change). |

Diff: `docs/experiments/order5_realwire_ack_stall_fix.diff` (single file, `src/embarlet/topic.cc`,
~89 added lines incl. comments and the `[ORDER5_HOLD_DIAG]` diagnostic; base =
reviewer's `01-core-protocol-clean` snapshot). Not committed.

## Ruled out / remaining uncertainties

- Stale CXL between trials: `metadata` zero mode covers control block + CV + GOI + TInode +
  PBR rings; `consumed_through=BATCHHEADERS_SIZE` at start is a legitimate init sentinel
  (topic_manager.cc:214). Only stale Blog payload persists — not implicated.
- Dedup (`FastDeduplicator`) false positives: exact-match table; batch_id =
  (broker<<48)|pbr_abs_index is unique per run — ruled out.
- Client-side ACK receive (EPOLLET fragmentation): partial-read state machine is correct;
  not implicated (frozen counts matched the broker-side frontier exactly).
- The separate TLA⁺ finding (stale-CV ACK relay epoch check) is untouched — no changes on
  the ACK relay path.
- Uncertainty: the O(held) per-epoch hold-age scan is a throughput cliff under mass holds
  (it is what starved the idle path). With Defect 1 fixed, mass holds should no longer form
  in healthy runs; the scan cost remains a latent scalability issue worth a follow-up.
- Note for the reviewer: remote client logs show `threads_per_broker from config: 3` — the
  `-n` flag appears not to reach the remote client via run_multiclient.sh EXEC_CMD; per-run
  totals still match (16 GiB, 15,728,640 msgs). Worth a separate look; not this bug.
