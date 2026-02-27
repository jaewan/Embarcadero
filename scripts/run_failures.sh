#!/bin/bash
# Broker failure test: start cluster, run throughput_test with mid-run broker kill, then plot.
# Ensure we run from project root (works when invoked from any directory).
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

if [ ! -d "build/bin" ]; then
    echo "Error: Cannot find build/bin directory (expected $PROJECT_ROOT/build/bin)"
    exit 1
fi
cd build/bin

# Config for brokers
HEAD_CONFIG_ARG="--config ../../config/embarcadero.yaml"
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
export EMBARCADERO_RUNTIME_MODE=${EMBARCADERO_RUNTIME_MODE:-failure}

# Auto-detect NUMA: use numactl only if available and >1 NUMA node exists.
# Override with EMBARLET_NUMA_BIND="" to disable entirely.
if [ -z "${EMBARLET_NUMA_BIND+x}" ]; then
  if command -v numactl &>/dev/null && [ "$(numactl --hardware 2>/dev/null | grep -c 'available:.*nodes')" -gt 0 ]; then
    numa_nodes=$(numactl --hardware 2>/dev/null | awk '/^available:/{print $2}')
    if [ "${numa_nodes:-1}" -gt 1 ]; then
      EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1"
      echo "NUMA detected ($numa_nodes nodes). Using: $EMBARLET_NUMA_BIND"
    else
      EMBARLET_NUMA_BIND=""
      echo "Single NUMA node detected. Running without NUMA binding."
    fi
  else
    EMBARLET_NUMA_BIND=""
    echo "numactl not found or NUMA not available. Running without NUMA binding."
  fi
fi

# --- Failure test parameters ---
NUM_BROKERS=${NUM_BROKERS:-4}
FAILURE_PERCENTAGE=${FAILURE_PERCENTAGE:-0.5}
NUM_BROKERS_TO_KILL=${NUM_BROKERS_TO_KILL:-1}
NUM_TRIALS=${NUM_TRIALS:-1}
test_cases=(4)
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-21474836480}
ORDER=${ORDER:-0}
ack=${ACK:-1}
sequencer=${SEQUENCER:-EMBARCADERO}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}

# Ensure failure data directory exists (absolute path so client can write there)
FAILURE_DATA_DIR="$(cd "$PROJECT_ROOT" && pwd)/data/failure"
mkdir -p "$FAILURE_DATA_DIR"
# Client writes to this dir when env is set (see publisher.cc, test_utils.cc)
export EMBARCADERO_FAILURE_DATA_DIR="$FAILURE_DATA_DIR"

