#!/bin/bash
# Ensure we run from project root (works when invoked from any directory, e.g. measure_bandwidth_proper.sh)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Throughput test: all brokers are data ingestion (head and followers use same config).

# Navigate to build/bin directory
if [ ! -d "build/bin" ]; then
    echo "Error: Cannot find build/bin directory (expected $PROJECT_ROOT/build/bin)"
    exit 1
fi
cd build/bin

# Use a unique shared-memory object per script invocation unless caller pins one.
# This avoids collisions with stale shm objects that may have incompatible ownership/mode.
if [ -z "${EMBARCADERO_CXL_SHM_NAME:-}" ]; then
  export EMBARCADERO_CXL_SHM_NAME="/CXL_SHARED_FILE_${UID}_$$_$(date +%s)"
fi
echo "Using EMBARCADERO_CXL_SHM_NAME=${EMBARCADERO_CXL_SHM_NAME}"

# Config for all brokers (relative to build/bin)
HEAD_CONFIG_ARG="--config ../../config/embarcadero.yaml"
CONFIG_ARG="--config ../../config/embarcadero.yaml"

# Cleanup any stale processes/ports from previous runs.
# Hugepages (MAP_HUGETLB) are released when processes exit (or munmap in destructors).
# To avoid "hugepage shortage" on the next run, we wait briefly so the kernel can return
# pages to the pool (see EMBARCADERO_CLEANUP_SETTLE_SEC below).
cleanup() {
  echo "Cleaning up stale brokers and ports..."
  pkill -9 -f "./embarlet" >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  # Clean up any stale ready signal files
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  # Clean up PID files
  rm -f /tmp/embarlet_head_pid 2>/dev/null || true
  # Clean up stale CXL shm files (only ones we created with our UID)
  if [ -n "${EMBARCADERO_CXL_SHM_NAME:-}" ]; then
    shm_unlink "$EMBARCADERO_CXL_SHM_NAME" 2>/dev/null || true
  fi
  # Clean up any old shm files with our UID pattern
  for shm_file in /dev/shm/CXL_SHARED_FILE_$(id -u)_*; do
    if [ -e "$shm_file" ]; then
      rm -f "$shm_file" 2>/dev/null || true
    fi
  done
  # Give processes a moment to exit
  sleep 1
  # Optional: extra settle time so hugepages are back in the pool before the next run.
  # Set EMBARCADERO_CLEANUP_SETTLE_SEC=2 (or higher) if the next run often sees hugepage shortage.
  local settle="${EMBARCADERO_CLEANUP_SETTLE_SEC:-0}"
  if [ "$settle" -gt 0 ] 2>/dev/null; then
    echo "Settling ${settle}s for hugepage pool..."
    sleep "$settle"
  fi
}

cleanup

# PERF OPTIMIZED: Enable hugepages by default for 9GB/s+ performance
# Runtime hugepage allocation with 256MB buffers provides optimal performance
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}

# NUMA: CPU on node 1; allow memory on nodes 1 and 2 so mbind(..., node_2) in cxl_manager works.
# Zero-core CXL is on node 2; script must use --membind=1,2 or first-touch after mbind can SIGBUS/OOM.
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"

NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-1}
HEAD_READY_TIMEOUT_SEC=${HEAD_READY_TIMEOUT_SEC:-90}
FOLLOWER_READY_TIMEOUT_SEC=${FOLLOWER_READY_TIMEOUT_SEC:-30}
# Use test type 1 (E2E) for validation - includes subscriber and DEBUG_check_order
test_cases=(${TEST_TYPE:-1})
# Use MESSAGE_SIZE environment variable or default to multiple sizes
if [ -n "$MESSAGE_SIZE" ]; then
    msg_sizes=($MESSAGE_SIZE)
else
    #msg_sizes=(128 256 512 1024 4096 16384 65536 262144 1048576)
    msg_sizes=(1024)  # 1KB message size for 8GB test
fi

# Total message size: 8GB = 8589934592 bytes
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-8589934592}


# Change these for Scalog and Corfu
# Order level 0 for unordered, 1 for ordered (not implemented yet), 4 for strong ordering, 5 for batch-level ordering
orders=(${ORDER:-5})
ack=${ACK:-1}
sequencer=${SEQUENCER:-EMBARCADERO}

# Removed wait_for_signal function - using sleep-based timing instead

# Function to start a process and return the actual embarlet process PID via file
start_process() {
  local command=$1
  local pid_file="/tmp/embarlet_head_pid"
  eval "$command" &
  local shell_pid=$!
  sleep 2  # Wait for process to start

  # Find the actual embarlet process PID. 
  # If shell_pid is numactl, actual_pid is its child.
  local actual_pid=$shell_pid
  local child_pid=$(pgrep -P $shell_pid 2>/dev/null | head -1)
  if [ -n "$child_pid" ]; then
    actual_pid=$child_pid
  fi

  echo "Started process: shell_pid=$shell_pid, actual_pid=$actual_pid" >&2
  echo $actual_pid > "$pid_file"
}

