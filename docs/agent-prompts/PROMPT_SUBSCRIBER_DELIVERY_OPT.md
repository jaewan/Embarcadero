# Agent prompt: Subscriber delivery-path performance hardening

Copy everything below the line into a new coding agent.

---

## TL;DR

Raise ORDER=5 **single-client ordered-delivery goodput** by optimizing the **subscriber** recv/parse/stage path (not the sequencer). The gate uses one subscriber connected to four brokers (four delivery sockets), not one TCP socket. Gate = local **8 GiB** steady run; success = **≥1.5×** windowed delivery MiB/s vs your recorded baseline (~280 MiB/s → ≥~420 MiB/s) with `Pass=1`. Prefer the smallest correct patch. Do not commit unless asked.

## Mission

Harden Embarcadero’s subscriber library so ordered consume can keep up with brokers. You may choose batching, pooling, zero-copy retained buffers, recv/parse decoupling, etc., but you must obey correctness and measurement constraints below.

This is **client-library engineering**, not a sequencer/ordering redesign. Do **not** change ORDER=5 commit semantics, GOI, hold/fence, or ACK/RF contracts.

## Why this exists

Fig2 (append→ACK under ORDER=5 ACK2 RF2 **memory-copy**) shows the architecture claim working: append stays ~0.22–0.29 ms p50 as load rises. **Publish→deliver** still collapses above ~270–400 MB/s because the subscriber cannot drain ordered messages fast enough. The paper scopes the architecture claim to append; this work is optional e2e/product hardening.

## Prior evidence (authoritative)

Read first: `docs/experiments/delivery_ceiling_profiling.md`

| Fact | Value |
|------|-------|
| Profile tip (historical) | `c150fd1` on branch `delivery-measure` |
| Sustained 8 GiB windowed delivery | ≈ **278–287 MiB/s** (`delivery_timestamp_p25_p75`) |
| After safe pool/in-place pass | ≈ **278 MiB/s** — **no gain** (`recvfix_8g2_*`) |
| Limiter | Four subscriber `ReceiveWorkerThread`s ~100% CPU |
| Not limiter | Broker export workers (idle in drain), CXL export read |
| Socket signature | Broker Send-Q / `notsent` huge, `rwnd_limited` ~94%, deep subscriber Recv-Q |
| Remaining cost (post-pool) | Recv-thread parse/stage + **per-message copy into `OwnedMessage::data`** |

Doc’s recommended next fixes (you may follow or replace after re-profile):

1. Ordered-path **zero-copy retained-buffer** lifetime model, **or**
2. **Decouple recv from parse/stage** so a drain thread keeps reading while others parse/stage

Do **not** start with broker `writev` / `sendmmsg` / `MSG_ZEROCOPY` / export sharding until receive workers are no longer pegged and Recv-Q stays shallow.

## Pin / preflight (do this first)

1. Record starting tip and worktree state: `git rev-parse HEAD`, `git status --short` → write both in the plan and results note. Do not fold unrelated dirty changes into this task.
2. Confirm `build/bin/throughput_test` and (for local brokers) `build/bin/embarlet` exist. Rebuild after touching client code.
3. Confirm the build has `COLLECT_LATENCY_STATS=ON`; `scripts/run_latency.sh` requires it to produce the delivery CSVs. Reconfigure/rebuild if it does not.
4. Check that no Fig1/Fig2 or other latency campaign owns the local broker ports, `build/bin` CSVs, or the intended `DATA_DIR`. The harness removes transient CSVs from `build/bin` and starts/stops local brokers.
5. Use a fresh, task-specific `DATA_DIR` and unique `RUN_ID` values. Prefer **`SCENARIO=local`** for the capacity gate.
6. Run a baseline on this exact tip and configuration before code changes. Do not use the historical MiB/s as the after-run denominator.

## Verification loop (mandatory)

```
preflight → BASELINE 8GiB recipe → record table row
         → short plan (≤15 lines) + choose A/B/C from signals
         → implement smallest patch
         → AFTER 8GiB same recipe
         → fill acceptance table
         → if <1.5×: one gated next experiment OR stop with negative result
         → do not thrash / do not broaden to broker export yet
```

Optional fast iteration: 1 GiB smoke between edits. **Never** treat smoke alone as the capacity result.

## Success criteria

### Must

