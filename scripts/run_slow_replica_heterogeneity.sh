#!/bin/bash
set -euo pipefail

# ---------------------------------------------------------------------------
# run_slow_replica_heterogeneity.sh
#
# Scientific claim tested (Appendix Table):
#   Embarcadero's ordering ACK is independent of replica-sync completion.
#
# Method: Run paired baseline and slow-sync trials.  The slow trial injects a
# sleep into every disk-durable replica sync thread before fdatasync.  Extract
# ACK1 (ordering) and ACK2 (durable) P99 latencies and compare three pairs.
#
# For Embarcadero, ACK1 comes from the sequencer's GOI commit and should not
# inherit the injected delay.  ACK2 waits for completion-vector advancement
# from the disk-durable sync path and therefore should inherit the delay.
#
# IMPORTANT — ACK level note:
#   publisher.cc sets have_ordered_metric = (ack_level_ == 1).
#   When ACK=2, append_send_to_ordered is NOT written to stage_latency_summary.csv.
#   Strategy: run two sub-trials — ACK=1 (captures ordered metric) and ACK=2
#   (captures durable-ack metric) — then merge into a single row.
#
# Usage:
#   bash scripts/run_slow_replica_heterogeneity.sh               # EMBARCADERO
#   SEQUENCER=SCALOG bash scripts/run_slow_replica_heterogeneity.sh
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT/build/bin"

NUM_BROKERS=${NUM_BROKERS:-4}
SEQUENCER=${SEQUENCER:-EMBARCADERO}
if [[ -z "${ORDER:-}" ]]; then
  case "$SEQUENCER" in
    EMBARCADERO) ORDER=5 ;;
    SCALOG) ORDER=1 ;;
    LAZYLOG) ORDER=2 ;;
    CORFU) ORDER=2 ;;
    *) echo "Unknown SEQUENCER=$SEQUENCER" >&2; exit 1 ;;
  esac
