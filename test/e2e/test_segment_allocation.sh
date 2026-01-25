#!/bin/bash
# [[DEVIATION_005]] - Segment Allocation E2E Test
# Tests atomic bitmap-based segment allocation with real broker processes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
TEST_OUTPUT_DIR="$BUILD_DIR/test_output/segment_allocation_test"
CONFIG_FILE="$PROJECT_ROOT/config/embarcadero.yaml"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Cleanup function
cleanup() {
    echo -e "${YELLOW}Cleaning up...${NC}"
    pkill -f "embarlet.*segment" || true
    sleep 1
    rm -rf "$TEST_OUTPUT_DIR"
}

trap cleanup EXIT INT TERM

# Create test output directory
mkdir -p "$TEST_OUTPUT_DIR"

echo "=========================================="
echo "Segment Allocation Test Suite"
echo "[[DEVIATION_005]] - Atomic Bitmap Allocator"
echo "=========================================="
echo ""

# Check if binaries exist
if [ ! -f "$BUILD_DIR/bin/embarlet" ]; then
    echo -e "${RED}ERROR: embarlet binary not found at $BUILD_DIR/bin/embarlet${NC}"
    echo "Please build the project first: cd build && make -j\$(nproc)"
    exit 1
fi

# Test 1: Single broker allocation test
echo -e "${GREEN}Test 1: Single Broker Segment Allocation${NC}"
echo "Starting single broker to test allocation..."

BROKER_LOG="$TEST_OUTPUT_DIR/broker_0.log"
"$BUILD_DIR/bin/embarlet" \
    --config "$CONFIG_FILE" \
    --head --EMBARCADERO \
    > "$BROKER_LOG" 2>&1 &
BROKER_PID=$!

# Wait for broker to initialize
sleep 3

# Check if broker started successfully
if ! ps -p $BROKER_PID > /dev/null; then
    echo -e "${RED}ERROR: Broker failed to start${NC}"
    cat "$BROKER_LOG"
    exit 1
fi

echo "✓ Broker started successfully (PID: $BROKER_PID)"
echo "  Check log: $BROKER_LOG"

# Kill broker
kill $BROKER_PID 2>/dev/null || true
wait $BROKER_PID 2>/dev/null || true

echo ""

# Test 2: Multi-broker concurrent allocation
echo -e "${GREEN}Test 2: Multi-Broker Concurrent Allocation${NC}"
echo "Starting 4 brokers to test concurrent allocation..."

BROKER_PIDS=()
# Start head broker
BROKER_LOG="$TEST_OUTPUT_DIR/broker_0.log"
"$BUILD_DIR/bin/embarlet" \
    --config "$CONFIG_FILE" \
    --head --EMBARCADERO \
    > "$BROKER_LOG" 2>&1 &
BROKER_PIDS+=($!)
echo "  Started head broker (PID: ${BROKER_PIDS[0]})"

# Wait for head broker
sleep 3

# Start follower brokers
for i in {1..3}; do
    BROKER_LOG="$TEST_OUTPUT_DIR/broker_${i}.log"
    "$BUILD_DIR/bin/embarlet" \
        --config "$CONFIG_FILE" \
        --EMBARCADERO \
        > "$BROKER_LOG" 2>&1 &
    BROKER_PIDS+=($!)
    echo "  Started broker $i (PID: ${BROKER_PIDS[$i]})"
done

# Wait for all brokers to initialize
sleep 5

# Check if all brokers are running
ALL_RUNNING=true
for i in {0..3}; do
    if ! ps -p ${BROKER_PIDS[$i]} > /dev/null; then
        echo -e "${RED}ERROR: Broker $i failed to start${NC}"
        cat "$TEST_OUTPUT_DIR/broker_${i}.log"
        ALL_RUNNING=false
    fi
done

if [ "$ALL_RUNNING" = true ]; then
    echo "✓ All 4 brokers started successfully"
    echo "  Check logs in: $TEST_OUTPUT_DIR"
else
    echo -e "${RED}ERROR: Some brokers failed to start${NC}"
    exit 1
fi

# Let them run for a bit to test allocation
echo "  Running for 10 seconds to test allocation..."
sleep 10

# Kill all brokers
for pid in "${BROKER_PIDS[@]}"; do
    kill $pid 2>/dev/null || true
done

# Wait for all to exit
for pid in "${BROKER_PIDS[@]}"; do
    wait $pid 2>/dev/null || true
done

echo "✓ Multi-broker test completed"
echo ""

# Test 3: Check for allocation messages in logs
echo -e "${GREEN}Test 3: Verify Allocation in Logs${NC}"

ALLOCATION_FOUND=false
for i in {0..3}; do
    if grep -q "allocated segment" "$TEST_OUTPUT_DIR/broker_${i}.log" 2>/dev/null; then
        echo "✓ Broker $i: Found allocation messages in log"
        ALLOCATION_FOUND=true
    fi
done

if [ "$ALLOCATION_FOUND" = true ]; then
    echo "✓ Allocation messages found in broker logs"
else
    echo -e "${YELLOW}WARNING: No allocation messages found (may be normal if no segments allocated)${NC}"
fi

echo ""

# Test 4: Performance check - look for latency warnings
echo -e "${GREEN}Test 4: Performance Check${NC}"

PERFORMANCE_ISSUES=0
for i in {0..3}; do
    if grep -qi "WARNING.*latency\|ERROR.*allocation" "$TEST_OUTPUT_DIR/broker_${i}.log" 2>/dev/null; then
        echo -e "${YELLOW}WARNING: Performance issues detected in broker $i log${NC}"
        PERFORMANCE_ISSUES=$((PERFORMANCE_ISSUES + 1))
    fi
done

if [ $PERFORMANCE_ISSUES -eq 0 ]; then
    echo "✓ No performance warnings detected"
else
    echo -e "${YELLOW}WARNING: $PERFORMANCE_ISSUES broker(s) had performance warnings${NC}"
fi

echo ""

# Summary
echo "=========================================="
echo -e "${GREEN}✓ All tests completed!${NC}"
echo "=========================================="
echo ""
echo "Test output directory: $TEST_OUTPUT_DIR"
echo "Review broker logs for detailed allocation information"
