#!/bin/bash
# Profile Hot Paths with perf
# Identifies CPU bottlenecks, cache misses, and branch mispredictions

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Configuration
PROFILE_DURATION=${PROFILE_DURATION:-30}  # seconds
ORDER=${ORDER:-5}
ACK=${ACK:-1}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-10737418240}  # 10GB

# Output files
RESULTS_DIR="$PROJECT_ROOT/data/performance_baseline"
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
PERF_DATA="$RESULTS_DIR/perf_data_${TIMESTAMP}.data"
PERF_REPORT="$RESULTS_DIR/perf_report_${TIMESTAMP}.txt"
PERF_FLAMEGRAPH="$RESULTS_DIR/perf_flamegraph_${TIMESTAMP}.svg"

echo "=========================================="
echo "Hot Path Profiling with perf"
echo "=========================================="
echo "Duration: $PROFILE_DURATION seconds"
echo "Order Level: $ORDER"
echo "ACK Level: $ACK"
echo "Output: $PERF_DATA"
echo "=========================================="
echo ""

# Check if perf is available
if ! command -v perf &> /dev/null; then
    echo "ERROR: perf is not installed"
    echo "Install with: sudo apt-get install linux-perf"
    exit 1
fi

# Cleanup function
cleanup() {
    echo "Cleaning up..."
    pkill -9 -f "throughput_test|embarlet" 2>/dev/null || true
    sleep 2
    rm -f /tmp/embarlet_*_ready build/bin/broker_*.log 2>/dev/null || true
}

cleanup

# Start brokers in background
export ORDER=$ORDER ACK=$ACK MESSAGE_SIZE=$MESSAGE_SIZE TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE

echo "Starting brokers..."
cd build/bin
./embarlet --config ../../config/embarcadero.yaml --broker-id 0 --head > /tmp/broker_0.log 2>&1 &
HEAD_PID=$!
sleep 5

for i in {1..3}; do
    numactl --cpunodebind=1 --membind=1 ./embarlet --config ../../config/embarcadero.yaml --broker-id $i > /tmp/broker_${i}.log 2>&1 &
    sleep 2
done

# Wait for brokers to be ready
echo "Waiting for brokers to be ready..."
for i in {1..30}; do
    if [ -f /tmp/embarlet_${HEAD_PID}_ready ]; then
        READY_COUNT=$(ls -1 /tmp/embarlet_*_ready 2>/dev/null | wc -l)
        if [ "$READY_COUNT" -ge "4" ]; then
            echo "All brokers ready!"
            break
        fi
    fi
    sleep 1
done

# Start throughput test in background
echo "Starting throughput test..."
cd "$PROJECT_ROOT"
./build/bin/throughput_test --config config/client.yaml > /tmp/throughput_test.log 2>&1 &
TEST_PID=$!

# Profile the head broker (where sequencer runs)
echo "Profiling head broker (PID: $HEAD_PID) for $PROFILE_DURATION seconds..."
perf record -g -F 99 -p $HEAD_PID --call-graph dwarf -o "$PERF_DATA" sleep $PROFILE_DURATION

# Stop test
pkill -TERM -P $TEST_PID 2>/dev/null || true
wait $TEST_PID 2>/dev/null || true

# Generate reports
echo ""
echo "Generating perf reports..."

# Flat report
perf report -i "$PERF_DATA" --stdio --no-children > "$PERF_REPORT" 2>&1

# Top functions
echo ""
echo "=========================================="
echo "Top 20 Functions by CPU Time"
echo "=========================================="
perf report -i "$PERF_DATA" --stdio --no-children --sort comm,dso,symbol | head -50

# Cache statistics
echo ""
echo "=========================================="
echo "Cache Statistics"
echo "=========================================="
perf stat -e cache-references,cache-misses,LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses -i "$PERF_DATA" 2>&1 | tail -10 || echo "Cache stats not available"

# Branch statistics
echo ""
echo "=========================================="
echo "Branch Prediction Statistics"
echo "=========================================="
perf stat -e branches,branch-misses -i "$PERF_DATA" 2>&1 | tail -5 || echo "Branch stats not available"

# Generate flamegraph if available
if command -v perf script &> /dev/null && command -v flamegraph.pl &> /dev/null; then
    echo ""
    echo "Generating flamegraph..."
    perf script -i "$PERF_DATA" | stackcollapse-perf.pl | flamegraph.pl > "$PERF_FLAMEGRAPH" 2>/dev/null || echo "Flamegraph generation skipped (tools not available)"
    if [ -f "$PERF_FLAMEGRAPH" ]; then
        echo "Flamegraph: $PERF_FLAMEGRAPH"
    fi
fi

cleanup

echo ""
echo "=========================================="
echo "Profiling Complete"
echo "=========================================="
echo "Perf data: $PERF_DATA"
echo "Report: $PERF_REPORT"
if [ -f "$PERF_FLAMEGRAPH" ]; then
    echo "Flamegraph: $PERF_FLAMEGRAPH"
fi
echo ""
echo "To view interactive report:"
echo "  perf report -i $PERF_DATA"
