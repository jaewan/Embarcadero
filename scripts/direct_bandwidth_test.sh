#!/bin/bash
# Direct bandwidth test - runs one iteration of each variant

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

ORDER=5
ACK=1
MESSAGE_SIZE=1024
TOTAL_MESSAGE_SIZE=1073741824  # 1GB

cleanup() {
    pkill -9 -f "embarlet|throughput_test" 2>/dev/null || true
    sleep 2
}

echo "=========================================="
echo "Direct Bandwidth Test"
echo "=========================================="
echo "Order: $ORDER, ACK: $ACK, Message Size: $MESSAGE_SIZE bytes"
echo "Total Data: $TOTAL_MESSAGE_SIZE bytes (1GB)"
echo ""

# Baseline
echo "[TEST 1/2] Baseline (EMBARCADERO_USE_BLOG_HEADER=0)"
export EMBARCADERO_USE_BLOG_HEADER=0
export ORDER=$ORDER ACK=$ACK MESSAGE_SIZE=$MESSAGE_SIZE TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE

cleanup
cd build/bin
echo "Starting brokers and running test..."
timeout 300 bash ../../scripts/run_throughput.sh > /tmp/baseline_direct.log 2>&1
BASELINE_BW=$(grep -iE "Bandwidth:|throughput" /tmp/baseline_direct.log | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "0")
cd "$PROJECT_ROOT"

if [ "$BASELINE_BW" != "0" ]; then
    echo "  ✓ Baseline bandwidth: $BASELINE_BW MB/s"
else
    echo "  ✗ Failed to extract bandwidth"
    echo "  Last 20 lines of log:"
    tail -20 /tmp/baseline_direct.log
fi

cleanup
sleep 3

# BlogHeader v2
echo ""
echo "[TEST 2/2] BlogHeader v2 (EMBARCADERO_USE_BLOG_HEADER=1)"
export EMBARCADERO_USE_BLOG_HEADER=1
export ORDER=$ORDER ACK=$ACK MESSAGE_SIZE=$MESSAGE_SIZE TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE

cd build/bin
echo "Starting brokers and running test..."
timeout 300 bash ../../scripts/run_throughput.sh > /tmp/blog_direct.log 2>&1
BLOG_BW=$(grep -iE "Bandwidth:|throughput" /tmp/blog_direct.log | grep -oE "[0-9]+\.[0-9]+" | head -1 || echo "0")
cd "$PROJECT_ROOT"

if [ "$BLOG_BW" != "0" ]; then
    echo "  ✓ BlogHeader v2 bandwidth: $BLOG_BW MB/s"
else
    echo "  ✗ Failed to extract bandwidth"
    echo "  Last 20 lines of log:"
    tail -20 /tmp/blog_direct.log
fi

# Comparison
echo ""
echo "=========================================="
echo "Results"
echo "=========================================="
echo "Baseline (BlogHeader=0):    $BASELINE_BW MB/s"
echo "BlogHeader v2 (BlogHeader=1): $BLOG_BW MB/s"

if [ "$BASELINE_BW" != "0" ] && [ "$BLOG_BW" != "0" ]; then
    RATIO=$(python3 << EOF
baseline = float("$BASELINE_BW")
blog = float("$BLOG_BW")
ratio = (blog / baseline) * 100
print(f"{ratio:.1f}")
EOF
)
    echo ""
    echo "BlogHeader v2 is ${RATIO}% of baseline"
    if (( $(echo "$RATIO >= 98" | bc -l) )); then
        echo "✓ Within acceptable range (>=98%)"
    else
        echo "⚠ Below acceptable range (<98%)"
    fi
fi

cleanup