# Array to store process IDs (must be declared before start_process uses it)
declare -a pids=()
overall_status=0

# Removed pipe creation - using sleep-based timing instead

# Helper function to wait for broker readiness signal
wait_for_broker_ready() {
  local expected_pid=$1
  local timeout=$2
  local elapsed=0
  local start_time=$(date +%s)  # Use date +%s for timing

  echo "Waiting for broker to signal readiness (timeout: ${timeout}s, PID: $expected_pid)..."

  # Wait for broker ready file - check for any new ready file (broker writes with its own PID)
  while [ $elapsed -lt $timeout ]; do
    # Check for ready file with expected PID first
    local ready_file="/tmp/embarlet_${expected_pid}_ready"
    if [ -f "$ready_file" ]; then
      echo "Broker ready! Found ready file: $ready_file (PID: $expected_pid) after ${elapsed}s"
      rm -f "$ready_file"
      return 0
    fi

    # Fallback: check for any new ready file (when expected_pid is numactl and embarlet is child).
    # Run find at most every 2s to avoid scanning /tmp every 0.1s.
    if [ $((elapsed % 2)) -eq 0 ] && [ $elapsed -ge 1 ]; then
    local any_ready_file=$(find /tmp -name "embarlet_*_ready" -newermt "@${start_time}" 2>/dev/null | head -1)
    if [ -n "$any_ready_file" ] && [ -f "$any_ready_file" ]; then
      local file_pid=$(basename "$any_ready_file" | sed 's/embarlet_\([0-9]*\)_ready/\1/')
      # Verify the process is still running
      if kill -0 $file_pid 2>/dev/null; then
        # Check if file_pid matches expected_pid (exact match)
        if [ "$file_pid" = "$expected_pid" ]; then
          echo "Broker ready! Found ready file: $any_ready_file (PID: $file_pid) after ${elapsed}s"
          rm -f "$any_ready_file"
          return 0
        fi
        # Check if file_pid is a descendant of expected_pid (expected_pid might be numactl, file_pid is embarlet)
        local current_pid=$file_pid
        local found=0
        for i in {1..5}; do  # Max 5 levels up the process tree
          local ppid=$(ps -o ppid= -p $current_pid 2>/dev/null | tr -d ' ')
          if [ -z "$ppid" ] || [ "$ppid" = "1" ]; then
            break
          fi
          if [ "$ppid" = "$expected_pid" ]; then
            found=1
            break
          fi
          current_pid=$ppid
        done
        if [ "$found" = "1" ]; then
          echo "Broker ready! Found ready file: $any_ready_file (PID: $file_pid, descendant of $expected_pid) after ${elapsed}s"
          rm -f "$any_ready_file"
          return 0
        fi
      fi
    fi
    fi

    # Check if process is still running (but don't fail immediately - it might be initializing)
    if ! kill -0 $expected_pid 2>/dev/null; then
      # Only fail if we've waited at least 5 seconds (broker initialization takes time)
      if [ $elapsed -ge 5 ]; then
        echo "ERROR: Broker process $expected_pid died before signaling readiness (after ${elapsed}s)"
        return 1
      fi
      # Otherwise, process might still be starting up, continue waiting
    fi

    # [[FIX]]: Optimize polling - use 0.1s sleep instead of 1s for faster response
    sleep 0.1
    elapsed=$(($(date +%s) - start_time))
  done

  echo "ERROR: Broker failed to signal readiness in ${timeout}s"
  echo "Expected PID: $expected_pid"
  echo "Process status: $(ps -p $expected_pid -o pid,comm,state 2>/dev/null || echo 'not found')"
  echo "Ready files found: $(ls /tmp/embarlet_*_ready 2>/dev/null | wc -l)"
  return 1
}

# Wait for multiple brokers to signal readiness (for parallel follower startup)
# Usage: wait_for_all_brokers_ready timeout pid1 pid2 pid3
# Returns 0 when all have ready files, 1 on timeout or if any process dies
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
      local ready_file="/tmp/embarlet_${expected_pid}_ready"
      if [ ! -f "$ready_file" ]; then
        all_ready=0
        # Check if process died
        if ! kill -0 $expected_pid 2>/dev/null && [ $elapsed -ge 5 ]; then
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

  echo "ERROR: Not all brokers signaled readiness in ${timeout}s (ready: $ready_count/$n)"
  return 1
}

