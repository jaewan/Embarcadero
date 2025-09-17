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
test_cases=(1)
#msg_sizes=(128 256  512 1024 4096 16384 65536 262144 1048576)
msg_sizes=(4096)


# Change these for Scalog and Corfu
# Order level 0 for unordered, 1 for ordered (not implemented yet), 4 for strong ordering, 5 for batch-level ordering
orders=(5)
ack=2
sequencer=EMBARCADERO

# Removed wait_for_signal function - using sleep-based timing instead

# Function to start a process
start_process() {
  local command=$1
  eval "$command" &
  pid=$!
  echo "Started process with command '$command' and PID $pid"
  pids+=($pid)
}

# Array to store process IDs
pids=()

# Removed pipe creation - using sleep-based timing instead

# Run experiments for each message size
for test_case in "${test_cases[@]}"; do
	for order in "${orders[@]}"; do
		for msg_size in "${msg_sizes[@]}"; do
		  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
				echo "Running trial $trial with message size $msg_size"

				# Start the processes
				start_process "$EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG --head --$sequencer > broker_0.log 2>&1"
				echo "Started head broker, waiting for initialization..."
				sleep 5  # Wait for head broker to initialize
				head_pid=${pids[-1]}  # Get the PID of the ./embarlet --head process
				
				for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
				  start_process "$EMBARLET_NUMA_BIND ./embarlet $CONFIG_ARG > broker_$i.log 2>&1"
				  echo "Started broker $i, waiting for initialization..."
				  sleep 3  # Wait for each broker to initialize
				done
				echo "All brokers started, waiting for cluster formation..."
				sleep 2

				# Run throughput test in foreground; stream output to terminal
				# Use 10GB total with 4 threads per broker for maximum performance
				# No NUMA binding for client - let OS optimize placement
				stdbuf -oL -eL ./throughput_test -m $msg_size -n 4 --record_results -t $test_case -o $order -a $ack --sequencer $sequencer

				# Test completed - now clean up broker processes
				echo "Test completed, cleaning up broker processes..."
				for pid in "${pids[@]}"; do
				  kill $pid 2>/dev/null || true
				  echo "Terminated broker process with PID $pid"
				done

				echo "All processes have finished for trial $trial with message size $msg_size"

				pids=()  # Clear the pids array for the next trial
				sleep 1
				cleanup
				sleep 2
			done
		done
	done
 done

echo "All experiments have finished."
