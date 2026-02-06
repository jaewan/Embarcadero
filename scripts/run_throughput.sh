#!/bin/bash
# Ensure we run from project root (works when invoked from any directory, e.g. measure_bandwidth_proper.sh)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Throughput test: all brokers are data ingestion (no sequencer-only head).
# This ensures broker 0 accepts publishes; required for Order 0/5 ACK=1 to get ACKs from all brokers.
export ALL_INGESTION=1
# Force head (broker 0) to be an ingestion broker so it gets PBR and advances CompletionVector (fixes B0 ACKs=0).
[ -n "${ALL_INGESTION}" ] && [ "${ALL_INGESTION}" = "1" ] && export EMBARCADERO_IS_SEQUENCER_NODE=0

# Navigate to build/bin directory
if [ ! -d "build/bin" ]; then
    echo "Error: Cannot find build/bin directory (expected $PROJECT_ROOT/build/bin)"
    exit 1
fi
cd build/bin

# Explicit config argument for binaries (relative to build/bin)
# ALL_INGESTION=1 (set above): head uses embarcadero.yaml, all 4 brokers accept publishes.
# To use sequencer-only head (broker 0 no data path), run with ALL_INGESTION=0 in env before this script.
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
  # Clean up any stale ready signal files
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  # Give processes a moment to exit
  sleep 1
}

cleanup

# Kernel socket buffers: REQUIRED for 10-12 GB/s. Without this, Linux caps SO_RCVBUF/SO_SNDBUF to ~208KB.
# Attempt tuning by default so ORDER=0 ACK=1 10GB can reach 10+ GB/s. Set EMBARCADERO_SKIP_KERNEL_TUNE=1 to skip.
if [ -z "${EMBARCADERO_SKIP_KERNEL_TUNE:-}" ]; then
  if (cd "$PROJECT_ROOT" && ./scripts/tune_kernel_buffers.sh 2>/dev/null); then
    echo "Kernel socket buffers tuned for high throughput (10+ GB/s)."
  else
    echo "Note: Kernel buffer tune skipped or failed (need: sudo ./scripts/tune_kernel_buffers.sh). For 10-12 GB/s run that once."
  fi
fi

# PERF OPTIMIZED: Enable hugepages by default for 9GB/s+ performance
# Runtime hugepage allocation with 256MB buffers provides optimal performance
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}

# NUMA Optimization: Bind embarlet processes to node 1 (closest to CXL node 2)
# This reduces memory access latency from 255x to 50x
# No CPU pinning - let OS schedule threads across all cores on node 1
EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1"

NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-1}
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
sequencer=EMBARCADERO

# Removed wait_for_signal function - using sleep-based timing instead

# Function to start a process and return the actual embarlet process PID
start_process() {
  local command=$1
  eval "$command" &
  local shell_pid=$!
  sleep 2  # Wait for process to start and fork
  
  # Find the actual embarlet process PID
  # Strategy: Find embarlet process that's a descendant of shell_pid
  local actual_pid=""
  
  # Get all embarlet PIDs and check which one is a descendant
  local all_embarlet_pids=$(pgrep -f "embarlet.*--config" 2>/dev/null)
  
  for pid in $all_embarlet_pids; do
    # Walk up the process tree to see if shell_pid is an ancestor
    local current=$pid
    local found=0
    for i in {1..10}; do  # Max 10 levels
      local parent=$(ps -o ppid= -p $current 2>/dev/null | tr -d ' ')
      if [ -z "$parent" ] || [ "$parent" = "1" ]; then
        break
      fi
      if [ "$parent" = "$shell_pid" ]; then
        found=1
        break
      fi
      current=$parent
    done
    if [ "$found" = "1" ]; then
      actual_pid=$pid
      break
    fi
  done
  
  # Fallback: use pgrep if descendant search failed
  if [ -z "$actual_pid" ]; then
    actual_pid=$(pgrep -f "embarlet.*--head" 2>/dev/null | head -1)
    [ -z "$actual_pid" ] && actual_pid=$(pgrep -f "embarlet.*--config" 2>/dev/null | head -1)
  fi
  
  # Last resort
  [ -z "$actual_pid" ] && actual_pid=$shell_pid
  
  echo "Started process: shell_pid=$shell_pid, actual_pid=$actual_pid" >&2
  printf "%d\n" $actual_pid  # Return PID via stdout (use printf to avoid issues)
}

# Array to store process IDs (must be declared before start_process uses it)
declare -a pids=()

