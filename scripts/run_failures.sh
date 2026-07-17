#!/usr/bin/env bash
# scripts/run_failures.sh
#
# Broker-failure throughput trace for paper Fig3 (fig:failure_throughput):
#   kill 1 of 4 brokers mid-run; 100 ms ACK windows; ORDER=5 prefix-safe hold
#   (or ORDER=4 arrival-order sensitivity).
#
# Defaults match working Embar CXL knobs (Fig1/Fig2): metadata CXL zero, 72 GiB,
# remote publisher on c4, head 10.10.10.10, wall-clock kill ~1.8 s after publish.
#
# Usage:
#   bash scripts/run_failures.sh
#   HOLD_MODE=arrival_order SCENARIO=remote bash scripts/run_failures.sh
#   SCENARIO=local ORDER=5 bash scripts/run_failures.sh   # local client smoke
#
# Key env (all overrideable):
#   HOLD_MODE=prefix_safe|arrival_order   (default prefix_safe → ORDER=5)
#   SCENARIO=remote|local                 (default remote → c4)
#   FAILURE_AFTER_MS=1800                 (wall-clock kill; 0 = use FAILURE_PERCENTAGE)
#   FAILURE_DATA_DIR=...                  (default data/failure or paper_eval path)
#   OUT_TAG=...                           (optional subdir under FAILURE_DATA_DIR)
#
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/lib/broker_lifecycle.sh"
cd "$PROJECT_ROOT"

if [ ! -d "build/bin" ]; then
  echo "Error: Cannot find build/bin directory (expected $PROJECT_ROOT/build/bin)" >&2
  exit 1
fi
broker_init_paths
cd build/bin

# ---------------------------------------------------------------------------
# Fig3 / Embar-aligned defaults
# ---------------------------------------------------------------------------
HOLD_MODE="${HOLD_MODE:-prefix_safe}"
SCENARIO="${SCENARIO:-remote}"
REMOTE_CLIENT_HOST="${REMOTE_CLIENT_HOST:-c4}"
REMOTE_CLIENT_BIN_DIR="${REMOTE_CLIENT_BIN_DIR:-/home/domin/Embarcadero/build/bin}"
REMOTE_CLIENT_CONFIG="${REMOTE_CLIENT_CONFIG:-/home/domin/Embarcadero/config/client.yaml}"
CLIENT_LD_LIBRARY_PATH="${CLIENT_LD_LIBRARY_PATH:-/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib}"

NUM_BROKERS="${NUM_BROKERS:-4}"
NUM_BROKERS_TO_KILL="${NUM_BROKERS_TO_KILL:-1}"
NUM_TRIALS="${NUM_TRIALS:-1}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-21474836480}"  # 20 GiB (paper)
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
FAILURE_PERCENTAGE="${FAILURE_PERCENTAGE:-0.5}"
# Paper kill≈1.8 s into publish. Implemented in publisher.cc when >0.
FAILURE_AFTER_MS="${FAILURE_AFTER_MS:-1800}"
FAILURE_MATCH_THROUGHPUT="${FAILURE_MATCH_THROUGHPUT:-1}"
export EMBARCADERO_FAILURE_MATCH_THROUGHPUT="$FAILURE_MATCH_THROUGHPUT"
export EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS="${EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS:-100}"

SEQUENCER="${SEQUENCER:-EMBARCADERO}"
ACK="${ACK:-1}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}"
THREADS_PER_BROKER="${THREADS_PER_BROKER:-4}"
CLIENT_TIMEOUT="${CLIENT_TIMEOUT:-600}"
BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-900}"
PRESERVE_REMOTE_BROKERS="${PRESERVE_REMOTE_BROKERS:-0}"
FORCE_RESTART_BROKERS="${FORCE_RESTART_BROKERS:-1}"

# HOLD_MODE → order + lease knobs (prefix-safe must not idle-gap-skip during TCP detect).
case "$HOLD_MODE" in
  prefix_safe|hold|b|B)
    ORDER="${ORDER:-5}"
    export EMBARCADERO_SESSION_LEASE_MS="${EMBARCADERO_SESSION_LEASE_MS:-180000}"
    export EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS="${EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS:-180000}"
    HOLD_LABEL="prefix_safe_hold"
    ;;
  arrival_order|nohold|no_hold|a|A)
    # Sensitivity: arrival order within each round (historical Fig3 panel a / ORDER=4).
    ORDER="${ORDER:-4}"
    HOLD_LABEL="arrival_order_no_hold"
    ;;
  *)
    echo "ERROR: unknown HOLD_MODE=$HOLD_MODE (use prefix_safe|arrival_order)" >&2
    exit 1
    ;;
