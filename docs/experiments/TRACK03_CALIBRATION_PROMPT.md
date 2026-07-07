# PROMPT — feed to moscxl claude-code (Track-03 baseline calibration, with commit)

```
You are a coding agent on moscxl. Calibrate the Track-03 CXL-mailbox baseline ports (Corfu/Scalog/
LazyLog) against their published floors, then COMMIT the bench edits + results. Testbed is idle.

WORKING DIR: ~/Embarcadero-sessions/s2-work  (git repo; branch `s2` @ 9901b93 = integrated base + S2,
has all Track-03 port code + benches). First:  git switch -c track03-calib s2
Do ALL work here. NEVER touch ~/Embarcadero (live tree) or other ~/Embarcadero-sessions/* dirs.

READ FIRST (authoritative): docs/experiments/TRACK03_CALIBRATION_BRIEF.md  and
docs/baselines/calibration_plan.md — targets, the honesty rule (directional floors; report sub-floor
honestly), and which numbers are out-of-scope.

BUILD (outside the flock lock):
  cd ~/Embarcadero-sessions/s2-work
  cmake -S . -B build -DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero-main-ab/build/_deps/grpc-src
  cmake --build build -j 64 --target corfu_mailbox_bench scalog_mailbox_bench lazylog_mailbox_bench

SCALE (edit the benches — they use compile-time constants + ignore argv):
  - src/cxl_manager/corfu_mailbox_bench.cc: run TWO configs — kMessagesPerRequest=1 (no-batch) and =4
    (batched); scale the per-client request count so the 30s deadline loop runs at a seconds-long
    plateau, not ~40ms of warmup.
  - src/cxl_manager/{scalog,lazylog}_mailbox_bench.cc: raise kEpochs (2000 → 10–50x) so the run is
    seconds long; report the plateau rate.
  Recompile after each edit (outside the lock).

RUN (take flock; log START/END to ~/Embarcadero-sessions/activity.log; kill strays by explicit PID —
NEVER `pkill -f throughput_test/embarlet`): each config >=3 trials (5 if you make a latency claim);
report median +/- range. Priority order: Corfu (the one directly-comparable number) -> Scalog ->
LazyLog.

TARGETS (meet-or-exceed; report sub-floor honestly as a mailbox-round-trip finding):
  Corfu >=570K tokens/s no-batch, >2M at batch=4 | Scalog >=18.7K writes/s/shard @4KB
  LazyLog >=1.34M metadata-appends/s + 1-RTT append (structural, exact — mailbox removes the gRPC
  coordinator round-trip). Latency percentiles are best-effort; throughput floors are required.

COMMIT (branch track03-calib; --no-verify --no-gpg-sign):
  - the bench N/batch edits, and
  - a calibration results section in docs/baselines/fairness_appendix.md (or new
    docs/baselines/calibration_results.md): per baseline target / measured(median+/-range, N, trials)
    / tolerance / verdict {meets-floor | below-floor-honest-finding | out-of-scope}. Note the scaled N
    and that it is a plateau number.
  Trailer: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>

RECONCILE:  git bundle create /tmp/track03_calib.bundle s2..track03-calib
REPORT: the results table, verdicts, commit SHA, /tmp/track03_calib.bundle. Testbed left clean. No push.
```