1. **Correctness:** ORDER=5 ordering assertions pass. Require the artifact `delivery_ordering_assertion.csv` to have `Pass=1`, `TimedOut=0`, `InvalidMessages=0`, `DuplicateTotalOrder=0`, `OutOfOrderTotalOrder=0`, `DuplicateUid=0`, and `MissingUid=0` for the 8 GiB run.
2. **Measurable gain** on sustained capacity (8 GiB preferred):
   - Metric: **windowed** `PayloadMiBPerSec` from `delivery_steady_throughput.csv` (p25–p75), not only total e2e MB/s
   - Target: **≥ 1.5×** your same-tip baseline (historical reference ~280 MiB/s → ≥ ~420 MiB/s)
   - Stretch: recover ~0.45–0.5 GiB/s windowed or better
3. **Bottleneck evidence:** after the fix, show (a) receive workers no longer pegged during drain, **or** (b) broker Send-Q / subscriber Recv-Q no longer deep while goodput rose. If goodput rose but workers still pegged, say so honestly.
4. **No silent contract changes** to ACK/RF/ORDER or “faster” by skipping ordered delivery.

### Should

5. 1 GiB smoke for iteration.  
6. Short before/after note under `docs/experiments/` + artifact paths.  
7. Focused diffs only in subscriber delivery (and minimal harness/test callers if API needs a compatible path).

### Must not

8. Change sequencer / GOI / hold-buffer / fence logic.  
9. Claim victory from Fig2 open-loop deliver latency alone.  
10. Break `ConsumeOrdered` / `ConsumeOrderedBatch` without a compatible path + updated callers.

## Acceptance table (fill this)

| run | tip SHA | windowed MiB/s | e2e MB/s | order assert | artifact_dir | notes |
|-----|---------|----------------|----------|--------------|--------------|-------|
| baseline | | | | | | |
| after | | | | | | |
| ratio (after/baseline) | same tip unless only uncommitted patch differs | | n/a | n/a | n/a | |

Record the exact CSV row used for each rate and compute `after / baseline` from the `PayloadMiBPerSec` column. Declare success only if ratio ≥ 1.5 **and** the after-run assertion passes.

## How to choose A / B / C (decision tree)

Re-check with a short profile/`ss` on **this tip** if anything looks different from the doc.

| Signal on this tip | Prefer |
|--------------------|--------|
| Recv workers pegged **and** samples/time dominated by memcpy into `OwnedMessage::data` / per-msg alloc | **A — zero-copy / retained buffers** |
| Recv-Q deep / `rwnd_limited` high **while** parse/stage CPU-bound on same threads as `recv` | **B — decouple recv vs parse/stage** |
| memcpy already gone but still dying in queue/lock/per-msg atomics / tiny stages | **C — heavier batching** (larger stage/consume spans) |
| Doc wrong on this tip (broker export now pegged, or workers not pegged) | **D — re-profile**, update diagnosis, **do not** large-rewrite yet |

If A and B both plausible, prefer **A** only if you can satisfy the zero-copy invariants below; otherwise do **B** first (usually safer). Prefer the **smaller correct patch** that can hit ≥1.5× before a full rewrite.

## Zero-copy / retained-buffer invariants (if you pick A)

Treat lifetime bugs as correctness P0 (SMR corruption).

**Must hold:**

1. A consumer-visible view into a chunk remains valid until `ConsumeOrdered` / `ConsumeOrderedBatch` has finished with that message (or an explicit release API runs).  
2. Chunk reclaim happens only after **all** outstanding views into that chunk are released — no early recycle on parse completion.  
3. No aliasing a mutable parse buffer across batches; no returning a recycled chunk while a view is live.  
4. Unordered `Consume()` path either stays on owned copies or gets the same lifetime rules — no silent use-after-free on one API.  
5. Under failure/partial batch/connection close: no leak **and** no reclaim-of-live-view.  
6. Run at least: ORDER=5 8 GiB assert pass; plus any existing subscriber/order tests you can run quickly. If ASAN/TSAN builds exist and are cheap, use them on a smoke; if not, document that gap.

If you cannot uphold these, **do not ship A** — choose B or C.

## Code map

