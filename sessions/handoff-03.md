# Handoff — Track 03 (baseline CXL-transport ports)

Branch: `session/03-baseline-ports` (off `chore/repo-reorg`). Do not commit/push without the human.

## Unit 1 — Ex ante porting rule (W1 on-paper closure) — DONE

**What:** Wrote `docs/baselines/porting_rule.md`, the normative rule that governs every baseline CXL
adaptation *before* any port is written (plan W4 / panel #4). One sentence: **protocol-preserving
substitution only — same messages, same state machine, transport swapped; anything that alters a
baseline's serialization structure is a redesign, not a port, and is forbidden.**

Grounded in the actual current implementations:
- **Corfu** — unary `GetTotalOrder(TotalOrderRequest)→TotalOrderResponse`, monotonic token counter,
  per-`client_id` mutex; token already per-batch (so E10 batched sequencer is a *port*, not redesign).
- **Scalog** — bidi `LocalCut ↔ GlobalCut`, per-epoch cut, `min` across replicas per shard.
- **LazyLog** — bidi `LocalProgress ↔ GlobalBinding`, binding-round deltas, `min` across replicas.

The doc freezes four invariants per baseline (message set, exchange pattern, state machine/decision
function, durability coupling), lists the redesign tripwires that would import Embarcadero's
architecture into leg 2 of the E1 waterfall, states what the transport swap *may* change (encoding,
ring-vs-stream, poll-vs-block, addressing, transport-forced back-pressure), gives a 5-question
decision test, and folds in the calibration + author-sanity-check commitments (panel #10). Preserves
the fairness fixes documented in `SCALOG_LIMITATION.md` §5 (5c/5d etc.).

**Staged:** `docs/baselines/porting_rule.md`
**Intended commit (docs — use `--no-verify`):**
```
git commit --no-verify -m "docs(baselines): ex ante porting rule for CXL baseline adaptations

Protocol-preserving substitution only (same messages, same state machine,
transport swapped). Per-baseline application for Corfu/Scalog/LazyLog, forbidden
redesign tripwires, calibration + author-check commitments. W1 on-paper closure
(plan W4/panel #4); protects the E1 waterfall's middle leg.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

## Unit 2 — CXL mailbox transport lib — DONE

**What:** Built the shared-memory RPC substrate the ports run on, from our own primitives (reuses
`Embarcadero::CXL::flush_cacheline/store_fence/invalidate_cacheline_for_read/full_fence/cpu_pause`
in `src/common/performance_utils.h` — the same discipline the Blog/PBR path uses; deliberately NOT
`std::atomic`, whose acquire/release does not cross a non-coherent CXL link).

- `src/cxl_transport/cxl_mailbox_ring.h` — single-writer/single-reader record ring. Monotonic
  cursors on isolated cache lines; producer flushes body+fence, then stamps per-slot `seq` as the
  single publish point; reader forces fresh fetch (invalidate+fence) before reading. Explicit
  back-pressure (full ring blocks producer = faithful gRPC flow-control window). Pure POD, offsets
  only, no pointers → valid across address spaces / hosts.
- `src/cxl_transport/cxl_mailbox.{h,cc}` — `MailboxSegment`: per-broker **duplex** ring pairs
  (`up(b)` broker→coord, `down(b)` coord→broker) + `BroadcastDown()`. One topology expresses all
  three baselines' frozen protocols as a pure transport swap (see header comment). Backends: POSIX
  shm (`CreateShm`/`AttachShm`) for standalone global sequencers, and in-place attach
  (`CreateInPlace`/`AttachInPlace`) over a region carved from the CXL segment.
- `src/cxl_transport/cxl_mailbox_smoke.cc` — 6 checks; wired as CTest `cxl_mailbox_smoke`.

**Verified on broker:** builds clean (`cmake --build build --target cxl_mailbox_smoke -j64`); smoke
run under flock **ALL PASSED** — FIFO+payload, back-pressure, concurrent SPSC (2M msgs),
segment duplex round-trip (40k req/resp), cross-process over fork (50k), POSIX-shm create/attach.
activity.log annotated START/END.

**Staged:** `src/cxl_transport/cxl_mailbox_ring.h`, `cxl_mailbox.h`, `cxl_mailbox.cc`,
`cxl_mailbox_smoke.cc`, and `src/CMakeLists.txt` (adds `cxl_transport` lib + `cxl_mailbox_smoke`
target/test). **Intended commit (code — pre-commit hook is interactive; run without --no-verify or
coordinate):**
```
git commit -m "feat(cxl_transport): CXL mailbox RPC substrate for baseline ports

Single-writer record rings (clwb/sfence discipline, no std::atomic across the
non-coherent link) + per-broker duplex MailboxSegment (up/down rings, broadcast).
POSIX-shm and in-place(CXL) backends. Smoke test (CTest) covers FIFO, back-pressure,
2M-msg SPSC, duplex round-trip, cross-process fork, and shm. Transport for the W4
baseline ports; see docs/baselines/porting_rule.md.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

## Unit 2b — Senior review + fix cycle (dynamic workflow) — DONE

Ran a review→fix→adversarial-verify workflow over the mailbox (6 design agents → 1 coherent apply
→ build/fix loop on broker → 3 adversarial verifiers). Applied: P0-1 alignment CHECKs + 64-aligned
test allocs; P0-2 loud single-writer contract + **debug-only concurrent-writer detector** (compiled
in this build — `CMAKE_BUILD_TYPE` empty ⇒ no `-DNDEBUG`); P1-3 `BroadcastDown` decoupled; P1-4
`volatile` on cross-CXL fields (seq/len/read_index; write_index is now producer-host-local); P1-5
hot-path rework (cached read_index shadow, deleted dead write_index flush, single seq+len fetch,
pack payload into one line for small records); P2 API (`TryConsume` CHECK not truncate, fd handoff,
comments).

**Two criticals the adversarial panel caught — I fixed by hand (did not trust the apply agent):**
1. *Fence bug* — overflow-payload path used `load_fence` after `InvalidateRange`; `clflush` needs
   `mfence` (full_fence) per the codebase's own docs, else a reader can memcpy stale payload after
   seeing the publish point. Live for any record >48 B (default `record_size`=512). Fixed →
   `full_fence`, and added `TestLargeRecordOverflow` (200 K × 200 B concurrent) — the overflow path
   had **no** test before, which is why the build agent reported green with the bug present.
2. *BroadcastDown liveness* — still retried wedged rings forever ⇒ a dead broker hangs the
   coordinator (the P1-3 hazard relocated). Fixed → one bounded decoupled pass returning
   `BroadcastStatus`; caller owns the wedged-broker policy. Regression-locked in the smoke test.

**Throughput: did NOT improve (~0.58–0.64 M msg/s, was ~0.605 M) — and I proved why.** Single-thread
interleaved microbench = 0.644 M/s ≈ two-thread 0.584 M/s ⇒ the wall is the irreducible per-message
clflush+mfence of the non-coherent-CXL discipline, not overhead or cross-core transfer. **My review's
"biases E1" alarm was overcalibrated:** baseline control traffic is per-epoch (Scalog/LazyLog
~2 K/s/broker) or per-batch (Corfu), i.e. 100–300× below 0.6 M/s, so the mailbox is not the
control-plane bottleneck and does not skew the waterfall. If E10's token rate ever needs more, the
only lever is **flush batching** (amortize fences across a burst) — noted, not built.

Rebuilt clean on broker; all 7 smoke checks pass (incl. new overflow + BroadcastDown-returns tests).

**⚠ CMake coupling:** `src/CMakeLists.txt` now contains BOTH my `cxl_transport` lib+smoke target AND
Track 04's `CXL_FAULT_INJECTION` hunks (add_subdirectory + embarlet link). Split at commit time /
coordinate with Track 04 — do not commit the file wholesale as one track's change.

## Unit 3a — Corfu token-path port (topology-independent core + E10 sequencer) — DONE

Built via a design→apply→build→adversarial-verify workflow, then hardened by hand.

**Topology decision (documented in fairness_appendix.md):** Corfu's token requester is the external
*client*, which cannot reach CXL. The faithful CXL adaptation co-locates the sequencer in the pod and
has the **ingress broker** post the token over the mailbox — preserving the serialization structure
(per-client FIFO, `expected_batch_seq` gate, the single `next_order_` monotonic counter = the E1/E10
knee) and swapping only the sequencer-facing transport. Requester relocation (client→broker) is the
one forced, documented deviation.

**What shipped:**
- `corfu_sequencer_service.{h,cc}`: **behavior-preserving refactor** — `GetTotalOrder` → shared
  `TokenStatus AssignToken(...)` core. The CXL port calls the **identical** `AssignToken`, so ordering
  is bit-for-bit the same code as the TCP baseline. Guarded by the unchanged `corfu_sequencer_fifo_smoke`.
- `corfu_mailbox_messages.h`: POD `CorfuTokenRequest`/`CorfuTokenGrant` (static_asserted, echo
  client_id+batch_seq for correlation).
- `corfu_mailbox_sequencer.{h,cc}`: `CorfuMailboxSequencer` — single poll thread draining up(b) rings,
  `AssignToken`, granting on down(b). One `CorfuSequencerImpl` shared.
- `corfu_mailbox_bench.cc`: E10 batched-sequencer bench + FIFO correctness harness (per-client FIFO,
  per-broker log_idx/batch_seq monotonicity, echo correlation) + **wedged-broker regression test**.
- `cxl_mailbox_ring.h`: added producer-side `HasSpace()`.

**Adversarial verify caught 1 real critical → I fixed by hand** (same discipline as the mailbox
round): the sequencer delivered grants with **blocking `Produce()`**, so a slow/dead broker's full
`down(b)` ring would HOL-block the whole sequencer and hang `Stop()`/`Join()`. Root cause: it consumed
the request and irreversibly `AssignToken`ed (advancing `next_order_`) *before* knowing it could
deliver. **Fix:** gate on `down.HasSpace()` **before** consuming/assigning (sole-writer ⇒ next
`TryProduce` is guaranteed), so a wedged broker is skipped, never blocks others, never hangs Stop.
Added a ctor `record_size >= sizeof(request/grant)` CHECK and a wedged-broker regression test.

**Verified on broker (all green):** builds clean; `corfu_sequencer_fifo_smoke` rc=0 (refactor is
behavior-preserving); `corfu_mailbox_bench` ALL CHECKS PASSED incl. `wedged-broker-does-not-block: ok
(healthy broker got 200/200 grants; Stop/Join returned)`.

> Perf note (NOT calibrated): bench reports ~0.35–0.40 M tokens/s, but N=16,384 over ~40 ms is
> warmup-dominated. For a real E10/calibration number the bench must scale N to millions over seconds
> and report the steady plateau + P50/P99. See calibration_plan.md — Corfu's committed floor is ≥570K
> tokens/s (Tango Fig. 2); if the mailbox round-trip keeps us below it, that is an honest finding to
> report, not to hide.

**Deferred (needs your sign-off): Unit 3b — E1 client-path integration** (wire the broker-relayed
token into embarlet/publisher.cc). It's the one piece that touches the client↔pod boundary and
Track-01-adjacent ingest code; I won't build it until you confirm the requester-relocation topology.

## Unit 6a — Calibration plan (panel #10) — DONE (research + plan; runs pending)

`docs/baselines/calibration_plan.md` written from 3 cited research reads of the primary PDFs
(Corfu NSDI'12/TOCS'13/Tango SOSP'13; Scalog NSDI'20; LazyLog SOSP'24). Key committed targets, all
framed **directionally** (hardware is strictly newer, so meet-or-exceed floors, not ±bands):
- **Corfu:** sequencer ≥570K tokens/s no-batch / >2M at batch=4 (the only single-host-comparable Corfu
  number). Append/latency/recovery = flash+1GbE-bound → out of scope.
- **Scalog:** per-shard ≥18.7K writes/s @4KB (floor) + ~1.3 ms single-shard latency at 0.1 ms
  interleaving interval. 52M headline = emulated cluster-aggregate → out of scope. No p99 published.
- **LazyLog:** 1-RTT append invariant (exact structural match) + ordering ≥1.34M metadata-appends/s +
  ~4× latency-reduction *ratio* vs an eager TCP baseline. RDMA/eRPC → absolute µs out of scope.
Honest scope statement + author-sanity-check plan included.

## PI decisions (recorded) + resulting doc corrections

**Decision 1 — Corfu E1 topology: proxy-relay ALLOWED, requester-collapse FORBIDDEN.**
The token-before-write test governs: after the change, the client's append must still become
ordered/acked only after a token round-trip it waited for. Allowed = broker is a *pure transport
proxy* (client issues the token, waits for it, then writes; the broker just relays the request over the
CXL mailbox to the co-located sequencer; client-visible 2 network RTTs preserved — the legitimate cost
E1 exposes). Forbidden = broker requests token + writes payload + acks in one client→broker RTT (that
is WBO / Embarcadero-minus-hold-buffer; voids E1 leg 2). My earlier "requester relocated client→broker"
phrasing drifted toward the forbidden version — **corrected** in `porting_rule.md §5.1` (added the
proxy-relay definition + the token-RTT-collapse tripwire) and `fairness_appendix.md` (KEPT/CHANGED
rewritten; explicit "NOT changed" clause). Unit 3a (sequencer + bench) is consistent: the bench driver
plays the broker-relay role and collapses no client RTT.
**Ownership:** the client-visible path must NOT go in `src/client/publisher.*` (Track 01, mid-rewrite).
Verified Corfu currently co-opts the shared publisher (`src/client/corfu_client.h` invoked from
`publisher.cc`) — that sharing is itself the smell. **Unit 3b will FORK a Track-03 Corfu baseline
client**; any unavoidable shared-publisher hook is *specced to Track 01, not applied* (avoid a third
03↔0x collision). I reverted a gratuitous 1-line `corfu_client.h` include tweak the workflow slipped in.

**Decision 2 — `e1_predictions.md`: human owns and commits; Track 03 only drafts baseline knees.**
Pre-registration integrity (panel #4): the party that builds/runs baselines must not lock the
predictions. Wrote `docs/baselines/e1_baseline_knee_predictions.draft.md` (Track-03 contribution, NOT
owned/committed here) with per-baseline knees + the key nuance: **CXL closes low-load latency for
Scalog/LazyLog (pod-internal coordination) but NOT for Corfu (client-visible token RTT)**; all three
keep an architectural knee (token-serialization / cut cadence / binding cadence). Track 01 adds
Embarcadero's knee + ~1.2–1.6× compression; human consolidates + commits before any E1/E2 run.

## Unit 4 — Scalog cut/report port — DONE (workflow + hand-hardening)

Built via workflow; adversarial verify caught real issues; I fixed by hand (pattern holds: every round has a real bug).

**What shipped:** `scalog_global_ordering_core.{h,cc}` (shared min-across-replicas-per-shard core,
extracted from `scalog_global_sequencer` and called by both the gRPC path and the mailbox path),
`scalog_mailbox_messages.h` (POD LocalCut/GlobalCut; map→fixed by-broker array, kMaxBrokers=32),
`scalog_mailbox_sequencer.{h,cc}` (poll → aggregate → `BroadcastDown`), `scalog_mailbox_bench.cc`
(independent min recompute + monotonicity + broadcast fidelity + wedged-broker regression).

**Adversarial verify caught 2 real defects → I fixed by hand:**
1. **Leg-1 fidelity break (the important one).** The "extraction" was NOT behavior-preserving: it
   replaced the gRPC baseline's original count-only readiness (`total_num_replicas == expected`) with
   a **new min-epoch barrier** baked into the shared `ComputeGlobalCut` — so the TCP baseline (E1 leg
   1) inherited a changed cut cadence *and* an epoch-regression→deadlock risk. **Fix:** made the epoch
   barrier **opt-in** (`use_epoch_barrier`, default false). gRPC path = count-only (original behaviour
   restored bit-for-bit); mailbox path opts in (it's poll-driven and needs round detection). Hardened
   `last_epoch` against regression (monotonic max). The min-across-replicas VALUES are unchanged and
   shared by both paths.
2. **Gratuitous `scalog_local_sequencer.cc` include tweak** (a Track-01-linked file) — reverted.

**Appendix corrected (was overclaiming):** separated the *shared min decision* (identical both paths)
from the *transport-specific readiness* (gRPC count-only vs mailbox opt-in barrier); added an honest
**durability-coupling caveat** — the core mins over the LocalCuts it *receives* (bench-verified with
explicit per-replica cuts), but whether the untouched broker-resident local seq reports true
per-replica cuts vs self-only under RF>1 is a **pre-existing** question (cf. SCALOG_LIMITATION §5c),
flagged for the human, NOT claimed end-to-end; and fixed the false "scalog_ack_invariant_test guards
the extraction" claim (it's a standalone gtest; the real guard is the bench's independent min recompute).

**Pre-existing, out of scope (flagged, not fixed):** the gRPC baseline's `stream->Write()` silently
ignores failures (scalog_global_sequencer.cc:164) — original baseline code; changing it would alter
leg 1.

**Verified on broker (all green):** builds clean; `scalog_ack_invariant` passes; `scalog_mailbox_bench`
ALL CHECKS PASSED (min-across-replicas, monotonic, broadcast-fidelity, wedged-broker); ~47K cuts/s,
~378K ordered-msg/s (perf not calibrated — small run; calibration floor ≥18.7K writes/s/shard @4KB).

## For other tracks
- **Track 01:** mailbox lib lives entirely in new `src/cxl_transport/`; does not touch
  `src/embarlet/topic.{cc,h}` or the epoch collector. Only shared dependency is the existing
  read-only `src/common/performance_utils.h` CXL helpers (no edits).
- **⚠ Track 04 (coupling flag):** Track 04 placed its fault-injection controller at
  `src/cxl_transport/faultinj/cxl_fault_inject.{cc,h}` — i.e. *inside the dir this track owns*. It's
  currently untracked in the shared working tree; I left it alone (not mine to commit/edit). Worth
  the human deciding whether that belongs under `src/cxl_transport/` or a Track-04-owned dir, since
  it interposes on the same `Embarcadero::CXL::*` seam my rings depend on.

## Next units (not started)
3–5. Port Corfu token / Scalog cut-report / LazyLog coordinator paths onto the mailbox (transport
   swap only, per the rule's per-baseline mapping); reproduce the batched CXL-Corfu sequencer (E10).
6. Calibration (≥3 trials under `flock`) + `docs/baselines/fairness_appendix.md` + author emails.

## Open questions for the human
- Confirm `src/cxl_transport/` as the home for the mailbox lib (vs. `src/common/`).
- OK to reuse the cached gRPC source dir on `broker` per BACKGROUND for builds?
