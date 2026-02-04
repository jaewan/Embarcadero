#!/bin/bash
# Profile broker (perf) while a throughput test is running. Test 2 from docs/SERIALIZATION_BOTTLENECK_PROFILING_PLAN.md.
# Usage: ./scripts/profile_broker_during_test.sh [perf_seconds]
# - Starts throughput test in background, waits for brokers, runs perf record for perf_seconds (default 10), then stops.
# - Writes perf.data in project root; run "perf report" to inspect.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

PERF_SEC="${1:-10}"
THROUGHPUT_LOG="${THROUGHPUT_LOG:-/tmp/profile_broker_throughput.log}"
PERF_DATA="${PERF_DATA:-$PROJECT_ROOT/perf.data}"

echo "=== Profile broker during test ==="
echo "  Perf sample duration: ${PERF_SEC}s"
echo "  Throughput log: $THROUGHPUT_LOG"
echo "  perf.data: $PERF_DATA"
echo ""

# Start throughput test in background
./scripts/run_throughput.sh 2>&1 | tee "$THROUGHPUT_LOG" &
THROUGHPUT_PID=$!

# Wait for brokers to be up and client to be sending (brokers ready ~10–20s, then test runs)
echo "Waiting 25s for brokers and test to be active..."
sleep 25

BROKER_PID=$(pgrep -f 'embarlet.*--config' | head -1)
if [ -z "$BROKER_PID" ]; then
  echo "ERROR: No broker PID found (pgrep embarlet.*--config). Check $THROUGHPUT_LOG"
  kill $THROUGHPUT_PID 2>/dev/null || true
  exit 1
fi

echo "Broker PID: $BROKER_PID — running perf record for ${PERF_SEC}s..."
perf record -g -p "$BROKER_PID" -o "$PERF_DATA" sleep "$PERF_SEC" || true

echo "Stopping throughput test (PID $THROUGHPUT_PID)..."
kill $THROUGHPUT_PID 2>/dev/null || true
wait $THROUGHPUT_PID 2>/dev/null || true

echo ""
echo "Done. To view profile:"
echo "  perf report -i $PERF_DATA"
echo "Look for: ReservePBRSlotAndWriteEntry, GetCXLBuffer, DrainPayloadToBuffer, recv."
echo "Throughput log: $THROUGHPUT_LOG"
