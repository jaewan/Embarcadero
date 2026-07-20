#!/usr/bin/env bash
# scripts/run_session_isolation.sh
#
# Per-session isolation experiment for paper:
#   "Embarcadero provides session-granularity failure isolation."
#
# 4 independent client sessions, each pinned to exactly 1 broker via
# EMBARCADERO_ORDER5_BROKER_ALLOWLIST.  One broker is killed mid-run.
# Expected result:
#   - Sessions on surviving brokers: throughput flat throughout.
#   - Session on killed broker: stalls for detection (≈114 ms) + lease, then
#     SESSION_FENCED; client resubmits suffix under fresh session.
#
# This directly contrasts with global-seal systems (Scalog, Corfu) where ALL
# sessions stall simultaneously on broker failure.
#
# CSV output: one per-session file in ISOLATION_DATA_DIR/:
#   session_N.csv  (columns: Timestamp(ms), Broker_N_GBps, Total_GBps)
#   combined.csv   (Timestamp(ms), Session_0..3_GBps, Total_GBps)
#
# Usage:
#   bash scripts/run_session_isolation.sh
#   FAILED_BROKER=2 bash scripts/run_session_isolation.sh
#   NUM_TRIALS=3 bash scripts/run_session_isolation.sh
#
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/lib/broker_lifecycle.sh"
cd "$PROJECT_ROOT"

if [ ! -d "build/bin" ]; then
  echo "Error: build/bin not found" >&2; exit 1
fi
broker_init_paths
cd build/bin

# ---------------------------------------------------------------------------
# Knobs
# ---------------------------------------------------------------------------
NUM_BROKERS="${NUM_BROKERS:-4}"
# Which broker (0-indexed) to kill mid-run.  Corresponds to session index.
FAILED_BROKER="${FAILED_BROKER:-1}"
NUM_TRIALS="${NUM_TRIALS:-1}"
# Each session sends this many bytes (not total — per session).
SESSION_BYTES="${SESSION_BYTES:-5368709120}"   # 5 GiB per session
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
FAILURE_AFTER_MS="${FAILURE_AFTER_MS:-1800}"   # kill at 1.8s
SESSION_LEASE_MS="${SESSION_LEASE_MS:-180000}"  # 3-min lease (prefix-safe)
IDLE_FORCE_EXPIRE_MS="${IDLE_FORCE_EXPIRE_MS:-180000}"
THREADS_PER_SESSION="${THREADS_PER_SESSION:-4}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}"   # RF=0: isolates ordering from replication
ORDER="${ORDER:-5}"
ACK="${ACK:-1}"
SCENARIO="${SCENARIO:-remote}"
# Clients: each session runs on a different host to avoid co-location effects.
# session 0→c4, session 1→c3, session 2→c1, session 3→local
# Override per-session hosts with SESSION_HOSTS_PIPE="c4|c3|c1|local"
declare -a SESSION_HOSTS=()
if [[ -n "${SESSION_HOSTS_PIPE:-}" ]]; then
  IFS='|' read -r -a SESSION_HOSTS <<< "$SESSION_HOSTS_PIPE"
else
  SESSION_HOSTS=("c4" "c3" "c1" "local")
fi
REMOTE_CLIENT_BIN_DIR="${REMOTE_CLIENT_BIN_DIR:-/home/domin/Embarcadero/build/bin}"
REMOTE_CLIENT_CONFIG="${REMOTE_CLIENT_CONFIG:-/home/domin/Embarcadero/config/client.yaml}"
CLIENT_LD_LIBRARY_PATH="${CLIENT_LD_LIBRARY_PATH:-/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib}"
CLIENT_HEAD_ADDR="${CLIENT_HEAD_ADDR:-${EMBARCADERO_HEAD_ADDR:-10.10.10.10}}"
BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-900}"
CLIENT_TIMEOUT="${CLIENT_TIMEOUT:-600}"
ISOLATION_DATA_DIR="${ISOLATION_DATA_DIR:-$PROJECT_ROOT/data/session_isolation}"
EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-77309411328}"  # 72 GiB

export EMBARCADERO_CXL_ZERO_MODE EMBARCADERO_CXL_SIZE
export EMBARCADERO_SESSION_LEASE_MS="$SESSION_LEASE_MS"
export EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS="$IDLE_FORCE_EXPIRE_MS"
export EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS="${EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS:-100}"

mkdir -p "$ISOLATION_DATA_DIR"
echo "ISOLATION_DATA_DIR=$ISOLATION_DATA_DIR"
echo "FAILED_BROKER=$FAILED_BROKER  FAILURE_AFTER_MS=$FAILURE_AFTER_MS"
echo "SESSION_BYTES=$SESSION_BYTES  NUM_TRIALS=$NUM_TRIALS"

