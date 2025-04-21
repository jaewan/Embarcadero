#!/bin/bash

pushd ../build/bin/

NUM_BROKERS=4
REMOTE_IP="192.168.60.173"
REMOTE_USER="domin"
PASSLESS_ENTRY="~/.ssh/id_rsa"
REMOTE_BIN_DIR="~/Jae/Embarcadero/build/bin"
REMOTE_PID_FILE="/tmp/remote_seq.pid"

# Define the configurations
declare -a configs=(
  "sequencer=EMBARCADERO"
  "sequencer=CORFU"
  "sequencer=SCALOG"
)

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

start_remote_sequencer() {
  local sequencer_bin=$1  # e.g., scalog_global_sequencer or corfu_global_sequencer
  echo "Starting remote sequencer on $REMOTE_IP..."

  ssh -o StrictHostKeyChecking=no -i "$PASSLESS_ENTRY" "$REMOTE_USER@$REMOTE_IP" bash <<EOF
    cd $REMOTE_BIN_DIR
    nohup ./$sequencer_bin > /tmp/${sequencer_bin}.log 2>&1 &
    echo \$! > $REMOTE_PID_FILE
EOF
}

stop_remote_sequencer() {
  echo "Stopping remote sequencer on $REMOTE_IP..."
  ssh -o StrictHostKeyChecking=no -i "$PASSLESS_ENTRY" "$REMOTE_USER@$REMOTE_IP" bash <<EOF
    if [ -f $REMOTE_PID_FILE ]; then
      kill \$(cat $REMOTE_PID_FILE) 2>/dev/null
      rm -f $REMOTE_PID_FILE
    fi
EOF
}

# Create output directories if they don't exist
mkdir -p ../../data/kv/

for config in "${configs[@]}"; do
	echo "============================================================"
    echo "Running configuration: $config"
    echo "============================================================"

    # Evaluate the configuration string to set variables
    eval "$config"

    # Array to store process IDs
    pids=()

    rm -f script_signal_pipe
    mkfifo script_signal_pipe

    # Run experiments for each message size
	echo "Running with message size $msg_size | Order: $order | Sequencer: $sequencer | Mode: $mode"

	# Start remote sequencer if needed
	if [[ "$sequencer" == "CORFU" ]]; then
	  start_remote_sequencer "corfu_global_sequencer"
	elif [[ "$sequencer" == "SCALOG" ]]; then
	  start_remote_sequencer "scalog_global_sequencer"
	fi

	# Start the processes
	start_process "./embarlet --head --$sequencer"
	wait_for_signal
	head_pid=${pids[-1]}  # Get the PID of the ./embarlet --head process
	sleep 3
	for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
	  start_process "./embarlet --$sequencer"
	  wait_for_signal
	done
	sleep 3

	start_process "./kv_test --sequencer $sequencer"

	# Wait for all processes to finish
	for pid in "${pids[@]}"; do
	  wait $pid
	  echo "Process with PID $pid finished"
	done

	echo "All processes have finished for message size $msg_size in $mode mode"
	pids=()  # Clear the pids array for the next trial

	# Stop remote process after each trial
	if [[ "$sequencer" == "CORFU" || "$sequencer" == "SCALOG" ]]; then
	  stop_remote_sequencer
	fi
	sleep 3

	# Move results to appropriate directory
	mv multi_get_results.csv ../../data/kv/${sequencer}_get.csv
	mv multi_put_results.csv ../../data/kv/${sequencer}_put.csv

    rm -f script_signal_pipe
done

# Plot results for this mode
popd
pushd ../data/kv/
python3 plot_kv.py
popd

echo "All experiments have finished."
