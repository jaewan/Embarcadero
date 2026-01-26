#!/bin/bash
# Proper Bandwidth Measurement Script
# Uses CSV output instead of log scraping, verifies environment, runs proper tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Configuration - matching historical 9-10 GB/s expectations
NUM_ITERATIONS=${NUM_ITERATIONS:-3}  # Reduced for faster testing, increase for production
ORDER=${ORDER:-5}
ACK=${ACK:-1}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}  # 1KB payload
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-10737418240}  # 10GB total (10737418240 bytes)
TEST_NUMBER=${TEST_NUMBER:-5}  # 5 = publish-only (matches run_throughput.sh default)

RESULTS_DIR="$PROJECT_ROOT/data/bandwidth_comparison"
mkdir -p "$RESULTS_DIR"

echo "=========================================="
echo "Proper Bandwidth Measurement"
echo "=========================================="
echo "Configuration:"
echo "  Iterations: $NUM_ITERATIONS"
echo "  Order: $ORDER, ACK: $ACK"
echo "  Message Size: $MESSAGE_SIZE bytes (1KB)"
TOTAL_GB=$(python3 -c "print(f'{int($TOTAL_MESSAGE_SIZE) / (1024**3):.1f}')")
echo "  Total Message Size: $TOTAL_MESSAGE_SIZE bytes (${TOTAL_GB}GB)"
echo "  Test Number: $TEST_NUMBER (5=publish-only, 1=E2E)"
echo ""

# Environment verification
echo "=========================================="
echo "Environment Verification"
echo "=========================================="

# Check hugepages
HUGE_PAGES=$(cat /proc/sys/vm/nr_hugepages 2>/dev/null || echo "0")
HUGE_PAGE_SIZE=$(grep Hugepagesize /proc/meminfo | awk '{print $2}' 2>/dev/null || echo "0")
if [ "$HUGE_PAGES" -gt 0 ]; then
    HUGE_TOTAL_MB=$((HUGE_PAGES * HUGE_PAGE_SIZE / 1024))
    echo "✓ Hugepages: $HUGE_PAGES pages × ${HUGE_PAGE_SIZE}KB = ${HUGE_TOTAL_MB}MB available"
else
    echo "⚠ Hugepages: Not configured (may impact performance)"
fi

# Check NUMA
if command -v numactl &> /dev/null; then
    NUMA_NODES=$(numactl --hardware 2>/dev/null | grep "available:" | awk '{print $2}' || echo "unknown")
    echo "✓ NUMA: $NUMA_NODES nodes available"
    numactl --hardware 2>/dev/null | grep "node 1" || echo "  (node 1 details not available)"
else
    echo "⚠ numactl: Not available"
fi

# Check CPU governor
if [ -f /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor ]; then
    GOVERNOR=$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo "unknown")
    echo "✓ CPU Governor: $GOVERNOR"
    if [ "$GOVERNOR" != "performance" ]; then
        echo "  ⚠ Consider setting to 'performance' for consistent results"
    fi
else
    echo "⚠ CPU Governor: Cannot determine"
fi

echo ""

# Cleanup function
cleanup() {
    pkill -9 -f "embarlet|throughput_test" 2>/dev/null || true
    sleep 2
    rm -f /tmp/embarlet_*_ready build/bin/broker_*.log 2>/dev/null || true
}

# Function to run a single test iteration and extract bandwidth from CSV
run_test_iteration() {
    local blog_header=$1
    local iteration=$2
    local csv_file="$RESULTS_DIR/result_${blog_header}_${iteration}.csv"
    
    export EMBARCADERO_USE_BLOG_HEADER=$blog_header
    export ORDER=$ORDER ACK=$ACK MESSAGE_SIZE=$MESSAGE_SIZE TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE
    export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
    
    cleanup
    
    # Remove old CSV to ensure fresh results
    rm -f data/throughput/pub/result.csv 2>/dev/null || true
    
    # Run test using run_throughput.sh (which calls throughput_test with --record_results)
    # run_throughput.sh expects to be run from project root and will cd to build/bin itself
    # Increased timeout to 600s (10 min) for 10GB tests - allows for broker startup, test execution, and ACK completion
    if timeout 600 bash scripts/run_throughput.sh > /tmp/test_${blog_header}_${iteration}.log 2>&1; then
        
        # Extract bandwidth from CSV (the authoritative source)
        # Wait a moment for CSV to be written
        sleep 1
        if [ -f "data/throughput/pub/result.csv" ]; then
            # CSV format: message_size,total_message_size,...,pub_bandwidth_mbps,sub_bandwidth_mbps,e2e_bandwidth_mbps
            # For test_number=5 (publish-only), pub_bandwidth_mbps is field 13 (pub_bandwidth_mbps)
            # Use awk to handle CSV properly (handles quoted fields)
            BANDWIDTH=$(tail -1 data/throughput/pub/result.csv | awk -F',' '{print $13}' | tr -d ' ' || echo "0")
            
            if [ -n "$BANDWIDTH" ] && [ "$BANDWIDTH" != "0" ] && [ "$BANDWIDTH" != "" ]; then
                echo "$BANDWIDTH"
                return 0
            fi
        fi
        
        # Fallback: try to extract from log (but log this as a warning)
        BANDWIDTH=$(grep -i "Bandwidth:" /tmp/test_${blog_header}_${iteration}.log | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "0")
        if [ "$BANDWIDTH" != "0" ]; then
            echo "  ⚠ Using log fallback (CSV not found)" >&2
            echo "$BANDWIDTH"
            return 0
        fi
    else
        cd "$PROJECT_ROOT"
        echo "0"
        return 1
    fi
    
    echo "0"
    return 1
}

