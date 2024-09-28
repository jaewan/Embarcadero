#!/bin/bash

pushd ../build/bin/

NUM_TRIALS=1
order_level=(0 1 2)
test_cases=(0 1)
#msg_sizes=(128 512 1024 4096 65536 1048576)
msg_sizes=(1024)

# Function to start a process
start_process() {
  local command=$1
  $command &
  pid=$!
  echo "Started process with command '$command' and PID $pid"
  pids+=($pid)
}

wait_for_signal() {
  while true; do
    read -r signal <script_signal_pipe
    if [ "$signal" ]; then
      echo "Received signal: $signal"
      break
    fi
  done
}

# Array to store process IDs
pids=()

mkfifo script_signal_pipe

# Run experiments for each message size
for msg_size in "${msg_sizes[@]}"; do
  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
    echo "Running trial $trial with message size $msg_size"

	# Start the processes
    start_process "./embarlet --head"
    head_pid=${pids[-1]}  # Get the PID of the ./embarlet --head process
	sleep 1
    for i in {1..3}; do
      start_process "./embarlet"
    done
	wait_for_signal
    start_process "./throughput_test -m $msg_size"

    # Wait for all processes to finish
    for pid in "${pids[@]}"; do
      wait $pid
      echo "Process with PID $pid finished"
    done

    echo "All processes have finished for trial $trial with message size $msg_size"
    pids=()  # Clear the pids array for the next trial
  done
done

rm script_signal_pipe
echo "All experiments have finished."
