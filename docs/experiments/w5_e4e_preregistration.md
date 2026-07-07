# W5 pre-registration — E4e / E1-leg3 / E6 predictions

**Committed BEFORE any headline E4e/E1/E6 run**, per `improvement_plan.md` §IV.1 (panels Flag 4,
#3, #4): "any result contradicting a pre-registered prediction → report it and revise the claim,
not the experiment." Everything run so far under `w5-variants` (Phase 0 fabric checks, the small
-scale protocol-correctness smoke tests in `w5_phase1_progress.md`) is plumbing verification, not
a headline measurement — none of it used a scale large enough to be a bandwidth/failure result,
so this pre-registration precedes the first real run as required.

**Scope:** Track 05's three contributions to the plan's experiment matrix — E4e (§IV.2 e), E1 leg
3 (§IV.2 E1), E6 both-variants R-inflation (§IV.2 E6) — plus the reviewer-D token-server repro
(§IV.2 E10 tie-in). Source docs: `docs/design/W5_RDMA_VARIANTS_SPEC.md`,
`docs/experiments/rdma_variants.md`.

---

## E4e — conjunction contrast (money figure, panel #1)

**W5-A column (leg-1 cost, broker-host kill):**
- **Predicted:** order survives (GOI/PBR/ControlBlock on the memserver, untouched by the kill);
  the killed broker's DRAM Blog is unrecoverable — 100% of its in-flight (unread-by-sequencer)
  payload for that host is lost. Detection: `kPeerDown` (RC `RETRY_EXC → QP ERROR` on the
  broker-direct Blog QP) fires **before** any lease timeout, because RC's own retry/timeout
  (`timeout=14`, `retry_cnt=7` in `ConnectRcQp`) resolves in low-hundreds-of-ms, faster than any
  sane lease window (≥1s per D1's placeholder). **Predicted detection latency: tens to a few
  hundred ms**, an order of magnitude below the lease backstop.
- **Predicted stall shape:** sessions routed through the killed broker stall for the detection
  window; sessions on the surviving broker are unaffected (no shared endpoint — this IS leg 2
  holding). This is qualitatively different from CXL's ≈0 stall (no data lost) and from a
  hypothetical shared-memserver-Blog failure (which would have zero *isolated* impact since no
  single broker owns it) — W5-A's signature is "some sessions total-loss + fast repair, others
  untouched."

**W5-B column (leg-2 cost, load ramp to saturation):**
- **Predicted knee: at c3's raw multi-QP device ceiling**, empirically ~90 Gb/s single-QP (Phase 0
  measurement) and expected to approach ~95-100 Gb/s aggregate with multiple QPs (per
  `ib_write_bw -q N`). **Predicted shape: goodput plateaus at that ceiling and does NOT rise
  further as more broker QPs/threads are added** — the CC-agnostic claim the spec commits to
  (§4.1 Decision 2). This is the single most falsifiable prediction here: if goodput keeps
  climbing well past the raw `ib_write_bw -q N` reference line, either the reference measurement
  was wrong or the funnel isn't actually saturated yet.
- **Predicted pull-path amplification:** sentinel-array poll traffic is small (num_brokers × 8B
  per poll) relative to payload, so its *share* of the funnel should be negligible at low load and
  grow as a *fraction* of a shrinking available margin near saturation — i.e., the poll doesn't
  meaningfully compete for bytes, but poll *latency* (time to get a completion) should inflate
  sharply near the knee because payload WRITEs and poll READs contend for the same NIC/CQ
  resources. **Predicted: detect-to-commit p999 latency degrades by ≥5× between 50% and ~100% of
  the ceiling** (directly measurable via the `detect_to_commit` stat already in `rdma_sequencer.cc`).

**CXL column (neither cost, process-level kill):** predicted ≈0 data loss, stall ≈ existing D1
session-fencing numbers (not this track's to measure); no NIC funnel signature at all (throughput
should be substrate-bound, not network-bound, at any load this track can generate). Labeled
explicitly "process-level, not host-level" per plan line 530 — not re-measured by Track 05.

**Compression prediction (shared axis with baselines):** N/A directly to W5 (W5 isn't a baseline
comparison), but the qualitative "CXL pays neither cost" summary bar should show **both** RDMA
columns' costs as clearly non-zero and CXL's as the visual zero — no numeric compression factor is
predicted here since the three columns measure structurally different things (loss/repair vs.
bandwidth ceiling vs. neither), which is itself the intended finding.

## E1 — leg 3 (Embarcadero-on-RDMA vs Embarcadero-on-CXL, substrate gain)