fi
REPLICATION_FACTOR=${REPLICATION_FACTOR:-2}
ACK=${ACK:-2}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-1073741824}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-4}
TEST_TYPE=${TEST_TYPE:-2}
TARGET_MBPS=${TARGET_MBPS:-0}
RUN_ACK2=${RUN_ACK2:-1}
CONFIG=${CONFIG:-config/embarcadero.yaml}
CLIENT_CONFIG=${CLIENT_CONFIG:-config/client.yaml}
# Use broker 1 (first follower in the replication chain) as the slow replica.
# Broker 0 is the head/sequencer; stopping broker 1 specifically tests whether
# a follower slowdown propagates back to the sequencer (Scalog/LazyLog) or not
# (Embarcadero).
SLOW_BROKER_INDEX=${SLOW_BROKER_INDEX:-1}
SKIP_SIGSTOP=${SKIP_SIGSTOP:-1}  # Default: use sync-sleep only; SIGSTOP confounds the sync-thread experiment.
NUM_TRIALS=${NUM_TRIALS:-3}  # Independent baseline/slow pairs for P99 dispersion
INJECT_AFTER_SEC=${INJECT_AFTER_SEC:-20}
PAUSE_SEC=${PAUSE_SEC:-10}
POINT_MAX_ATTEMPTS=${POINT_MAX_ATTEMPTS:-3}
CLUSTER_SETTLE_SEC=${CLUSTER_SETTLE_SEC:-15}
POST_START_SETTLE_SEC=${POST_START_SETTLE_SEC:-30}
TIMEOUT_SEC=180
RUN_TIMEOUT_SEC=${RUN_TIMEOUT_SEC:-$((TIMEOUT_SEC + 60))}
OUTDIR=${OUTDIR:-data/latency/slow_replica}
if [[ "$OUTDIR" != /* ]]; then
  OUTDIR="$PROJECT_ROOT/$OUTDIR"
fi
mkdir -p "$OUTDIR"
# Clear any stale /dev/shm CXL segments from prior runs
rm -f /dev/shm/CXL_* /dev/shm/cxl_* 2>/dev/null || true

# Resolve BROKER_IP for SCALOG local-sequencer mode (SKIP_REMOTE_SCALOG_SEQUENCER=1)
BROKER_IP=${BROKER_IP:-$(hostname -I | awk '{print $1}')}

# Replication sink must be disk-durable: the injection hook is in the fdatasync
# path and is intentionally bypassed by the memory-copy sink.
export EMBARCADERO_CHAIN_REPLICATION_SINK="${EMBARCADERO_CHAIN_REPLICATION_SINK:-disk-durable}"
unset EMBARCADERO_CHAIN_REPLICATION_INMEM 2>/dev/null || true
unset EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY 2>/dev/null || true
# 72 GiB admits four 8-GiB segments after layout-v4 metadata (64 GiB admits
# only three) without prefaulting an unnecessary extra 56 GiB per sub-trial.
# Set replica disk dirs for disk-durable sink.
export EMBARCADERO_REPLICA_DISK_DIRS="/home/domin/Embarcadero/.Replication/disk0,/home/domin/Embarcadero/.Replication/disk1"
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-77309411328}"
if [[ "$SEQUENCER" == "SCALOG" ]]; then
  # Required by both brokers and the ACK2 client: only the CXL polling path
  # publishes Scalog's per-replica media-durable frontier.
  export SCALOG_CXL_MODE=1
fi
# Every cluster start below uses a newly-created shm object and cleanup removes
# the previous one, so untouched PBR pages are kernel-zeroed. Metadata mode is
# sufficient; a full 128-GiB clear only adds ~45 s per sub-trial.
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"

EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-numactl --cpunodebind=0 --membind=0}"
SEQUENCER_PID=""

cleanup() {
  kill_inject_bg
  pkill -9 -f "./embarlet" >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  pkill -9 -f "scalog_global_sequencer" >/dev/null 2>&1 || true
  pkill -9 -f "corfu_global_sequencer" >/dev/null 2>&1 || true
  pkill -9 -f "lazylog_global_sequencer" >/dev/null 2>&1 || true
  rm -f /tmp/embarlet_*_ready >/dev/null 2>&1 || true
  # Clear ALL Embarcadero CXL shared memory segments from /dev/shm.
  # Stale segments from crashed or killed runs accumulate and cause
  # EEXIST errors or wrong-experiment state on the next cluster start.
  rm -f /dev/shm/CXL_* /dev/shm/cxl_* 2>/dev/null || true
  SEQUENCER_PID=""
}

# Also clear stale /dev/shm at script startup before any cluster starts.
cleanup_shm_stale() {
  local before_count
  before_count=$(ls /dev/shm/CXL_* /dev/shm/cxl_* 2>/dev/null | wc -l)
  if [ "$before_count" -gt 0 ]; then
    echo "[shm-clean] removing $before_count stale CXL shm segment(s) from /dev/shm" >&2
    rm -f /dev/shm/CXL_* /dev/shm/cxl_* 2>/dev/null || true
  fi
}

wait_for_brokers() {
  local timeout_s=$1
  local expected=$2
  local start_ts
  start_ts=$(date +%s)
  while true; do
    local ready_files=(/tmp/embarlet_*_ready)
    local count=0
    if [ -e "${ready_files[0]}" ]; then
      count=${#ready_files[@]}
    fi
    if [ "$count" -ge "$expected" ]; then
      return 0
    fi
    if [ $(( $(date +%s) - start_ts )) -ge "$timeout_s" ]; then
      return 1
    fi
    sleep 0.1
  done
}

start_cluster() {
  # Pass RF to embarlet so it initialises the replication chain.
  # Without this, EMBARCADERO_REPLICATION_FACTOR=0 in the broker and ACK=2
  # clients wait forever for a replica that never starts.
  # Fresh SHM per cluster start to avoid topic count accumulating across attempts.
  export EMBARCADERO_CXL_SHM_NAME="/CXL_SLOWREP_${UID}_$$_$(date +%s)"
  # Clean up any previous SHM from this driver instance.
  rm -f /dev/shm/CXL_SLOWREP_${UID}_* 2>/dev/null || true
  export EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"
  local pids=()
  if [[ "$SEQUENCER" == "CORFU" ]]; then
    ./corfu_global_sequencer >/tmp/hetero_corfu_sequencer.log 2>&1 &
    SEQUENCER_PID="$!"
    sleep 1
  elif [[ "$SEQUENCER" == "SCALOG" ]]; then
    SKIP_REMOTE_SCALOG_SEQUENCER=1 EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP" \
      ./scalog_global_sequencer >/tmp/hetero_scalog_sequencer.log 2>&1 &
    SEQUENCER_PID="$!"
    sleep 1
  elif [[ "$SEQUENCER" == "LAZYLOG" ]]; then
    # LazyLog global sequencer runs locally; SKIP_REMOTE_LAZYLOG_SEQUENCER=1
    # tells embarlet not to SSH out for it.
    SKIP_REMOTE_LAZYLOG_SEQUENCER=1 EMBARCADERO_LAZYLOG_SEQ_IP="$BROKER_IP" EMBARCADERO_NUM_BROKERS="$NUM_BROKERS" \
      ./lazylog_global_sequencer >/tmp/hetero_lazylog_sequencer.log 2>&1 &
    SEQUENCER_PID="$!"
    sleep 1
  fi

  if [[ "$SEQUENCER" == "SCALOG" ]]; then
    # Pass env vars so embarlet's local sequencer connects to the local global seq
    SKIP_REMOTE_SCALOG_SEQUENCER=1 EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP" \
      $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --head --$SEQUENCER \
      --replicate_to_disk \
      >/tmp/hetero_broker_0.log 2>&1 &
    pids+=("$!")
    for ((i=1; i<NUM_BROKERS; i++)); do
      SKIP_REMOTE_SCALOG_SEQUENCER=1 EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP" \
        $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --$SEQUENCER \
        --replicate_to_disk \
        >/tmp/hetero_broker_${i}.log 2>&1 &
      pids+=("$!")
    done
  elif [[ "$SEQUENCER" == "LAZYLOG" ]]; then
    # LazyLog: SIGSTOP injection to stall replication_done.
    # LAZYLOG_CXL_MODE=1: CXL-based replica polling writes replication_done
    # only after the replica-side fdatasync completes (disk-durable sink).
    # With disk-durable: SIGSTOP on broker 1 freezes its fdatasync, which
    # freezes its replication_done update, which blocks broker 0's
    # SendLocalProgress (min(replication_done) stays stuck), stalling ordering.
    # disk-durable sink creates the ~50-200ms fdatasync window that SIGSTOP hits.
    LAZYLOG_CXL_MODE=1 \
    EMBARCADERO_CHAIN_REPLICATION_SINK=disk-durable \
    EMBARCADERO_REPLICA_DISK_DIRS="$EMBARCADERO_REPLICA_DISK_DIRS" \
    SKIP_REMOTE_LAZYLOG_SEQUENCER=1 EMBARCADERO_LAZYLOG_SEQ_IP="$BROKER_IP" EMBARCADERO_NUM_BROKERS="$NUM_BROKERS" \
      $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --head --LAZYLOG --replicate_to_disk \
      >/tmp/hetero_broker_0.log 2>&1 &
    pids+=("$!")
    for ((i=1; i<NUM_BROKERS; i++)); do
      LAZYLOG_CXL_MODE=1 \
      EMBARCADERO_CHAIN_REPLICATION_SINK=disk-durable \
      EMBARCADERO_REPLICA_DISK_DIRS="$EMBARCADERO_REPLICA_DISK_DIRS" \
      SKIP_REMOTE_LAZYLOG_SEQUENCER=1 EMBARCADERO_LAZYLOG_SEQ_IP="$BROKER_IP" EMBARCADERO_NUM_BROKERS="$NUM_BROKERS" \
        $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --LAZYLOG --replicate_to_disk \
        >/tmp/hetero_broker_${i}.log 2>&1 &
      pids+=("$!")
    done
  else
    $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --head --$SEQUENCER \
      >/tmp/hetero_broker_0.log 2>&1 &
    pids+=("$!")
    for ((i=1; i<NUM_BROKERS; i++)); do
      $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --$SEQUENCER \
        >/tmp/hetero_broker_${i}.log 2>&1 &
      pids+=("$!")
    done
  fi

  wait_for_brokers 120 "$NUM_BROKERS"
  # All broker readyfiles written. Poll the single head management port (12140)
  # until it accepts connections, then sleep 15s to let followers fully init.
  local _ts_pp _pt=120
  _ts_pp=$(date +%s)
  while ! nc -z 127.0.0.1 12140 2>/dev/null; do
    [ $(( $(date +%s) - _ts_pp )) -ge $_pt ] && { echo "[WARN] head mgmt port 12140 not up after ${_pt}s" >&2; break; }
    sleep 0.5
  done
  # Extra settle: followers need time after readyfile to be fully ready
  # (LazyLog binding connection, CXL segment prefault, replica init).
  sleep "$CLUSTER_SETTLE_SEC"
  rm -f /tmp/embarlet_*_ready
  echo "${pids[*]}"
}

INJECT_BG_PID=""

inject_slowdown() {
  local pid=$1
  local delay=${2:-$INJECT_AFTER_SEC}  # optional per-call override
  # SKIP_SIGSTOP=1: rely only on EMBARCADERO_SYNC_SLEEP_MS, no SIGSTOP.
  if [ "${SKIP_SIGSTOP:-0}" = "1" ]; then return 0; fi
  (
    sleep "$delay"
    kill -STOP "$pid" >/dev/null 2>&1 || true
    sleep "$PAUSE_SEC"
    kill -CONT "$pid" >/dev/null 2>&1 || true
  ) &
  INJECT_BG_PID=$!
}

# Per-sequencer injection delay (seconds after test start).
# EMBARCADERO: 20s (sync-sleep runs throughout 180s test; early is fine).
# LAZYLOG: 3s  (test runs ~10-60s depending on data; must fire EARLY to
#               catch messages mid-binding, not after all messages are bound).
# SCALOG:  20s (same as EMBARCADERO default; test runs long enough).
lazylog_inject_delay() {
  if [[ "$SEQUENCER" == "LAZYLOG" ]]; then
    echo "${LAZYLOG_INJECT_AFTER_SEC:-3}"
  else
    echo "$INJECT_AFTER_SEC"
  fi
}

kill_inject_bg() {
  if [ -n "$INJECT_BG_PID" ] && kill -0 "$INJECT_BG_PID" 2>/dev/null; then
    kill "$INJECT_BG_PID" 2>/dev/null || true
    wait "$INJECT_BG_PID" 2>/dev/null || true
  fi
  INJECT_BG_PID=""
}

terminate_pid_bounded() {
  local pid="$1"
  [[ -z "$pid" ]] && return 0
  if ! kill -0 "$pid" 2>/dev/null; then
    wait "$pid" 2>/dev/null || true
    return 0
  fi
  kill -TERM "$pid" 2>/dev/null || true
  for _ in $(seq 1 50); do
    if ! kill -0 "$pid" 2>/dev/null; then
      wait "$pid" 2>/dev/null || true
      return 0
    fi
    sleep 0.1
  done
  kill -KILL "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

# Extract a field from stage_latency_summary.csv.
# CSV header: Stage,Average,Min,p50,p99,p999,Max,Count  (cols 1-8)
# col=5 => p99
extract_metric() {
  local file="$1"
  local metric="$2"
  local col="$3"
  if [ ! -f "$file" ]; then
    echo "NA"
    return
  fi
  local v
  v=$(awk -F',' -v m="$metric" -v c="$col" 'NR>1 && $1==m {print $c; exit}' "$file")
  if [ -z "$v" ]; then
    echo "NA"
  else
    echo "$v"
  fi
}

# run_mode <label> <inject_slow:0|1> <ack_level>
# ack_level=1 => records append_send_to_ordered (ACK1/ordering)
# ack_level=2 => records append_send_to_ack    (ACK2/durable)
run_mode() {
  local mode="$1"
  local inject="$2"
  local ack_level="$3"
  local run_dir="$OUTDIR/${SEQUENCER}/$mode"
  mkdir -p "$run_dir"

  local success=0
  for ((attempt=1; attempt<=POINT_MAX_ATTEMPTS; attempt++)); do
    cleanup
    sleep 2  # Let signals settle before starting new cluster
    # EMBARCADERO_SYNC_SLEEP_MS: set before cluster start so sync threads sleep.
    # inject=1 trials use INJECT_SYNC_SLEEP_MS; baseline trials leave it unset.
    if [ "$inject" = "1" ] && [ -n "${INJECT_SYNC_SLEEP_MS:-}" ]; then
      if [[ "$SEQUENCER" == "SCALOG" || "$SEQUENCER" == "LAZYLOG" ]]; then
        export EMBARCADERO_SCALOG_SYNC_SLEEP_MS="$INJECT_SYNC_SLEEP_MS"
        export EMBARCADERO_SCALOG_SYNC_SLEEP_TARGET_BROKER="$SLOW_BROKER_INDEX"
        export EMBARCADERO_SCALOG_SYNC_SLEEP_SOURCE_BROKER="${SLOW_SOURCE_BROKER_INDEX:-0}"
        unset EMBARCADERO_SYNC_SLEEP_MS 2>/dev/null || true
      else
        export EMBARCADERO_SYNC_SLEEP_MS="$INJECT_SYNC_SLEEP_MS"
        unset EMBARCADERO_SCALOG_SYNC_SLEEP_MS \
              EMBARCADERO_SCALOG_SYNC_SLEEP_TARGET_BROKER \
              EMBARCADERO_SCALOG_SYNC_SLEEP_SOURCE_BROKER 2>/dev/null || true
      fi
    else
      unset EMBARCADERO_SYNC_SLEEP_MS 2>/dev/null || true
      unset EMBARCADERO_SCALOG_SYNC_SLEEP_MS \
            EMBARCADERO_SCALOG_SYNC_SLEEP_TARGET_BROKER \
            EMBARCADERO_SCALOG_SYNC_SLEEP_SOURCE_BROKER 2>/dev/null || true
    fi
    # Remove stale durable replica files before brokers open them. Unlinking
    # after cluster start leaves the process writing an anonymous inode.
    if [[ ("$SEQUENCER" == "LAZYLOG" || "$SEQUENCER" == "SCALOG") &&
          "$REPLICATION_FACTOR" -gt 0 ]]; then
      IFS="," read -r -a _rdirs <<< "${EMBARCADERO_REPLICA_DISK_DIRS:-}"
      for _d in "${_rdirs[@]:-}"; do
        [[ -d "$_d" ]] && rm -f "$_d"/scalog_replication_log*_replica*.dat 2>/dev/null || true
        [[ -d "$_d" ]] && rm -f "$_d"/replica_b*.dat 2>/dev/null || true
      done
      echo "  [replica-clean] cleared stale replica files before cluster start" >&2
    fi
    local broker_pid_line
    broker_pid_line=$(start_cluster)
    read -r -a broker_pids <<<"$broker_pid_line"
    sleep "$POST_START_SETTLE_SEC"

    if [ "$SLOW_BROKER_INDEX" -lt 0 ] || [ "$SLOW_BROKER_INDEX" -ge "${#broker_pids[@]}" ]; then
      echo "Invalid SLOW_BROKER_INDEX=$SLOW_BROKER_INDEX (pid count=${#broker_pids[@]})" >&2
      cleanup
      return 1
    fi

    if [ "$inject" = "1" ]; then
      inject_slowdown "${broker_pids[$SLOW_BROKER_INDEX]}" "$(lazylog_inject_delay)"
    fi

    # ─── Replication-ready warmup ─────────────────────────────────────────────
    # Send 16 MiB without recording latency to trigger the first fdatasync cycle
    # on all ReplicaPollingLoop threads. Without this, replication_done stays at
    # kReplicationNotStarted and LazyLog's min(replication_done) is always 0,
    # causing a permanent 75% ACK stall in the baseline trial.
    # Disabled by default: throughput_test currently uses a fixed TestTopic and
    # restarts message UIDs at zero. A same-topic warmup therefore makes every
    # measured delivery check report duplicates/missing UIDs. The measured run
    # initializes replication_done itself and is accepted only if it completes.
    if [[ "$SEQUENCER" == "LAZYLOG" && "${LAZYLOG_SAME_TOPIC_WARMUP:-0}" == "1" ]]; then
      echo "  [warmup] initializing replication_done for LAZYLOG (16 MiB, no record)..." >&2
      local warmup_ok=0
      local warmup_log="$run_dir/warmup_attempt${attempt}_ack${ack_level}.log"
      $CLIENT_NUMA_BIND timeout 120s ./throughput_test \
        --config "../../${CLIENT_CONFIG}" \
        -n "$THREADS_PER_BROKER" -m "$MESSAGE_SIZE" -s $((16*1024*1024)) \
        -t "$TEST_TYPE" -o "$ORDER" -a "$ack_level" -r "$REPLICATION_FACTOR" \
        --sequencer "$SEQUENCER" -l 0 >"$warmup_log" 2>&1 \
        && warmup_ok=1 || warmup_ok=0
      if [[ "$warmup_ok" -eq 1 ]]; then
        echo "  [warmup] done — sleeping 5s for replication_done to settle" >&2
        sleep 5
      else
        echo "  [warmup] ERROR: failed — refusing an uninitialized LazyLog trial" >&2
        cleanup
        continue
      fi
    fi

    rm -f stage_latency_summary.csv pub_latency_stats.csv latency_stats.csv \
          pub_cdf_latency_us.csv cdf_latency_us.csv

    local run_log="$run_dir/run_attempt${attempt}_ack${ack_level}.log"
    echo "Running mode=$mode ack_level=${ack_level} attempt=$attempt/$POINT_MAX_ATTEMPTS sequencer=$SEQUENCER" >&2

    local test_cmd=(
      ./throughput_test
      --config "../../${CLIENT_CONFIG}"
      -n "$THREADS_PER_BROKER"
      -m "$MESSAGE_SIZE"
      -s "$TOTAL_MESSAGE_SIZE"
      -t "$TEST_TYPE"
      -o "$ORDER"
      -a "$ack_level"
      -r "$REPLICATION_FACTOR"  # RF=2 for all sub-trials. LazyLog ACK1 requires RF=2: at RF=1 replication_done is self-only and advances immediately on memory-copy ingest, so SIGSTOP has nothing to stall. At RF=2 stopping broker 1 withholds its replication_done from broker 0's TInode, blocking broker 0's local_progress report and stalling ordering.
      --sequencer "$SEQUENCER"
      --record_results
      -l 0
    )
    if [[ "$TARGET_MBPS" != "0" ]]; then
      test_cmd+=(--target_mbps "$TARGET_MBPS" --steady_rate)
    fi

    local run_ok=0
    if command -v timeout >/dev/null 2>&1; then
      if $CLIENT_NUMA_BIND timeout "${RUN_TIMEOUT_SEC}s" "${test_cmd[@]}" 2>&1 | tee "$run_log" >/dev/null; then
        run_ok=1
      fi
    else
      if $CLIENT_NUMA_BIND "${test_cmd[@]}" 2>&1 | tee "$run_log" >/dev/null; then
        run_ok=1
      fi
    fi

    # Always copy latency CSVs first (written before any assertion exits).
    for f in stage_latency_summary.csv pub_latency_stats.csv latency_stats.csv \
              pub_cdf_latency_us.csv cdf_latency_us.csv; do
      if [ -f "$f" ]; then
        cp "$f" "$run_dir/${f%.csv}_ack${ack_level}.csv"
      fi
    done
    for ((broker_idx=0; broker_idx<NUM_BROKERS; broker_idx++)); do
      cp -f "/tmp/hetero_broker_${broker_idx}.log" \
        "$run_dir/broker${broker_idx}_attempt${attempt}_ack${ack_level}.log" \
        2>/dev/null || true
    done

    # Fail closed: a CSV from an assertion-failed or incomplete run is not a
    # measurement. Slow cells must also prove that the intended follower/source
    # sync boundary executed the injection.
    local catastrophic_fail=0
    if grep -Eq "Latency test failed|Subscriber poll timeout|Subscriber::Poll timeout|Exception during latency test|not all messages acknowledged" "$run_log"; then
      catastrophic_fail=1
    fi
    local has_stage_csv=0
    if [ -f "$run_dir/stage_latency_summary_ack${ack_level}.csv" ] && \
       grep -q "append_send_to_ordered\|append_send_to_ack" \
         "$run_dir/stage_latency_summary_ack${ack_level}.csv" 2>/dev/null; then
      has_stage_csv=1
    fi
    local injection_ok=1
    if [[ "$inject" -eq 1 &&
          ("$SEQUENCER" == "SCALOG" || "$SEQUENCER" == "LAZYLOG") ]]; then
      if ! grep -q "\\[SCALOG_SYNC_SLEEP\\].*target_broker=${SLOW_BROKER_INDEX}.*source_broker=${SLOW_SOURCE_BROKER_INDEX:-0}" \
          "$run_dir"/broker*_attempt"${attempt}"_ack"${ack_level}".log \
          2>/dev/null; then
        injection_ok=0
        echo "[ERROR] no targeted sync-sleep evidence in slow cell" >&2
      fi
    fi
    if [ "${run_ok:-0}" -eq 1 ] && [ "$catastrophic_fail" -eq 0 ] && \
       [ "$has_stage_csv" -eq 1 ] && [ "$injection_ok" -eq 1 ]; then
      success=1
    fi

    for pid in "${broker_pids[@]}"; do
      terminate_pid_bounded "$pid"
    done
    if [ -n "$SEQUENCER_PID" ]; then
      terminate_pid_bounded "$SEQUENCER_PID"
      SEQUENCER_PID=""
    fi

    if [ "$success" -eq 1 ]; then
      break
    fi
  done

  if [ "$success" -ne 1 ]; then
    return 1
  fi
  return 0
}

# ---------------------------------------------------------------------------
# Collect ordered (ACK1) and durable (ACK2) P99s for one system/mode pair.
# Because publisher.cc only emits append_send_to_ordered when ack_level=1,
# and only emits append_send_to_ack (durable) when ack_level>=2, we run two
# sub-trials per mode and read the metric from the matching CSV.
# ---------------------------------------------------------------------------
collect_pair() {
  local mode="$1"    # baseline | slow_injected
  local inject="$2"  # 0 | 1
  local run_dir="$OUTDIR/${SEQUENCER}/$mode"

  local ordered_p99="NA"
  local ack_p99="NA"
  local status="PASS"

  # Sub-trial for ACK1 (ordering latency)
  if run_mode "$mode" "$inject" 1; then
    ordered_p99=$(extract_metric \
      "$run_dir/stage_latency_summary_ack1.csv" "append_send_to_ordered" 5)
  else
    echo "[WARN] $SEQUENCER $mode ACK=1 sub-trial failed" >&2
    status="PARTIAL"
  fi

  # Scalog's port exposes ordering through the replica-cut gate but its
  # independent ACK2 path does not complete; RUN_ACK2=0 measures the relevant
  # ACK1 coupling without manufacturing an ACK2 number.
  if [[ "$RUN_ACK2" == "1" ]]; then
    if run_mode "$mode" "$inject" 2; then
      ack_p99=$(extract_metric \
        "$run_dir/stage_latency_summary_ack2.csv" "append_send_to_ack" 5)
    else
      echo "[WARN] $SEQUENCER $mode ACK=2 sub-trial failed" >&2
      status="PARTIAL"
    fi
  fi

  if [ "$ordered_p99" = "NA" ] && [ "$ack_p99" = "NA" ]; then
    status="FAIL"
  fi

  echo "$SEQUENCER,$mode,$status,$ordered_p99,$ack_p99"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
RESULT_CSV="$OUTDIR/slow_replica_comparison.csv"
echo "system,mode,status,ordered_p99_us,durable_p99_us" > "$RESULT_CSV"

echo "=== Testing SEQUENCER=$SEQUENCER ===" >&2

for _trial in $(seq 1 "$NUM_TRIALS"); do
  _saved_outdir="$OUTDIR"
  OUTDIR="$OUTDIR/trial_${_trial}"
  mkdir -p "$OUTDIR"
  echo "--- Trial $_trial/$NUM_TRIALS ---" >&2

  baseline_row=$(collect_pair "baseline" "0")
  slow_row=$(collect_pair "slow_injected" "1")

  OUTDIR="$_saved_outdir"
  echo "$baseline_row" >> "$RESULT_CSV"
  echo "$slow_row" >> "$RESULT_CSV"
  echo "  trial $_trial baseline: $baseline_row" >&2
  echo "  trial $_trial slow:     $slow_row" >&2
done

# ---------------------------------------------------------------------------
# Human-readable delta table
# ---------------------------------------------------------------------------
awk -F',' '
function median(values, n, sorted, i, j, tmp) {
  for (i=1; i<=n; i++) sorted[i]=values[i]
  for (i=1; i<=n; i++)
    for (j=i+1; j<=n; j++)
      if (sorted[i] > sorted[j]) {
        tmp=sorted[i]; sorted[i]=sorted[j]; sorted[j]=tmp
      }
  if (n % 2) return sorted[(n+1)/2]
  return (sorted[n/2] + sorted[n/2+1]) / 2
}
NR==1 {next}
{
  system_name=$1; mode=$2
  if (mode=="baseline") {
    if ($4 != "NA") { ++nbo; baseline_ordered[nbo]=$4 }
    if ($5 != "NA") { ++nbd; baseline_durable[nbd]=$5 }
  }
  if (mode=="slow_injected") {
    if ($4 != "NA") { ++nso; slow_ordered[nso]=$4 }
    if ($5 != "NA") { ++nsd; slow_durable[nsd]=$5 }
  }
}
END {
  fmt = "%-13s | %-14s | %10s | %10s | %12s | %12s\n"
  printf fmt, "System", "Mode", "ACK1-P99us", "ACK2-P99us", "ACK1-delta%", "ACK2-delta%"
  printf fmt, "-------------", "--------------", "----------", "----------", "------------", "------------"
  bo=(nbo > 0) ? median(baseline_ordered,nbo) : "NA"
  bd=(nbd > 0) ? median(baseline_durable,nbd) : "NA"
  so=(nso > 0) ? median(slow_ordered,nso) : "NA"
  sd=(nsd > 0) ? median(slow_durable,nsd) : "NA"
  dp_o = (bo != "NA" && so != "NA" && bo>0) ? sprintf("%+.1f%%", 100*(so-bo)/bo) : "NA"
  dp_d = (bd != "NA" && sd != "NA" && bd>0) ? sprintf("%+.1f%%", 100*(sd-bd)/bd) : "NA"
  printf fmt, system_name, "baseline median", bo, bd, "---", "---"
  printf fmt, system_name, "slow median", so, sd, dp_o, dp_d
}
' "$RESULT_CSV" || true

echo ""
if [[ "$SEQUENCER" == "SCALOG" ]]; then
  echo "Claim: Scalog ACK1 inherits follower-sync delay because its global cut"
  echo "       is gated by the replica-persistence frontier."
else
  echo "Claim: Embarcadero ACK1 is unaffected by follower-sync delay, while"
  echo "       ACK2 waits for the durable completion frontier."
fi
echo ""
echo "Artifacts: $OUTDIR"
echo "CSV:       $RESULT_CSV"

cleanup
