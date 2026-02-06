#!/bin/bash
# Broker failure test: start cluster, run throughput_test with mid-run broker kill, then plot.
# Ensure we run from project root (works when invoked from any directory).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

if [ ! -d "build/bin" ]; then
    echo "Error: Cannot find build/bin directory (expected $PROJECT_ROOT/build/bin)"
    exit 1
fi
cd build/bin

# Config for brokers (same as run_throughput.sh)
ALL_INGESTION=${ALL_INGESTION:-1}
if [ -n "${ALL_INGESTION}" ] && [ "${ALL_INGESTION}" = "1" ]; then
  HEAD_CONFIG_ARG="--config ../../config/embarcadero.yaml"
else
  HEAD_CONFIG_ARG="--config ../../config/embarcadero_sequencer_only.yaml"
fi
CONFIG_ARG="--config ../../config/embarcadero.yaml"

# Cleanup any stale processes/ports from previous runs
cleanup() {
  echo "Cleaning up stale brokers and ports..."
  pkill -f "./embarlet" >/dev/null 2>&1 || true
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  sleep 1
}

cleanup

# Optional kernel buffer tuning
if [ -n "${EMBARCADERO_TUNE_KERNEL_BUFFERS:-}" ]; then
  (cd "$PROJECT_ROOT" && ./scripts/tune_kernel_buffers.sh) || echo "Warning: kernel buffer tune failed. Continuing."
fi

export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1}"

# --- Failure test parameters ---
NUM_BROKERS=${NUM_BROKERS:-4}
FAILURE_PERCENTAGE=${FAILURE_PERCENTAGE:-0.15}
NUM_BROKERS_TO_KILL=${NUM_BROKERS_TO_KILL:-1}
NUM_TRIALS=${NUM_TRIALS:-1}
test_cases=(4)
# 20 GiB total message size (same order of magnitude as original 21474836480)
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-21474836480}
ORDER=${ORDER:-0}
ack=${ACK:-1}
sequencer=EMBARCADERO

# Ensure failure data directory exists (client writes real_time_acked_throughput.csv and failure_events.csv here)
FAILURE_DATA_DIR="$PROJECT_ROOT/data/failure"
mkdir -p "$FAILURE_DATA_DIR"

# Wait for a single broker to signal readiness (file-based, non-blocking)
wait_for_broker_ready() {
  local expected_pid=$1
  local timeout=$2
  local elapsed=0
  local start_time=$(date +%s)

  echo "Waiting for broker to signal readiness (timeout: ${timeout}s, PID: $expected_pid)..."
  while [ $elapsed -lt $timeout ]; do
    local ready_file="/tmp/embarlet_${expected_pid}_ready"
    if [ -f "$ready_file" ]; then
      echo "Broker ready: $ready_file (PID: $expected_pid) after ${elapsed}s"
      rm -f "$ready_file"
      return 0
    fi
    local any_ready_file=$(find /tmp -name "embarlet_*_ready" -newermt "@${start_time}" 2>/dev/null | head -1)
    if [ -n "$any_ready_file" ] && [ -f "$any_ready_file" ]; then
      local file_pid=$(basename "$any_ready_file" | sed 's/embarlet_\([0-9]*\)_ready/\1/')
      if kill -0 "$file_pid" 2>/dev/null; then
        if [ "$file_pid" = "$expected_pid" ]; then
          echo "Broker ready: $any_ready_file (PID: $file_pid) after ${elapsed}s"
          rm -f "$any_ready_file"
          return 0
        fi
        current_pid=$file_pid
        for _ in 1 2 3 4 5; do
          ppid=$(ps -o ppid= -p "$current_pid" 2>/dev/null | tr -d ' ')
          [ -z "$ppid" ] || [ "$ppid" = "1" ] && break
          if [ "$ppid" = "$expected_pid" ]; then
            echo "Broker ready: $any_ready_file (PID: $file_pid, descendant of $expected_pid) after ${elapsed}s"
            rm -f "$any_ready_file"
            return 0
          fi
          current_pid=$ppid
        done
      fi
    fi
    if ! kill -0 "$expected_pid" 2>/dev/null && [ $elapsed -ge 5 ]; then
      echo "ERROR: Broker process $expected_pid died before signaling readiness (after ${elapsed}s)"
      return 1
    fi
    sleep 0.1
    elapsed=$(($(date +%s) - start_time))
  done
  echo "ERROR: Broker failed to signal readiness in ${timeout}s (PID: $expected_pid)"
  return 1
}

