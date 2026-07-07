# PROMPT — feed to moscxl claude-code (δ measurement, with commit)

```
You are a coding agent on moscxl. Measure the open-load append->ack tail that sets D1's adaptive-δ,
then COMMIT the results. ssh access is fine; the testbed is idle.

WORKING DIR: ~/Embarcadero-sessions/s2-work  (git repo; branch `s2` @ 9901b93 = integrated base + S2).
Create your work branch first:  git switch -c delta-measure s2
Do ALL work here. NEVER touch ~/Embarcadero (live tree) or other ~/Embarcadero-sessions/* dirs.

READ FIRST: docs/experiments/DELTA_MEASUREMENT_BRIEF.md — it is the authoritative method (why
append_send_to_ack not e2e; open-loop not steady; the sweep; the extraction; δ floor/cap derivation).

BUILD (outside the flock lock):
  cd ~/Embarcadero-sessions/s2-work
  cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero-main-ab/build/_deps/grpc-src -DCOLLECT_LATENCY_STATS=ON
  cmake --build build -j 64
(COLLECT_LATENCY_STATS is REQUIRED — it emits the stage CSVs.)

MEASURE (take the flock lock; log START/END to ~/Embarcadero-sessions/activity.log; compile OUTSIDE
the lock; kill strays by explicit PID — NEVER `pkill -f throughput_test/embarlet`, and keep those
strings out of any long-lived shell argv or the scripts' own pkill self-kills you):
  Primary — loopback OPEN-LOAD sweep (open_loop, NOT steady):
    ORDERS=5 ACK_LEVEL=1 NUM_BROKERS=4 MSG_SIZE=1024 SCENARIO=local NUM_TRIALS=3 \
    PACING_MODE=open_loop LOAD_POINTS_MBPS="1000 2000 4000 6000 8000" \
    TOTAL_MESSAGE_SIZE=4294967296 SEQUENCER=EMBARCADERO \
    bash scripts/run_latency_vs_load.sh
  (If run_latency_vs_load.sh is inconvenient: scripts/run_latency.sh with ORDERS=5 MODES=burst
   TARGET_MBPS=<each>. MODES=burst = open-loop; MODES=steady is the one to AVOID.)
  Report where achieved_offered stops tracking target (the knee) — δ is set from the p99.9 at/just
  below the intended operating load, not the overloaded tail.

EXTRACT (per load point, from pub_latency_stats.csv, row Metric==append_send_to_ack_batch_latency):
  p50, p99, p99.9 (µs). Also grab append_submit_to_ack_batch_latency p99.9 for context.

COMMIT (on branch delta-measure; --no-verify --no-gpg-sign; the hook is interactive/signing hangs):
  1. NEW: docs/experiments/delta_measurement_results.md — a table
     [offered target vs achieved] × [append_send_to_ack p50/p99/p99.9, submit_to_ack p99.9] + the
     identified knee + the recommendation: δ_floor (≈ steady p99.9 seed, ~1.7 ms) and
     δ_cap (>= 2× the open-load append_send_to_ack p99.9 at the operating load).
  2. EDIT docs/design/D1_SESSION_FIFO_DESIGN_v2.md: in §5.2 replace the δ_floor/δ_cap PLACEHOLDER with
     the measured values (cite the results doc), and in §9 mark residual #2 (open-load δ gate) CLOSED
     with the measured p99.9. Keep edits minimal + surgical.
  End commit message with:
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>

RECONCILE (so Track 01 can pull it into canonical):
  git bundle create /tmp/delta.bundle s2..delta-measure
  Report the bundle path + the commit SHA.

REPORT BACK: the p99.9 table, the knee, the recommended δ_floor/δ_cap, the commit SHA, and
/tmp/delta.bundle. Testbed left clean (lock free, no strays). Do NOT push.
```
