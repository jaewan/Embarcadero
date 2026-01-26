#!/bin/bash
# Throughput Regression Test: Compare baseline vs BlogHeader v2
# Runs performance baseline script for both variants and compares results

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Configuration
NUM_ITERATIONS=${NUM_ITERATIONS:-5}  # Reduced for faster testing
ORDER=${ORDER:-5}
ACK=${ACK:-1}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-1073741824}  # 1GB for faster testing

RESULTS_DIR="$PROJECT_ROOT/data/performance_baseline"
mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "Throughput Regression Test: BlogMessageHeader"
echo "=========================================="
echo "Iterations: $NUM_ITERATIONS"
echo "Order Level: $ORDER"
echo "Message Size: $MESSAGE_SIZE bytes"
echo "Total Data: $TOTAL_MESSAGE_SIZE bytes"
echo ""

# Cleanup function
cleanup() {
    pkill -9 -f "embarlet|throughput_test" 2>/dev/null || true
    sleep 2
}

# Test 1: Baseline
echo "=========================================="
echo "[TEST 1/2] Baseline (EMBARCADERO_USE_BLOG_HEADER=0)"
echo "=========================================="
export EMBARCADERO_USE_BLOG_HEADER=0
export NUM_ITERATIONS=$NUM_ITERATIONS
export ORDER=$ORDER
export ACK=$ACK
export MESSAGE_SIZE=$MESSAGE_SIZE
export TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE

cleanup
BASELINE_SUMMARY=$(mktemp)
BASELINE_CSV=""
if bash scripts/measure_performance_baseline.sh > "$BASELINE_SUMMARY" 2>&1; then
    BASELINE_CSV=$(ls -t "$RESULTS_DIR"/baseline_*.csv 2>/dev/null | head -1 || echo "")
    if [ -n "$BASELINE_CSV" ] && [ -f "$BASELINE_CSV" ]; then
        echo "Baseline CSV: $BASELINE_CSV"
        BASELINE_PASSED=1
    else
        echo "ERROR: Baseline CSV file not found"
        cat "$BASELINE_SUMMARY"
        BASELINE_PASSED=0
    fi
else
    echo "ERROR: Baseline test failed"
    cat "$BASELINE_SUMMARY" | tail -50
    BASELINE_PASSED=0
fi
rm -f "$BASELINE_SUMMARY"

# Extract baseline metrics from CSV
if [ -n "$BASELINE_CSV" ] && [ -f "$BASELINE_CSV" ]; then
    BASELINE_MEAN=$(python3 << EOF
import csv
import statistics
try:
    with open('$BASELINE_CSV', 'r') as f:
        reader = csv.DictReader(f)
        results = [float(row['bandwidth_mbps']) for row in reader if row['status'] == 'success']
    if results:
        print(f"{statistics.mean(results):.2f}")
    else:
        print("0")
except:
    print("0")
EOF
)
    BASELINE_P95=$(python3 << EOF
import csv
import statistics
try:
    with open('$BASELINE_CSV', 'r') as f:
        reader = csv.DictReader(f)
        results = sorted([float(row['bandwidth_mbps']) for row in reader if row['status'] == 'success'])
    if results:
        p95_idx = int(len(results) * 0.95)
        p95 = results[p95_idx] if p95_idx < len(results) else results[-1]
        print(f"{p95:.2f}")
    else:
        print("0")
except:
    print("0")
EOF
)
    BASELINE_CV=$(python3 << EOF
import csv
import statistics
try:
    with open('$BASELINE_CSV', 'r') as f:
        reader = csv.DictReader(f)
        results = [float(row['bandwidth_mbps']) for row in reader if row['status'] == 'success']
    if results and len(results) > 1:
        mean = statistics.mean(results)
        stdev = statistics.stdev(results)
        cv = (stdev/mean*100) if mean > 0 else 0
        print(f"{cv:.2f}")
    else:
        print("0")
except:
    print("0")
EOF
)
else
    BASELINE_MEAN=0
    BASELINE_P95=0
    BASELINE_CV=0
fi

echo ""
echo "Baseline Metrics:"
echo "  Mean: $BASELINE_MEAN MB/s"
echo "  P95: $BASELINE_P95 MB/s"
echo "  CV: $BASELINE_CV%"
echo ""

# Cleanup before next test
cleanup
sleep 3

# Test 2: BlogHeader v2
echo "=========================================="
echo "[TEST 2/2] BlogHeader v2 (EMBARCADERO_USE_BLOG_HEADER=1)"
echo "=========================================="
export EMBARCADERO_USE_BLOG_HEADER=1
export NUM_ITERATIONS=$NUM_ITERATIONS
export ORDER=$ORDER
export ACK=$ACK
export MESSAGE_SIZE=$MESSAGE_SIZE
export TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE

BLOG_SUMMARY=$(mktemp)
BLOG_CSV=""
if bash scripts/measure_performance_baseline.sh > "$BLOG_SUMMARY" 2>&1; then
    BLOG_CSV=$(ls -t "$RESULTS_DIR"/baseline_*.csv 2>/dev/null | head -1 || echo "")
    if [ -n "$BLOG_CSV" ] && [ -f "$BLOG_CSV" ] && [ "$BLOG_CSV" != "$BASELINE_CSV" ]; then
        echo "BlogHeader v2 CSV: $BLOG_CSV"
        BLOG_PASSED=1
    else
        echo "ERROR: BlogHeader CSV file not found or same as baseline"
        cat "$BLOG_SUMMARY"
        BLOG_PASSED=0
    fi
