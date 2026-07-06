# E1 baseline-knee predictions — Track-03 DRAFT contribution

> **Ownership / status.** This is **not** the pre-registered file. Per `sessions/README` (IV.1 is a
> human task) and W1 closure #8, the **human (PI) owns and commits `e1_predictions.md`**, and that
> commit timestamp *is* the pre-registration. This draft supplies only the **baseline-knee content**
> (Track 03 has the deepest model of the baselines). Track 01 supplies Embarcadero's knee and the
> ~1.2–1.6× raw-throughput compression estimate; the human consolidates all of it plus the SLO-curve
> methodology into `e1_predictions.md` and commits it **before any E1/E2 run**. Integrity point
> (panel #4): the party that builds and runs the baselines must not also lock the predictions —
> hence hand-up, not self-commit.

## What is predicted
For each baseline, two distinct quantities:
- **(L) Low-load latency** — does the CXL transport close the client-visible latency gap vs the TCP
  baseline? (This is the "baselines-on-CXL close most of it" claim in IV.1 — true for some, not all.)
- **(K) Knee** — where the latency-vs-load curve turns up (the architectural limit), reported as the
  throughput at which SLO (1–100 ms) can no longer be met. This is the headline instrument.

Reference numbers are from `calibration_plan.md` (primary-source-cited). All bands are directional —
original hardware is older/slower, so these are floors/shapes, not ±targets.

## Corfu — the exception: CXL does NOT close low-load latency
- **(L) Predict: Corfu-on-CXL ≈ Corfu-on-TCP in low-load client latency — CXL closes little of it.**
  Rationale: Corfu's client-visible cost is **two network RTTs** (token, *then* write). The token hop
  is `client→broker→sequencer→broker→client`; CXL only accelerates the pod-internal broker↔sequencer
  leg (sub-µs), which is negligible against the client↔pod network RTT that dominates. The client is
  outside the CXL pod (methodology rule: no co-located publisher in headlines), so the token RTT stays
  network-bound. This is the honest Corfu-specific exception to IV.1's "baselines-on-CXL close most
  low-load latency."
- **(K) Predict: knee at token-serialization saturation** — the single `next_order_` counter +
  per-client mutex. Reference sequencer ceiling ≥570K tokens/s no-batch, >2M at batch=4 (Tango Fig. 2).
  **Open quantity to measure, flagged:** the CXL-mailbox request/grant round-trip may bound
  Corfu-on-CXL *below* the batched-RIO reference; if so, the mailbox round-trip (not the counter) is
  Corfu-on-CXL's limit — a reportable finding, not a bug (see calibration_plan.md Corfu note).

## Scalog — CXL closes most low-load latency; knee at cut cadence
- **(L) Predict: Scalog-on-CXL closes most of the low-load latency gap vs Scalog-on-TCP.** Rationale:
  Scalog's coordination (local cut → global cut) is **pod-internal**, so the gRPC coordinator RTT that
  dominates its latency is exactly what the CXL mailbox removes. Residual floor = the cut cadence.
- **(K) Predict: knee at the cut-cadence floor** — the interleaving interval (original design point
  0.1 ms, §6.3). Ordering latency is gated by the cut interval regardless of transport; the curve turns
  up when global-cut aggregation saturates. Per-shard throughput reference ≥18.7K writes/s @4KB (floor).

## LazyLog — CXL closes most low-load latency; knee at binding cadence
- **(L) Predict: LazyLog-on-CXL closes most of the low-load latency gap vs LazyLog-on-TCP.** Rationale:
  the coordinator/binding path is pod-internal; CXL removes the coordinator RTT. (Original LazyLog is
  RDMA/eRPC; our comparison is TCP→CXL for the coordinator transport.) The append fast path is already
  1-RTT durability with lazy background binding, so append latency is transport-light; the CXL benefit
  shows on the ordering/read-visibility path.
- **(K) Predict: knee at the binding-round cadence / single-leader ordering ceiling** —
  ≥1.34M metadata-appends/s (SOSP'24 §6.6), CPU/algorithm-bound. The curve turns up when the single
  leader's binding rate saturates.

## Cross-cutting commitments
1. **Direction split (the crux):** low-load latency is closed by CXL for **Scalog/LazyLog** (pod-internal
   coordination) but **not for Corfu** (client-visible token RTT). Do not let a single "baselines-on-CXL
   close low-load latency" sentence paper over the Corfu exception.
2. **All three retain an architectural knee** (token-serialization / cut cadence / binding cadence) that
   arrives **far earlier** than Embarcadero's (no token RTT and no cut/binding cadence on the ACK path).
   Embarcadero's knee location + the ~1.2–1.6× compression are Track-01/human inputs, not predicted here.
3. **E1 can shoot the author (kept honest by pre-registration):** if any baseline-on-CXL *also* closes
   the knee — not just low-load latency — then the SLO headline is transport, not architecture, and the
   paper retreats to E4/E6. This file is committed before runs precisely so that outcome is reported
   whichever way it lands.

## Handoff
Track 03 → human: consolidate the above into `e1_predictions.md`, add Embarcadero's knee (Track 01) +
the compression estimate + SLO-curve method, review, and **commit before E1/E2**. No E1 run should
start until that commit exists.
