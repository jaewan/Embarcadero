# Handoff: ORDER=5 ordered-ACK throughput ceiling investigation

> **RESOLVED 2026-07-12 (same day, later session) — see §Resolution at the bottom.**
> The "ACK coupling" framing below is wrong: the 3.4 GB/s cap was broker-side
> ordered ingest present at ALL ack levels (ACK=0's 5.8/11 GB/s were client send
> rates absorbed by kernel socket buffers). Three root causes fixed:
> shmem first-touch fault serialization (zero_mode=metadata), quadratic
> IsOrder5HeldSlot in CommitEpoch, and full-GOI reconnect scans. Post-fix:
> 1-proc ACK=1 5.7 GB/s (client producer-bound), 2-proc same-host 8.2–9.3 GB/s.

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

---

## Resolution (2026-07-12, follow-up session)

### What the ceiling actually was
Broker-side ordered ingest capped at ~3.4 GB/s in EVERY configuration; ACK=1
merely made the client wait for it. The ACK=0 "5.8 / 11 GB/s" cells only measured
client send rate into kernel socket buffers (~130 MB Recv-Q per connection at the
rmem cap; ~2–3 GB total standing) — broker-side ordering was never verified and
kept draining for >1 s after the client exited. Evidence: [ORDER5_COMMIT_PROFILE]
msgs/s ≈ 3.5M in all runs; scanner push == sequencer commit (no internal backlog);
`Send-done` ≫ `Bandwidth` in the client log under ACK=1.

### Root causes (all fixed on main)
1. **First-touch shmem page-fault serialization** (the 3.4 GB/s wall).
   `EMBARCADERO_CXL_ZERO_MODE=metadata` (used by the eval cells for fast startup;
   the script default is `full`) leaves the 46 GB segment/BLog region unfaulted.
   Every 4 KB first-touch faults inside `recv()` into CXL shm and all 24 recv
   threads across the 4 broker processes serialize on the shared shm file's
   mapping spinlock (`shmem_add_to_page_cache`; perf: 68%
   `native_queued_spin_lock_slowpath` under `_copy_to_iter`; /proc stacks +
   per-TID jiffies: 6 recv threads pegged at 100% CPU at 145 MB/s each).
   ~875K faults/s × 4 KB ≈ 3.4 GB/s — invariant to every application knob, which
   is why the seven earlier hypothesis flips were all flat.
   **Fix:** background 8-thread `MADV_POPULATE_WRITE` prefault of the segment
   region in the `CXLManager` ctor (head broker, metadata mode only; 46 GB in
   ~12 s, done before traffic; kill switch `EMBARCADERO_CXL_BG_PREFAULT=0`).
   Note madvise requires page-aligned addresses; region offsets are only
   cacheline-aligned.
2. **Quadratic CommitEpoch under load.** `IsOrder5HeldSlot` walked every held
   batch (all shards, under shard mutexes) once per batch-list entry —
   O(batches × total_held) per epoch. Under 2-proc saturation the hold buffer
   grows as the sequencer slows: 24 GiB 2-proc runs collapsed to 1.95 GB/s and
   ACK stalls exceeded the client retransmit backstop.
   **Fix:** `CollectOrder5HeldSlotIdentities` — one sweep into a flat_hash_set
   per `AdvanceConsumedThroughForProcessedSlots` call, O(1) membership.
3. **Full-GOI scan per reconnect.** ACK stalls → client `RetransmitThread`
   resends each due batch via `SendRawBatchToBroker`, which opens a NEW
   connection per batch; each connection's session-open path ran
   `ScanGOICommittedHwm` FORWARD over the entire committed GOI with a
   serializing clflush per 128 B entry (seconds per call; ~580 SESSION_OPENs per
   broker in the collapsed run → recv-pool starvation feedback loop).
   **Fix:** scan BACKWARD with early exit (sound because the W1.2 invariant
   keeps per-session client_seq strictly increasing in GOI order).

### Post-fix numbers (1 trial each, standard cell, zero_mode=metadata)
| Cell | Before | After |
|---|---|---|
| 1-proc ACK=1 8 GiB | 3.48 | **5.68 GB/s** (Send-done ≈ Bandwidth → client producer-bound) |
| 2-proc ACK=1 24 GiB | 1.95 (collapse) | **8.47 GB/s** |
| 2-proc ACK=1 32 GiB | — | **9.32 GB/s** |

### Remaining work
- 2-proc residual: EpochSequencerThread ~95% busy; goi/export/metadata/cv timers
  only account for ~45 ms/s of ~950 ms/s inside CommitEpoch. Suspects: the
  held-slot sweep itself (hold buffer size × calls/s), spatial-guard
  `FindSessionEntry` CXL reads per ready batch, `ensure_cv_tracker` flushes.
  perf attribution kept failing (perf.data truncated when brokers restart);
  capture with a longer run or record to a pipe.
- The client single-threaded producer loop (~5.8 GB/s) is now the 1-proc bound.
- E4a smoke gate result: see `order5_e4a_gate` note in the session log / commit.
- Measurement hygiene for all future cells: enable `EMBAR_ORDER5_COMMIT_PROFILE=1`
  and compare broker commit msgs/s with client Send-done vs Bandwidth.
