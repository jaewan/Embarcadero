#!/bin/bash
# Latency Regression Test: Compare baseline vs BlogHeader v2
# Runs ORDER=5 latency tests for multiple message sizes

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Configuration
ORDER=5
ACK=1
MESSAGE_SIZES=(256 1024 65536)  # 256B, 1KB, 64KB
TOTAL_MESSAGE_SIZE=268435456  # 256MB per test (small for latency mode)
NUM_TRIALS=3  # Reduced for faster testing

RESULTS_DIR="$PROJECT_ROOT/data/latency/order5"
mkdir -p "$RESULTS_DIR"/baseline
mkdir -p "$RESULTS_DIR"/blog_v2

echo "=========================================="
echo "Latency Regression Test: BlogMessageHeader"
echo "=========================================="
echo "Order Level: $ORDER"
echo "Message Sizes: ${MESSAGE_SIZES[@]}"
echo "Trials per size: $NUM_TRIALS"
echo ""

# Cleanup function
cleanup() {
    pkill -9 -f "embarlet|throughput_test" 2>/dev/null || true
    sleep 2
}

# Function to start brokers (reuse from run_throughput.sh pattern)
start_brokers() {
    cd build/bin
    local num_brokers=4
    local pids=()
    
    # Start head broker
    numactl --cpunodebind=1 --membind=1 ./embarlet --config ../../config/embarcadero.yaml --head --EMBARCADERO > broker_0.log 2>&1 &
    local head_pid=$!
    pids+=($head_pid)
    sleep 3
    
    # Start follower brokers
    for ((i=1; i<$num_brokers; i++)); do
        numactl --cpunodebind=1 --membind=1 ./embarlet --config ../../config/embarcadero.yaml --EMBARCADERO > broker_${i}.log 2>&1 &
        pids+=($!)
        sleep 1
    done
    
    # Wait for brokers to be ready (simplified - just wait)
    sleep 5
    
    cd "$PROJECT_ROOT"
    echo "${pids[@]}"
}

# Function to run latency test
run_latency_test() {
    local blog_header=$1
    local msg_size=$2
    local trial=$3
    export EMBARCADERO_USE_BLOG_HEADER=$blog_header
    
    cd build/bin
    ./throughput_test -t 2 -o $ORDER --sequencer EMBARCADERO -a $ACK -m $msg_size -s $TOTAL_MESSAGE_SIZE --record_results --config ../../config/client.yaml > latency_test_${blog_header}_${msg_size}_${trial}.log 2>&1
    cd "$PROJECT_ROOT"
}

# Test each message size
for msg_size in "${MESSAGE_SIZES[@]}"; do
    echo "=========================================="
    echo "Testing message size: $msg_size bytes"
    echo "=========================================="
    
    # Baseline tests
    echo "[BASELINE] Running $NUM_TRIALS trials..."
    cleanup
    BROKER_PIDS=($(start_brokers))
    
    for trial in $(seq 1 $NUM_TRIALS); do
        echo "  Trial $trial/$NUM_TRIALS..."
        run_latency_test 0 $msg_size $trial
        sleep 2
    done
    
    # Move latency files
    if [ -f build/bin/cdf_latency_us.csv ]; then
        mv build/bin/cdf_latency_us.csv "$RESULTS_DIR/baseline/cdf_${msg_size}.csv"
    fi
    if [ -f build/bin/latency_stats.csv ]; then
        mv build/bin/latency_stats.csv "$RESULTS_DIR/baseline/stats_${msg_size}.csv"
    fi
    
    # Kill brokers
    for pid in "${BROKER_PIDS[@]}"; do
        kill $pid 2>/dev/null || true
    done
    cleanup
    sleep 3
    
    # BlogHeader v2 tests
    echo "[BLOGHEADER V2] Running $NUM_TRIALS trials..."
    BROKER_PIDS=($(start_brokers))
    
    for trial in $(seq 1 $NUM_TRIALS); do
        echo "  Trial $trial/$NUM_TRIALS..."
        run_latency_test 1 $msg_size $trial
        sleep 2
    done
    
    # Move latency files
    if [ -f build/bin/cdf_latency_us.csv ]; then
        mv build/bin/cdf_latency_us.csv "$RESULTS_DIR/blog_v2/cdf_${msg_size}.csv"
    fi
    if [ -f build/bin/latency_stats.csv ]; then
        mv build/bin/latency_stats.csv "$RESULTS_DIR/blog_v2/stats_${msg_size}.csv"
    fi
    
    # Kill brokers
    for pid in "${BROKER_PIDS[@]}"; do
        kill $pid 2>/dev/null || true
    done
    cleanup
    sleep 3
    
    echo ""
done

# Compare results
echo "=========================================="
echo "Latency Comparison"
echo "=========================================="

for msg_size in "${MESSAGE_SIZES[@]}"; do
    baseline_stats="$RESULTS_DIR/baseline/stats_${msg_size}.csv"
    blog_stats="$RESULTS_DIR/blog_v2/stats_${msg_size}.csv"
    
    if [ -f "$baseline_stats" ] && [ -f "$blog_stats" ]; then
        echo "Message size: $msg_size bytes"
        
        # Extract p50 and p99 (assuming CSV format with headers)
        BASELINE_P50=$(awk -F',' 'NR==2 {print $1}' "$baseline_stats" 2>/dev/null || echo "0")
        BASELINE_P99=$(awk -F',' 'NR==2 {print $2}' "$baseline_stats" 2>/dev/null || echo "0")
        BLOG_P50=$(awk -F',' 'NR==2 {print $1}' "$blog_stats" 2>/dev/null || echo "0")
        BLOG_P99=$(awk -F',' 'NR==2 {print $2}' "$blog_stats" 2>/dev/null || echo "0")
        
        echo "  Baseline: p50=${BASELINE_P50}us, p99=${BASELINE_P99}us"
        echo "  BlogHeader: p50=${BLOG_P50}us, p99=${BLOG_P99}us"
        
        # Simple comparison (would need proper CSV parsing)
        echo "  Comparison: See CSV files for details"
    else
        echo "Message size: $msg_size bytes - Stats files not found"
    fi
    echo ""
done

echo "Latency regression test completed"
echo "Results in: $RESULTS_DIR"