# Wait for all given broker PIDs to signal readiness
wait_for_all_brokers_ready() {
  local timeout=$1
  shift
  local pids=("$@")
  local start_time=$(date +%s)
  local elapsed=0
  local n=${#pids[@]}

  echo "Waiting for $n broker(s) to signal readiness (timeout: ${timeout}s, PIDs: ${pids[*]})..."
  while [ $elapsed -lt $timeout ]; do
    local all_ready=1
    for expected_pid in "${pids[@]}"; do
      if [ ! -f "/tmp/embarlet_${expected_pid}_ready" ]; then
        all_ready=0
        if ! kill -0 "$expected_pid" 2>/dev/null && [ $elapsed -ge 5 ]; then
          echo "ERROR: Broker process $expected_pid died before signaling readiness"
          return 1
        fi
      fi
    done
    if [ "$all_ready" = "1" ]; then
      echo "All $n broker(s) ready after ${elapsed}s"
      for expected_pid in "${pids[@]}"; do
        rm -f "/tmp/embarlet_${expected_pid}_ready" 2>/dev/null || true
      done
      return 0
    fi
    sleep 0.1
    elapsed=$(($(date +%s) - start_time))
  done
  echo "ERROR: Not all brokers signaled readiness in ${timeout}s"
  return 1
}

# --- Run failure trials ---
for test_case in "${test_cases[@]}"; do
  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
    echo "=== Failure trial $trial / $NUM_TRIALS (test_case=$test_case, kill $NUM_BROKERS_TO_KILL broker(s) at ${FAILURE_PERCENTAGE}% of data) ==="

    pids=()
    # Start head broker
    $EMBARLET_NUMA_BIND ./embarlet $HEAD_CONFIG_ARG --head --$sequencer > broker_0_trial${trial}.log 2>&1 &
    shell_pid=$!
    sleep 0.5
    head_pid=$(pgrep -f "embarlet.*--head" 2>/dev/null | head -1)
    if [ -z "$head_pid" ]; then
      child=$(ps --ppid $shell_pid -o pid=,comm= --no-headers 2>/dev/null | grep embarlet | awk '{print $1}')
      [ -n "$child" ] && head_pid=$child || head_pid=$shell_pid
    fi
    pids+=($head_pid)
    echo "Started head broker with PID $head_pid"

    if ! wait_for_broker_ready "$head_pid" 60; then
      echo "Head broker failed to initialize, aborting trial"
      for pid in "${pids[@]}"; do kill $pid 2>/dev/null || true; done
      pids=()
      cleanup
      continue
    fi

    # Start follower brokers in parallel
    broker_shell_pids=()
    for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
      $EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG > broker_${i}_trial${trial}.log 2>&1 &
      broker_shell_pids+=($!)
    done
    sleep 0.5
    follower_pids=()
    for broker_shell_pid in "${broker_shell_pids[@]}"; do
      broker_pid=""
      for pid in $(pgrep -f "embarlet.*--config" 2>/dev/null | grep -v "embarlet.*--head" || true); do
        current=$pid
        for j in 1 2 3 4 5; do
          parent=$(ps -o ppid= -p $current 2>/dev/null | tr -d ' ')
          [ -z "$parent" ] || [ "$parent" = "1" ] && break
          if [ "$parent" = "$broker_shell_pid" ]; then
            broker_pid=$pid
            break 2
          fi
          current=$parent
        done
      done
      [ -z "$broker_pid" ] && broker_pid=$(ps --ppid $broker_shell_pid -o pid= --no-headers 2>/dev/null | tr -d ' ' | head -1)
      [ -z "$broker_pid" ] && broker_pid=$broker_shell_pid
      follower_pids+=($broker_pid)
      pids+=($broker_pid)
    done
    echo "Started follower brokers with PIDs: ${follower_pids[*]}"

    if ! wait_for_all_brokers_ready 20 "${follower_pids[@]}"; then
      echo "One or more followers failed to initialize, aborting trial"
      for pid in "${pids[@]}"; do kill $pid 2>/dev/null || true; done
      pids=()
      cleanup
      continue
    fi
    echo "All brokers ready, cluster formed. Starting failure throughput test..."

    # Longer ACK timeout when ack>=1 (failure + redirect can delay ACKs)
    if [ "$ack" != "0" ]; then
      export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-90}"
    fi
    THREADS_PER_BROKER=${THREADS_PER_BROKER:-$([ "$NUM_BROKERS" = "1" ] && echo 1 || echo 3)}

    # Run failure test in foreground (test_case 4 = broker failure at publish)
    stdbuf -oL -eL ./throughput_test --config ../../config/client.yaml -n $THREADS_PER_BROKER -m ${MESSAGE_SIZE:-1024} \
      -s $TOTAL_MESSAGE_SIZE --record_results -t $test_case \
      --num_brokers_to_kill $NUM_BROKERS_TO_KILL --failure_percentage $FAILURE_PERCENTAGE \
      -o $ORDER -a $ack --sequencer $sequencer -l 0
    test_exit_code=$?
    if [ $test_exit_code -ne 0 ]; then
      echo "ERROR: Failure throughput test exited with code $test_exit_code"
    fi

    # Clean up broker processes
    echo "Test completed, cleaning up broker processes..."
    for pid in "${pids[@]}"; do
      kill $pid 2>/dev/null || true
      echo "Terminated broker PID $pid"
    done
    pids=()
    sleep 0.5
    cleanup
    sleep 1
  done
done

# Plot results (client writes CSV under data/failure when HOME or path is set; plot from project root)
if [ -f "$FAILURE_DATA_DIR/real_time_acked_throughput.csv" ]; then
  echo "Plotting failure run results..."
  python3 "$PROJECT_ROOT/scripts/plot/plot_failure.py" \
    "$FAILURE_DATA_DIR/real_time_acked_throughput.csv" failure \
    --events "$FAILURE_DATA_DIR/failure_events.csv" 2>/dev/null || true
else
  echo "No real_time_acked_throughput.csv found in $FAILURE_DATA_DIR (client may write to \$HOME/Embarcadero/data/failure). Skipping plot."
  if [ -n "$HOME" ] && [ -f "$HOME/Embarcadero/data/failure/real_time_acked_throughput.csv" ]; then
    python3 "$PROJECT_ROOT/scripts/plot/plot_failure.py" \
      "$HOME/Embarcadero/data/failure/real_time_acked_throughput.csv" failure \
      --events "$HOME/Embarcadero/data/failure/failure_events.csv" 2>/dev/null || true
  fi
fi

echo "All failure experiments have finished."
