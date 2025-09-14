#!/bin/bash

pushd ../build/bin/

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

# Prefer THP by default; set EMBAR_USE_HUGETLB=1 to force HugeTLB
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-0}

NUM_BROKERS=4
NUM_TRIALS=1
test_cases=(1)
#msg_sizes=(128 256  512 1024 4096 16384 65536 262144 1048576)
msg_sizes=(4096)


# Change these for Scalog and Corfu
# Order level 0 for unordered, 1 for ordered (not implemented yet), 4 for strong ordering
orders=(4)
ack=2
sequencer=EMBARCADERO

wait_for_signal() {
  while true; do
    read -r signal <script_signal_pipe
    if [ "$signal" ]; then
      echo "Received signal: $signal"
      break
    fi
  done
}

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

rm -f script_signal_pipe
mkfifo script_signal_pipe

# Run experiments for each message size
for test_case in "${test_cases[@]}"; do
	for order in "${orders[@]}"; do
		for msg_size in "${msg_sizes[@]}"; do
		  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
				echo "Running trial $trial with message size $msg_size"

				# Start the processes
				start_process "./embarlet $CONFIG_ARG --head --$sequencer > broker_0.log 2>&1"
				wait_for_signal
				head_pid=${pids[-1]}  # Get the PID of the ./embarlet --head process
				sleep 3
				for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
				  start_process "./embarlet $CONFIG_ARG > broker_$i.log 2>&1"
				  wait_for_signal
				done
				sleep 3

				# Run throughput test in foreground; stream output to terminal
				# Use 10GB total (2.5GB per broker) to test maximum performance with 1 thread per broker
				stdbuf -oL -eL ./throughput_test -m $msg_size -n 1 --record_results -t $test_case -o $order -a $ack --sequencer $sequencer

				# Wait for all broker processes to finish
				for pid in "${pids[@]}"; do
				  wait $pid
				  echo "Process with PID $pid finished"
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

rm -f script_signal_pipe
rm -f script_signal_pipe
echo "All experiments have finished."
