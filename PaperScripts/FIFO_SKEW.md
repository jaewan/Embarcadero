# FIFO Skew Experiment — `run_fifo_skew.sh`

## Scientific claim being measured

The paper claims that under WBO (write-broadcast ordering, CXL-Scalog), per-client
FIFO is violated once inter-server skew δ exceeds the sequencer's round period R.
Embarcadero (ORDER=5) never violates FIFO: it either holds delivery (waiting for
the sequencer-assigned position to be durably visible) or fences the session if the
lease expires.

This script produces the dedicated empirical figure validating the **T/R boundary**:
the skew threshold at which Scalog transitions from zero-violation to non-zero
violation rate, while Embarcadero holds at 0% violations across all δ.

## T/R prediction

- **Scalog (ORDER=1, WBO):** The Scalog global sequencer assigns total-order slots in
  discrete rounds of period `R_cut` (typically ~1 ms). When inter-server skew δ > R_cut,
  broker A and broker B each observe different ordering for the same logical round.
  A subscriber that subscribes to both brokers will receive messages out of per-client
  FIFO order. Prediction: **violation_rate > 0 when δ ≳ R_cut**.

- **Embarcadero (ORDER=5):** The epoch-based sequencer assigns per-client monotone slots.
  Before delivering any message, the broker verifies that all preceding slots are
  committed. Delivery is held until the slot frontier advances. No delivery is made out
  of per-client order. Prediction: **violation_rate = 0 for all δ**.

- **Fence events (Embarcadero only):** When the session lease expires (configurable via
  `EMBARCADERO_SESSION_LEASE_MS`), rather than guessing at ordering, the broker sends a
  `SESSION_FENCED` signal to the publisher. This is recorded as `fence_count` in the
  CSV. A high fence rate at large δ is expected and is a feature, not a bug — fencing
  prevents the alternative (stale delivery). The publisher transparently reopens the
  session and continues.

## CSV output

`data/paper_eval/fifo_skew/<CAMPAIGN_ID>/results.csv`:

| Column | Description |
|--------|-------------|
| `delta_ms` | Injected inter-server skew (ms) via tc netem on broker NIC |
| `system` | `EMBARCADERO` or `SCALOG` |
| `trial` | Trial index within the campaign pass |
| `violation_count` | Total out-of-order deliveries across both clients |
| `fence_count` | SESSION_FENCED events observed at publishers |
| `total_messages` | Total messages delivered across both clients |
| `violation_rate_pct` | `violation_count / total_messages × 100` |
| `fence_rate_pct` | `fence_count / total_messages × 100` |
| `status` | `ok` or `fail` |

## How to interpret fence_rate vs violation_rate

- For **Embarcadero**: `violation_rate_pct` should be 0 for all δ. `fence_rate_pct`
  may be non-zero at large δ (δ > session lease / 2 as a rough heuristic). Fences are
  correctness-preserving: they prevent stale delivery, and the publisher recovers.
  Plotting: show fence_rate as a secondary axis or hashed bar to distinguish it from
  the Scalog violation bars.

- For **Scalog**: `fence_rate_pct` is always 0 (Scalog has no session lease mechanism).
  `violation_rate_pct` should transition from ~0 at δ = 0 to a positive (and growing)
  rate as δ crosses R_cut. The exact R_cut depends on the Scalog sequencer's configured
  round interval.

## How to run

```bash
# Full sweep (default: 3 trials × 7 δ points × 2 systems = 42 runs)
bash PaperScripts/run_fifo_skew.sh

# Check readiness without running
bash PaperScripts/run_fifo_skew.sh --preflight

# Quick smoke test
NUM_TRIALS=1 DELTA_POINTS_MS="0 1.0 3.0" bash PaperScripts/run_fifo_skew.sh

# One system only
SYSTEMS="SCALOG" bash PaperScripts/run_fifo_skew.sh

# Custom NIC (override autodetected enp193s0f0np0)
EMBARCADERO_NETEM_IFACE=eth0 bash PaperScripts/run_fifo_skew.sh

# Resume into same campaign (appends to existing CSV)
CAMPAIGN_ID=fifo_skew_tr_boundary NUM_TRIALS=5 bash PaperScripts/run_fifo_skew.sh

# Fast iteration (fewer messages, coarser δ)
MSGS_PER_CLIENT=10000 DELTA_POINTS_MS="0 1 3 5" NUM_TRIALS=1 bash PaperScripts/run_fifo_skew.sh
```

## Expected plot shape

```
violation_rate_pct (%)
 ^
 |                               SCALOG
 |                        .----*
 |                   .---*
 |              .---*
 |         .---*
 |    ----*
 |--*-----------------------------------  EMBARCADERO (always 0)
 +--+----+----+----+----+----+-----> delta_ms
    0   0.5  1.0  1.5  2.0  3.0  5.0
                    ^
                   R_cut (Scalog round period, ~1 ms)
```

- Embarcadero: flat line at 0% violation across all δ.
- Scalog: zero at δ=0, step-up onset at δ ≈ R_cut, then growing rate.
- fence_rate (Embarcadero only): secondary trace, rises at large δ, stays below
  violation curve (fencing prevents violation; fence itself is not an error).

## Dependencies

- `tc` (iproute2) with `sudo` passwordless on the broker host for the netem interface.
- `enp193s0f0np0` (or `EMBARCADERO_NETEM_IFACE`) must exist on the broker.
- `embarlet`, `scalog_global_sequencer`, `throughput_test` built in `build/bin/`.
- Remote clients `c4`, `c3` (or `CLIENT_A`, `CLIENT_B`) must have `throughput_test`
  built in `~/Embarcadero/build/bin/`.
- Python 3 for CSV row-append utility.

## Notes on netem placement

The netem delay is placed on the broker's **egress** NIC queue (`tc qdisc add dev
<NETEM_IFACE> root netem delay ...`). This delays packets sent from the broker to
clients, simulating broker A being slow to acknowledge/export its view of the log
relative to broker B. From the subscriber's perspective, the two brokers' export
streams become skewed by δ ms — recreating the inter-server skew scenario the
T/R theorem describes.

An alternative placement would be ingress `ifb` redirection, but egress netem on the
primary 100G NIC achieves the same observable effect with far simpler setup.
