#!/bin/bash

pushd ../build/bin/

# Explicit config argument for binaries (relative to build/bin)
CONFIG_ARG="--config ../../config/embarcadero.yaml"

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
  $command &
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
			start_process "./embarlet $CONFIG_ARG --head --$sequencer"
			wait_for_signal
			head_pid=${pids[-1]}  # Get the PID of the ./embarlet --head process
			sleep 3
			for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
			  start_process "./embarlet $CONFIG_ARG"
			  wait_for_signal
			done
			sleep 3
			#for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
			 # wait_for_signal
			#done
			OUT_LOG="throughput_test_${msg_size}_o${order}_t${test_case}.log"
			./throughput_test -m $msg_size --record_results -t $test_case -o $order -a $ack --sequencer $sequencer > "$OUT_LOG" 2>&1 &
			pid=$!
			echo "Started process with command './throughput_test ...' and PID $pid"
			pids+=($pid)

			# Wait for all processes to finish
			for pid in "${pids[@]}"; do
			  wait $pid
			  echo "Process with PID $pid finished"
			done

			echo "All processes have finished for trial $trial with message size $msg_size"
			# Check correctness from subscriber logs
			if grep -qE "DEBUG: Order check.*PASSED|overall result: PASSED|Verification PASSED" "$OUT_LOG"; then
			  echo "Correctness PASSED (found PASS markers in $OUT_LOG)"
			else
			  if grep -qE "Order check failed|overall result: FAILED|DEBUG Check Failed|VERIFY FAIL" "$OUT_LOG"; then
			    echo "Correctness FAILED (see $OUT_LOG)" >&2
			    exit 1
			  else
			    echo "Correctness status inconclusive (no PASS/FAIL markers found). Check $OUT_LOG"
			  fi
			fi

			pids=()  # Clear the pids array for the next trial
			sleep 3
		  done
		done
	done
done

rm -f script_signal_pipe
rm -f script_signal_pipe
echo "All experiments have finished."