- **Predicted RDMA↔CXL delta:** CXL's 200–500 ns loads vs. RDMA's µs-scale (single-digit to
  low-tens of µs, per Phase-0/smoke measurements: observed RTT p50 ≈ 12–30 µs for a full 4-WR
  producer batch over the real fabric) implies roughly **1–2 orders of magnitude lower per-op
  latency for CXL**, but this gap should **compress significantly at the throughput/knee level**
  once batching amortizes the per-op cost — consistent with the plan's general expectation that
  "low-load latency is largely transport" (§IV.1) and CXL's advantage should be most visible
  **below** the knee, not at it.
- **Per spec §6 note:** W5-B (not W5-A) is architecturally the closer analog to Embarcadero-on-CXL
  (a memory pool reached over a transport, only the transport differs — same "everything on one
  passive endpoint" shape). If the plan's E1-leg-3 slot is filled with W5-A instead (per the
  plan's literal text), expect the delta to conflate substrate gain with the leg-1/leg-2
  architecture difference; flagging this now so a surprising E1 result is diagnosed against the
  right hypothesis (substrate vs. architecture) rather than immediately doubted.
- **Explicit acknowledgment (plan line 508-511):** if baseline-on-CXL closes the knee too, E1 "can
  shoot the author" — the SLO headline would be transport, not substrate. This track will publish
  whatever E1 shows, per the plan's own rule.

## E6 — R-inflation under load (both variants)

- **Predicted ranking (spec §6, rdma_variants.md):** **W5-B inflates fastest** (payload writes +
  sequencer poll/read both contend on c3's one NIC — confirmed mechanically true by this track's
  own architecture, not just a guess) **> W5-A inflates less** (per-broker NICs on the payload
  path; only the small metadata channel is shared) **> CXL inflates least** (not measured by this
  track; expected near-flat by construction — no NIC in the payload path at all).
- **Predicted mechanism, tied directly to code already built:** `stale_misses` in
  `rdma_sequencer.cc` (genuine ring-wrap-vs-poll-rate losses, not a proxy) should stay near-zero
  until the offered load approaches each variant's respective knee, then rise sharply — this *is*
  R inflating in the harness's own terms (repair/backlog cost rising as the sequencer falls
  behind). Overlay predicted (analytic, from queueing-under-contention intuition) vs. observed
  (measured `stale_misses` + `detect_to_commit` percentiles) once the funnel sweep data exists.
- **tc-netem jitter sweep:** not yet run; prediction deferred to that specific experiment's own
  setup (shared methodology with Track 02's CXL-side E6, per the plan).

## Reviewer-D token server (E10 tie-in)

- **Predicted:** a minimal multi-QP one-sided `FETCH_ADD` token server reproduces **~30M tokens/s
  in aggregate** across enough QPs/threads to exceed the single-QP atomic cap (empirically 16
  outstanding/QP per `ConnectRcQp`'s `max_dest_rd_atomic=16`, measured 1.99 Mops/s single-QP in
  Phase 0 — so ~15-16 QPs should be in the right range to approach 30M/s, extrapolating linearly;
  this is the falsifiable part).
- **Predicted irrelevance mechanism (the actual point, per spec §5):** token RTT (a full
  client-critical-path round trip per ordered message) and the repair loop (straggler
  wait/gap-fill/retransmit) are untouched by raising the token rate — Embarcadero pays neither on
  its ACK path (per-session prefix from local state). **Predicted: client-observed added latency
  from the token RTT alone (even at unsaturated token rates) exceeds Embarcadero's own
  `append_send_to_ack` numbers from the delta-measurement work** (1.0-1.1 ms typical, 4.95 ms
  worst — `delta-measure` branch), demonstrating the rate was never the binding constraint.

---

## Falsifiability summary (what would make this track revise a claim, not the experiment)

1. W5-B funnel goodput keeps rising well past the `ib_write_bw -q N` reference as more QPs are
   added → the "CC-agnostic plateau" claim is wrong or the reference was mismeasured.
2. W5-A's kPeerDown detection does NOT fire meaningfully faster than a lease timeout → the
   "dual-signal, RDMA-fast-path" claim (spec §3.4 C5) doesn't hold on this fabric.
3. E1's RDMA→CXL gap is small at the knee → the SLO headline is transport, not substrate (plan's
   own "E1 can shoot the author" — publish anyway, per plan line 511).
4. W5-A's R-inflation is NOT smaller than W5-B's → the "per-broker NICs help" architectural
   argument is wrong, a genuinely interesting negative result worth reporting as-is.
