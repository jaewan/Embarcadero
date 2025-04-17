#!/bin/bash

pushd ../build/bin/

NUM_BROKERS=4
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
  #"orders=(0); replication=0; ack=0; sequencer=EMBARCADERO"
  #"orders=(4); replication=0; ack=0; sequencer=EMBARCADERO"
  #"orders=(4); replication=1; ack=1; sequencer=EMBARCADERO"
  #"orders=(2); replication=0; ack=0; sequencer=CORFU"
  #"orders=(2); replication=1; ack=1; sequencer=CORFU"
  "orders=(1); replication=0; ack=0; sequencer=SCALOG"
  "orders=(1); replication=1; ack=1; sequencer=SCALOG"
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

# Run each configuration
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
for order in "${orders[@]}"; do
  for msg_size in "${msg_sizes[@]}"; do
  echo "Running trial $trial with message size $msg_size | Order: $order | Replication: $replication | AckLevel: $ack | Sequencer: $sequencer"

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

  start_process "./throughput_test -m $msg_size -t $test_case -o $order -r $replication --sequencer $sequencer -a $ack"

  # Wait for all processes to finish
  for pid in "${pids[@]}"; do
	wait $pid
	echo "Process with PID $pid finished"
  done

  echo "All processes have finished for trial $trial with message size $msg_size"
  pids=()  # Clear the pids array for the next trial
  # Stop remote process after each trial
  if [[ "$sequencer" == "CORFU" || "$sequencer" == "SCALOG" ]]; then
	  stop_remote_sequencer
  fi
  sleep 3

  mv latency_stats.csv ../../data/breakdown/${sequencer}_${order}_${replication}_latency.csv
  done
done

  rm -f script_signal_pipe
  echo "Finished configuration: $config"
done

echo "All experiments have finished."
