#!/bin/bash
# run_throughput.sh - Optimized for fast, clean throughput benchmarking.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# --- Configuration ---
NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-1}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-8589934592} # 8GB default
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TEST_TYPE=${TEST_TYPE:-1}
ORDER=${ORDER:-5}
ACK=${ACK:-1}
SEQUENCER=${SEQUENCER:-EMBARCADERO}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-$([ "$NUM_BROKERS" = "1" ] && echo 1 || echo 4)}
QUIET=${QUIET:-0}

# --- Environment Setup ---
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
export EMBARCADERO_CXL_ZERO_MODE=${EMBARCADERO_CXL_ZERO_MODE:-full}
export EMBARCADERO_RUNTIME_MODE=${EMBARCADERO_RUNTIME_MODE:-throughput}
if [ -z "${EMBARCADERO_CXL_SHM_NAME:-}" ]; then
  export EMBARCADERO_CXL_SHM_NAME="/CXL_SHARED_THROUGHPUT_${UID}"
  shm_unlink "$EMBARCADERO_CXL_SHM_NAME" 2>/dev/null || true
fi

# NUMA binding
EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"
CLIENT_NUMA_BIND="numactl --cpunodebind=0 --membind=0"
if [[ "$SEQUENCER" == "CORFU" ]]; then
  EMBARLET_NUMA_BIND=""
  CLIENT_NUMA_BIND=""
fi

cd build/bin || { echo "Error: build/bin not found"; exit 1; }

# --- Helper Functions ---
cleanup() {
  [ "$QUIET" != "1" ] && echo "Cleaning up..."
  pkill -9 -f "./embarlet" >/dev/null 2>&1
  pkill -9 -f "throughput_test" >/dev/null 2>&1
  pkill -9 -f "corfu_global_sequencer" >/dev/null 2>&1
  rm -f /tmp/embarlet_*_ready 2>/dev/null
  shm_unlink "$EMBARCADERO_CXL_SHM_NAME" 2>/dev/null || true
  sleep 1
}

wait_for_brokers() {
  local timeout=$1
  shift
  local expected_count=$1
  local start=$(date +%s)
  echo "Waiting for $expected_count brokers to signal readiness..."
  while true; do
    local ready_files=(/tmp/embarlet_*_ready)
    local ready=0
    if [ -e "${ready_files[0]}" ]; then
      ready=${#ready_files[@]}
    fi
    [ "$ready" -ge "$expected_count" ] && return 0
    [ $(($(date +%s) - start)) -gt "$timeout" ] && return 1
    sleep 0.1
  done
}

# --- Main Execution ---
overall_status=0
cleanup

for ((trial=1; trial<=NUM_TRIALS; trial++)); do
  echo "=== Trial $trial ($SEQUENCER Order $ORDER, $NUM_BROKERS brokers, msg=$MESSAGE_SIZE) ==="
  
  # Start Sequencer if needed
  if [[ "$SEQUENCER" == "CORFU" ]]; then
    ./corfu_global_sequencer > /tmp/corfu_sequencer.log 2>&1 &
    sleep 1
  fi

  # Start Brokers
  # Head
  $EMBARLET_NUMA_BIND ./embarlet --config ../../config/embarcadero.yaml --head --$SEQUENCER > broker_0.log 2>&1 &

  # Followers
  for ((i=1; i<NUM_BROKERS; i++)); do
    $EMBARLET_NUMA_BIND ./embarlet --config ../../config/embarcadero.yaml > broker_$i.log 2>&1 &
  done

  if ! wait_for_brokers 60 $NUM_BROKERS; then
    echo "ERROR: Brokers failed to start"
    overall_status=1
    cleanup
    continue
  fi
  rm -f /tmp/embarlet_*_ready

  # Run Test
  echo "Running throughput test (type $TEST_TYPE)..."
  $CLIENT_NUMA_BIND ./throughput_test --config ../../config/client.yaml -n $THREADS_PER_BROKER -m $MESSAGE_SIZE -s $TOTAL_MESSAGE_SIZE -t $TEST_TYPE -o $ORDER -a $ACK --sequencer $SEQUENCER -l 0 -r 0
  [ ${PIPESTATUS[0]} -ne 0 ] && overall_status=1

  cleanup
done

echo "Done."
exit $overall_status
