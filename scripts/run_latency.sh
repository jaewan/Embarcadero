#!/bin/bash
set -euo pipefail

export EMBARCADERO_RUNTIME_MODE="${EMBARCADERO_RUNTIME_MODE:-latency}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BIN_DIR="$PROJECT_ROOT/build/bin"
DATA_DIR="$PROJECT_ROOT/data/latency"

# NOTE: This script requires recompilation with COLLECT_LATENCY_STATS macro defined.
# Build with:
#   cmake -S . -B build -DCOLLECT_LATENCY_STATS=ON
#   cmake --build build -j

NUM_BROKERS="${NUM_BROKERS:-4}"
TEST_CASE="${TEST_CASE:-2}"
MSG_SIZE="${MSG_SIZE:-1024}"
ACK_LEVEL="${ACK_LEVEL:-1}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-4294967296}" # 4 GiB explicit default
NUM_TRIALS="${NUM_TRIALS:-1}"
RUN_ID="${RUN_ID:-$(date +%Y%m%d_%H%M%S)}"
PLOT_RESULTS="${PLOT_RESULTS:-0}"
TARGET_MBPS="${TARGET_MBPS:-0}"

REMOTE_IP="${REMOTE_IP:-192.168.60.173}"
REMOTE_USER="${REMOTE_USER:-domin}"
PASSLESS_ENTRY="${PASSLESS_ENTRY:-~/.ssh/id_rsa}"
REMOTE_BIN_DIR="${REMOTE_BIN_DIR:-~/Jae/Embarcadero/build/bin}"
REMOTE_PID_FILE="${REMOTE_PID_FILE:-/tmp/remote_seq.pid}"

EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-numactl --cpunodebind=0 --membind=0}"

# Canonical order levels:
# - EMBARCADERO strong ordering => order=5
# - CORFU comparison => order=2 (total order mode in current harness)
declare -a configs=(
  "order=5; sequencer=EMBARCADERO"
  "order=2; sequencer=CORFU"
  # "order=1; sequencer=SCALOG"
)

read -r -a modes <<< "${MODES:-steady}"

wait_for_signal() {
  while true; do
    read -r signal < script_signal_pipe
    if [[ -n "$signal" ]]; then
      echo "Received signal: $signal"
      break
    fi
  done
}

start_process() {
  local command=$1
  $command &
  local pid=$!
  echo "Started process: '$command' (PID=$pid)"
  pids+=("$pid")
}

start_remote_sequencer() {
  local sequencer_bin=$1
  echo "Starting remote sequencer '$sequencer_bin' on $REMOTE_IP..."
  ssh -o StrictHostKeyChecking=no -i "$PASSLESS_ENTRY" "$REMOTE_USER@$REMOTE_IP" bash <<EOF
    cd $REMOTE_BIN_DIR
    nohup ./$sequencer_bin > /tmp/${sequencer_bin}.log 2>&1 &
    echo \$! > $REMOTE_PID_FILE
EOF
}

stop_remote_sequencer() {
  echo "Stopping remote sequencer on $REMOTE_IP..."
  ssh -o StrictHostKeyChecking=no -i "$PASSLESS_ENTRY" "$REMOTE_USER@$REMOTE_IP" bash <<EOF
    if [ -f $REMOTE_PID_FILE ]; then
      kill \$(cat $REMOTE_PID_FILE) 2>/dev/null || true
      rm -f $REMOTE_PID_FILE
    fi
EOF
}

mkdir -p "$DATA_DIR"

pushd "$BIN_DIR" >/dev/null