esac
ack="$ACK"
sequencer="$SEQUENCER"
test_cases=(4)

# CXL knobs matching Fig2 (metadata clear; 72 GiB admits 4×8 GiB segments).
export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}"
export EMBARCADERO_RUNTIME_MODE="${EMBARCADERO_RUNTIME_MODE:-failure}"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
export EMBARCADERO_CXL_MAP_POPULATE="${EMBARCADERO_CXL_MAP_POPULATE:-0}"
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-77309411328}"
export EMBAR_ORDER5_EPOCH_US="${EMBAR_ORDER5_EPOCH_US:-500}"
export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-300}"

# Head / client addressing
if [[ "$SCENARIO" == "remote" ]]; then
  export EMBARCADERO_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-10.10.10.10}"
  CLIENT_HEAD_ADDR="${CLIENT_HEAD_ADDR:-$EMBARCADERO_HEAD_ADDR}"
else
  CLIENT_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-${REMOTE_HEAD_ADDR:-127.0.0.1}}"
fi

# Output directory (optional OUT_TAG for panel isolation under paper_eval).
FAILURE_DATA_DIR="${FAILURE_DATA_DIR:-$PROJECT_ROOT/data/failure}"
if [[ -n "${OUT_TAG:-}" ]]; then
  FAILURE_DATA_DIR="$FAILURE_DATA_DIR/$OUT_TAG"
fi
mkdir -p "$FAILURE_DATA_DIR"
export EMBARCADERO_FAILURE_DATA_DIR="$FAILURE_DATA_DIR"

if [[ "${FAILURE_AFTER_MS}" -gt 0 ]] 2>/dev/null; then
  export EMBARCADERO_FAILURE_AFTER_MS="$FAILURE_AFTER_MS"
else
  unset EMBARCADERO_FAILURE_AFTER_MS || true
fi

# NUMA: prefer CPU node 1 + membind including CXL node 2 when present (Fig2/lifecycle).
if [ -z "${EMBARLET_NUMA_BIND+x}" ]; then
  if command -v numactl &>/dev/null && numactl --hardware 2>/dev/null | grep -q 'node 1'; then
    if numactl --hardware 2>/dev/null | grep -q 'node 2'; then
      EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"
    else
      EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1"
    fi
    echo "NUMA: $EMBARLET_NUMA_BIND"
  else
    EMBARLET_NUMA_BIND=""
  fi
fi

HEAD_CONFIG_ARG="--config ../../config/embarcadero.yaml"
CONFIG_ARG="--config ../../config/embarcadero.yaml"

SUMMARY_TSV="/tmp/run_failures_summary_$$.tsv"
printf 'trial\treported_mb_s\tactive_mean_gbps\tactive_std_gbps\tactive_peak_gbps\n' > "$SUMMARY_TSV"

