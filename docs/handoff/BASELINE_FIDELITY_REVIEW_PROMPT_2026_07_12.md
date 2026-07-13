# Review prompt: Embarcadero design/implementation vs. baseline (Corfu/Scalog/LazyLog) fidelity

You are a systems-and-distributed-algorithms reviewer examining the Embarcadero
repo at `~/Embarcadero` (branch `main`). This is NOT a benchmarking or
performance task — no cluster access needed, no runs to launch. This is a
close-reading correctness/fairness audit. Your job: determine whether the
baseline implementations (CXL-Corfu, CXL-Scalog, CXL-LazyLog) faithfully
preserve each system's real-world architectural weaknesses in **replication**,
or whether the CXL port accidentally erases a weakness that the original
paper's design actually has — which would make the comparison against
Embarcadero unfairly favorable.

## Why this matters (read this first)

Embarcadero's central thesis is that **durability is the default expectation
for a shared log** (that's the point of the abstraction), and that
write-before-order systems (Corfu/Scalog/LazyLog) pay an architectural tax to
get it that Embarcadero's read-before-order design does not. If our ports of
those baselines quietly remove that tax — e.g., by parallelizing something
that was serialized in the original paper, or by not requiring true
multi-replica agreement before acknowledging — then every throughput/latency
number in the paper's comparison table is contaminated: we would be beating a
strawman, not the real systems.

The repo has already done a serious amount of this audit — read it, don't
re-derive it:

- `docs/baselines/fairness_appendix.md` — per-baseline KEPT vs CHANGED ledger
  under `docs/baselines/porting_rule.md`'s four frozen invariants (messages,
  exchange pattern, state machine, durability coupling). Read this FIRST,
  fully. It documents what was frozen and what was allowed to change (transport
  only), with an explicit "forbidden move we did NOT make" for Corfu.
- `docs/baselines/SCALOG_LIMITATION.md` — a list of real bugs found and fixed
  (§1–5), and §6's explanation for why Scalog and LazyLog converge to similar
  performance on CXL (both reduce to: write to CXL, gRPC round-trip to a
  remote coordinator for global order, coordinator dominates). §5c and §5d are
  specifically about a **self-replica-only vs. min-across-replicas** bug that
  gave LazyLog an unfair advantage — this is the exact class of question you
  are re-auditing, just in a different code path (see below).
- `docs/baselines/calibration_plan.md`, `docs/baselines/e1_baseline_knee_predictions.draft.md`
  — calibration methodology and the Corfu "honest below-floor finding"
  (mailbox RTT-bound, not tuned away) as an example of the kind of finding you
  should be looking for.

## The specific question that motivated this review

Reasoning to check, step by step — confirm or refute each with code, not
assumption:

1. **Corfu**: client gets a total-order token from a central sequencer, then
   the primary chain-replicates to the tail over the network, and the client
   is ACKed only once the tail finishes. This is real Corfu's bottleneck
   (network + serialization down the chain). **Question**: does our CXL port
   replicate in PARALLEL (all replicas fetch directly from CXL concurrently)
   instead of chain order? If so, does that erase the serialization cost that
   is part of Corfu's real design, making our Corfu look artificially fast at
   RF>1? Read: `src/disk_manager/chain_replication.cc` (842 lines — the name
   itself suggests chain, but verify the actual replica-to-replica dependency:
   does replica i+1 wait on replica i, or do all replicas read independently
   off CXL the moment data lands?), `src/disk_manager/corfu_replication_manager.cc`,
   `src/disk_manager/corfu_replication_client.cc`. Cross-check against
   `fairness_appendix.md`'s Corfu section — it discusses the *sequencer* hop's
   fairness in detail but says comparatively little about the *replication*
   hop's fairness. That asymmetry of attention is itself worth flagging if the
   replication side turns out to have a fidelity gap.

