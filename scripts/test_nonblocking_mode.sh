#!/bin/bash
# Test script for Phase 2 non-blocking NetworkManager implementation
# Tests connection routing and basic functionality

set -e  # Exit on error

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"

echo "========================================="
echo "Non-Blocking Mode Test - Phase 2"
echo "========================================="
echo

# Configuration
export EMBARCADERO_USE_NONBLOCKING=1
export EMBARCADERO_STAGING_POOL_NUM_BUFFERS=32
export EMBARCADERO_STAGING_POOL_BUFFER_SIZE_MB=2
export EMBARCADERO_NUM_PUBLISH_RECEIVE_THREADS=4
export EMBARCADERO_NUM_CXL_ALLOCATION_WORKERS=2

# Use smaller CXL size for testing (2GB - minimum for segment allocation)
export EMBARCADERO_CXL_SIZE=$((2 * 1024 * 1024 * 1024))
export EMBARCADERO_CXL_EMUL_SIZE=$((2 * 1024 * 1024 * 1024))

# Small test size for quick validation
export TOTAL_MESSAGE_SIZE=$((100 * 1024 * 1024))  # 100 MB
export MSG_SIZE=1024  # 1 KB messages
export NUM_PUBLISHERS=4

echo "Configuration:"
echo "  Non-blocking mode: ENABLED"
echo "  Staging pool: ${EMBARCADERO_STAGING_POOL_NUM_BUFFERS} × ${EMBARCADERO_STAGING_POOL_BUFFER_SIZE_MB}MB"
echo "  Publish receive threads: ${EMBARCADERO_NUM_PUBLISH_RECEIVE_THREADS}"
echo "  CXL allocation workers: ${EMBARCADERO_NUM_CXL_ALLOCATION_WORKERS}"
echo "  Test size: $((TOTAL_MESSAGE_SIZE / 1024 / 1024)) MB"
echo "  Publishers: ${NUM_PUBLISHERS}"
echo

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    pkill -9 -f embarlet || true
    pkill -9 -f throughput_test || true
    sleep 1
}

trap cleanup EXIT

# Start broker
echo "Starting broker with non-blocking mode..."
cd "$PROJECT_ROOT"
"$BUILD_DIR/bin/embarlet" --head > /tmp/embarlet_nonblocking.log 2>&1 &
BROKER_PID=$!

echo "Broker PID: $BROKER_PID"
sleep 3

# Check if broker is running
if ! kill -0 $BROKER_PID 2>/dev/null; then
    echo "ERROR: Broker failed to start!"
    echo "Log output:"
    cat /tmp/embarlet_nonblocking.log
    exit 1
fi

# Verify non-blocking mode was enabled
if grep -q "Non-blocking mode enabled" /tmp/embarlet_nonblocking.log; then
    echo "✓ Non-blocking mode enabled successfully"
else
    echo "ERROR: Non-blocking mode not enabled in broker log"
    cat /tmp/embarlet_nonblocking.log
    exit 1
fi

# Verify threads launched
if grep -q "PublishReceiveThread started" /tmp/embarlet_nonblocking.log; then
    echo "✓ PublishReceiveThreads launched"
else
    echo "WARNING: PublishReceiveThread launch not confirmed in logs"
fi

if grep -q "CXLAllocationWorker started" /tmp/embarlet_nonblocking.log; then
    echo "✓ CXLAllocationWorkers launched"
else
    echo "WARNING: CXLAllocationWorker launch not confirmed in logs"
fi

echo
echo "Running throughput test..."

# Run throughput test (test_number=0 is pub/sub, 1 is E2E)
"$BUILD_DIR/bin/throughput_test" \
    -m $MSG_SIZE \
    -s $TOTAL_MESSAGE_SIZE \
    -t 0 \
    -o 5 \
    -a 1 \
    --sequencer EMBARCADERO \
    > /tmp/throughput_test_nonblocking.log 2>&1 &

TEST_PID=$!
echo "Throughput test PID: $TEST_PID"

# Wait for test to complete (with timeout)
TIMEOUT=60  # seconds
ELAPSED=0
while kill -0 $TEST_PID 2>/dev/null; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    if [ $ELAPSED -ge $TIMEOUT ]; then
        echo "ERROR: Test timed out after ${TIMEOUT}s"
        kill -9 $TEST_PID || true
        exit 1
    fi

    # Show progress every 10 seconds
    if [ $((ELAPSED % 10)) -eq 0 ]; then
        echo "  Test running... ${ELAPSED}s elapsed"
    fi
done

wait $TEST_PID
TEST_EXIT_CODE=$?

echo
echo "========================================="
echo "Test Results"
echo "========================================="
echo

if [ $TEST_EXIT_CODE -eq 0 ]; then
    echo "✓ Throughput test completed successfully"

    # Extract throughput from logs
    if grep -q "Throughput" /tmp/throughput_test_nonblocking.log; then
        echo
        echo "Performance metrics:"
        grep -A5 "Throughput" /tmp/throughput_test_nonblocking.log | head -10
    fi

    # Check for connection routing in broker log
    echo
    echo "Connection routing verification:"
    if grep -q "Enqueued publish connection for non-blocking handling" /tmp/embarlet_nonblocking.log; then
        CONNECTIONS=$(grep -c "Enqueued publish connection" /tmp/embarlet_nonblocking.log)
        echo "✓ Routed ${CONNECTIONS} connections to non-blocking path"
    else
        echo "⚠ No connections routed (check logs)"
    fi

    # Check for batches processed
    if grep -q "batch_complete=1" /tmp/embarlet_nonblocking.log; then
        BATCHES=$(grep -c "batch_complete=1" /tmp/embarlet_nonblocking.log)
        echo "✓ Processed ${BATCHES} batches via CXL allocation"
    else
        echo "⚠ No batches marked complete (check logs)"
    fi

    # Check staging pool utilization
    if grep -q "StagingPool::Allocate" /tmp/embarlet_nonblocking.log; then
        echo "✓ Staging pool allocations occurred"
    fi

    echo
    echo "========================================="
    echo "✓ Phase 2 Non-Blocking Test PASSED"
    echo "========================================="
    exit 0
else
    echo "✗ Throughput test failed with exit code: $TEST_EXIT_CODE"
    echo
    echo "Broker log (last 50 lines):"
    tail -50 /tmp/embarlet_nonblocking.log
    echo
    echo "Test log (last 50 lines):"
    tail -50 /tmp/throughput_test_nonblocking.log
    exit 1
fi