echo "===== Failure Benchmark Configuration ====="
echo "  Brokers:          $NUM_BROKERS"
echo "  Kill:             $NUM_BROKERS_TO_KILL broker(s) at ${FAILURE_PERCENTAGE} of data"
echo "  Total data:       $TOTAL_MESSAGE_SIZE bytes"
echo "  Message size:     $MESSAGE_SIZE bytes"
echo "  Order:            $ORDER"
echo "  ACK level:        $ack"
echo "  Sequencer:        $sequencer"
echo "  Trials:           $NUM_TRIALS"
echo "  Output dir:       $FAILURE_DATA_DIR"
echo "============================================"

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
      [ -z "$expected_pid" ] && continue
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
    echo ""
    echo "================================================================="
    echo "=== Failure trial $trial / $NUM_TRIALS (test_case=$test_case, kill $NUM_BROKERS_TO_KILL broker(s) at ${FAILURE_PERCENTAGE} of data) ==="
    echo "================================================================="

    pids=()
    # Start head broker; $! is the PID of the background embarlet process
    $EMBARLET_NUMA_BIND ./embarlet $HEAD_CONFIG_ARG --head --$sequencer > broker_0_trial${trial}.log 2>&1 &
    head_pid=$!
    pids+=($head_pid)
    echo "Started head broker with PID $head_pid (log: broker_0_trial${trial}.log)"

    if ! wait_for_broker_ready "$head_pid" 60; then
      echo "Head broker failed to initialize, aborting trial"
      for pid in "${pids[@]}"; do kill $pid 2>/dev/null || true; done
      pids=()
      cleanup
      continue
    fi

    echo "Starting follower brokers..."
    # Start follower brokers in parallel
    broker_shell_pids=()
    for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
      $EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG > broker_${i}_trial${trial}.log 2>&1 &
      broker_shell_pids+=($!)
    done
    sleep 0.5
    # $! from each "cmd &" is the PID of that process; use them directly as follower PIDs
    follower_pids=("${broker_shell_pids[@]}")
    for pid in "${follower_pids[@]}"; do pids+=($pid); done
    echo "Started follower brokers with PIDs: ${follower_pids[*]} (waiting up to 90s for ready)"

    if ! wait_for_all_brokers_ready 90 "${follower_pids[@]}"; then
      echo "One or more followers failed to initialize (timeout 90s), aborting trial"
      for pid in "${pids[@]}"; do kill $pid 2>/dev/null || true; done
      pids=()
      cleanup
      continue
    fi
    echo "All $NUM_BROKERS brokers ready, cluster formed."

    # Longer ACK timeout when ack>=1 (failure + redirect can delay ACKs)
    if [ "$ack" != "0" ]; then
      export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-120}"
    fi
    THREADS_PER_BROKER=${THREADS_PER_BROKER:-$([ "$NUM_BROKERS" = "1" ] && echo 1 || echo 3)}

    echo "Starting failure throughput test..."
    echo "  threads_per_broker=$THREADS_PER_BROKER, message_size=$MESSAGE_SIZE"
    echo "  Throughput/events will be written to: $FAILURE_DATA_DIR"

    # Run failure test with timeout to prevent infinite hang (Bug: publish loop can stall on ACK backpressure)
    CLIENT_LOG="client_failure_trial${trial}.log"
    CLIENT_TIMEOUT=${CLIENT_TIMEOUT:-300}  # 5 minutes max
    set +e
    timeout --signal=TERM --kill-after=10 "$CLIENT_TIMEOUT" \
      stdbuf -oL -eL ./throughput_test --config ../../config/client.yaml \
      -n $THREADS_PER_BROKER -m $MESSAGE_SIZE \
      -s $TOTAL_MESSAGE_SIZE --record_results -t $test_case \
      --num_brokers_to_kill $NUM_BROKERS_TO_KILL --failure_percentage $FAILURE_PERCENTAGE \
      -o $ORDER -a $ack --sequencer $sequencer -l 0 2>&1 | tee "$CLIENT_LOG"
    test_exit_code=${PIPESTATUS[0]}
    set -e

    if [ $test_exit_code -ne 0 ]; then
      echo "WARNING: Failure throughput test exited with code $test_exit_code"
      echo "  Check $CLIENT_LOG and broker logs for details."
    else
      echo "Failure throughput test completed successfully."
    fi

    # Clean up broker processes
    echo "Cleaning up broker processes..."
    for pid in "${pids[@]}"; do
      kill $pid 2>/dev/null && echo "  Terminated broker PID $pid" || echo "  Broker PID $pid already exited"
    done
    pids=()
    sleep 0.5
    cleanup
    sleep 1
  done
done

# Plot results
echo ""
echo "===== Plotting Results ====="
THROUGHPUT_CSV="$FAILURE_DATA_DIR/real_time_acked_throughput.csv"
EVENTS_CSV="$FAILURE_DATA_DIR/failure_events.csv"

if [ -f "$THROUGHPUT_CSV" ]; then
  echo "Found throughput data: $THROUGHPUT_CSV"
  if [ -f "$EVENTS_CSV" ]; then
    echo "Found event data: $EVENTS_CSV"
    python3 "$PROJECT_ROOT/scripts/plot/plot_failure.py" \
      "$THROUGHPUT_CSV" "$FAILURE_DATA_DIR/failure" \
      --events "$EVENTS_CSV" 2>&1 || echo "Warning: plot generation failed"
  else
    python3 "$PROJECT_ROOT/scripts/plot/plot_failure.py" \
      "$THROUGHPUT_CSV" "$FAILURE_DATA_DIR/failure" 2>&1 || echo "Warning: plot generation failed"
  fi
else
  echo "No throughput CSV found at $THROUGHPUT_CSV"
  # Fallback: check HOME-relative path (client writes to $HOME/Embarcadero/data/failure/)
  ALT_CSV="$HOME/Embarcadero/data/failure/real_time_acked_throughput.csv"
  ALT_EVENTS="$HOME/Embarcadero/data/failure/failure_events.csv"
  if [ -f "$ALT_CSV" ]; then
    echo "Found throughput data at alternate path: $ALT_CSV"
    python3 "$PROJECT_ROOT/scripts/plot/plot_failure.py" \
      "$ALT_CSV" "$FAILURE_DATA_DIR/failure" \
      --events "$ALT_EVENTS" 2>&1 || echo "Warning: plot generation failed"
  else
    echo "No throughput data found. Skipping plot."
  fi
fi

echo ""
echo "All failure experiments have finished."
