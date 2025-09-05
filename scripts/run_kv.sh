#!/bin/bash

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")"/.. && pwd)"
BUILD_DIR="$REPO_ROOT/build"
BIN_DIR="$BUILD_DIR/bin"

NUM_BROKERS=4
REMOTE_IP="192.168.60.173"
REMOTE_USER="domin"
PASSLESS_ENTRY="$HOME/.ssh/id_rsa"
REMOTE_BIN_DIR="/home/${REMOTE_USER}/Jae/Embarcadero/build/bin"
REMOTE_PID_FILE="/tmp/remote_seq.pid"

# Define the configurations
declare -a configs=(
	"sequencer=EMBARCADERO"
	"sequencer=CORFU"
	"sequencer=SCALOG"
)

wait_for_signal() {
	local timeout_sec=${1:-20}
	if command -v timeout >/dev/null 2>&1; then
		if signal=$(timeout ${timeout_sec}s cat script_signal_pipe); then
			echo "Received signal: ${signal}"
		else
			echo "No signal within ${timeout_sec}s, continuing..."
		fi
	else
		# Fallback without timeout utility
		local start_ts=$(date +%s)
		while true; do
			if read -r signal < script_signal_pipe; then
				if [ "$signal" ]; then
					echo "Received signal: $signal"
					break
				fi
			fi
			now=$(date +%s)
			if [ $((now - start_ts)) -ge ${timeout_sec} ]; then
				echo "No signal within ${timeout_sec}s, continuing..."
				break
			fi
		done
	fi
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
	ssh -o StrictHostKeyChecking=no -i "$PASSLESS_ENTRY" "$REMOTE_USER@$REMOTE_IP" "\
		cd '$REMOTE_BIN_DIR' && \
		nohup './$sequencer_bin' > '/tmp/${sequencer_bin}.log' 2>&1 & echo \$! > '$REMOTE_PID_FILE'"
}

stop_remote_sequencer() {
	echo "Stopping remote sequencer on $REMOTE_IP..."
	ssh -o StrictHostKeyChecking=no -i "$PASSLESS_ENTRY" "$REMOTE_USER@$REMOTE_IP" "\
		if [ -f '$REMOTE_PID_FILE' ]; then \
			kill \$(cat '$REMOTE_PID_FILE') 2>/dev/null || true; \
			rm -f '$REMOTE_PID_FILE'; \
		fi"
}

# Build if needed
mkdir -p "$BUILD_DIR"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD_DIR" -j >/dev/null

# Create output directories if they don't exist
mkdir -p "$REPO_ROOT/data/kv/"

pushd "$BIN_DIR" >/dev/null

for config in "${configs[@]}"; do
	echo "============================================================"
	echo "Running configuration: $config"
	echo "============================================================"

	# Evaluate the configuration string to set variables
	eval "$config"

	# Array to store process IDs
	pids=()

	rm -f script_signal_pipe || true
	mkfifo script_signal_pipe

	echo "Launching brokers for sequencer: $sequencer"

	# Start remote sequencer if needed
	if [[ "$sequencer" == "CORFU" ]]; then
		start_remote_sequencer "corfu_global_sequencer"
	elif [[ "$sequencer" == "SCALOG" ]]; then
		start_remote_sequencer "scalog_global_sequencer"
	fi

	# Start the processes
	start_process "./embarlet --head --$sequencer --config $REPO_ROOT/config/embarcadero.yaml"
	wait_for_signal 20
	sleep 1
	for ((i = 1; i <= NUM_BROKERS - 1; i++)); do
		start_process "./embarlet --$sequencer --config $REPO_ROOT/config/embarcadero.yaml"
		wait_for_signal 20
	done
	sleep 2

	start_process "./kv_test --sequencer $sequencer --num_brokers $NUM_BROKERS"

	# Wait for kv_test to finish
	kv_pid=${pids[-1]}
	wait "$kv_pid"
	echo "kv_test finished (PID $kv_pid)"

	# Terminate brokers
	for pid in "${pids[@]}"; do
		if [[ "$pid" != "$kv_pid" ]]; then
			# Wait up to 20s for process to exit on its own
			for i in {1..20}; do
				if ! kill -0 "$pid" 2>/dev/null; then
					break
				fi
				sleep 1
			done
			# If still running, send SIGTERM then SIGKILL as last resort
			if kill -0 "$pid" 2>/dev/null; then
				kill "$pid" 2>/dev/null || true
				sleep 1
				if kill -0 "$pid" 2>/dev/null; then
					kill -9 "$pid" 2>/dev/null || true
				fi
			fi
		fi
	done

	# Stop remote process after each trial
	if [[ "$sequencer" == "CORFU" || "$sequencer" == "SCALOG" ]]; then
		stop_remote_sequencer
	fi
	sleep 1

	# Move results to appropriate directory
	mv -f multi_get_results.csv "$REPO_ROOT/data/kv/${sequencer}_get.csv" || true
	mv -f multi_put_results.csv "$REPO_ROOT/data/kv/${sequencer}_put.csv" || true

	rm -f script_signal_pipe || true
done

popd >/dev/null

echo "All experiments have finished. Results in $REPO_ROOT/data/kv/."
