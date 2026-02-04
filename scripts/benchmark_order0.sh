#!/bin/bash
# Order 0 Throughput Benchmark
# Measures pure recv path throughput (no sequencer overhead).
# Use with recv_direct_to_cxl: true and order=0 for lowest-latency ACK path.
#
# Prerequisites: Brokers must be running (e.g. start via test/e2e or run_throughput.sh).
# Optional: TOTAL_MESSAGE_SIZE (bytes), MESSAGE_SIZE (bytes). Defaults: 1GB total, 1KB msg.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_ROOT/build}"
BIN_DIR="$BUILD_DIR/bin"
CONFIG_DIR="$PROJECT_ROOT/config"

ORDER=0
ACK_LEVEL=1
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
# Default 1GB for a quick benchmark; set TOTAL_MESSAGE_SIZE=10737418240 for 10GB
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-1073741824}"
LOG_FILE="${LOG_FILE:-/tmp/benchmark_order0_$$.log}"

if [ ! -x "$BIN_DIR/throughput_test" ]; then
    echo "ERROR: $BIN_DIR/throughput_test not found or not executable. Build the project first."
    exit 1
fi
if [ ! -f "$CONFIG_DIR/client.yaml" ]; then
    echo "ERROR: $CONFIG_DIR/client.yaml not found."
    exit 1
fi

echo "=== Order 0 Throughput Benchmark ==="
echo "Configuration: ORDER=$ORDER, ACK_LEVEL=$ACK_LEVEL, MSG_SIZE=$MESSAGE_SIZE, TOTAL_SIZE=$TOTAL_MESSAGE_SIZE"
echo "This measures pure recv path throughput without sequencer overhead."
echo "Ensure brokers are running with recv_direct_to_cxl: true for optimal results."
echo ""

"$BIN_DIR/throughput_test" \
    --config "$CONFIG_DIR/client.yaml" \
    -m "$MESSAGE_SIZE" \
    -s "$TOTAL_MESSAGE_SIZE" \
    -t 5 \
    -o "$ORDER" \
    -a "$ACK_LEVEL" \
    --sequencer EMBARCADERO \
    > "$LOG_FILE" 2>&1

EXIT_CODE=$?
if [ $EXIT_CODE -ne 0 ]; then
    echo "ERROR: throughput_test exited with code $EXIT_CODE"
    tail -50 "$LOG_FILE"
    exit $EXIT_CODE
fi

if grep -q "Throughput\|GB/s" "$LOG_FILE"; then
    echo "Result:"
    grep -E "Throughput|GB/s|throughput" "$LOG_FILE" | tail -5
else
    echo "Output (last 30 lines):"
    tail -30 "$LOG_FILE"
fi
echo ""
echo "Full log: $LOG_FILE"