cleanup() {
  echo "Cleaning up stale brokers and clients..."
  pkill -x throughput_test >/dev/null 2>&1 || true
  if [[ "$SCENARIO" == "remote" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$REMOTE_CLIENT_HOST" \
      'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
  fi
  if ! broker_is_remote_mode || [ "$PRESERVE_REMOTE_BROKERS" != "1" ]; then
    broker_cleanup
  fi
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  sleep 1
}

cleanup

if [ -n "${EMBARCADERO_TUNE_KERNEL_BUFFERS:-}" ]; then
  (cd "$PROJECT_ROOT" && ./scripts/tune_kernel_buffers.sh) || echo "Warning: kernel buffer tune failed. Continuing."
fi

echo "===== Failure Benchmark Configuration ====="
echo "  HOLD_MODE:        $HOLD_MODE ($HOLD_LABEL) ORDER=$ORDER"
echo "  SCENARIO:         $SCENARIO"
echo "  Brokers:          $NUM_BROKERS  kill=$NUM_BROKERS_TO_KILL"
if [ "${FAILURE_AFTER_MS}" -gt 0 ] 2>/dev/null; then
  echo "  Kill:             wall-clock ${FAILURE_AFTER_MS} ms after publish start"
else
  echo "  Kill:             at ${FAILURE_PERCENTAGE} of data sent"
fi
echo "  Total data:       $TOTAL_MESSAGE_SIZE bytes  msg=$MESSAGE_SIZE"
echo "  ACK/RF:           $ack / $REPLICATION_FACTOR"
echo "  Sequencer:        $sequencer  threads/broker=$THREADS_PER_BROKER"
echo "  CXL:              zero=$EMBARCADERO_CXL_ZERO_MODE size=$EMBARCADERO_CXL_SIZE ready_timeout=${BROKER_READY_TIMEOUT_SEC}s"
echo "  Client head:      $CLIENT_HEAD_ADDR"
echo "  Output dir:       $FAILURE_DATA_DIR"
echo "============================================"

wait_for_broker_ready() {
  local expected_pid=$1
  local timeout=$2
  local elapsed=0
  local start_time
  start_time=$(date +%s)
  echo "Waiting for broker readiness (timeout=${timeout}s, PID=$expected_pid)..."
  while [ $elapsed -lt $timeout ]; do
    if [ -f "/tmp/embarlet_${expected_pid}_ready" ]; then
      echo "Broker ready after ${elapsed}s"
      rm -f "/tmp/embarlet_${expected_pid}_ready"
      return 0
    fi
    if ! kill -0 "$expected_pid" 2>/dev/null && [ $elapsed -ge 5 ]; then
      echo "ERROR: Broker $expected_pid died before ready" >&2
      return 1
    fi
    sleep 0.5
    elapsed=$(($(date +%s) - start_time))
  done
  echo "ERROR: Broker $expected_pid not ready in ${timeout}s" >&2
  return 1
}

wait_for_all_brokers_ready() {
  local timeout=$1
  shift
  local pids=("$@")
  local start_time elapsed=0
  start_time=$(date +%s)
  echo "Waiting for ${#pids[@]} brokers (timeout=${timeout}s)..."
  while [ $elapsed -lt $timeout ]; do
    local all_ready=1
    for expected_pid in "${pids[@]}"; do
      [ -z "$expected_pid" ] && continue
      if [ ! -f "/tmp/embarlet_${expected_pid}_ready" ]; then
        all_ready=0
        if ! kill -0 "$expected_pid" 2>/dev/null && [ $elapsed -ge 5 ]; then
          echo "ERROR: Broker $expected_pid died before ready" >&2
          return 1
        fi
      fi
    done
    if [ "$all_ready" = "1" ]; then
      echo "All brokers ready after ${elapsed}s"
      for expected_pid in "${pids[@]}"; do
        rm -f "/tmp/embarlet_${expected_pid}_ready" 2>/dev/null || true
      done
      return 0
    fi
    sleep 0.5
    elapsed=$(($(date +%s) - start_time))
  done
  echo "ERROR: Not all brokers ready in ${timeout}s" >&2
  return 1
}

collect_remote_artifacts() {
  # Shared NFS usually makes this a no-op; scp covers non-shared homes.
  local remote_dir="$1"
  ssh -o BatchMode=yes -o ConnectTimeout=10 "$REMOTE_CLIENT_HOST" \
    "test -d $(printf '%q' "$remote_dir")" 2>/dev/null || return 0
  for f in real_time_acked_throughput.csv failure_events.csv; do
    scp -o StrictHostKeyChecking=no \
      "${REMOTE_CLIENT_HOST}:${remote_dir}/$f" \
      "$FAILURE_DATA_DIR/$f" 2>/dev/null || true
  done
}

summarize_trial_results() {
  local trial=$1
  local client_log=$2
  local trial_csv="$FAILURE_DATA_DIR/real_time_acked_throughput_trial${trial}.csv"
  local trial_events="$FAILURE_DATA_DIR/failure_events_trial${trial}.csv"
  local source_csv="$FAILURE_DATA_DIR/real_time_acked_throughput.csv"
  local source_events="$FAILURE_DATA_DIR/failure_events.csv"

  if [ -f "$source_csv" ]; then
    cp "$source_csv" "$trial_csv"
  fi
  if [ -f "$source_events" ]; then
    cp "$source_events" "$trial_events"
  fi

  local reported_mb_s
  reported_mb_s=$(sed -n 's/.*Bandwidth (active-window headline): \([0-9][0-9.]*\) MB\/s.*/\1/p' "$client_log" | tail -n 1)
  if [ -z "$reported_mb_s" ]; then
    reported_mb_s=$(sed -n 's/.*Bandwidth: \([0-9][0-9.]*\) MB\/s.*/\1/p' "$client_log" | tail -n 1)
  fi
  if [ -z "$reported_mb_s" ]; then
    reported_mb_s="nan"
  fi

  local csv_to_analyze="$trial_csv"
  if [ ! -f "$csv_to_analyze" ] && [ -f "$source_csv" ]; then
    csv_to_analyze="$source_csv"
  fi

  local active_stats="nan nan nan"
  if [ -f "$csv_to_analyze" ]; then
    active_stats=$(python3 - <<'PY' "$csv_to_analyze"
import csv, math, sys
path = sys.argv[1]
rows = []
with open(path, newline='') as f:
    for row in csv.DictReader(f):
        try:
            rows.append(float(row["Total_GBps"]))
        except (KeyError, ValueError):
            pass
if not rows:
    print("nan nan nan"); raise SystemExit
threshold = 0.01
active = [i for i, v in enumerate(rows) if v > threshold]
trimmed = rows[:active[-1] + 2] if active else rows[:]
if len(trimmed) > 5:
    peak = max(trimmed)
    cutoff = peak * 0.70
    rolling = []
    for i in range(len(trimmed)):
        window = trimmed[max(0, i - 2):i + 1]
        rolling.append(sum(window) / len(window))
    above = [i for i, v in enumerate(rolling) if v >= cutoff]
    if above:
        trimmed = trimmed[:above[-1] + 1]
if not trimmed:
    print("nan nan nan"); raise SystemExit
mean = sum(trimmed) / len(trimmed)
var = sum((v - mean) ** 2 for v in trimmed) / len(trimmed)
print(f"{mean:.3f} {var**0.5:.3f} {max(trimmed):.3f}")
PY
)
  fi

  local active_mean active_std active_peak
  read -r active_mean active_std active_peak <<< "$active_stats"
  echo "Trial $trial summary:"
  echo "  Reported bandwidth: ${reported_mb_s} MB/s"
  echo "  Active-window throughput: mean=${active_mean} GB/s std=${active_std} GB/s peak=${active_peak} GB/s"
  printf '%s\t%s\t%s\t%s\t%s\n' \
    "$trial" "$reported_mb_s" "$active_mean" "$active_std" "$active_peak" >> "$SUMMARY_TSV"
}

# --- Trials ---
for test_case in "${test_cases[@]}"; do
  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
    echo ""
    echo "================================================================="
    echo "=== Failure trial $trial/$NUM_TRIALS HOLD_MODE=$HOLD_MODE ORDER=$ORDER ==="
    echo "================================================================="

    pids=()
    if broker_is_remote_mode; then
      echo "Ensuring remote broker cluster on $REMOTE_BROKER_HOST..."
      if ! broker_ensure_cluster "$NUM_BROKERS" "$BROKER_READY_TIMEOUT_SEC" "$sequencer"; then
        echo "Remote broker startup failed, aborting trial" >&2
        cleanup
        continue
      fi
    else
      # Brokers must advertise the dataplane IP so a remote publisher can connect.
      if [[ "$SCENARIO" == "remote" ]]; then
        export EMBARCADERO_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-10.10.10.10}"
      else
        unset EMBARCADERO_HEAD_ADDR || true
      fi

      $EMBARLET_NUMA_BIND ./embarlet $HEAD_CONFIG_ARG --head --"$sequencer" \
        > "broker_0_trial${trial}.log" 2>&1 &
      head_pid=$!
      pids+=("$head_pid")
      echo "Started head broker PID $head_pid"

      if ! wait_for_broker_ready "$head_pid" "$BROKER_READY_TIMEOUT_SEC"; then
        echo "Head broker failed to initialize, aborting trial" >&2
        for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
        pids=()
        cleanup
        continue
      fi

      echo "Starting follower brokers (--$sequencer)..."
      broker_shell_pids=()
      for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
        $EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG --"$sequencer" \
          > "broker_${i}_trial${trial}.log" 2>&1 &
        broker_shell_pids+=($!)
      done
      sleep 0.5
      follower_pids=("${broker_shell_pids[@]}")
      for pid in "${follower_pids[@]}"; do pids+=("$pid"); done

      if ! wait_for_all_brokers_ready "$BROKER_READY_TIMEOUT_SEC" "${follower_pids[@]}"; then
        echo "Follower init failed, aborting trial" >&2
        for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
        pids=()
        cleanup
        continue
      fi
      echo "All $NUM_BROKERS brokers ready."
    fi

    CLIENT_LOG="$FAILURE_DATA_DIR/client_failure_trial${trial}.log"
    RECORD_RESULTS_ARG=()
    if [ -n "${EMBARCADERO_RECORD_RESULTS:-}" ]; then
      RECORD_RESULTS_ARG+=(--record_results)
    fi

    # Ensure remote output dir exists when client runs on c4.
    if [[ "$SCENARIO" == "remote" ]]; then
      ssh -o BatchMode=yes "$REMOTE_CLIENT_HOST" \
        "mkdir -p $(printf '%q' "$FAILURE_DATA_DIR")" 2>/dev/null || true
    fi

    echo "Starting failure throughput_test (scenario=$SCENARIO)..."
    set +e
    if [[ "$SCENARIO" == "remote" ]]; then
      remote_after_export=""
      if [[ -n "${EMBARCADERO_FAILURE_AFTER_MS:-}" ]]; then
        remote_after_export="export EMBARCADERO_FAILURE_AFTER_MS=$(printf '%q' "$EMBARCADERO_FAILURE_AFTER_MS") && "
      fi
      # Forward session lease / idle-force-expire so remote client matches broker
      # knobs. Without this the client uses the 30 s default while the broker
      # waits 180 s (prefix_safe) — the client ACK-timeout fires first.
      if [[ -n "${EMBARCADERO_SESSION_LEASE_MS:-}" ]]; then
        remote_after_export+="export EMBARCADERO_SESSION_LEASE_MS=$(printf '%q' "$EMBARCADERO_SESSION_LEASE_MS") && "
      fi
      if [[ -n "${EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS:-}" ]]; then
        remote_after_export+="export EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=$(printf '%q' "$EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS") && "
      fi
      # shellcheck disable=SC2029
      timeout --signal=TERM --kill-after=10 "$CLIENT_TIMEOUT" \
        ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$REMOTE_CLIENT_HOST" \
        "cd $(printf '%q' "$REMOTE_CLIENT_BIN_DIR") && \
         export LD_LIBRARY_PATH=$(printf '%q' "$CLIENT_LD_LIBRARY_PATH")\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH} && \
         export EMBARCADERO_FAILURE_DATA_DIR=$(printf '%q' "$FAILURE_DATA_DIR") && \
         export EMBARCADERO_FAILURE_MATCH_THROUGHPUT=$(printf '%q' "$FAILURE_MATCH_THROUGHPUT") && \
         export EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS=$(printf '%q' "$EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS") && \
         export EMBARCADERO_ACK_TIMEOUT_SEC=$(printf '%q' "$EMBARCADERO_ACK_TIMEOUT_SEC") && \
         ${remote_after_export}\
         ./throughput_test --config $(printf '%q' "$REMOTE_CLIENT_CONFIG") \
           -n $THREADS_PER_BROKER -m $MESSAGE_SIZE \
           -s $TOTAL_MESSAGE_SIZE ${RECORD_RESULTS_ARG[*]+"${RECORD_RESULTS_ARG[*]}"} -t $test_case \
           --head_addr $(printf '%q' "$CLIENT_HEAD_ADDR") \
           --num_brokers_to_kill $NUM_BROKERS_TO_KILL --failure_percentage $FAILURE_PERCENTAGE \
           -o $ORDER -a $ack --sequencer $sequencer -r $REPLICATION_FACTOR -l 0" \
        2>&1 | tee "$CLIENT_LOG"
      test_exit_code=${PIPESTATUS[0]}
      collect_remote_artifacts "$FAILURE_DATA_DIR"
    else
      timeout --signal=TERM --kill-after=10 "$CLIENT_TIMEOUT" \
        stdbuf -oL -eL ./throughput_test --config ../../config/client.yaml \
        -n "$THREADS_PER_BROKER" -m "$MESSAGE_SIZE" \
        -s "$TOTAL_MESSAGE_SIZE" ${RECORD_RESULTS_ARG[@]+"${RECORD_RESULTS_ARG[@]}"} -t "$test_case" \
        --head_addr "$CLIENT_HEAD_ADDR" \
        --num_brokers_to_kill "$NUM_BROKERS_TO_KILL" --failure_percentage "$FAILURE_PERCENTAGE" \
        -o "$ORDER" -a "$ack" --sequencer "$sequencer" -r "$REPLICATION_FACTOR" -l 0 \
        2>&1 | tee "$CLIENT_LOG"
      test_exit_code=${PIPESTATUS[0]}
    fi
    set -e

    if [ "$test_exit_code" -ne 0 ]; then
      echo "WARNING: failure test exited $test_exit_code (see $CLIENT_LOG)" >&2
    else
      echo "Failure throughput test completed successfully."
    fi

    # Persist panel metadata for the combined plotter.
    cat > "$FAILURE_DATA_DIR/panel_metadata.txt" <<EOF
