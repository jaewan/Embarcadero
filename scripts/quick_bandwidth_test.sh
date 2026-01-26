#!/bin/bash
# Quick bandwidth comparison test

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

NUM_ITERATIONS=${NUM_ITERATIONS:-3}
ORDER=5
ACK=1
MESSAGE_SIZE=1024
TOTAL_MESSAGE_SIZE=1073741824  # 1GB

cleanup() {
    pkill -9 -f "embarlet|throughput_test" 2>/dev/null || true
    sleep 2
}

echo "=========================================="
echo "Quick Bandwidth Comparison Test"
echo "=========================================="
echo "Iterations: $NUM_ITERATIONS"
echo "Order: $ORDER, ACK: $ACK, Message Size: $MESSAGE_SIZE"
echo ""

# Baseline
echo "[BASELINE] EMBARCADERO_USE_BLOG_HEADER=0"
export EMBARCADERO_USE_BLOG_HEADER=0
export ORDER=$ORDER ACK=$ACK MESSAGE_SIZE=$MESSAGE_SIZE TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE

BASELINE_RESULTS=()
for i in $(seq 1 $NUM_ITERATIONS); do
    echo "  Iteration $i/$NUM_ITERATIONS..."
    cleanup
    
    cd build/bin
    output=$(timeout 120 bash ../../scripts/run_throughput.sh 2>&1 | tee /tmp/baseline_iter_${i}.log)
    cd "$PROJECT_ROOT"
    
    # Extract bandwidth
    bandwidth=$(echo "$output" | grep -iE "Bandwidth:|throughput" | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "0")
    if [ "$bandwidth" != "0" ]; then
        BASELINE_RESULTS+=($bandwidth)
        echo "    → $bandwidth MB/s"
    else
        echo "    → Failed to extract bandwidth"
        echo "$output" | tail -10
    fi
    sleep 2
done

echo ""
echo "[BLOGHEADER V2] EMBARCADERO_USE_BLOG_HEADER=1"
export EMBARCADERO_USE_BLOG_HEADER=1

BLOG_RESULTS=()
for i in $(seq 1 $NUM_ITERATIONS); do
    echo "  Iteration $i/$NUM_ITERATIONS..."
    cleanup
    
    cd build/bin
    output=$(timeout 120 bash ../../scripts/run_throughput.sh 2>&1 | tee /tmp/blog_iter_${i}.log)
    cd "$PROJECT_ROOT"
    
    # Extract bandwidth
    bandwidth=$(echo "$output" | grep -iE "Bandwidth:|throughput" | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "0")
    if [ "$bandwidth" != "0" ]; then
        BLOG_RESULTS+=($bandwidth)
        echo "    → $bandwidth MB/s"
    else
        echo "    → Failed to extract bandwidth"
        echo "$output" | tail -10
    fi
    sleep 2
done

# Calculate statistics
echo ""
echo "=========================================="
echo "Results Summary"
echo "=========================================="

if [ ${#BASELINE_RESULTS[@]} -gt 0 ]; then
    BASELINE_MEAN=$(python3 << EOF
import statistics
results = [float(x) for x in ${BASELINE_RESULTS[@]}]
print(f"{statistics.mean(results):.2f}")
EOF
)
    echo "Baseline (BlogHeader=0):"
    echo "  Results: ${BASELINE_RESULTS[@]}"
    echo "  Mean: $BASELINE_MEAN MB/s"
else
    BASELINE_MEAN=0
    echo "Baseline: No successful iterations"
fi

if [ ${#BLOG_RESULTS[@]} -gt 0 ]; then
    BLOG_MEAN=$(python3 << EOF
import statistics
results = [float(x) for x in ${BLOG_RESULTS[@]}]
print(f"{statistics.mean(results):.2f}")
EOF
)
    echo ""
    echo "BlogHeader v2 (BlogHeader=1):"
    echo "  Results: ${BLOG_RESULTS[@]}"
    echo "  Mean: $BLOG_MEAN MB/s"
else
    BLOG_MEAN=0
    echo ""
    echo "BlogHeader v2: No successful iterations"
fi

if [ "$BASELINE_MEAN" != "0" ] && [ "$BLOG_MEAN" != "0" ]; then
    RATIO=$(python3 << EOF
baseline = float("$BASELINE_MEAN")
blog = float("$BLOG_MEAN")
ratio = (blog / baseline) * 100
print(f"{ratio:.1f}")
EOF
)
    echo ""
    echo "Comparison:"
    echo "  BlogHeader v2 is ${RATIO}% of baseline"
    if (( $(echo "$RATIO >= 98" | bc -l) )); then
        echo "  ✓ Within acceptable range (>=98%)"
    else
        echo "  ✗ Below acceptable range (<98%)"
    fi
fi

cleanup