# Test 1: Baseline
echo "=========================================="
echo "[TEST 1/2] Baseline (EMBARCADERO_USE_BLOG_HEADER=0)"
echo "=========================================="
BASELINE_RESULTS=()
for i in $(seq 1 $NUM_ITERATIONS); do
    echo "  Iteration $i/$NUM_ITERATIONS..."
    bw=$(run_test_iteration 0 $i)
    if [ "$bw" != "0" ] && [ -n "$bw" ]; then
        BASELINE_RESULTS+=($bw)
        echo "    → $bw MB/s"
    else
        echo "    ✗ Failed or timeout"
        tail -20 /tmp/test_0_${i}.log 2>/dev/null | head -10 || true
    fi
    sleep 3  # Brief pause between iterations
done

# Test 2: BlogHeader v2
echo ""
echo "=========================================="
echo "[TEST 2/2] BlogHeader v2 (EMBARCADERO_USE_BLOG_HEADER=1)"
echo "=========================================="
BLOG_RESULTS=()
for i in $(seq 1 $NUM_ITERATIONS); do
    echo "  Iteration $i/$NUM_ITERATIONS..."
    bw=$(run_test_iteration 1 $i)
    if [ "$bw" != "0" ] && [ -n "$bw" ]; then
        BLOG_RESULTS+=($bw)
        echo "    → $bw MB/s"
    else
        echo "    ✗ Failed or timeout"
        tail -20 /tmp/test_1_${i}.log 2>/dev/null | head -10 || true
    fi
    sleep 3
done

# Calculate statistics
echo ""
echo "=========================================="
echo "Results Analysis"
echo "=========================================="

# Build Python arrays from bash arrays
BASELINE_PY_ARRAY="[$(IFS=','; echo "${BASELINE_RESULTS[*]}")]"
BLOG_PY_ARRAY="[$(IFS=','; echo "${BLOG_RESULTS[*]}")]"

STATS=$(python3 << EOF
import statistics

baseline = [float(x) for x in $BASELINE_PY_ARRAY if x]
blog = [float(x) for x in $BLOG_PY_ARRAY if x]

if baseline:
    baseline_mean = statistics.mean(baseline)
    baseline_stdev = statistics.stdev(baseline) if len(baseline) > 1 else 0.0
    baseline_cv = (baseline_stdev / baseline_mean * 100) if baseline_mean > 0 else 0.0
    baseline_sorted = sorted(baseline)
    baseline_p95 = baseline_sorted[int(len(baseline_sorted) * 0.95)] if baseline_sorted else baseline_mean
    print(f"BASELINE_MEAN={baseline_mean:.2f}")
    print(f"BASELINE_STDEV={baseline_stdev:.2f}")
    print(f"BASELINE_CV={baseline_cv:.2f}")
    print(f"BASELINE_P95={baseline_p95:.2f}")
    print(f"BASELINE_COUNT={len(baseline)}")
else:
    print("BASELINE_MEAN=0")
    print("BASELINE_STDEV=0")
    print("BASELINE_CV=0")
    print("BASELINE_P95=0")
    print("BASELINE_COUNT=0")

if blog:
    blog_mean = statistics.mean(blog)
    blog_stdev = statistics.stdev(blog) if len(blog) > 1 else 0.0
    blog_cv = (blog_stdev / blog_mean * 100) if blog_mean > 0 else 0.0
    blog_sorted = sorted(blog)
    blog_p95 = blog_sorted[int(len(blog_sorted) * 0.95)] if blog_sorted else blog_mean
    print(f"BLOG_MEAN={blog_mean:.2f}")
    print(f"BLOG_STDEV={blog_stdev:.2f}")
    print(f"BLOG_CV={blog_cv:.2f}")
    print(f"BLOG_P95={blog_p95:.2f}")
    print(f"BLOG_COUNT={len(blog)}")
else:
    print("BLOG_MEAN=0")
    print("BLOG_STDEV=0")
    print("BLOG_CV=0")
    print("BLOG_P95=0")
    print("BLOG_COUNT=0")
EOF
)

