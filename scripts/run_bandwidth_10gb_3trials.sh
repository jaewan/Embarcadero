#!/bin/bash
# Run bandwidth measurement from project root: 3 trials × 10 GB, publish-only (Order 0).
# Uses throughput_test directly (not run_throughput.sh). Start brokers in build/bin, run client from project root.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

BIN_DIR="$PROJECT_ROOT/build/bin"
CONFIG_DIR="$PROJECT_ROOT/config"
DATA_PUB_CSV="$PROJECT_ROOT/data/throughput/pub/result.csv"

# 10 GB total
TOTAL_MESSAGE_SIZE=10737418240
MESSAGE_SIZE=1024
ORDER=0
ACK=1
TEST_TYPE=5   # publish-only
SEQUENCER=EMBARCADERO
NUM_BROKERS=4
NUM_TRIALS=3

if [ ! -d "$BIN_DIR" ]; then
  echo "ERROR: $BIN_DIR not found. Build the project first."
  exit 1
fi

export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1"

cleanup() {
  echo "Cleaning up brokers..."
  pkill -f "./embarlet" 2>/dev/null || true
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  sleep 2
}

wait_for_broker_ready() {
  local expected_pid=$1
  local timeout=$2
  local elapsed=0
  local start_time=$(date +%s)
  while [ $elapsed -lt $timeout ]; do
    local ready_file="/tmp/embarlet_${expected_pid}_ready"
    [ -f "$ready_file" ] && { rm -f "$ready_file"; echo "Broker ready (PID $expected_pid) after ${elapsed}s"; return 0; }
    local any_ready=$(find /tmp -name "embarlet_*_ready" -newermt "@${start_time}" 2>/dev/null | head -1)
    if [ -n "$any_ready" ]; then
      local file_pid=$(basename "$any_ready" | sed 's/embarlet_\([0-9]*\)_ready/\1/')
      if kill -0 "$file_pid" 2>/dev/null; then
        local current=$file_pid
        for _ in 1 2 3 4 5; do
          [ "$current" = "$expected_pid" ] && { rm -f "$any_ready"; echo "Broker ready (PID $file_pid) after ${elapsed}s"; return 0; }
          current=$(ps -o ppid= -p "$current" 2>/dev/null | tr -d ' ')
          [ -z "$current" ] || [ "$current" = "1" ] && break
        done
      fi
    fi
    kill -0 "$expected_pid" 2>/dev/null || { [ $elapsed -ge 5 ] && echo "Broker $expected_pid died"; return 1; }
    sleep 0.5
    elapsed=$(($(date +%s) - start_time))
  done
  echo "Broker $expected_pid not ready within ${timeout}s"
  return 1
}

wait_for_all_followers() {
  local timeout=$1
  shift
  local pids=("$@")
  local start_time=$(date +%s)
  local elapsed=0
  while [ $elapsed -lt $timeout ]; do
    local all=1
    for pid in "${pids[@]}"; do
      [ -f "/tmp/embarlet_${pid}_ready" ] || all=0
    done
    [ "$all" = "1" ] && {
      for pid in "${pids[@]}"; do rm -f "/tmp/embarlet_${pid}_ready" 2>/dev/null; done
      echo "All ${#pids[@]} followers ready after ${elapsed}s"
      return 0
    }
    sleep 0.5
    elapsed=$(($(date +%s) - start_time))
  done
  echo "Followers not all ready within ${timeout}s"
  return 1
}

cleanup

echo "=========================================="
echo "Bandwidth: 3 trials × 10 GB, publish-only (Order 0)"
echo "=========================================="

mkdir -p "$(dirname "$DATA_PUB_CSV")"
RESULTS=()

for trial in 1 2 3; do
  echo "--- Trial $trial / $NUM_TRIALS ---"
  cleanup
  sleep 1

  # Start brokers from build/bin (config paths relative to build/bin)
  # Save logs per trial so profile (10s snapshot) can be parsed from longer runs
  cd "$BIN_DIR"

  $EMBARLET_NUMA_BIND ./embarlet --config ../../config/embarcadero_sequencer_only.yaml --head --EMBARCADERO > broker_0_trial${trial}.log 2>&1 &
  sleep 1
  head_pid=$(pgrep -f "embarlet.*--head" 2>/dev/null | head -1)
  [ -z "$head_pid" ] && head_pid=$(ps --ppid $! -o pid= --no-headers 2>/dev/null | tr -d ' ' | head -1)
  [ -z "$head_pid" ] && head_pid=$!
  echo "Head broker PID: $head_pid"

  if ! wait_for_broker_ready "$head_pid" 60; then
    echo "Head broker failed to become ready, skipping trial $trial"
    cleanup
    continue
  fi

  for ((i = 1; i < NUM_BROKERS; i++)); do
    $EMBARLET_NUMA_BIND ./embarlet --config ../../config/embarcadero.yaml > broker_${i}_trial${trial}.log 2>&1 &
  done
  sleep 2
  follower_pids=()
  for pid in $(pgrep -f "embarlet.*--config" 2>/dev/null); do
    [ "$pid" = "$head_pid" ] && continue
    follower_pids+=("$pid")
  done
  echo "Follower PIDs: ${follower_pids[*]}"
  if [ ${#follower_pids[@]} -gt 0 ]; then
    wait_for_all_followers 25 "${follower_pids[@]}" || true
  fi

  echo "All brokers ready. Running throughput_test (10 GB) from project root..."
  cd "$PROJECT_ROOT"

  set +e
  stdbuf -oL -eL "$BIN_DIR/throughput_test" \
    --config "$CONFIG_DIR/client.yaml" \
    -m $MESSAGE_SIZE \
    -s $TOTAL_MESSAGE_SIZE \
    --record_results \
    -t $TEST_TYPE \
    -o $ORDER \
    -a $ACK \
    --sequencer $SEQUENCER \
    -l 0
  code=$?
  set -e
  if [ $code -ne 0 ]; then
    echo "throughput_test trial $trial exited with code $code"
  fi
  if [ -f "$DATA_PUB_CSV" ]; then
    bw=$(tail -1 "$DATA_PUB_CSV" | awk -F',' '{print $12}' | tr -d ' ')
    [ -n "$bw" ] && RESULTS+=("$bw") && echo "  Bandwidth: $bw MB/s"
  fi
  sleep 1
done

cleanup

echo ""
echo "=========================================="
echo "Results (pub_bandwidth_mbps)"
echo "=========================================="
for i in "${!RESULTS[@]}"; do
  echo "  Trial $((i+1)): ${RESULTS[i]} MB/s"
done
if [ ${#RESULTS[@]} -ge 1 ]; then
  avg=$(echo "${RESULTS[@]}" | tr ' ' '\n' | awk '{s+=$1; n++} END {if(n) printf "%.2f", s/n}')
  echo "  Mean: $avg MB/s"
fi
echo "=========================================="