| Area | Path |
|------|------|
| Subscriber core | `src/client/subscriber.h`, `src/client/subscriber.cc` |
| Receive + parse hot path | `ReceiveWorkerThread` → `ParseAndStageOrderedBytes` → `StageOrderedMessages` |
| Consumer API | `ConsumeOrdered`, `ConsumeOrderedBatch` |
| Ownership | `OwnedMessage`, `AcquireOwnedMessage` / `RecycleOwnedMessage` |
| Ordered queue/locking | `pending_messages_`, `consume_mutex_`, `TryPopOrderedMessagesLocked` |
| Harness drain | `src/client/test_utils.cc` (`EMBARCADERO_DELIVERY_DRAIN_BATCH`) |
| Run harness | `scripts/run_latency.sh` |
| Prior evidence | `docs/experiments/delivery_ceiling_profiling.md` |

## Measurement recipe (capacity gate)

Same shape as the profile doc. Run once for baseline (no your changes), once after:

```bash
env DATA_DIR=data/latency/delivery_ceiling_opt \
  RUN_ID=<baseline_or_after_tag> \
  TARGET_MBPS=1000 \
  THREADS_PER_BROKER=1 \
  ORDERS=5 \
  MODES=steady \
  SEQUENCER=EMBARCADERO \
  ACK_LEVEL=1 \
  REPLICATION_FACTOR=0 \
  MSG_SIZE=1024 \
  TOTAL_MESSAGE_SIZE=8589934592 \
  NUM_TRIALS=1 \
  NUM_BROKERS=4 \
  SCENARIO=local \
  EMBARCADERO_DEDUPE_LATENCY=1 \
  EMBARCADERO_SUBSCRIBER_DIAG=1 \
  EMBARCADERO_ORDER5_EXPORT_OVERRUN_FATAL=1 \
  EMBARCADERO_DELIVERY_DRAIN_BATCH=4096 \
  bash scripts/run_latency.sh
```

Collect:

- `delivery_steady_throughput.csv` (gate metric)
- `delivery_ordering_assertion.csv`
- `delivery_latency_stats.csv` / stage breakdown if present
- Brief CPU and/or `ss` evidence if you claim the bottleneck moved

For the capacity comparison, keep every listed environment value (especially `NUM_BROKERS=4`, `MSG_SIZE`, `TARGET_MBPS`, `EMBARCADERO_DELIVERY_DRAIN_BATCH`, and `SCENARIO`) identical between baseline and after. Do not change the metric, drain batch, timeout, or client/broker placement to manufacture a ratio. If host noise or a failed run makes either result unusable, rerun that same side before interpreting it.

Optional remote/Fig2 check is nice-to-have; **local 8 GiB** is the gate.

## Design freedom

You may implement A, B, C, or a combination. In your **first response**, write ≤15 lines:

- Tip SHA  
- Chosen approach (A/B/C/D) + which signal justified it  
- Patch sketch  
- How you will prove ≥1.5×  

Then implement.

## Landmines

- `EMBARCADERO_ORDER5_EXPORT_OVERRUN_FATAL=1` → overruns/skips must stay zero. Do not disable it for the comparison.  
- Lock order: no inversions with `consume_mutex_` / pool mutexes.  
- `OwnedMessagePtr` has a recycler that can take the pool mutex when a message is destroyed. Never destroy/recycle one while holding `consume_mutex_` if the resulting lock order can invert.
- Don’t regress unordered `Consume()` without tests.  
- Preserve `OrderedMessageView` validity for the documented lifetime of `last_returned_` / `last_returned_batch_`; a view must not outlive its backing storage.
- Do not alter `test_utils.cc` metric/assertion semantics merely to make a result look better. Harness-only changes must be necessary for a compatible API and called out in the results.
- Match existing CMake; rebuild client/`throughput_test` as needed.

## Deliverables

1. Code changes.  
2. Filled acceptance table + artifact dirs.  
3. Short design note: what changed, why the pool pass didn’t move the ceiling, remaining limiter if any.  
4. If you miss 1.5×: stop with a crisp negative result + **one** next gated experiment.

## Out of scope

Fig2/Fig1 campaign scripts, paper text, disk ACK2 durability path, Corfu/Scalog ports, sequencer epoch tuning, multi-host CXL, broker export sharding (until subscriber is no longer the pegged limiter).

## Working style

- Measure before large rewrites.  
- Evidence over intuition when doc and tip disagree.  
- Focused diffs; no unrelated cleanup.  
- Do not commit unless the user asks.

---

End of prompt.
