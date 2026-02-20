#!/bin/bash
set -euo pipefail

# NOTE: This script requires recompilation with COLLECT_LATENCY_STATS macro defined
# to enable latency measurement functionality. Build with:
#   cmake -DCOLLECT_LATENCY_STATS=ON ..
# Without this macro, latency collection is completely compiled out for optimal performance.

pushd ../build/bin/

NUM_BROKERS=4
test_case=2
msg_size=1024
ack_level=1
REMOTE_IP="192.168.60.173"
REMOTE_USER="domin"
PASSLESS_ENTRY="~/.ssh/id_rsa"
REMOTE_BIN_DIR="~/Jae/Embarcadero/build/bin"
REMOTE_PID_FILE="/tmp/remote_seq.pid"
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-numactl --cpunodebind=0 --membind=0}"

# Define the configurations
declare -a configs=(
  "order=4; sequencer=EMBARCADERO"
  "order=2; sequencer=CORFU"
  #"order=1; sequencer=SCALOG"
)

# Define the experiment modes
declare -a modes=(
  "steady"
  #"bursty"
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
mkdir -p ../../data/latency/steady
mkdir -p ../../data/latency/bursty

# Run each configuration for both steady and bursty modes
for mode in "${modes[@]}"; do
  echo "========================================================"
  echo "Running experiments in $mode mode"
  echo "========================================================"

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
	start_process "$EMBARLET_NUMA_BIND ./embarlet --head --$sequencer"
	wait_for_signal
	head_pid=${pids[-1]}  # Get the PID of the ./embarlet --head process
	sleep 3
	for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
	  start_process "$EMBARLET_NUMA_BIND ./embarlet --$sequencer"
	  wait_for_signal
	done
	sleep 3

	# Run throughput test with or without --steady_rate based on mode
	if [[ "$mode" == "steady" ]]; then
	  start_process "$CLIENT_NUMA_BIND ./throughput_test -m $msg_size --record_results -t $test_case -o $order -a $ack_level --sequencer $sequencer -r 1 --steady_rate"
	else
	  start_process "$CLIENT_NUMA_BIND ./throughput_test -m $msg_size --record_results -t $test_case -o $order -a $ack_level --sequencer $sequencer -r 1"
	fi

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
	if [[ -f cdf_latency_us.csv ]]; then
	  mv cdf_latency_us.csv ../../data/latency/$mode/${sequencer}_latency.csv
	fi
	if [[ -f latency_stats.csv ]]; then
	  mv latency_stats.csv ../../data/latency/$mode/${sequencer}_latency_stats.csv
	fi
	if [[ -f pub_cdf_latency_us.csv ]]; then
	  mv pub_cdf_latency_us.csv ../../data/latency/$mode/${sequencer}_pub_latency.csv
	fi
	if [[ -f pub_latency_stats.csv ]]; then
	  mv pub_latency_stats.csv ../../data/latency/$mode/${sequencer}_pub_latency_stats.csv
	fi

    rm -f script_signal_pipe
    echo "Finished configuration: $config in $mode mode"
  done
done

# Plot results for this mode
popd
pushd ../data/latency/
python3 plot_latency.py latency
popd


echo "All experiments have finished."
