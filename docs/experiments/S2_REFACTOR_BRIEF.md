# S2 refactor — implementation brief (for the moscxl-side agent)

**From:** Track 01. **Goal:** the S2 prerequisite for D1 — collapse the 4 duplicated
seq-classification loops in `ProcessLevel5BatchesShard` into ONE unified classifier, so D1's
session-fencing later lands in a single place. **This is the first step of D1 §7 sequencing.**

**Base tree:** `~/Embarcadero-sessions/01-core-protocol-clean` (clean Track-01 = W1 fixes + W1.2
assert + the ORDER=5 scanner-freeze fix; builds, ORDER=5 repro 7/8, W1.2 0-violations). Work there.
**Design refs:** `docs/design/D1_SESSION_FIFO_DESIGN_v2.md` §3 & §7; `docs/experiments/code_review_w1.md`
(S2+C5 section). All anchors verified vs `chore/repo-reorg` @ `7ec0e70`.

## Scope — two commits, in order

### Commit 1: behavior-preserving classifier EXTRACTION (byte-identical for ORDER=5)
The decision tree (`== next_expected` → emit; `< ` → late/dedup; `>` → hold-or-force-skip) is
duplicated across **4 per-record loops** in `src/embarlet/topic.cc` `ProcessLevel5BatchesShard`
(`true_client_chain = UsesTrueClientChainOrdering(order_)` at **topic.cc:5258**; ORDER=5 ⇒ true):

| # | Loop | site | keying |
|---|------|------|--------|
| 1 | fast-path true_chain | `for (PendingBatch5* p : ordered)` **5393** | `client_id` |
| 2 | fast-path legacy | `for (PendingBatch5* p : vec)` **5500** | `MakeClientBrokerStreamKey` (5489) |
| 3 | general true_chain | `for (auto jt=it; jt!=group_end)` **5612** | `client_id` |
| 4 | general legacy | `for (auto jt=bit; jt!=broker_end)` **5723** | `MakeClientBrokerStreamKey` (5711) |