hold_mode=$HOLD_MODE
hold_label=$HOLD_LABEL
order=$ORDER
ack=$ack
replication_factor=$REPLICATION_FACTOR
failure_after_ms=${FAILURE_AFTER_MS:-0}
scenario=$SCENARIO
client_host=${REMOTE_CLIENT_HOST:-local}
total_message_size=$TOTAL_MESSAGE_SIZE
message_size=$MESSAGE_SIZE
num_brokers=$NUM_BROKERS
cxl_zero_mode=$EMBARCADERO_CXL_ZERO_MODE
epoch_us=${EMBAR_ORDER5_EPOCH_US:-}
trial=$trial
EOF

    summarize_trial_results "$trial" "$CLIENT_LOG"

    echo "Cleaning up broker processes..."
    for pid in "${pids[@]}"; do
      kill "$pid" 2>/dev/null && echo "  Terminated PID $pid" || true
    done
    pids=()
    sleep 0.5
    cleanup
    sleep 1
  done
done

echo ""
echo "===== Plotting Results ====="
THROUGHPUT_CSV="$FAILURE_DATA_DIR/real_time_acked_throughput.csv"
EVENTS_CSV="$FAILURE_DATA_DIR/failure_events.csv"
if [ -f "$THROUGHPUT_CSV" ]; then
  if [ -f "$EVENTS_CSV" ]; then
    python3 "$PROJECT_ROOT/scripts/plot/plot_failure.py" \
      "$THROUGHPUT_CSV" "$FAILURE_DATA_DIR/failure" \
      --events "$EVENTS_CSV" 2>&1 || echo "Warning: plot generation failed"
  else
    python3 "$PROJECT_ROOT/scripts/plot/plot_failure.py" \
      "$THROUGHPUT_CSV" "$FAILURE_DATA_DIR/failure" 2>&1 || echo "Warning: plot generation failed"
  fi