# Run experiments for each message size
for test_case in "${test_cases[@]}"; do
	for order in "${orders[@]}"; do
		for msg_size in "${msg_sizes[@]}"; do
		  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
				echo "Running trial $trial with message size $msg_size"

			# Start the processes
			# [[FIX]]: Use start_process so we get actual embarlet PID → fast ready check via /tmp/embarlet_${pid}_ready
			pid_file="/tmp/embarlet_head_pid"
			start_process "$EMBARLET_NUMA_BIND ./embarlet $HEAD_CONFIG_ARG --head --$sequencer > broker_0_trial${trial}.log 2>&1"
			head_pid=$(cat "$pid_file" 2>/dev/null | tr -d '\n ' || echo "")
			rm -f "$pid_file"
				pids=()
				pids+=($head_pid)
				echo "Started head broker with PID $head_pid"

				# Wait for head broker to signal readiness (cold CXL setup can take 30–60s).
				if ! wait_for_broker_ready "$head_pid" "$HEAD_READY_TIMEOUT_SEC"; then
					echo "Head broker failed to initialize, aborting trial"
					overall_status=1
					# Kill all processes and skip to next trial
					for pid in "${pids[@]}"; do
						kill $pid 2>/dev/null || true
					done
					pids=()
					sleep 1
					cleanup
					continue
				fi
				
				# Start follower brokers in parallel (saves ~4–6s vs sequential)
				broker_shell_pids=()
				for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
				  $EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG > broker_${i}_trial${trial}.log 2>&1 &
				  broker_shell_pids+=($!)
				done
				sleep 0.2
				# Track spawned PIDs directly; avoids stale-process mismatches from global pgrep scans.
				follower_pids=()
				for broker_shell_pid in "${broker_shell_pids[@]}"; do
				  follower_pids+=($broker_shell_pid)
				  pids+=($broker_shell_pid)
				done
				echo "Started follower brokers with PIDs: ${follower_pids[*]}"
				if ! wait_for_all_brokers_ready "$FOLLOWER_READY_TIMEOUT_SEC" "${follower_pids[@]}"; then
				  echo "One or more followers failed to initialize, aborting trial"
				  overall_status=1
				  for pid in "${pids[@]}"; do kill $pid 2>/dev/null || true; done
				  pids=()
				  sleep 1
				  cleanup
				  continue 2
				fi
				echo "All brokers ready, cluster formed"

				# Run throughput test in foreground; stream output to terminal
				# No NUMA binding for client - let OS optimize placement
			# Total message size: 8GB (8589934592 bytes) for bandwidth measurement
			# Longer ACK timeout when ack=1 so test can complete (all-ingestion or backpressure can delay ACKs).
			# [[Phase B]] Order 5 has B0 tail-ACK convergence issues; use 180s for order=5, 120s otherwise.
			if [ "$ack" = "1" ]; then
				if [ "$order" = "5" ]; then
					export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-180}"
				else
					export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-120}"
				fi
			fi
			# Threads per broker: use 1 when NUM_BROKERS=1; otherwise 4 for 10+ GB/s (was 3; 4×4=16 threads saturates better).
			THREADS_PER_BROKER=${THREADS_PER_BROKER:-$([ "$NUM_BROKERS" = "1" ] && echo 1 || echo 4)}
			# [[FIX: Disable Replication]] Force -r 0 to measure memory-only throughput and avoid ChainReplicationManager errors/latency.
			stdbuf -oL -eL ./throughput_test --config ../../config/client.yaml -n $THREADS_PER_BROKER -m $msg_size -s $TOTAL_MESSAGE_SIZE --record_results -t $test_case -o $order -a $ack --sequencer $sequencer -l 0 -r 0 2>&1 | tee "throughput_test_trial${trial}.log"
			test_exit_code=${PIPESTATUS[0]}
			if [ $test_exit_code -ne 0 ]; then
				echo "ERROR: Throughput test failed with exit code $test_exit_code"
				overall_status=1
				# Still clean up brokers even if test failed
			fi
			# Test completed - graceful shutdown so brokers can drain (EpochSequencerThread, etc.)
			echo "Test completed, sending SIGTERM to broker processes for graceful shutdown..."
			for pid in "${pids[@]}"; do
				kill -TERM "$pid" 2>/dev/null || true
			done
			# Wait up to 10s for brokers to exit (drain can take ~3s, plus cleanup)
			for i in 1 2 3 4 5 6 7 8 9 10; do
				all_gone=true
				for pid in "${pids[@]}"; do
					kill -0 "$pid" 2>/dev/null && all_gone=false || true
				done
				$all_gone && break
				sleep 1
			done
			for pid in "${pids[@]}"; do
				if kill -0 "$pid" 2>/dev/null; then
					echo "Broker PID $pid did not exit in 10s, sending SIGKILL"
					kill -9 "$pid" 2>/dev/null || true
				else
					echo "Broker PID $pid exited gracefully"
				fi
			done

			echo "All processes have finished for trial $trial with message size $msg_size"

			# Clear pids array and save broker logs for inspection (especially on subscribe timeout)
			pids=()
			mkdir -p "../../data/throughput/logs"
			log_subdir="../../data/throughput/logs/trial_${trial}_$(date +%Y%m%d_%H%M%S)"
			mkdir -p "$log_subdir"
			mv broker_*_trial${trial}.log "$log_subdir"/ 2>/dev/null || true
			sleep 0.5
			cleanup
			sleep 1
		done
		done
	done
done

echo "All experiments have finished."
exit $overall_status
