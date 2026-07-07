# δ (retransmit constant) measurement brief — for the moscxl agent

**Goal:** measure the **open-load `append_send_to_ack` p99.9** for ORDER=5/ACK=1, which sets D1's
adaptive-δ floor/cap (D1 v2 §5.2, §9.2). This is the one remaining W1 measurement gate (W1.1 measured
it under **steady** pacing — `w1_findings.md:224` — which is NOT valid for δ; δ must be derived from
**open-loop / offered-load** tail behavior). δ is a retransmit **backstop** (D2 NACK is primary), so we
want the worst-case ack RTT a publisher sees under real offered load, then set δ_cap ≥ 2× it.

**Why append→ack, not e2e:** δ governs the **publisher's** retransmit timer = the send→ACK round trip.
Use `append_send_to_ack_batch_latency` (publisher-side), NOT `publish_to_deliver` (subscriber-side, the
733 ms tail that is irrelevant to retransmit).

## Setup
- **Tree:** `~/Embarcadero-sessions/s2-work` (branch `s2` @ 9901b93 = integrated base + S2; builds).
- **Build (outside lock):** `cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero-main-ab/build/_deps/grpc-src -DCOLLECT_LATENCY_STATS=ON && cmake --build build -j64`
  (COLLECT_LATENCY_STATS is REQUIRED — it emits the stage CSVs).

## Run (under flock; log START/END to activity.log; kill strays by PID)
**Primary — loopback open-load sweep** (simplest, definitely works; append→ack is largely broker-side
so loopback is a valid δ proxy):
```
ORDERS=5 ACK_LEVEL=1 NUM_BROKERS=4 MSG_SIZE=1024 SCENARIO=local NUM_TRIALS=3 \
PACING_MODE=open_loop LOAD_POINTS_MBPS="1000 2000 4000 6000 8000" \
TOTAL_MESSAGE_SIZE=4294967296 SEQUENCER=EMBARCADERO \
bash scripts/run_latency_vs_load.sh
```
(If `run_latency_vs_load.sh` isn't convenient, equivalent: `scripts/run_latency.sh` with `ORDERS=5
MODES=burst TARGET_MBPS=<each>` — `MODES=burst` = open-loop, `MODES=steady` is the one to AVOID.)

**Sweep intent:** span below-knee → saturation. If 8000 saturates (achieved_offered << target), that
point IS the knee; report where achieved_offered stops tracking target. δ should be set from the p99.9
at/just-below the intended operating load, not the overloaded tail.

## Extract (per load point, from `pub_latency_stats.csv`)
The row `Metric == append_send_to_ack_batch_latency` — report **p50, p99, p99.9** (µs). Also grab
`append_submit_to_ack_batch_latency` p99.9 (includes client-side submit queueing) for context.

## Deliverable
A table: offered-load (target vs achieved) × {append_send_to_ack p50/p99/p99.9, submit_to_ack p99.9}.
Then a one-line recommendation: **δ_floor** (≈ steady p99.9, ~1.7 ms seed) and **δ_cap** (≥ 2× the
open-load `append_send_to_ack` p99.9 at the operating load). This closes D1 v2 §9.2 and unblocks
commit G's DeltaEstimator constants.

## Optional refinement (only if quick)
Repeat one load point **real-wire** from **c3** (100G, `EMBARCADERO_HEAD_ADDR=10.10.10.10`,
`CLIENT_HOSTS_CSV=c3`, client at `~/Embarcadero-sessions/01-core-protocol/build/bin` with
`CLIENT_LD_LIBRARY_PATH` set to that bin dir) — real NIC RTT makes it the most representative δ number.
If the remote-latency path is fiddly, skip; loopback open-load satisfies the §9.2 gate.

## Discipline
Compile outside the lock; hold the lock only for runs. **Never `pkill -f throughput_test/embarlet`**
(self-kills your shell / matches argv). Verify lock free + no strays after. Do NOT commit (measurement
only) — report the table.
```