2. **Scalog/LazyLog**: messages go primary broker → replicas fetch/receive →
   every broker (primary + replicas) reports a local cut/progress value → a
   remote sequencer computes `min` across all reports → that min is the
   durable/ordered frontier. **Question A**: is broker→replica distribution
   chain or parallel in our port, and does that match original Scalog/LazyLog
   (check the papers' actual replication topology, not just our code)?
   **Question B (the important one)**: for the *local cut/progress a broker
   reports*, does it reflect what that broker has confirmed from ALL its
   replicas (true min-across-replicas, the correct/harder semantics) or only
   its own local write (self-replica-only, the easier/unfair semantics)?
   `SCALOG_LIMITATION.md` §5c says this exact bug existed for LazyLog and was
   fixed (broker-resident `lazylog_local_sequencer.cc`), and implies Scalog's
   local sequencer was already correct. But `fairness_appendix.md`'s Scalog
   AND LazyLog sections (written later, for the CXL-mailbox port of the
   *global* sequencer) both contain an explicit, unresolved caveat: **"What
   the broker-resident local sequencer actually reports per round (true
   per-replica cuts vs. self-replica-only progress under RF>1) is a
   pre-existing property of the untouched local sequencer... flagged for the
   human as a separate baseline-fidelity question."** This means: as of the
   mailbox port, nobody has actually re-verified whether the §5c fix's
   guarantee (min-across-replicas) still holds end-to-end. **This is your
   highest-priority thing to check.** Read `src/cxl_manager/scalog_local_sequencer.cc`
   and `src/cxl_manager/lazylog_local_sequencer.cc` line by line for the
   local-cut/local-progress computation and confirm whether it's a true min
   over per-replica confirmations or a shortcut over the primary's own state.

3. **Your staggler-lag point**: in a parallel-replicate-then-min-cut design,
   the whole pipeline's ordering rate is gated by the SLOWEST replica/broker
   each round (a straggler — network or server-caused). Confirm this is
   actually true of our implementation (does the global sequencer wait for
   ALL expected replicas before computing the cut, per round, with no partial
   credit)? `SCALOG_LIMITATION.md` §5e and `fairness_appendix.md`'s
   "Readiness / durability coupling" bullets for Scalog/LazyLog describe
   exactly this all-or-nothing gate — read them and verify the code matches
   (`ScalogGlobalOrderingCore::ComputeGlobalCut`, `LazyLogBindingCore::ComputeGlobalBinding`
   in `src/cxl_manager/`). Then think about whether this is a *fair*
   representation of the original systems' straggler sensitivity or whether
   our port makes it artificially worse or better than the papers describe
   (e.g., original Scalog's Paxos-backed global cut may have different
   straggler tolerance than a hard all-broker-must-report gate).

## Additional drawbacks to look for (don't stop at the three above)

Think about what ELSE could make a baseline's replication look better or
worse than the real system, in either direction:

- **ACK timing vs. durability**: does the baseline's client-visible ACK
  actually wait for the same durability guarantee the original paper claims,
  or does our port ACK earlier/later? (`SCALOG_LIMITATION.md` §5a/§5b are
  exactly this class of bug, already found and fixed for ordered/durable
  frontier — check whether the FIX still holds under the current code, and
  whether the same class of bug could exist in the replication-durability
  interaction specifically, not just the ordering-durability interaction.)
- **Batching asymmetry**: does Embarcadero batch messages before
  replicating/ordering in a way that gives it an advantage the baselines
  don't get an equivalent opportunity to use (or vice versa)? Check batch
  granularity in the token/cut/binding request path for each baseline vs.
  Embarcadero's own batch admission.
- **Failure/recovery cost NOT modeled**: do the baselines get to skip the
  recovery-after-crash cost that a real deployment would pay (session
  reconnect, replica resync, sequencer restart state rebuild) while
  Embarcadero's equivalent cost IS measured (see this session's E4 failure
  suite work — `scripts/run_failure_suite.sh`)? An apples-to-apples failure
  comparison requires the baselines to pay their real recovery cost too
  (`SCALOG_LIMITATION.md` doesn't appear to discuss baseline crash recovery
  at all — that may be a gap).
- **Network hop count**: for Corfu specifically, `fairness_appendix.md`
  explicitly preserves "the client's two network round trips (token, then
  write)" — confirm this really is exercised end-to-end in the benchmark
  harness (`scripts/run_multiclient.sh` CORFU path) and isn't accidentally
  collapsed to one hop somewhere in the client library
  (`src/client/corfu_client.h` — the appendix itself flags this file as "a
  clean-baseline smell" because Corfu currently reuses Embarcadero's shared
  publisher).
- **RF=0 handling**: `SCALOG_LIMITATION.md` §5e describes an RF=0 readiness
  bug (fixed). Verify the fix doesn't silently change RF=0 behavior in a way
  that then makes RF>0 comparisons inconsistent with RF=0 baselines elsewhere
  in the E2/E9 throughput matrix.