for mode in "${modes[@]}"; do
  echo "========================================================"
  echo "Running mode: $mode"
  echo "Run ID: $RUN_ID"
  echo "========================================================"

  for config in "${configs[@]}"; do
    eval "$config"
    echo "--------------------------------------------------------"
    echo "Configuration: sequencer=$sequencer order=$order msg_size=$MSG_SIZE ack=$ACK_LEVEL total_bytes=$TOTAL_MESSAGE_SIZE"
    echo "--------------------------------------------------------"

    for trial in $(seq 1 "$NUM_TRIALS"); do
      echo "[Trial $trial/$NUM_TRIALS] mode=$mode sequencer=$sequencer order=$order"

      pids=()
      rm -f script_signal_pipe
      mkfifo script_signal_pipe

      rm -f cdf_latency_us.csv latency_stats.csv pub_cdf_latency_us.csv pub_latency_stats.csv

      if [[ "$sequencer" == "CORFU" ]]; then
        start_remote_sequencer "corfu_global_sequencer"
      elif [[ "$sequencer" == "SCALOG" ]]; then
        start_remote_sequencer "scalog_global_sequencer"
      fi

      start_process "$EMBARLET_NUMA_BIND ./embarlet --head --$sequencer"
      wait_for_signal
      sleep 3

      for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
        start_process "$EMBARLET_NUMA_BIND ./embarlet --$sequencer"
        wait_for_signal
      done
      sleep 3

      if [[ "$mode" == "steady" ]]; then
        start_process "$CLIENT_NUMA_BIND ./throughput_test -m $MSG_SIZE -s $TOTAL_MESSAGE_SIZE --record_results -t $TEST_CASE -o $order -a $ACK_LEVEL --sequencer $sequencer -r 1 --steady_rate --target_mbps $TARGET_MBPS"
      else
        start_process "$CLIENT_NUMA_BIND ./throughput_test -m $MSG_SIZE -s $TOTAL_MESSAGE_SIZE --record_results -t $TEST_CASE -o $order -a $ACK_LEVEL --sequencer $sequencer -r 1 --target_mbps $TARGET_MBPS"
      fi

      for pid in "${pids[@]}"; do
        wait "$pid"
        echo "Process PID $pid finished"
      done

      if [[ "$sequencer" == "CORFU" || "$sequencer" == "SCALOG" ]]; then
        stop_remote_sequencer
      fi
      sleep 3

      TRIAL_DIR="$DATA_DIR/$mode/$RUN_ID/${sequencer}_order${order}_ack${ACK_LEVEL}_msg${MSG_SIZE}_bytes${TOTAL_MESSAGE_SIZE}_trial${trial}"
      mkdir -p "$TRIAL_DIR"

      if [[ -f cdf_latency_us.csv ]]; then
        mv cdf_latency_us.csv "$TRIAL_DIR/cdf_latency_us.csv"
      fi
      if [[ -f latency_stats.csv ]]; then
        mv latency_stats.csv "$TRIAL_DIR/latency_stats.csv"
      fi
      if [[ -f pub_cdf_latency_us.csv ]]; then
        mv pub_cdf_latency_us.csv "$TRIAL_DIR/pub_cdf_latency_us.csv"
      fi
      if [[ -f pub_latency_stats.csv ]]; then
        mv pub_latency_stats.csv "$TRIAL_DIR/pub_latency_stats.csv"
      fi

      cat > "$TRIAL_DIR/run_metadata.txt" <<EOF
run_id=$RUN_ID
mode=$mode
trial=$trial
sequencer=$sequencer
order=$order
ack_level=$ACK_LEVEL
message_size=$MSG_SIZE
total_message_size=$TOTAL_MESSAGE_SIZE
num_brokers=$NUM_BROKERS
test_case=$TEST_CASE
runtime_mode=$EMBARCADERO_RUNTIME_MODE
target_mbps=$TARGET_MBPS
EOF

      rm -f script_signal_pipe
      echo "Saved trial artifacts to: $TRIAL_DIR"
    done
  done
done

popd >/dev/null

if [[ "$PLOT_RESULTS" == "1" ]]; then
  pushd "$DATA_DIR" >/dev/null
  python3 plot_latency.py latency
  popd >/dev/null
fi

echo "All latency experiments finished. Results under: $DATA_DIR"