# Parse stats
eval "$STATS"

echo "Baseline (BlogHeader=0):"
if [ "$BASELINE_COUNT" -gt 0 ]; then
    echo "  Results: ${BASELINE_RESULTS[@]}"
    echo "  Mean: $BASELINE_MEAN MB/s"
    echo "  StdDev: $BASELINE_STDEV MB/s"
    echo "  CV: $BASELINE_CV%"
    echo "  P95: $BASELINE_P95 MB/s"
    echo "  Expected: 9000-10000 MB/s (9-10 GB/s)"
    if (( $(echo "$BASELINE_MEAN >= 9000" | bc -l) )); then
        echo "  ✓ Within expected range"
    else
        echo "  ⚠ Below expected range - check environment/config"
    fi
else
    echo "  ✗ No successful iterations"
fi

echo ""
echo "BlogHeader v2 (BlogHeader=1):"
if [ "$BLOG_COUNT" -gt 0 ]; then
    echo "  Results: ${BLOG_RESULTS[@]}"
    echo "  Mean: $BLOG_MEAN MB/s"
    echo "  StdDev: $BLOG_STDEV MB/s"
    echo "  CV: $BLOG_CV%"
    echo "  P95: $BLOG_P95 MB/s"
else
    echo "  ✗ No successful iterations"
fi

# Comparison
echo ""
echo "Comparison:"
if [ "$BASELINE_COUNT" -gt 0 ] && [ "$BLOG_COUNT" -gt 0 ]; then
    RATIO=$(python3 << EOF
baseline = float("$BASELINE_MEAN")
blog = float("$BLOG_MEAN")
if baseline > 0:
    ratio = (blog / baseline) * 100
    print(f"{ratio:.1f}")
else:
    print("0")
EOF
)
    
    echo "  BlogHeader v2 is ${RATIO}% of baseline"
    echo "  Baseline: $BASELINE_MEAN MB/s"
    echo "  BlogHeader: $BLOG_MEAN MB/s"
    echo "  Difference: $(python3 -c "print(f'{abs($BLOG_MEAN - $BASELINE_MEAN):.2f}')") MB/s"
    
    if (( $(echo "$RATIO >= 98" | bc -l) )); then
        echo "  ✓ Within acceptable range (>=98%)"
        EXIT_CODE=0
    else
        echo "  ✗ Regression detected (<98%)"
        EXIT_CODE=1
    fi
    
    # Write summary to file
    SUMMARY_FILE="$RESULTS_DIR/summary_$(date +%Y%m%d_%H%M%S).txt"
    cat > "$SUMMARY_FILE" << EOF
Bandwidth Comparison Summary
============================
Date: $(date)
Configuration:
  Order: $ORDER, ACK: $ACK
  Message Size: $MESSAGE_SIZE bytes
  Total Message Size: $TOTAL_MESSAGE_SIZE bytes
  Test Number: $TEST_NUMBER
  Iterations: $NUM_ITERATIONS

Baseline (BlogHeader=0):
  Mean: $BASELINE_MEAN MB/s
  StdDev: $BASELINE_STDEV MB/s
  CV: $BASELINE_CV%
  P95: $BASELINE_P95 MB/s
  Count: $BASELINE_COUNT
  Values: ${BASELINE_RESULTS[@]}

BlogHeader v2 (BlogHeader=1):
  Mean: $BLOG_MEAN MB/s
  StdDev: $BLOG_STDEV MB/s
  CV: $BLOG_CV%
  P95: $BLOG_P95 MB/s
  Count: $BLOG_COUNT
  Values: ${BLOG_RESULTS[@]}

Comparison:
  Ratio: ${RATIO}%
  Difference: $(python3 -c "print(f'{abs($BLOG_MEAN - $BASELINE_MEAN):.2f}')") MB/s
  Status: $([ $EXIT_CODE -eq 0 ] && echo "PASS" || echo "FAIL")
EOF
    echo ""
    echo "Summary written to: $SUMMARY_FILE"
else
    echo "  ✗ Cannot compare - missing results"
    EXIT_CODE=1
fi

cleanup
exit $EXIT_CODE
