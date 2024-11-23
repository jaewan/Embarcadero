#!/bin/bash

pushd ../build/bin/

NUM_BROKERS=4
FAILURE_PERCENTAGE=0.2
NUM_BROKERS_TO_KILL=1
NUM_TRIALS=1
test_cases=(4)
msg_sizes=(1024)

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
  $command &
  pid=$!
  echo "Started process with command '$command' and PID $pid"
  pids+=($pid)
}

# Array to store process IDs
pids=()

rm script_signal_pipe
mkfifo script_signal_pipe

# Run experiments for each message size
for test_case in "${test_cases[@]}"; do
	for msg_size in "${msg_sizes[@]}"; do
	  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
		echo "Running trial $trial with message size $msg_size"

		# Start the processes
		start_process "./embarlet --head"
		wait_for_signal
		head_pid=${pids[-1]}  # Get the PID of the ./embarlet --head process
		sleep 1
		for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
		  start_process "./embarlet"
		done
		for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
		  wait_for_signal
		done
		start_process "./throughput_test -m $msg_size --record_results -t $test_case --num_brokers_to_kill $NUM_BROKERS_TO_KILL --failure_percentage $FAILURE_PERCENTAGE"

		# Wait for all processes to finish
		for pid in "${pids[@]}"; do
		  wait $pid
		  echo "Process with PID $pid finished"
		done

		echo "All processes have finished for trial $trial with message size $msg_size"
		pids=()  # Clear the pids array for the next trial
	  done
	done
done

rm script_signal_pipe
echo "All experiments have finished."