else
  echo "No throughput CSV at $THROUGHPUT_CSV"
fi

echo ""
echo "===== Trial Summary ====="
python3 - <<'PY' "$SUMMARY_TSV"
import csv, math, sys
path = sys.argv[1]
rows = list(csv.DictReader(open(path), delimiter='\t'))
def vals(name):
    out = []
    for row in rows:
        try:
            v = float(row[name])
        except (TypeError, ValueError):
            continue
        if not math.isnan(v):
            out.append(v)
    return out
for name, unit in (("reported_mb_s", "MB/s"), ("active_mean_gbps", "GB/s"), ("active_peak_gbps", "GB/s")):
    vs = vals(name)
    if not vs:
        print(f"{name}: no successful samples"); continue
    mean = sum(vs) / len(vs)
    std = (sum((v - mean) ** 2 for v in vs) / len(vs)) ** 0.5
    print(f"{name}: [{', '.join(f'{v:.3f}' for v in vs)}] {unit}")
    print(f"  avg={mean:.3f} {unit} stddev={std:.3f} {unit} n={len(vs)}")
PY

echo ""
echo "All failure experiments finished."
echo "Artifacts: $FAILURE_DATA_DIR"
echo "HOLD_MODE=$HOLD_MODE ORDER=$ORDER"
