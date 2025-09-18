#!/bin/bash

# This script automates the setup, execution, and cleanup of the network emulation benchmark.

# Exit on any error
set -e

# Ensure we're in the script's directory
cd "$(dirname "$0")"

# --- 1. Cleanup previous runs ---
echo "🧹 [Step 1/5] Cleaning up any previous network emulation environments..."
# Use bash -c to ensure sudo credentials are asked for upfront if needed.
bash -c "./cleanup_emulation.sh"
echo "✅ Cleanup complete."

# --- 2. Setup the network environment ---
echo "🚀 [Step 2/5] Setting up the virtual network environment..."
bash -c "./setup_emulation.sh"
echo "✅ Network setup complete."

# --- 3. Compile the C++ applications ---
echo "💻 [Step 3/5] Compiling the broker and client applications..."
if [ ! -d "build" ]; then
    mkdir build
fi
cd build
cmake ..
make
cd ..
echo "✅ Compilation complete."

# --- 4. Run the benchmark ---
echo "🚦 [Step 4/5] Starting the benchmark..."

BROKER_PIDS=()
NUM_BROKERS=20
BROKER_PREFIX="broker-ns-"
CLIENT_NS="client-ns"
LOG_DIR="logs"
mkdir -p $LOG_DIR

# Start brokers in the background
echo "-> Starting $NUM_BROKERS brokers in their namespaces..."
for i in $(seq 1 $NUM_BROKERS)
do
  NS_NAME="${BROKER_PREFIX}${i}"
  # Run the broker in the background and store its PID
  sudo ip netns exec $NS_NAME ./build/broker > "${LOG_DIR}/broker_${i}.log" 2>&1 &
  BROKER_PIDS+=($!)
done

echo "-> All brokers started. Waiting 3 seconds for them to initialize..."
sleep 3

# Start the client
echo "-> Starting the client application..."
sudo ip netns exec $CLIENT_NS ./build/client | tee "${LOG_DIR}/client.log"

# --- 5. Cleanup ---
echo "🛑 [Step 5/5] Benchmark finished. Cleaning up processes and network..."

# Kill all broker processes
echo "-> Stopping broker processes..."
for PID in "${BROKER_PIDS[@]}"; do
    # Check if process exists before killing
    if kill -0 $PID > /dev/null 2>&1; then
        sudo kill -SIGTERM $PID
    fi
done
# Wait a moment for processes to terminate
sleep 2

# Cleanup the network environment
bash -c "./cleanup_emulation.sh"

echo "🎉 Test complete. Logs are available in the 'logs' directory."
