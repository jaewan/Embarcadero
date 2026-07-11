# Handoff: ORDER=5 ordered-ACK throughput ceiling investigation

You are a systems engineering agent working on **Embarcadero** (distributed
totally-ordered pub/sub log over CXL memory, OSDI/SOSP'27 resubmission) at
`~/Embarcadero` (git, branch `main`). Your mission: **find and fix why a
single client's ACK-confirmed ORDER=5 throughput caps at ~3.4 GB/s** while
the same pipeline ingests 11 GB/s without ACK confirmation.

## Rules
- Never `pkill -f`; kill only by explicit PID. Never touch trees you didn't create.
- Commit: `--no-verify --no-gpg-sign`, trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.
- Any change to session/ACK machinery MUST pass the E4 regression gate:
  `CLIENT_LD_LIBRARY_PATH=~/Embarcadero/lib EMBARCADERO_NETWORK_IO_THREADS=64 SMOKE=1 SUITE=E4a bash scripts/run_failure_suite.sh`
  → expect per-session stall P50 ≤ 5 ms, max ≤ 10 ms, 0 unrecovered.
- One run at a time on the cluster (`flock -n /tmp/embarcadero_run_multiclient.lock echo FREE`).

## Read first (ramp-up order)
1. `~/.claude/.../memory` recall gives you `throughput-ceiling-investigation.md`
   and `order5-session-storm-n3.md` — the full investigation ledger. Trust them.
2. `docs/handoff/MOSCXL_AGENT_HANDOFF_2026_07_11.md` — cluster topology (c4/c3/c1
   clients, moscxl broker; passwordless sudo everywhere; c4 = primary, 10.10.10.12).
3. Code, in this order:
   - `src/embarlet/topic.cc` — `BrokerScannerWorker5` (~line 6781), `EpochSequencerThread`,
     `FlushAccumulatedCV`, `UpdatePerClientOrdered` (~1755), `AckRelayEpochValid` (~1807)
   - `src/network_manager/network_manager.cc` — AckThread relay loop (~2700–2960:
     fast-poll/full-check of the per-client frontier, ack send)
   - `src/client/publisher.cc` — `RecordUnackedBatch`/`CompleteUnackedThrough`
     (~552–630), `EpollAckThread`, send loop `send_batch_header` (~2860–3080)
   - `src/common/configuration.h` — ConfigValue (now memoized, commit `eda896f5`)
   - `src/client/test_utils.cc` — `PublishThroughputTest` (line ~1257) is what
     TEST_TYPE=5 runs (bare Publish loop, NO throttle); the 100 MB in-flight
     window lives only in `FailurePublishThroughputTest` (test type 4).

## Established facts (all measured 2026-07-12; do not re-derive)
| Config (c4 → moscxl, ORDER=5, epoch=100µs) | GB/s |
|---|---|
| 1 process, ACK=1 | **3.4** ← the mystery |
| 1 process, ACK=0 | 5.8 (single-threaded producer loop is the per-process bound; threads 6→12 no change) |
| 2 processes same host, ACK=1 | 3.4 total (1.7 each — secondary mystery) |
| 2 processes same host, ACK=0 | 11.0 (brokers+ordering ingest near line rate) |
| Raw TCP reference (6-proc iperf3) | 12.25 (NEVER use single-process iperf3 — it self-caps ~2.75) |

**Eliminated as the ACK=1 limiter** (single-variable flips, all flat at ~3.4):
client in-flight window (100 MB–2 GB), publisher threads (3–12), epoch (100/500 µs),
broker ReqReceive pool (6/32/64), kernel socket buffers (208 KB/256 MB),
load size (1–8 GiB), getenv-in-hot-loop waste (fixed in `eda896f5`; perf-verified
gone; ceiling unchanged — scanner cycles were wait-spin).

**Key interpretation**: under ACK=1 every measurement (timeseries + naive) reports
the *ordered-ACK arrival rate* — the per-client frontier advances at ~3.4 GB/s,
i.e. a standing ~29 ms lag exists somewhere in
recv → PBR publish → scanner pickup → epoch admission → GOI commit → ACK relay → client.
The head broker is NOT CPU-bound (post-fix profile: scanner 61% wait-spin).

## Your plan (suggested)
1. **Stage-timestamp instrumentation**: add nanosecond timestamps for one traced
   batch per N (sampled, not per-batch) across the pipeline stages above; log
   deltas broker-side. Cheap, env-gated (e.g. `EMBARCADERO_ORDER5_STAGE_TRACE=1`).
2. Run the standard verification cell (below), read the stage deltas, find the
   ~29 ms residence stage.
3. Candidate suspects to check while instrumenting:
   - AckThread's fast-poll/full-check cadence and its CXL invalidate+fence pattern
     (`network_manager.cc` ~2840): how often does the relay actually *send*?
   - `UpdatePerClientOrdered` cadence: who calls it, per what granularity?
   - Client `EpollAckThread` consumption rate (it runs ~100% CPU — what is it doing
     per ack message?).
   - GOI commit→relay visibility (CXL flush/poll interval).
4. Fix, rebuild (`make -C build -j48 embarlet` broker; client:
   `numactl --cpunodebind=0 --membind=0 make -C build-portable -j24 throughput_test`,
   then scp `build-portable/bin/throughput_test` to c4/c3/c1 `~/Embarcadero/build/bin/`
   and smoke: `./throughput_test --definitely_bad_option` must print a cxxopts error).
5. Verify: cell below ≥ 5 GB/s → then 2-proc same-host ACK=1 ≥ 10 → then E4 gate.

## Standard verification cell (1-proc ACK=1)
```bash
env EMBAR_ORDER5_EPOCH_US=100 THREADS_PER_BROKER=6 EMBARCADERO_NETWORK_IO_THREADS=64 \
  NUM_CLIENTS=1 CLIENT_HOSTS_CSV=c4 CLIENT_NUMAS_CSV=1 NUM_BROKERS=4 \
  NUM_TRIALS=3 WARMUP_TRIALS=1 TOTAL_MESSAGE_SIZE=$((8*1024**3)) MESSAGE_SIZE=1024 \
  SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=0 TEST_TYPE=5 \
  EMBARCADERO_RUNTIME_MODE=throughput EMBARCADERO_CXL_ZERO_MODE=metadata \
  EMBARCADERO_CXL_MAP_POPULATE=0 EMBARCADERO_HEAD_ADDR=10.10.10.10 \
  CLIENT_LD_LIBRARY_PATH="$HOME/Embarcadero/lib" \
  bash scripts/run_multiclient.sh
```
ACK=0 variant: `ACK=0` (expect ~5.8). 2-proc: `NUM_CLIENTS=2 CLIENT_HOSTS_CSV=c4,c4 CLIENT_NUMAS_CSV=1,1 TOTAL_MESSAGE_SIZE=$((16*1024**3))`.

## Profiling notes
- perf WORKS on moscxl (`perf record -F 499 -g -p $(pgrep -x embarlet | head -1)`),
  BROKEN on c4 (kernel-tools mismatch, no internet). For client-side, use
  `/proc/PID/task/*/stat` jiffies sampling over ssh.
- Clients pinned NUMA per host: c4→1, c3→1, c1→0. c1 needs no special handling
  now (libs fixed system-wide); c3 requires `CLIENT_LD_LIBRARY_PATH` (transitive
  gflags — DT_RUNPATH is not transitive).

## Session-fence/storm context (why some machinery exists)
Session-open storms and reroute spins were fixed 2026-07-11/12
(commits `8d998271`, pool sizing; see memory `order5-session-storm-n3.md`).
The unacked-batch holding in the client exists for session-fence suffix
retransmit — do not remove it; make it cheaper only if it proves hot.

## After the fix lands (stretch goals, in order)
1. N-node scaling ladder for the paper: N_hosts={1,2,3} × 2 proc/host, ACK=1,
   `EMBARCADERO_ORDER5_HOME_BROKERS=4`, NUM_TRIALS=5 — the paper's headline figure
   (expect ~port line rate 12.3 GB/s at N≥2).
2. Verify ordering frontier kept pace at max ingest (broker ordered counters vs
   published bytes) — required for the "ordered at line rate" claim.
3. Check whether the memoization also lifted the subscriber delivery ceiling
   (~270 MB/s, receive-worker CPU-bound — same ConfigValue pattern may apply):
   rerun `scripts/run_delivery_scoping.sh`.
