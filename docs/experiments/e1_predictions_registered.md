# E1/E3/E4 Predictions Registered

**Status:** pre-registered before E1/E3/E4 headline runs.
**Branch:** `eval-matrix`.
**Base commit inspected:** `a1d1ee14134ac1487432df9182a4d0729a81faa5`.
**Registration date:** 2026-07-08.

## Phase-0 Gate

This file is the first Phase-0 action for the D1-dependent evaluation matrix. No E1, E3, E4, or
other headline benchmark run may start until this file is committed. If any subsequent result
contradicts a prediction below, the paper will report the contradiction and revise the claim, not
revise the experiment.

Before writing this registration, the evaluator inspected the existing harness and baseline context:

- `scripts/run_throughput.sh` and `scripts/lib/run_throughput_impl.sh`
- `scripts/run_latency.sh`
- `scripts/run_latency_vs_load.sh`
- `scripts/run_latency_low_load.sh`
- `scripts/run_latency_regression.sh`
- `scripts/publication/run_latency_cell.sh`
- `docs/baselines/e1_baseline_knee_predictions.draft.md`
- `docs/baselines/calibration_plan.md`
- `docs/baselines/fairness_appendix.md`
- `docs/baselines/porting_rule.md`
- `docs/baselines/SCALOG_LIMITATION.md`

The predictions below consolidate the Track-03 draft with the current baseline porting rule,
calibration plan, and harness contracts.

## Method Commitments

E1 compares four legs:

1. baseline-on-TCP
2. baseline-on-CXL-transport
3. Embarcadero-on-RDMA
4. Embarcadero-on-CXL

Leg 2 is interpreted only under the ex ante baseline porting rule: same messages, same exchange
pattern, same state machine, same durability/ACK coupling, transport swapped to the CXL mailbox.
Any result that shows leg 2 closing the headline knee is a valid result and narrows the paper's
claim; it is not a tuning failure.

Headline experiments will use remote clients, not co-located publishers. They will report median
and range across at least three trials, and at least five trials for p99.9/tail claims. Throughput
will be reported in messages/s and GB/s across the message-size sweep `{128B, 512B, 1KB, 4KB, 16KB}`.
Latency will be reported as latency-vs-load curves at 10%, 50%, 70%, and 90% of each system's own
measured peak, avoiding past-saturation tails.

## Prediction 1: Raw-Throughput Compression

Raw throughput improvements from Embarcadero-on-CXL over the honest CXL-upgraded baselines are
expected to compress to roughly **1.2x to 1.6x** once the baselines receive the same CXL substrate.
This is intentionally modest. Table-4-style N=2 gaps and the baseline calibration notes suggest that
transport and local memory bandwidth remove a large part of the raw bandwidth gap. The remaining
throughput advantage should come from Embarcadero's ordering architecture and ACK path, not from a
many-x faster wire.

If the measured compression is lower than this range, the paper should retreat from a broad raw
throughput claim and focus on the knee/failure results. If it is higher, the paper should still
separate substrate effects from architecture effects through the waterfall rather than folding all
gain into Embarcadero.

## Prediction 2: Low-Load Latency Is Mostly Transport, With a Corfu Exception

For Scalog and LazyLog, baseline-on-CXL is expected to close most of the low-load latency gap versus
baseline-on-TCP because their coordination paths are pod-internal and dominated by the coordinator
round trip that the CXL mailbox replaces.

Corfu is the important exception. Corfu-on-CXL is expected to close little of the low-load
client-visible latency gap because the client still waits for a token before the write, and the
client remains outside the CXL pod. The broker may act only as a transport proxy for the sequencer
hop; the client-visible token RTT is preserved by the porting rule.

Therefore the generalized prediction is: **low-load latency is largely transport for the
pod-internal coordinator baselines, but not for Corfu's client-visible token path.**

## Prediction 3: The Knee Is Architectural

The central E1/E3 prediction is that the latency knee remains architecture-bound after the CXL
transport swap.

- **Corfu:** knee at token-path saturation. The earlier calibration notes show Corfu-on-CXL is likely
  mailbox-RTT-bound rather than counter-bound, with the faithful CXL mailbox path below the Tango
  no-batch and batch=4 floors. Either way, the knee is still the token path: a client-visible
  request/grant dependency before append completion.
- **Scalog:** knee at global-cut cadence / cut aggregation. CXL removes transport overhead, but the
  ordered frontier still waits for local-cut reporting, min aggregation across replicas/shards, and
  global-cut application.
- **LazyLog:** knee at binding-round cadence / single-leader ordering. The append fast path preserves
  LazyLog's 1-RTT invariant, but ordered visibility still depends on background binding rounds.
- **Embarcadero:** knee should arrive later because the ACK path does not wait for a token RTT, global
  cut cadence, or binding cadence. Ordering is driven by the sequencer observing published batches
  and session state in CXL memory.

If baseline-on-CXL also moves the SLO knee to Embarcadero's region, the E1/SLO headline becomes a
transport result rather than an architecture result. That outcome must be reported as-is.

## Prediction 4: SLO Curve Shape

The E3 SLO curve should show Embarcadero sustaining higher throughput at tight SLO thresholds than
the baselines once load approaches the architectural knee. At low load, Scalog-on-CXL and
LazyLog-on-CXL may approach Embarcadero's latency floor, while Corfu should remain separated by the
client-visible token path. At 70% and 90% of each system's own peak, the baseline curves should turn
up earlier because their ordering/visibility dependencies serialize progress on token, cut, or
binding cadence.

The SLO curve must be derived for both append latency and end-to-end publish-to-deliver latency,
using the harness outputs that distinguish publisher batch latency from `publish_to_deliver_latency`.

## Prediction 5: Failure Behavior

Embarcadero broker-kill recovery is expected to be approximately **delta**, single-digit
milliseconds under the configured session timeout, and per-session rather than a global stall. D1's
session-based FIFO should let independent sessions recover independently, with no global
zero-throughput hole.

By contrast, the baselines are expected to expose seal/cut/reconfiguration stalls:

- Corfu must recover token/write progress after sequencer or log-unit disruption.
- Scalog must re-establish cut progress and the durable ordered frontier.
- LazyLog must re-establish binding progress and durable visibility.

If E4 shows Embarcadero has a broad cross-session stall or recovery substantially above delta, the
paper must revise the failure claim. If a baseline recovers at delta with no global hole, that result
must also be reported and the contrast narrowed.

## Interpretation Rule

The evaluator will not reinterpret the experiment after seeing the data. The registered predictions
above decide the claim boundaries:

- prediction confirmed: use the result as evidence for the corresponding claim;
- prediction contradicted: report the contradiction, explain the observed mechanism, and revise or
  remove the claim;
- ambiguous result: report the raw data and mechanism, then narrow the claim to the supported subset.

The purpose of E1/E3/E4 is measurement, not defense of a predetermined headline.