# Removed pipe creation - using sleep-based timing instead

# Helper function to wait for broker readiness signal
wait_for_broker_ready() {
  local expected_pid=$1
  local timeout=$2
  local elapsed=0
  local start_time=$(date +%s)

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
    
    # Also check for any new ready file (fallback - broker may have different PID due to numactl forking)
    # The ready file is created by embarlet using getpid(), which is the actual embarlet PID
    # But expected_pid might be numactl's PID, so we need to check if the ready file's PID
    # is a descendant of expected_pid
    # [[FIX]]: Only check files created AFTER we started waiting (prevent race with previous runs)
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
			# [[FIX]]: Include trial number in log filename to prevent overwriting
			# [[SEQUENCER_ONLY]]: Head broker uses sequencer-only config (no data path)
			$EMBARLET_NUMA_BIND ./embarlet $HEAD_CONFIG_ARG --head --$sequencer > broker_0_trial${trial}.log 2>&1 &
			shell_pid=$!
			sleep 0.5
				# Find actual embarlet PID (numactl execs into embarlet, so child is embarlet)
				head_pid=$(pgrep -f "embarlet.*--head" 2>/dev/null | head -1)
				if [ -z "$head_pid" ]; then
				# Fallback: check if child is embarlet
				child=$(ps --ppid $shell_pid -o pid=,comm= --no-headers 2>/dev/null | grep embarlet | awk '{print $1}')
				[ -n "$child" ] && head_pid=$child || head_pid=$shell_pid
				fi
				# [[FIX]]: Clear pids array at start of each trial
				pids=()
				pids+=($head_pid)
				echo "Started head broker with PID $head_pid"

				# Wait for head broker to signal readiness (60s max; healthy startup ~5–15s)
				if ! wait_for_broker_ready "$head_pid" 60; then
					echo "Head broker failed to initialize, aborting trial"
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
				sleep 0.5
				# Discover embarlet PIDs for each started process (one per shell PID)
				follower_pids=()
				for broker_shell_pid in "${broker_shell_pids[@]}"; do
				  broker_pid=""
				  all_embarlet_pids=$(pgrep -f "embarlet.*--config" 2>/dev/null | grep -v "embarlet.*--head" || true)
				  for pid in $all_embarlet_pids; do
				    current=$pid
				    for j in {1..5}; do
				      parent=$(ps -o ppid= -p $current 2>/dev/null | tr -d ' ')
				      if [ -z "$parent" ] || [ "$parent" = "1" ]; then break; fi
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
				  sleep 1
				  cleanup
				  continue 2
				fi
				echo "All brokers ready, cluster formed"

				# Run throughput test in foreground; stream output to terminal
				# No NUMA binding for client - let OS optimize placement
			# Total message size: 8GB (8589934592 bytes) for bandwidth measurement
			# Longer ACK timeout when ack=1 so test can complete (all-ingestion or backpressure can delay ACKs).
			if [ "$ack" = "1" ]; then
				export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-60}"
			fi
			# Threads per broker: use 1 when NUM_BROKERS=1; otherwise 4 for 10+ GB/s (was 3; 4×4=16 threads saturates better).
			THREADS_PER_BROKER=${THREADS_PER_BROKER:-$([ "$NUM_BROKERS" = "1" ] && echo 1 || echo 4)}
			stdbuf -oL -eL ./throughput_test --config ../../config/client.yaml -n $THREADS_PER_BROKER -m $msg_size -s $TOTAL_MESSAGE_SIZE --record_results -t $test_case -o $order -a $ack --sequencer $sequencer -l 0 2>&1 | tee "throughput_test_trial${trial}.log"
			test_exit_code=${PIPESTATUS[0]}
			if [ $test_exit_code -ne 0 ]; then
				echo "ERROR: Throughput test failed with exit code $test_exit_code"
				# Still clean up brokers even if test failed
			fi
			# Test completed - graceful shutdown so brokers can drain (EpochSequencerThread2, etc.)
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

			# Clear pids array and clean up log files from this trial
			pids=()
			if [ -d "../../data/throughput/logs" ]; then
				mkdir -p "../../data/throughput/logs/trial_${trial}_$(date +%Y%m%d_%H%M%S)"
				mv broker_*_trial${trial}.log ../../data/throughput/logs/trial_${trial}_$(date +%Y%m%d_%H%M%S)/ 2>/dev/null || true
			fi
			sleep 0.5
			cleanup
			sleep 1
		done
		done
	done
done

echo "All experiments have finished."
