# Track 02 — TLA⁺ specification (W2)

**Read `sessions/00-BACKGROUND.md` first.** Track id: `02-tla-spec`. Branch: `session/02-tla-spec`.

**Goal.** A machine-checked TLA⁺ (PlusCal or plain TLA⁺ + TLC) spec of the session protocol under
the adversarial cross-product, shipped as an artifact and cited in the paper. Depends only on the
*design* (D1–D4), which is already written down — start now.

**Plan sections:** `Paper/improvement_plan.md` → W2 (scenario list), D1–D4, G4. Also skim D5 (ACK
contracts) and D3/D4 (frontier, assign-at-release) for the guard interactions.

## You OWN
- New directory `spec/` (create it): `.tla` + `.cfg` model files, a `spec/README.md`, and a
  `spec/results/` with TLC output summaries.
- You may add a short `docs/design/protocol_spec.md` cross-referencing the TLA⁺ modules to D1–D4.

## Do NOT touch
- Any `src/`, `Paper/Text/`, or other tracks' files. This track is self-contained in `spec/`.

## Scope — model the state machine
Sessions; PBR publication (two-phase, ABA guard); hold/fence; sequencer dedup (`expected_seq`); GOI
commit; spatial guard + wrap-fence; leases. Model **safety** (per-session prefix guarantee: the log
holds a submission-order prefix of each session; ACKed ⇔ committed; late/duplicate rejected, never
reordered) and the availability transitions (stall → retransmit → fence).

## Scenarios to check (minimum — from W2)
1. Broker crash pre- vs post-PBR publication.
2. Retransmit races (batch *n* to a different broker).
3. **The named one:** a retransmitted duplicate arriving at S_new inside the spatial-guard window.
4. Lease false positive (SIGSTOP-style pause) → duplicate discarded by dedup (safety holds).
5. **Stale-CV ACK relay after election (NEW, v4/D2):** `S_old` advances CV entries post-election for
   batches whose GOI entries readers will discard by epoch; a broker must **not** relay ACK1 for an
   entry that never becomes visible. Model the **ACK-relay control-block epoch check** and show it
   preserves "ACKed iff committed." This is the newly-found bug — prioritize it.
6. Client crash mid-stream → uncommitted suffix fenced, never committed.
7. Sequencer failover during an open gap → S_new reads session tables + rescans PBRs from committed
   frontier (D3/D7).
8. Module failure between ACK1/ACK2 (D5) → order survives, payload may not; surfaced not silent
   (this is the theorem's *sole divergence* clause — model reader agreement up to it).

## Coordinate
- **Track 01** owns the concrete protocol. Get their *frozen session/lease design* before finalizing
  invariants; if you find a counterexample, hand it to Track 01 (this is the point of doing spec
  before code — the plan says "TLA⁺ first, code second" for the assign-at-release × spatial-guard
  interleaving).

## Build/test
- TLC runs locally or on `broker` (Java). If you run heavy model-checking on `broker`, it's
  CPU-only (no CXL/ports) so **no lock needed** — but use a modest `-workers` count and your own
  dir `~/Embarcadero-sessions/02-tla-spec/`.

## Done criteria
- Spec + `.cfg` for each scenario in `spec/`; TLC passes for safety invariants (or produces a
  counterexample you've handed to Track 01); `spec/README.md` explains how to reproduce; a paragraph
  drafted for the paper's §5 TLA⁺ summary (hand to Track 07). Handoff note with findings.
