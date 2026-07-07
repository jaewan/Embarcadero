# Open-load append→ack tail measurement — δ constants for D1 §5.2

**Date:** 2026-07-07. **Branch:** `delta-measure` (base `s2` @ 9901b93). **Owner gate:** D1 v2
§9 residual #2 / must-fix #12 ("δ from open-load tail, not steady pacing").
**Method:** `docs/experiments/DELTA_MEASUREMENT_BRIEF.md` — loopback open-loop offered-load sweep,
ORDER=5 ACK=1 RF=0, 4 brokers, 1 KiB msgs, 4 GiB/trial, 3 trials/point,
`scripts/run_latency_vs_load.sh` (PACING_MODE=open_loop → harness `burst` mode).
**Run dir:** `data/latency_vs_load/delta_measure/EMBARCADERO_order5_ack1_rf0/run_20260707T024651Z`
(moscxl; full ~2.7 GB with logs). **Committed audit subset (~200 KB):**
`delta_measurement_raw/` — per-trial `pub_latency_stats.csv` / `pub_cdf_latency_us.csv` /
`stage_latency_summary.csv` / `run_metadata.txt`. The 4.951 ms anchor is verifiable there
(`points/003_4000mbps/.../trial1`: p99.9 = max = 4951 µs, a single sample; trials 2/3 = 755/1143 µs).
**Note (provenance):** the wrapper `run_latency_vs_load.sh` defaults `ORDER=0` (singular) and
forwards it as `ORDERS="$ORDER"` to `run_latency.sh` (which itself defaults `ORDERS="0 5"`); so
setting only `ORDERS` at the wrapper level is silently overridden to ORDER=0. This run set `ORDER=5`
explicitly — verified `order=5` in the wrapper `metadata.env` and in each per-trial
`run_metadata.txt` (the value actually passed to `throughput_test -o`).

## Results (µs; per point: median across the 3 trials, worst trial in parens)

`append_send_to_ack_batch_latency` (publisher send→ACK — the δ-relevant RTT):

| target MB/s | achieved MB/s | p50 | p99 | p99.9 (worst trial) | submit→ack p99.9 (worst) |
|---|---|---|---|---|---|
| 1000 | 1000.0 | 359 | 686 | 1089 (1228) | 3573 (3736) |
| 2000 | 2000.0 | 344 | 683 | 881 (1200) | 2696 (3156) |
| 4000 | 3999.6 | 247 | 779 | 1143 (4951) | 2421 (6161) |
| 6000 | 5999.8 | 277 | 791 | 1052 (3856) | 2292 (4814) |
| 8000 | 7905.7 | 273 | 862 | 1074 (3437) | 2162 (4426) |

Samples: 545 fully-acked batches/trial (1635/point) → p99.9 is a top-1/2-sample statistic per
trial; hence median + worst-trial reported. 15/15 trials completed; 0 W1.2 violations expected
path (assert not armed for latency runs; sequencing counters clean in broker logs).

## Knee

Achieved offered load tracks target exactly (≤0.1% off) through **6000 MB/s**; the first
divergence is at **8000 MB/s (achieved 7905.7, −1.2%)** — the knee onset. Operating load for δ
purposes = at/just-below the knee (4000–6000 MB/s points).

## Key finding

**The open-load send→ack tail does NOT track the open-load e2e tail.** D1 v2 §5.2 feared
δ_cap ≥ ~166 ms if the ack tail tracked the ~83 ms e2e p99; measured, the ack tail is three
orders smaller: p99.9 ≈ 1.0–1.1 ms typical, 4.95 ms worst single trial across the operating
range. The 83 ms e2e tail lives in subscriber delivery, not the publisher ACK path.

## Recommended δ constants (closes D1 v2 §9 residual #2)

*(Post-measurement adversarial review, 2026-07-07, raised the cap 10 → 12 for an honest ≥2× margin
including the +0.2 ms wire RTT and to absorb the single-sample anchor; the measurement is unchanged.)*

- **δ_floor (`kDeltaFloorMs`) = 1.7 ms** — set from the OPEN-LOAD typical p99.9 (~1.1 ms;
  median-across-trials max 1.143 ms) plus margin; flooring at/above the typical tail avoids spurious
  retransmits in steady state. (The steady-pacing 1.68 ms, `w1_findings.md:222`, only *seeds*
  srtt/rttvar at t=0 per must-fix #12 — it is NOT the floor's justification.)
- **δ_cap (`kDeltaCapMs`) = 12 ms** — ≥2× the worst open-load `append_send_to_ack` p99.9 *including*
  the +0.2 ms real-wire NIC RTT: 2×(4.951+0.2) = 10.30 ms, which 12 clears with honest slack. (A bare
  10 ms is only 1.01× its own basis and dips below 2× once wire RTT is added — the reason the review
  bumped it.) 12 ms is also ≈2× the worst `submit_to_ack` p99.9 (2×6.16 = 12.3 ms), but that axis is
  NOT what δ measures — the retransmit timer is armed from `last_send_ts` (send→ack); submit-side
  queueing is bounded by producer backpressure / session_lease, not δ. Satisfies §9 residual-#1
  `kDeltaCapMs (12 ms) < session_lease_ms (1000–2000 ms placeholder) < PBR_fill_time_ms`.
- Estimator: `delta_ms = clamp(srtt + 4·rttvar, 1.7, 12)`, α=1/8, β=1/4, Karn sampling.

**Operative timer is the FLOOR, not the cap.** In the measured regime `srtt+4·rttvar` ≈ 0.3 ms clamps
up to the floor, so δ = floor = 1.7 ms in normal operation and the cap is essentially never reached;
the floor (1.7 ms) sits below the worst-trial p99.9 (4.95 ms), so ≲0.1 % of bad-trial batches see one
spurious first retransmit — idempotent (per-session `batch_seq` dedup), deduped, exponentially
backed-off; ≈5 duplicate 1 KiB sends per stalled batch, bounded bandwidth, never a correctness event.
Karn suppresses the RTT sample of retransmitted (attempt>0) batches, so srtt/rttvar stay biased low
and δ effectively equals the floor by design. Accepted for a backstop behind the D2 NACK primary.

Caveats: (1) the **4.951 ms anchor is a single nearest-rank p99.9 sample** (n≈545/trial → p99.9 = the
per-trial max, `latency_stats.h:46`) at the **anti-monotone 4000 MB/s** point (the worst is *lower* at
the 8000 knee) on **loopback** — three reasons to distrust it as a hard worst-case; raising the cap to
12 absorbs the fragility for a backstop. (2) Real-wire c3 adds ~0.2 ms NIC RTT (0.19 ms fabric RTT),
already folded into the cap. (3) Single-client. **Follow-up validation (D2-phase, non-blocking for D1
freeze):** re-run the 4000-point in isolation, add a `SCENARIO=remote` real-wire point and a
multi-client point, and commit the raw per-point CDF CSVs so the anchor is auditable. If any later run
shows a larger send→ack p99.9, raise `kDeltaCapMs` to 2× that — the constant errs safe when larger (§5.2).
