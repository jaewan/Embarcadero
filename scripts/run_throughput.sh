#!/bin/bash

# Navigate to build/bin directory
if [ -d "build/bin" ]; then
    cd build/bin
elif [ -d "../build/bin" ]; then
    cd ../build/bin
else
    echo "Error: Cannot find build/bin directory"
    exit 1
fi

# Explicit config argument for binaries (relative to build/bin)
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

# PERF OPTIMIZED: Enable hugepages by default for 9GB/s+ performance
# Runtime hugepage allocation with 256MB buffers provides optimal performance
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}

# NUMA Optimization: Bind embarlet processes to node 1 (closest to CXL node 2)
# This reduces memory access latency from 255x to 50x
# No CPU pinning - let OS schedule threads across all cores on node 1
EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1"

NUM_BROKERS=4
NUM_TRIALS=1
test_cases=(5)
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

# Run experiments for each message size
for test_case in "${test_cases[@]}"; do
	for order in "${orders[@]}"; do
		for msg_size in "${msg_sizes[@]}"; do
		  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
				echo "Running trial $trial with message size $msg_size"

			# Start the processes
			# [[FIX]]: Include trial number in log filename to prevent overwriting
			$EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG --head --$sequencer > broker_0_trial${trial}.log 2>&1 &
			shell_pid=$!
			sleep 2
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

				# Wait for head broker to signal readiness (smart polling, not fixed sleep)
				if ! wait_for_broker_ready "$head_pid" 120; then
					echo "Head broker failed to initialize, aborting trial"
					# Kill all processes and skip to next trial
					for pid in "${pids[@]}"; do
						kill $pid 2>/dev/null || true
					done
					pids=()
					sleep 2
					cleanup
					continue
				fi
				
				# Start follower brokers
			# [[FIX]]: Track PIDs per broker to avoid non-deterministic pgrep results
			for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
			  # [[FIX]]: Include trial number in log filename to prevent overwriting
			  $EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG > broker_${i}_trial${trial}.log 2>&1 &
			  broker_shell_pid=$!
				  sleep 2
			  
			  # [[FIX]]: Find the specific embarlet PID that's a child of this broker_shell_pid
			  # This ensures we get the correct PID for THIS broker, not just any broker
			  broker_pid=""
			  max_attempts=10
			  attempt=0
			  
			  while [ -z "$broker_pid" ] && [ "${attempt:-0}" -lt "$max_attempts" ]; do
				    # First try: direct child of shell_pid
				    broker_pid=$(ps --ppid $broker_shell_pid -o pid=,comm= --no-headers 2>/dev/null | grep embarlet | awk '{print $1}' | head -1)
				    
				    # Second try: walk up process tree from all embarlet processes
				    if [ -z "$broker_pid" ]; then
				      all_embarlet_pids=$(pgrep -f "embarlet.*--config" 2>/dev/null | grep -v "embarlet.*--head")
				      for pid in $all_embarlet_pids; do
				        # Check if this pid is a descendant of broker_shell_pid
				        current=$pid
				        found=0
				        for j in {1..5}; do
				          parent=$(ps -o ppid= -p $current 2>/dev/null | tr -d ' ')
				          if [ -z "$parent" ] || [ "$parent" = "1" ]; then
				            break
				          fi
				          if [ "$parent" = "$broker_shell_pid" ]; then
				            found=1
				            break
				          fi
				          current=$parent
				        done
				        if [ "$found" = "1" ]; then
				          broker_pid=$pid
				          break
				        fi
				      done
				    fi
				    
				    if [ -z "$broker_pid" ]; then
				      sleep 0.2
				      attempt=$((attempt + 1))
				    fi
				  done
				  
				  # Fallback: use shell_pid if we couldn't find embarlet
				  [ -z "$broker_pid" ] && broker_pid=$broker_shell_pid
				  
				  # [[FIX]]: Verify this PID is not already in pids array (avoid duplicates)
				  already_tracked=0
				  for existing_pid in "${pids[@]}"; do
				    if [ "$existing_pid" = "$broker_pid" ]; then
				      already_tracked=1
				      break
				    fi
				  done
				  
				  if [ "$already_tracked" = "0" ]; then
				    pids+=($broker_pid)
				    echo "Started broker $i with PID $broker_pid"
				  else
				    echo "WARNING: Broker $i PID $broker_pid already tracked, skipping"
				    continue
				  fi

				  # Wait for broker to signal readiness (should be fast, no CXL allocation)
				  if ! wait_for_broker_ready "$broker_pid" 30; then
					  echo "Broker $i failed to initialize, aborting trial"
					  for pid in "${pids[@]}"; do
						  kill $pid 2>/dev/null || true
					  done
					  pids=()
					  sleep 2
					  cleanup
					  continue 2  # Skip to next trial
				  fi
				done
				echo "All brokers ready, cluster formed"

				# Run throughput test in foreground; stream output to terminal
				# No NUMA binding for client - let OS optimize placement
			# Total message size: 8GB (8589934592 bytes) for bandwidth measurement
			# [[FIX]]: Capture exit code before if statement to report correctly
			stdbuf -oL -eL ./throughput_test --config ../../config/client.yaml -m $msg_size -s $TOTAL_MESSAGE_SIZE --record_results -t $test_case -o $order -a $ack --sequencer $sequencer
			test_exit_code=$?
			if [ $test_exit_code -ne 0 ]; then
				echo "ERROR: Throughput test failed with exit code $test_exit_code"
				# Still clean up brokers even if test failed
			fi

				# Test completed - now clean up broker processes
				echo "Test completed, cleaning up broker processes..."
				for pid in "${pids[@]}"; do
				  kill $pid 2>/dev/null || true
				  echo "Terminated broker process with PID $pid"
				done

				echo "All processes have finished for trial $trial with message size $msg_size"

				# [[FIX]]: Clear pids array and clean up log files from this trial
				pids=()
				# Move log files to results directory to prevent overwriting in next trial
				if [ -d "../../data/throughput/logs" ]; then
					mkdir -p "../../data/throughput/logs/trial_${trial}_$(date +%Y%m%d_%H%M%S)"
					mv broker_*_trial${trial}.log ../../data/throughput/logs/trial_${trial}_$(date +%Y%m%d_%H%M%S)/ 2>/dev/null || true
				fi
				sleep 1
				cleanup
				sleep 2
			done
		done
	done
 done

echo "All experiments have finished."