cleanup() {
  pkill -x throughput_test >/dev/null 2>&1 || true
  for h in "${SESSION_HOSTS[@]}"; do
    [[ "$h" == "local" ]] && continue
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$h" \
      'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
  done
  broker_cleanup
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  sleep 1
}
trap cleanup EXIT INT TERM
cleanup

# ---------------------------------------------------------------------------
# Helper: run one session (blocking, writes its own timeseries CSV)
# ---------------------------------------------------------------------------
run_one_session() {
  local session_id="$1"
  local broker_id="$2"       # which broker this session is pinned to
  local host="$3"
  local ts_file="$4"         # where to write timeseries CSV
  local log_file="$5"

  local common_env=""
  common_env+="export LD_LIBRARY_PATH=$(printf '%q' "$CLIENT_LD_LIBRARY_PATH")"
  common_env+="\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH} && "
  common_env+="export EMBARCADERO_ORDER5_BROKER_ALLOWLIST=$(printf '%q' "$broker_id") && "
  common_env+="export EMBARCADERO_THROUGHPUT_TIMESERIES_FILE=$(printf '%q' "$ts_file") && "
  common_env+="export EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS=$(printf '%q' "${EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS}") && "
  common_env+="export EMBARCADERO_SESSION_LEASE_MS=$(printf '%q' "$SESSION_LEASE_MS") && "
  common_env+="export EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=$(printf '%q' "$IDLE_FORCE_EXPIRE_MS") && "
  # Only the failed session should kill a broker.
  if [[ "$broker_id" -eq "$FAILED_BROKER" ]]; then
    common_env+="export EMBARCADERO_FAILURE_AFTER_MS=$(printf '%q' "$FAILURE_AFTER_MS") && "
  fi

  local cmd
  cmd="./throughput_test --config $(printf '%q' "$REMOTE_CLIENT_CONFIG")"
  cmd+=" -n $THREADS_PER_SESSION"
  cmd+=" -m $MESSAGE_SIZE"
  cmd+=" -s $SESSION_BYTES"
  cmd+=" -t 4"              # failure test type
  cmd+=" --head_addr $(printf '%q' "$CLIENT_HEAD_ADDR")"
  cmd+=" --num_brokers_to_kill 1"
  cmd+=" --failure_percentage 1.0"  # kill after FAILURE_AFTER_MS ms (wall-clock mode)
  cmd+=" -o $ORDER -a $ACK"
  cmd+=" -r $REPLICATION_FACTOR"
  cmd+=" -l 0"

  if [[ "$host" == "local" ]]; then
    eval "export EMBARCADERO_ORDER5_BROKER_ALLOWLIST=$broker_id"
    eval "export EMBARCADERO_THROUGHPUT_TIMESERIES_FILE=$(printf '%q' "$ts_file")"
    eval "export EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS=${EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS}"
    eval "export EMBARCADERO_SESSION_LEASE_MS=$SESSION_LEASE_MS"
    eval "export EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=$IDLE_FORCE_EXPIRE_MS"
    [[ "$broker_id" -eq "$FAILED_BROKER" ]] && \
      eval "export EMBARCADERO_FAILURE_AFTER_MS=$FAILURE_AFTER_MS"
    # shellcheck disable=SC2086
    timeout --signal=TERM --kill-after=10 "$CLIENT_TIMEOUT" \
      stdbuf -oL -eL ./throughput_test --config ../../config/client.yaml \
      -n "$THREADS_PER_SESSION" -m "$MESSAGE_SIZE" -s "$SESSION_BYTES" -t 4 \
      --head_addr "$CLIENT_HEAD_ADDR" \
      --num_brokers_to_kill 1 --failure_percentage 1.0 \
      -o "$ORDER" -a "$ACK" -r "$REPLICATION_FACTOR" -l 0 \
      >"$log_file" 2>&1
  else
    # Ensure remote output dir exists
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" \
      "mkdir -p $(printf '%q' "$(dirname "$ts_file")")" 2>/dev/null || true
    # shellcheck disable=SC2029
    timeout --signal=TERM --kill-after=10 "$CLIENT_TIMEOUT" \
      ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$host" \
      "cd $(printf '%q' "$REMOTE_CLIENT_BIN_DIR") && \
       ${common_env} ${cmd}" \
      >"$log_file" 2>&1
    # Pull timeseries CSV back if on non-shared filesystem
    scp -o StrictHostKeyChecking=no \
      "${host}:${ts_file}" "$ts_file" 2>/dev/null || true
  fi
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
for trial in $(seq 1 "$NUM_TRIALS"); do
  echo ""
  echo "===== Trial $trial / $NUM_TRIALS ====="
  trial_dir="$ISOLATION_DATA_DIR/trial_${trial}"
  mkdir -p "$trial_dir"

  # Start brokers
  local_pids=()
  EMBARCADERO_CXL_SHM_NAME="${EMBARCADERO_CXL_SHM_NAME:-/CXL_ISOLATE_$$}"
  export EMBARCADERO_CXL_SHM_NAME
  export EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"

  echo "Starting $NUM_BROKERS brokers..."
  local_pids=()
  for ((i=0; i<NUM_BROKERS; i++)); do
    if [[ $i -eq 0 ]]; then
      numactl --cpunodebind=1 --membind=1,2 \
        ./embarlet --config ../../config/embarcadero.yaml --head --EMBARCADERO \
        >/tmp/isolate_broker_${i}.log 2>&1 &
    else
      numactl --cpunodebind=1 --membind=1,2 \
        ./embarlet --config ../../config/embarcadero.yaml --EMBARCADERO \
        >/tmp/isolate_broker_${i}.log 2>&1 &
    fi
    local_pids+=("$!")
  done

  echo "Waiting for brokers to be ready (timeout=${BROKER_READY_TIMEOUT_SEC}s)..."
  broker_wait_for_ready_files "$NUM_BROKERS" "$BROKER_READY_TIMEOUT_SEC" || {
    echo "ERROR: brokers not ready" >&2
    for pid in "${local_pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    cleanup
    continue
  }
  echo "All $NUM_BROKERS brokers ready."

  # Launch all sessions concurrently — one per broker
  declare -a session_pids=()
  for ((sid=0; sid<NUM_BROKERS; sid++)); do
    local_host="${SESSION_HOSTS[$sid]:-local}"
    ts_out="$trial_dir/session_${sid}.csv"
    log_out="$trial_dir/session_${sid}.log"
    echo "  Launching session $sid → broker $sid on host $local_host"
    run_one_session "$sid" "$sid" "$local_host" "$ts_out" "$log_out" &
    session_pids+=("$!")
  done

  echo "All $NUM_BROKERS sessions running. Waiting for completion..."
  for spid in "${session_pids[@]}"; do
    wait "$spid" || true
  done
  echo "All sessions finished."

  # Merge per-session CSVs into combined.csv
  combined="$trial_dir/combined.csv"
  python3 - "$trial_dir" "$combined" "$NUM_BROKERS" "$FAILED_BROKER" << 'PYEOF'
import sys, csv, os
from collections import defaultdict

trial_dir, combined, n_brokers, failed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])

