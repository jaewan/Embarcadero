#!/bin/bash

pushd ../build/bin/

NUM_BROKERS=4
NUM_TRIALS=3
test_cases=(5)
msg_sizes=(128 256  512 1024 4096 16384 65536 262144 1048576)


# Change these for Scalog and Corfu
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
		  for ((trial=1; trial<=NUM_TRIALS; trial++)); do
			echo "Running trial $trial with message size $msg_size"

			# Start the processes
			start_process "./embarlet --head --$sequencer"
			wait_for_signal
			head_pid=${pids[-1]}  # Get the PID of the ./embarlet --head process
			sleep 3
			for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
			  start_process "./embarlet"
			  wait_for_signal
			done
			sleep 3
			#for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
			 # wait_for_signal
			#done
			start_process "./throughput_test -m $msg_size --record_results -t $test_case -o $order -a $ack --sequencer $sequencer"

			# Wait for all processes to finish
			for pid in "${pids[@]}"; do
			  wait $pid
			  echo "Process with PID $pid finished"
			done

			echo "All processes have finished for trial $trial with message size $msg_size"
			pids=()  # Clear the pids array for the next trial
			sleep 3
		  done
		done
	done
done

rm script_signal_pipe
echo "All experiments have finished."
