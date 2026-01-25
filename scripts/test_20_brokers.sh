#!/bin/bash

# Test script for 20 brokers without TC throttling
# This script starts 20 brokers and runs a throughput test

echo "🚀 Starting 20-broker test without TC throttling..."
echo "   Config: 20_brokers_optimized.yaml"
echo "   Buffer: 15.36GB (20 × 768MB)"
echo "   Test: 10.7GB message throughput"

cd build/bin

# Clean up any existing processes
echo "🧹 Cleaning up existing processes..."
pkill -f "./embarlet" >/dev/null 2>&1 || true
pkill -f "./throughput_test" >/dev/null 2>&1 || true

# Wait for cleanup
sleep 2

# Start head broker first
echo "   -> Starting head broker (broker 0)..."
numactl --cpunodebind=1 --membind=2 ./embarlet --config ../../config/20_brokers_optimized.yaml --head > broker_0.log 2>&1 &
HEAD_PID=$!
sleep 3

# Start remaining 19 brokers
for i in $(seq 1 19); do
    echo "   -> Starting broker $i..."
    numactl --cpunodebind=1 --membind=2 ./embarlet --config ../../config/20_brokers_optimized.yaml > broker_$i.log 2>&1 &
    sleep 1
done

echo "   -> Waiting for cluster formation (30 seconds)..."
sleep 30

echo "🧪 Starting throughput test..."
echo "   Command: ./throughput_test --config ../../config/20_brokers_optimized.yaml -o 0 --sequencer EMBARCADERO -m 4096 -n 1"

# Run the test with 2-minute timeout
timeout 120 ./throughput_test --config ../../config/20_brokers_optimized.yaml -o 0 --sequencer EMBARCADERO -m 4096 -n 1

TEST_EXIT_CODE=$?

echo ""
echo "🧹 Cleaning up broker processes..."
pkill -f "./embarlet" >/dev/null 2>&1 || true

if [ $TEST_EXIT_CODE -eq 124 ]; then
    echo "❌ Test timed out after 2 minutes"
    echo "💡 This suggests the test is taking too long even without TC throttling"
elif [ $TEST_EXIT_CODE -eq 0 ]; then
    echo "✅ Test completed successfully!"
else
    echo "❌ Test failed with exit code: $TEST_EXIT_CODE"
fi

echo ""
echo "📊 Broker logs available in:"
for i in $(seq 0 19); do
    if [ -f "broker_$i.log" ]; then
        echo "   - broker_$i.log"
    fi
done

echo ""
echo "✅ 20-broker test completed"