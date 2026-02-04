#!/bin/bash
# Serialization bottleneck: discriminative thread-count test and profiling commands.
# See docs/SERIALIZATION_BOTTLENECK_PROFILING_PLAN.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

LOG_BASELINE="${LOG_BASELINE:-/tmp/serialization_baseline.log}"
LOG_REDUCED="${LOG_REDUCED:-/tmp/serialization_reduced_threads.log}"

echo "=== Serialization experiments (profile-first) ==="
echo "Baseline log: $LOG_BASELINE (12 threads from config)"
echo "Reduced log:  $LOG_REDUCED (4 threads: THREADS_PER_BROKER=1)"
echo ""

# Extract e2e MB/s from throughput run log (last "End-to-end bandwidth" or "seconds, X.XX MB/s" line)
extract_e2e_mbps() {
  local log="$1"
  local e2e
  # Prefer "End-to-end bandwidth: X.XX MB/s"
  e2e=$(grep "End-to-end bandwidth:" "$log" 2>/dev/null | sed -n 's/.*End-to-end bandwidth: \([0-9.]*\).*/\1/p' | tail -1)
  if [ -n "$e2e" ]; then
    echo "$e2e"
    return
  fi
  # Fallback: "X seconds, Y.YY MB/s" from E2E result (last occurrence)
  e2e=$(grep "seconds,.*MB/s" "$log" 2>/dev/null | sed -n 's/.*seconds, \([0-9.]*\) MB\/s.*/\1/p' | tail -1)
  if [ -n "$e2e" ]; then
    echo "$e2e"
    return
  fi
  echo ""
}

# --- Test 1: Baseline (12 threads from config) ---
echo "--- Run 1: Baseline (threads_per_broker from config, 12 threads) ---"
THREADS_PER_BROKER= ./scripts/run_throughput.sh 2>&1 | tee "$LOG_BASELINE" || true
BASELINE_MBPS=$(extract_e2e_mbps "$LOG_BASELINE")
echo "Baseline E2E throughput: ${BASELINE_MBPS:-unknown} MB/s"
echo ""

# --- Test 2: Reduced threads (4 total: -n 1) ---
echo "--- Run 2: Reduced threads (THREADS_PER_BROKER=1 → 4 threads total) ---"
THREADS_PER_BROKER=1 ./scripts/run_throughput.sh 2>&1 | tee "$LOG_REDUCED" || true
REDUCED_MBPS=$(extract_e2e_mbps "$LOG_REDUCED")
echo "Reduced-thread E2E throughput: ${REDUCED_MBPS:-unknown} MB/s"
echo ""

# --- Interpretation ---
echo "=== Interpretation (see docs/SERIALIZATION_BOTTLENECK_PROFILING_PLAN.md) ==="
VALID_THRESHOLD=10.0
if [ -n "$BASELINE_MBPS" ] && [ -n "$REDUCED_MBPS" ] && awk "BEGIN { exit !($BASELINE_MBPS >= $VALID_THRESHOLD && $REDUCED_MBPS >= $VALID_THRESHOLD) }"; then
  echo "Baseline (12 threads): ${BASELINE_MBPS} MB/s"
  echo "Reduced (4 threads):  ${REDUCED_MBPS} MB/s"
  if awk "BEGIN { exit !($REDUCED_MBPS >= $BASELINE_MBPS * 0.85) }"; then
    echo "→ Throughput ~same with fewer threads → suggests LOOPBACK (or single bottleneck) limit."
  elif awk "BEGIN { exit !($REDUCED_MBPS < $BASELINE_MBPS * 0.6) }"; then
    echo "→ Throughput dropped with fewer threads → broker was helping; more threads = more parallelism."
  else
    echo "→ Throughput changed moderately → run perf/strace (below) to pinpoint."
  fi
elif grep -q "ACK Timeout\|ACK wait timed out" "$LOG_BASELINE" "$LOG_REDUCED" 2>/dev/null; then
  echo "One or both runs failed with ACK timeout (E2E reported 0.00 MB/s). Fix ACK path or run with -a 0 for send-only comparison."
  echo "  grep -E 'ACK Timeout|End-to-end bandwidth' $LOG_BASELINE $LOG_REDUCED"
else
  echo "Could not parse valid MB/s from logs (need both >= ${VALID_THRESHOLD} MB/s). Grep manually:"
  echo "  grep -E 'End-to-end bandwidth|MB/s|ACK' $LOG_BASELINE $LOG_REDUCED"
fi
echo ""

# --- Next steps: perf and strace ---
echo "=== Next: profile broker (run while test is active) ==="
echo "  BROKER_PID=\$(pgrep -f 'embarlet.*--config' | head -1)"
echo "  perf record -g -p \$BROKER_PID sleep 10"
echo "  perf report"
echo ""
echo "  # Broker pipeline profile: enable in config/embarcadero.yaml:"
echo "  #   enable_publish_pipeline_profile: true"
echo "  # Then grep broker logs for: [PublishPipelineProfile]"
echo ""
echo "  sudo strace -c -f -p \$BROKER_PID 2>&1 | tee /tmp/strace_broker.txt  # then grep recv"
echo ""
