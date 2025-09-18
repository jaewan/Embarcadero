#!/bin/bash

pushd ../build/bin/

NUM_BROKERS=4

NUM_TRIALS=5
ACK=0
orders=(0 1)
replication_factors=(0)
test_cases=(1)
msg_sizes=(128 512 1024 4096 65536 1048576)

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
	for order in "${orders[@]}"; do
		for msg_size in "${msg_sizes[@]}"; do
		  for replication_factor in "${replication_factors[@]}"; do
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
				start_process "./throughput_test -m $msg_size --record_results -t $test_case -r $replication_factor -a $ACK -o $order"

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
	done
done

rm script_signal_pipe
echo "All experiments have finished."