Extract ONE classifier reproducing every branch **exactly**. The only variation axes (per
code_review_w1.md's line-by-line map) are: (A) key = `client_id` (true_chain) vs `stream_key`
(legacy); (B) the `true_client_chain` bool; (C) which `record_*` side-effects fire
(`record_hold_insert`/`record_forced_skip`/`record_late_drop`/`accumulate_logical_only`/
`per_client_terminalized_delta_epoch`); (D) the **LT divergence** — true_chain LT = *drop-late* (no
emit; accumulate_logical_only + late_drop + per_client_terminalized), legacy LT = *emit-late*
(order5_fifo_violations_++ + push ready + MarkEmitted). Branches to reproduce, each verified:
- **DUP-already-emitted** (`is_duplicate && IsEmitted`): true_chain → `terminalize_already_emitted` + continue; legacy → plain continue.
- **IsEmitted-only**: mark_sequenced + advance next_expected if seq≥next_expected; true_chain → terminalize + continue; legacy → if `!CheckAndInsertBatchId` push ready + MarkEmitted + fifo_violations++.
- **EQ** (identical all 4): mark_sequenced; advance_next_expected; if `!CheckAndInsertBatchId` → MarkEmitted + push ready.
- **LT**: see (D).
- **GT force-skip** (hold full): sleep 10µs; if `deferred < max` push deferred + continue; else mark_sequenced; next_expected=seq+1; skipped/forced-skip counters; MarkEmitted; push ready. legacy also fifo_violations++; true_chain also record_forced_skip.
- **GT hold-insert**: dedup `stream_map.find(seq)`; build `HoldEntry5` from `cached_*`; emplace; `hold_buffer_size++`; `clients_with_held_batches.insert(key)`; `InvalidateOrder5HeldSlot(hdr)`; push held-marker. true_chain also record_hold_insert.

Suggested signature (adapt to the actual locals):
```cpp
// returns nothing; mutates shard/state/emitted/ready exactly as the inline branches did
void ClassifyOrder5Batch(Level5ShardState& shard, ClientState5& state, ClientEmitTracker& emitted,
                         size_t key, PendingBatch5& p /*or *jt/*p*/, bool true_client_chain,
                         std::vector<PendingBatch5>& ready, /* side-effect lambdas or refs */ ...);
```
The 4 sites keep their own grouping/sort/seed prologue; only the **inner per-record body** becomes a
call. **HARD REQUIREMENT: byte-identical behavior for ORDER=5** (true_client_chain==true). Do NOT
change the LT/GT/EQ semantics; this commit is pure de-duplication.

**Do NOT touch in commit 1:**
- The **seed lines** (legacy `state.next_expected = vec.front()->batch_seq` / `bit->batch_seq`) — that's commit 2.
- The **expiry gap-skip** at **topic.cc:5985** (`// Sequence is beyond next_expected - skip the gap`) and 5951 — that is **D1's fence anchor**, a LATER D1 step, NOT part of S2. Leave the expiry sweep as-is.
- The **ORDER=5 scanner-freeze fix** (`kBatchHeaderFlagRetired`, `[[SCANNER_RETIRED_SKIP]]`, `[[SCANNER_FORWARD_ONLY_RESYNC]]` in `BrokerScannerWorker5`, and `ClearOrder5PublishState`). Untouched.
- `src/embarlet/topic.h` W1.2 members / `commit_order_last_seq_` etc.

### Commit 2: seed unification (fixes C5; legacy-only behavior change)
Unify new-session seeding to `next_expected = 0` (the true_chain rule at ~5331-5336/5550-5554) for the
legacy sites too, replacing `state.next_expected = <first-observed batch_seq>` (legacy seeds around
5489-5499 / 5711-5721 region). **No-op for ORDER=5** (already seeds 0); behavior change for ORDER=4
(`kOrderClientBrokerStream`) only — intended (fixes C5: first-observed seed silently drops earlier
striped batches). Keep this a SEPARATE commit so the ORDER=5-identical extraction is bisectable from
the legacy fix.

## Verification (harness already exists)
Build the clean tree, then per commit run the exact repro I used (`/tmp/clean_verify_A.sh` on moscxl,
or replicate): `NUM_BROKERS=4 TEST_TYPE=5 ORDER=5 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024
THREADS_PER_BROKER=4 SEQUENCER=EMBARCADERO EMBAR_ASSERT_COMMIT_ORDER=1 TRIAL_MAX_ATTEMPTS=1
bash scripts/singlenode_run_throughput.sh` — **×8 trials**.
- **Pass bar (commit 1):** ORDER=5 pass rate ≈ current baseline (**≥7/8**), **W1.2 commit-order
  violations = 0**, **0 FATAL/CHECK aborts**, throughput ~11.5–12.3 GB/s (no regression). Byte-identical
  ⇒ these should match the pre-refactor numbers.
- **Pass bar (commit 2):** same ORDER=5 results (unchanged); optionally an ORDER=4 sanity run showing
  the C5 fix (earlier-striped batches no longer dropped).

## Testbed discipline (learned the hard way)
- `flock -w 2400 ~/Embarcadero-sessions/testbed.lock -c "..."`; log START/END to `~/Embarcadero-sessions/activity.log`; **compile outside the lock**.
- **Cleanup:** never `pkill -f embarlet` (matches your own ssh command → self-kill, ssh exit 255). Kill by explicit PID (`pgrep -x embarlet`) or the flock holder's process group. `fuser -k` on the shared lock is blocked.
- Track-01-owned files ONLY (`topic.cc`, `topic.h`). Never touch `~/Embarcadero` or other sessions' dirs. Do NOT git-commit (leave staged; describe the two commits' contents).

## Deliverable
The two-commit change staged in the clean tree, verification numbers (ORDER=5 pass rate + W1.2 = 0 +
throughput) for each commit, and confirmation that ORDER=5 behavior is byte-identical after commit 1.
Report back; I fold it into the Track-01 deliverable and proceed to the next D1 §7 step (CXL session table).