else
    echo "ERROR: BlogHeader test failed"
    cat "$BLOG_SUMMARY" | tail -50
    BLOG_PASSED=0
fi
rm -f "$BLOG_SUMMARY"

# Extract BlogHeader metrics from CSV
if [ -n "$BLOG_CSV" ] && [ -f "$BLOG_CSV" ]; then
    BLOG_MEAN=$(python3 << EOF
import csv
import statistics
try:
    with open('$BLOG_CSV', 'r') as f:
        reader = csv.DictReader(f)
        results = [float(row['bandwidth_mbps']) for row in reader if row['status'] == 'success']
    if results:
        print(f"{statistics.mean(results):.2f}")
    else:
        print("0")
except:
    print("0")
EOF
)
    BLOG_P95=$(python3 << EOF
import csv
import statistics
try:
    with open('$BLOG_CSV', 'r') as f:
        reader = csv.DictReader(f)
        results = sorted([float(row['bandwidth_mbps']) for row in reader if row['status'] == 'success'])
    if results:
        p95_idx = int(len(results) * 0.95)
        p95 = results[p95_idx] if p95_idx < len(results) else results[-1]
        print(f"{p95:.2f}")
    else:
        print("0")
except:
    print("0")
EOF
)
    BLOG_CV=$(python3 << EOF
import csv
import statistics
try:
    with open('$BLOG_CSV', 'r') as f:
        reader = csv.DictReader(f)
        results = [float(row['bandwidth_mbps']) for row in reader if row['status'] == 'success']
    if results and len(results) > 1:
        mean = statistics.mean(results)
        stdev = statistics.stdev(results)
        cv = (stdev/mean*100) if mean > 0 else 0
        print(f"{cv:.2f}")
    else:
        print("0")
except:
    print("0")
EOF
)
else
    BLOG_MEAN=0
    BLOG_P95=0
    BLOG_CV=0
fi

echo ""
echo "BlogHeader v2 Metrics:"
echo "  Mean: $BLOG_MEAN MB/s"
echo "  P95: $BLOG_P95 MB/s"
echo "  CV: $BLOG_CV%"
echo ""

# Comparison and acceptance criteria
echo "=========================================="
echo "Regression Analysis"
echo "=========================================="

# Convert to float for comparison (using awk)
COMPARE_RESULT=$(awk -v baseline="$BASELINE_MEAN" -v blog="$BLOG_MEAN" 'BEGIN {
    if (baseline > 0 && blog > 0) {
        ratio = blog / baseline
        if (ratio >= 0.98) {
            print "PASS"
        } else {
            print "FAIL"
        }
    } else {
        print "ERROR"
    }
}')

P95_COMPARE=$(awk -v baseline="$BASELINE_P95" -v blog="$BLOG_P95" 'BEGIN {
    if (baseline > 0 && blog > 0) {
        ratio = blog / baseline
        if (ratio >= 0.98) {
            print "PASS"
        } else {
            print "FAIL"
        }
    } else {
        print "ERROR"
    }
}')

CV_BASELINE_OK=$(awk -v cv="$BASELINE_CV" 'BEGIN { if (cv < 10.0) print "PASS"; else print "FAIL" }')
CV_BLOG_OK=$(awk -v cv="$BLOG_CV" 'BEGIN { if (cv < 10.0) print "PASS"; else print "FAIL" }')

echo "Throughput Mean: $COMPARE_RESULT (BlogHeader: ${BLOG_MEAN} MB/s vs Baseline: ${BASELINE_MEAN} MB/s)"
echo "Throughput P95: $P95_COMPARE (BlogHeader: ${BLOG_P95} MB/s vs Baseline: ${BASELINE_P95} MB/s)"
echo "Baseline CV: $CV_BASELINE_OK (${BASELINE_CV}%)"
echo "BlogHeader CV: $CV_BLOG_OK (${BLOG_CV}%)"
echo ""

# Final verdict
if [ "$COMPARE_RESULT" = "PASS" ] && [ "$P95_COMPARE" = "PASS" ] && [ "$CV_BASELINE_OK" = "PASS" ] && [ "$CV_BLOG_OK" = "PASS" ] && [ $BASELINE_PASSED -eq 1 ] && [ $BLOG_PASSED -eq 1 ]; then
    echo "✓ Throughput regression test PASSED"
    echo "  - Mean throughput within 2% of baseline"
    echo "  - P95 throughput within 2% of baseline"
    echo "  - Both variants have CV < 10%"
    exit 0
else
    echo "✗ Throughput regression test FAILED"
    [ "$COMPARE_RESULT" != "PASS" ] && echo "  - Mean throughput regression detected"
    [ "$P95_COMPARE" != "PASS" ] && echo "  - P95 throughput regression detected"
    [ "$CV_BASELINE_OK" != "PASS" ] && echo "  - Baseline CV too high"
    [ "$CV_BLOG_OK" != "PASS" ] && echo "  - BlogHeader CV too high"
    exit 1
fi
