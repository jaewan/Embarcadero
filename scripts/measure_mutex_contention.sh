#!/bin/bash
# Measure Mutex Contention for global_seq_batch_seq_mu_
# Uses perf to measure lock contention events

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Configuration
MEASURE_DURATION=${MEASURE_DURATION:-60}  # seconds
ORDER=${ORDER:-5}
ACK=${ACK:-1}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-10737418240}  # 10GB

# Output files
RESULTS_DIR="$PROJECT_ROOT/data/performance_baseline"
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
CONTENTION_REPORT="$RESULTS_DIR/mutex_contention_${TIMESTAMP}.txt"

echo "=========================================="
echo "Mutex Contention Measurement"
echo "=========================================="
echo "Duration: $MEASURE_DURATION seconds"
echo "Target: global_seq_batch_seq_mu_"
echo "Output: $CONTENTION_REPORT"
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

# Measure lock contention events
echo "Measuring mutex contention for $MEASURE_DURATION seconds..."
echo "Profiling head broker (PID: $HEAD_PID)..."

# Use perf to measure lock contention
# Note: This requires kernel support for lock events
perf stat -e \
    lock:lock_acquire,lock:lock_release,\
    contention:contentions,contention:contentions:u,\
    contention:contentions:k,\
    contention:wait_time,contention:wait_time:u,\
    contention:wait_time:k \
    -p $HEAD_PID sleep $MEASURE_DURATION > "$CONTENTION_REPORT" 2>&1 || {
    echo "Lock contention events not available, using alternative method..."
    
    # Alternative: Use perf to measure context switches and CPU time
    perf stat -e \
        context-switches,context-switches:u,context-switches:k,\
        cpu-migrations,\
        page-faults,page-faults:u,page-faults:k \
        -p $HEAD_PID sleep $MEASURE_DURATION > "$CONTENTION_REPORT" 2>&1
}

# Stop test
pkill -TERM -P $TEST_PID 2>/dev/null || true
wait $TEST_PID 2>/dev/null || true

# Analyze results
echo ""
echo "=========================================="
echo "Mutex Contention Analysis"
echo "=========================================="
cat "$CONTENTION_REPORT"

# Extract key metrics
CONTENTIONS=$(grep -i "contentions" "$CONTENTION_REPORT" | grep -oE "[0-9,]+" | head -1 | tr -d ',' || echo "0")
WAIT_TIME=$(grep -i "wait_time" "$CONTENTION_REPORT" | grep -oE "[0-9,]+" | head -1 | tr -d ',' || echo "0")
CTX_SWITCHES=$(grep -i "context-switches" "$CONTENTION_REPORT" | grep -oE "[0-9,]+" | head -1 | tr -d ',' || echo "0")

echo ""
echo "=========================================="
echo "Summary"
echo "=========================================="
echo "Lock contentions: $CONTENTIONS"
echo "Wait time (cycles): $WAIT_TIME"
echo "Context switches: $CTX_SWITCHES"

# Assessment
if [ "$CONTENTIONS" != "0" ] && [ "$CONTENTIONS" != "" ]; then
    CONTENTION_RATE=$((CONTENTIONS / MEASURE_DURATION))
    echo "Contention rate: $CONTENTION_RATE per second"
    
    if [ "$CONTENTION_RATE" -lt 100 ]; then
        echo "Assessment: ✓ Low contention (<100/sec) - Lock-free CAS not needed"
    elif [ "$CONTENTION_RATE" -lt 1000 ]; then
        echo "Assessment: ⚠ Moderate contention (100-1000/sec) - Consider lock-free CAS"
    else
        echo "Assessment: ✗ High contention (>1000/sec) - Lock-free CAS recommended"
    fi
else
    echo "Assessment: Unable to measure lock contention directly"
    echo "  Using context switches as proxy: $CTX_SWITCHES"
    if [ "$CTX_SWITCHES" -lt 10000 ]; then
        echo "  ✓ Low context switching - Mutex likely not a bottleneck"
    else
        echo "  ⚠ High context switching - May indicate contention"
    fi
fi

cleanup

echo ""
echo "=========================================="
echo "Measurement Complete"
echo "=========================================="
echo "Report: $CONTENTION_REPORT"
