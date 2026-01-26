#!/bin/bash
# Simplified Throughput Regression Test
# Runs a few iterations directly and compares results

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

NUM_ITERATIONS=${NUM_ITERATIONS:-3}
ORDER=${ORDER:-5}
ACK=${ACK:-1}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-1073741824}  # 1GB

RESULTS_DIR="$PROJECT_ROOT/data/performance_regression"
mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "Throughput Regression Test (Simplified)"
echo "=========================================="
echo "Iterations: $NUM_ITERATIONS"
echo ""

# Cleanup
cleanup() {
    pkill -9 -f "embarlet|throughput_test" 2>/dev/null || true
    sleep 2
}

# Function to run throughput test and extract bandwidth
run_test_iteration() {
    local blog_header=$1
    local iteration=$2
    export EMBARCADERO_USE_BLOG_HEADER=$blog_header
    export ORDER=$ORDER ACK=$ACK MESSAGE_SIZE=$MESSAGE_SIZE TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE
    
    cleanup
    
    # Run single iteration using run_throughput.sh
    cd build/bin
    local output=$(timeout 300 bash ../../scripts/run_throughput.sh 2>&1 | tee /tmp/throughput_iter_${blog_header}_${iteration}.log)
    cd "$PROJECT_ROOT"
    
    # Extract bandwidth from output
    local bandwidth=$(echo "$output" | grep -i "Bandwidth:" | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "0")
    echo "$bandwidth"
}

# Test 1: Baseline
echo "[TEST 1/2] Baseline (BlogHeader=0)"
BASELINE_RESULTS=()
for i in $(seq 1 $NUM_ITERATIONS); do
    echo "  Iteration $i/$NUM_ITERATIONS..."
    bw=$(run_test_iteration 0 $i)
    if [ "$bw" != "0" ]; then
        BASELINE_RESULTS+=($bw)
        echo "    Bandwidth: $bw MB/s"
    else
        echo "    Failed to extract bandwidth"
    fi
    sleep 2
done

# Test 2: BlogHeader v2
echo ""
echo "[TEST 2/2] BlogHeader v2 (BlogHeader=1)"
BLOG_RESULTS=()
for i in $(seq 1 $NUM_ITERATIONS); do
    echo "  Iteration $i/$NUM_ITERATIONS..."
    bw=$(run_test_iteration 1 $i)
    if [ "$bw" != "0" ]; then
        BLOG_RESULTS+=($bw)
        echo "    Bandwidth: $bw MB/s"
    else
        echo "    Failed to extract bandwidth"
    fi
    sleep 2
done

# Calculate statistics using Python
BASELINE_STATS=$(python3 << EOF
import statistics
results = [float(x) for x in ${BASELINE_RESULTS[@]}]
if results:
    mean = statistics.mean(results)
    stdev = statistics.stdev(results) if len(results) > 1 else 0.0
    sorted_results = sorted(results)
    p95_idx = int(len(sorted_results) * 0.95)
    p95 = sorted_results[p95_idx] if p95_idx < len(sorted_results) else sorted_results[-1]
    cv = (stdev/mean*100) if mean > 0 else 0
    print(f"{mean:.2f} {p95:.2f} {cv:.2f}")
else:
    print("0 0 0")
EOF
)

BLOG_STATS=$(python3 << EOF
import statistics
results = [float(x) for x in ${BLOG_RESULTS[@]}]
if results:
    mean = statistics.mean(results)
    stdev = statistics.stdev(results) if len(results) > 1 else 0.0
    sorted_results = sorted(results)
    p95_idx = int(len(sorted_results) * 0.95)
    p95 = sorted_results[p95_idx] if p95_idx < len(sorted_results) else sorted_results[-1]
    cv = (stdev/mean*100) if mean > 0 else 0
    print(f"{mean:.2f} {p95:.2f} {cv:.2f}")
else:
    print("0 0 0")
EOF
)

BASELINE_MEAN=$(echo $BASELINE_STATS | cut -d' ' -f1)
BASELINE_P95=$(echo $BASELINE_STATS | cut -d' ' -f2)
BASELINE_CV=$(echo $BASELINE_STATS | cut -d' ' -f3)

BLOG_MEAN=$(echo $BLOG_STATS | cut -d' ' -f1)
BLOG_P95=$(echo $BLOG_STATS | cut -d' ' -f2)
BLOG_CV=$(echo $BLOG_STATS | cut -d' ' -f3)

echo ""
echo "=========================================="
echo "Results Summary"
echo "=========================================="
echo "Baseline:"
echo "  Mean: $BASELINE_MEAN MB/s"
echo "  P95: $BASELINE_P95 MB/s"
echo "  CV: $BASELINE_CV%"
echo ""
echo "BlogHeader v2:"
echo "  Mean: $BLOG_MEAN MB/s"
echo "  P95: $BLOG_P95 MB/s"
echo "  CV: $BLOG_CV%"
echo ""

# Comparison
COMPARE=$(python3 << EOF
baseline = float("$BASELINE_MEAN")
blog = float("$BLOG_MEAN")
if baseline > 0 and blog > 0:
    ratio = blog / baseline
    if ratio >= 0.98:
        print("PASS")
    else:
        print("FAIL")
else:
    print("ERROR")
EOF
)

P95_COMPARE=$(python3 << EOF
baseline = float("$BASELINE_P95")
blog = float("$BLOG_P95")
if baseline > 0 and blog > 0:
    ratio = blog / baseline
    if ratio >= 0.98:
        print("PASS")
    else:
        print("FAIL")
else:
    print("ERROR")
EOF
)

echo "Regression Analysis:"
echo "  Mean: $COMPARE (${BLOG_MEAN} vs ${BASELINE_MEAN} MB/s)"
echo "  P95: $P95_COMPARE (${BLOG_P95} vs ${BASELINE_P95} MB/s)"
echo "  Baseline CV: $([ $(echo "$BASELINE_CV < 10" | bc) -eq 1 ] && echo "PASS" || echo "FAIL")"
echo "  BlogHeader CV: $([ $(echo "$BLOG_CV < 10" | bc) -eq 1 ] && echo "PASS" || echo "FAIL")"
echo ""

if [ "$COMPARE" = "PASS" ] && [ "$P95_COMPARE" = "PASS" ]; then
    echo "✓ Throughput regression test PASSED"
    exit 0
else
    echo "✗ Throughput regression test FAILED"
    exit 1
fi
