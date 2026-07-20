# Fig 3b — Per-Session Isolation During Broker Failure

## Claim

Embarcadero provides **session-granularity failure isolation**: when one broker
dies, only the sessions that had in-flight batches to that broker stall. Sessions
targeting surviving brokers continue at full throughput with zero interruption —
no global seal, no global cut, no barrier across sessions.

This directly contrasts with write-before-order systems that require a global
seal (Scalog's cut service) or epoch reconfiguration (Corfu) on any server
failure.

## Experiment design

Four independent ORDER=5 client sessions, each pinned to exactly one broker via
`EMBARCADERO_ORDER5_BROKER_ALLOWLIST`. Broker 1 is killed at t=1.8 s.

| Session | Broker | Expected behavior |
|---------|--------|-------------------|
| 0 | 0 (alive) | Flat throughput throughout |
| 1 | 1 (killed) | Stalls at t=1.8 s; SESSION_FENCED at lease; suffix replay |
| 2 | 2 (alive) | Flat throughput throughout |
| 3 | 3 (alive) | Flat throughput throughout |

The figure plots per-session ACK throughput at 100 ms windows. The surviving
sessions' lines should be flat (zero stall). The failed session shows the
detection + fence + replay timeline.

## Fixed knobs

| Knob | Value | Why |
|------|-------|-----|
| Brokers | 4 | Paper topology |
| Sessions | 4 (one per broker) | Demonstrates per-session isolation |
| Session bytes | 5 GiB each | Long enough for the failure to bite mid-run |
| Message size | 1024 B | Paper default |
| FAILED_BROKER | 1 | Default; override via env |
| Kill at | 1800 ms | Consistent with Fig3 panels (a)/(b) |
| Session lease | 180 s | Long lease; fence fires only if retransmit fails |
| RF | 0 | Isolates ordering from replication in this figure |
| ORDER | 5 | Prefix-safe hold |
| ACK | 1 | Order-visible ACK |

## How to run

```bash
# Default (broker 1 killed, 3 trials, remote clients c4/c3/c1/local)
bash PaperScripts/run_fig3_session_isolation.sh

# Kill a different broker
FAILED_BROKER=2 bash PaperScripts/run_fig3_session_isolation.sh

# Quick smoke (1 trial, local clients)
NUM_TRIALS=1 SCENARIO=local SESSION_BYTES=$((500*1024*1024)) \
  SESSION_HOSTS_PIPE="local|local|local|local" \
  bash PaperScripts/run_fig3_session_isolation.sh

# Replot only (if data already exists)
python3 scripts/publication/plot_session_isolation.py \
  --data-dir data/paper_eval/fig3/fig3_session_isolation/trial_1 \
  --failed-broker 1 --kill-ms 1800 \
  --out Paper/Figures/session_isolation.pdf
```

## Output

- `data/paper_eval/fig3/fig3_session_isolation/trial_N/session_K.csv` — per-session timeseries
- `data/paper_eval/fig3/fig3_session_isolation/trial_N/combined.csv` — merged
- `Paper/Figures/session_isolation.pdf` — the figure

## Interpretation

**If experiment succeeds:**
- Sessions 0, 2, 3: `Session_K_GBps` column flat throughout → zero stall
- Session 1: drops to 0 at kill, stays 0 through detection + lease + replay,
  then resumes → total downtime = detection (≈114 ms) + lease (180 s, configurable)
- `Total_GBps` drops by ≈25% at kill (one of four sessions stalls), recovers

**If experiment fails:**
- All sessions stall simultaneously → indicates either (a) global hold bug, or
  (b) sessions are not correctly pinned to separate brokers (check
  EMBARCADERO_ORDER5_BROKER_ALLOWLIST is forwarded)

## Implementation notes

No C++ changes required. `EMBARCADERO_ORDER5_BROKER_ALLOWLIST` was added to
`publisher.cc` (lines 325–349) and allows restricting which brokers a session
routes to. Setting it to a single broker ID pins the session completely.

The kill mechanism (`--num_brokers_to_kill 1 --failure_percentage 1.0` with
`EMBARCADERO_FAILURE_AFTER_MS=1800`) is inherited from the existing failure
harness. The only change: each session runs as an independent `throughput_test`
process with its own allowlist.

## Paper placement

This experiment supports the claim in §7.2 (Q2):

> "No global seal, no cut service, and no silent reorder occur at any point:
> the hold buffer enforces the contract locally, without coordinating with
> surviving servers."

Currently the paper shows this at the *aggregate* level (Total_GBps trace).
This experiment shows it at the *per-session* level: surviving sessions are
provably unaffected.

Add as a new panel (c) to the failure figure, or as a companion figure.
Caption: "Per-session isolation: four independent ORDER=5 sessions during
broker-1 failure. Sessions targeting surviving brokers (0, 2, 3) see zero
throughput disruption; only the session pinned to the failed broker stalls.
No global seal is required."

## Correctness checklist

1. [ ] `EMBARCADERO_ORDER5_BROKER_ALLOWLIST=K` correctly restricts session K
       to broker K (verify from publisher.cc log output on session start)
2. [ ] All 4 sessions start before the kill fires (verify kill_at_ms > startup_ms)
3. [ ] `Total_GBps` in each session CSV is ACK-based (not sent-based)
4. [ ] combined.csv rows: Total_GBps = sum of all Session_K_GBps at that timestamp
5. [ ] Surviving sessions show non-zero throughput throughout (no global stall)