rows = defaultdict(dict)
for sid in range(n_brokers):
    f = os.path.join(trial_dir, f"session_{sid}.csv")
    if not os.path.exists(f):
        print(f"WARNING: {f} missing", file=sys.stderr)
        continue
    with open(f) as fp:
        reader = csv.DictReader(fp)
        for row in reader:
            t = int(row["Timestamp(ms)"])
            # use Total_GBps (ACK-based) as the session throughput signal
            rows[t][f"Session_{sid}_GBps"] = float(row.get("Total_GBps", 0))

if not rows:
    print("WARNING: no session data found", file=sys.stderr)
    sys.exit(0)

session_cols = [f"Session_{i}_GBps" for i in range(n_brokers)]
with open(combined, "w", newline="") as fp:
    writer = csv.DictWriter(fp, fieldnames=["Timestamp(ms)"] + session_cols + ["Total_GBps", "Failed_Session"])
    writer.writeheader()
    for t in sorted(rows):
        row = {"Timestamp(ms)": t, "Failed_Session": failed}
        total = 0.0
        for col in session_cols:
            v = rows[t].get(col, 0.0)
            row[col] = v
            total += v
        row["Total_GBps"] = total
        writer.writerow(row)

print(f"Combined CSV written: {combined}  rows={len(rows)}")
PYEOF

  # Cleanup
  for pid in "${local_pids[@]}"; do
    kill "$pid" 2>/dev/null || true
  done
  sleep 0.5
  cleanup
  rm -f "/dev/shm${EMBARCADERO_CXL_SHM_NAME:-/CXL_ISOLATE_$$}" 2>/dev/null || true
  sleep 1
done

echo ""
echo "===== Session isolation experiment complete ====="
echo "Data: $ISOLATION_DATA_DIR"
