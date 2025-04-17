#!/bin/bash

pushd ../build/bin/

NUM_BROKERS=1
test_case=2
msg_sizes=(1024)
REMOTE_IP="192.168.60.173"
REMOTE_USER="domin"
PASSLESS_ENTRY="~/.ssh/id_rsa"
REMOTE_BIN_DIR="~/Jae/Embarcadero/build/bin"
REMOTE_PID_FILE="/tmp/remote_seq.pid"

echo -e "\e[31mYou must recompile Embarcadero with NUM_MAX_BROKERS 1 in both Emb and sequencer.\e[0m"

# Define the configurations
declare -a configs=(
"orders=(4); ack=2; sequencer=EMBARCADERO"
"orders=(2); ack=2; sequencer=CORFU"
"orders=(1); ack=1; sequencer=SCALOG"
)

wait_for_signal() {
    while true; do
        read -r signal < script_signal_pipe
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

# Function to start remote sequencer
start_remote_sequencer() {
    local sequencer_bin=$1 # e.g., scalog_global_sequencer or corfu_global_sequencer
    echo "Starting remote sequencer $sequencer_bin on $REMOTE_IP..."

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

# Run each configuration low load
for config in "${configs[@]}"; do
    echo "============================================================"
    echo "Running configuration: $config"
    echo "============================================================"

    # Evaluate the configuration string to set variables
    eval "$config"

    # Array to store process IDs for the current configuration run
    pids=()

    # Create named pipe for signaling
    rm -f script_signal_pipe
    mkfifo script_signal_pipe

    # Run experiments for each order and message size defined in the config
    for order in "${orders[@]}"; do
        for msg_size in "${msg_sizes[@]}"; do
            # Removed undefined $trial variable from echo
            echo "Running with message size $msg_size | Order: $order | Ack: $ack | Sequencer: $sequencer"

            # Start remote sequencer if needed
            if [[ "$sequencer" == "CORFU" ]]; then
                start_remote_sequencer "corfu_global_sequencer"
            elif [[ "$sequencer" == "SCALOG" ]]; then
                start_remote_sequencer "scalog_global_sequencer"
            fi

            # Start the local processes
            echo "Starting head embarlet..."
            start_process "./embarlet --head --$sequencer"
            wait_for_signal # Wait for head embarlet to signal readiness

            # Get the PID of the ./embarlet --head process
            head_pid=${pids[-1]}
            echo "Head embarlet PID: $head_pid. Waiting before starting others..."
            sleep 3 # Give head time to fully initialize

            # Start remaining brokers if NUM_BROKERS > 1
            for ((i = 1; i < NUM_BROKERS; i++)); do
                start_process "./embarlet --$sequencer"
                wait_for_signal # Wait for this broker to signal readiness
            done

            sleep 3

            start_process "./throughput_test -m $msg_size --record_results -t $test_case -o $order -a $ack --sequencer $sequencer -r 1 -s 493568"

            # Wait for all background processes started in *this trial*
            # Note: wait command without PID waits for all child processes.
            # Waiting specifically for PIDs in the array is safer.
            for pid in "${pids[@]}"; do
                wait $pid
                echo "Process with PID $pid finished"
            done

            echo "All processes have finished for message size $msg_size | Order: $order"
            pids=() # Clear the pids array for the next iteration

            # Stop remote process after each trial if it was started
            if [[ "$sequencer" == "CORFU" || "$sequencer" == "SCALOG" ]]; then
                stop_remote_sequencer
            fi
            echo "Waiting before next iteration..."
            sleep 3

            # Check if result files exist before moving
            if [ -f cdf_latency_us.csv ]; then
                mkdir -p ../../data/latency/low_load/ # Ensure directory exists
                mv cdf_latency_us.csv ../../data/latency/low_load/${sequencer}_${order}_${msg_size}_latency.csv
            else
                echo "Warning: cdf_latency_us.csv not found."
            fi
            if [ -f latency_stats.csv ]; then
                 mkdir -p ../../data/latency/low_load/ # Ensure directory exists
                 mv latency_stats.csv ../../data/latency/low_load/${sequencer}_${order}_${msg_size}_latency_stats.csv
            else
                 echo "Warning: latency_stats.csv not found."
            fi

        done # End msg_size loop
    done # End order loop

    # Clean up pipe after all trials for a configuration are done
    rm -f script_signal_pipe
    echo "Finished configuration: $config"
done # End config loop

popd # Match the first pushd

pushd ../data/latency/
python3 plot_latency.py latency
popd 

echo "All experiments have finished."