- **Calibration honesty**: `fairness_appendix.md`'s calibration section
  reports Corfu BELOW its literature floor as an honest finding (mailbox
  RTT-bound) — good practice. Check whether Scalog/LazyLog's "meets-floor"
  verdicts deserve the same scrutiny, i.e., are they meeting the floor for
  the right reason (structural, per the doc's own caveat about writes/sec
  being out of scope), or could a similar hidden shortcut be inflating them?

## Files to read, in order

1. `docs/baselines/porting_rule.md` — the four frozen invariants; the rule
   everything else is judged against.
2. `docs/baselines/fairness_appendix.md` — full read, all three baseline
   sections plus the calibration results at the end.
3. `docs/baselines/SCALOG_LIMITATION.md` — full read, especially §5 and §6.
4. `docs/baselines/calibration_plan.md`, `docs/baselines/e1_baseline_knee_predictions.draft.md`
5. Code, Corfu: `src/disk_manager/chain_replication.{h,cc}`,
   `src/disk_manager/corfu_replication_manager.{h,cc}`,
   `src/disk_manager/corfu_replication_client.cc`, `src/client/corfu_client.h`,
   `src/cxl_manager/corfu_mailbox_sequencer.{h,cc}` (if present — check),
   `src/protobuf/corfu_*.proto`.
6. Code, Scalog: `src/cxl_manager/scalog_local_sequencer.{h,cc}`,
   `src/cxl_manager/scalog_global_sequencer.{h,cc}`,
   `src/cxl_manager/scalog_global_ordering_core.{h,cc}`,
   `src/cxl_manager/scalog_mailbox_sequencer.{h,cc}`,
   `src/disk_manager/scalog_replication_manager.{h,cc}` (note
   `StartReplicaPollingThread` — confirms replicas POLL/pull rather than
   chain-push; verify this matches real Scalog's replication model).
7. Code, LazyLog: `src/cxl_manager/lazylog_local_sequencer.{h,cc}`,
   `src/cxl_manager/lazylog_global_sequencer.{h,cc}`,
   `src/cxl_manager/lazylog_binding_core.{h,cc}`,
   `src/cxl_manager/lazylog_mailbox_sequencer.{h,cc}`.
8. Embarcadero's own durability/ACK path for contrast:
   `src/network_manager/network_manager.cc` (`GetOffsetToAck`,
   `GetOffsetToAckFast` — the ACK1/ACK2 clamp §5b touches these),
   `src/embarlet/topic.cc` (`AckRelayEpochValid`, replication-related
   sections) — so you can compare Embarcadero's real durability coupling
   against what each baseline's port claims to preserve.
9. The original papers if available in `docs/` or `Paper/` (Corfu/Tango SOSP'13,
   Scalog NSDI'20, LazyLog reference) — at minimum, `Paper/RelatedWork.tex`
   and `Paper/Text/Appendix.tex` (referenced by `SCALOG_LIMITATION.md` as
   discussing the write-before-order coupling thesis) for how the paper
   *describes* each baseline's architecture, so you can check the
   implementation against the paper's own characterization, not just against
   the original external papers.

## What to produce

A findings report, one entry per issue, each with:
- **Claim**: what the code/docs currently do or assert.
- **Verification**: what you read to confirm or refute it (file:line).
- **Verdict**: FAITHFUL (matches original system's real weakness) /
  ADVANTAGES-EMBARCADERO (port removes or weakens a real baseline cost,
  biasing the comparison) / ADVANTAGES-BASELINE (port adds a cost the real
  system doesn't have, biasing the comparison the other way) / OPEN
  (insufficient evidence, needs a targeted experiment or author clarification).
- **Severity**: does this affect a number currently in the paper (Sec7
  tables/figures), or is it latent?

End with a prioritized list: which findings should block the next eval
campaign (i.e., require a code fix + re-run before the numbers are
trustworthy) vs. which are fine to note as limitations/future work.

Do NOT modify any code or run any benchmarks in this pass — this is read-only
analysis. If you find something that clearly needs a code fix, describe the
fix precisely (file:line, what changes) but stop short of applying it; surface
it as a finding for the human to prioritize alongside the rest of the
publication roadmap.
