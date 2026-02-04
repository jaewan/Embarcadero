#!/bin/bash
# Run throughput test and recv-bottleneck diagnostics in one go: start test, wait for
# measurement to begin, run diagnose_recv_bottleneck.sh while brokers are alive, then
# wait for test to finish. See docs/DIAGNOSTIC_RUNBOOK_RECV.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

THROUGHPUT_LOG="${THROUGHPUT_LOG:-/tmp/throughput_run.log}"
DIAG_WAIT_SEC="${DIAG_WAIT_SEC:-28}"

echo "=== Throughput test with diagnostics ==="
echo "Throughput log: $THROUGHPUT_LOG"
echo "Will run diagnose_recv_bottleneck.sh after ${DIAG_WAIT_SEC}s (brokers still alive)."
echo ""

# 1. Start throughput test in background
./scripts/run_throughput.sh 2>&1 | tee "$THROUGHPUT_LOG" &
THROUGHPUT_PID=$!

# 2. Wait for measurement to start (brokers up, client sending)
echo "Waiting ${DIAG_WAIT_SEC}s for brokers and measurement to start..."
sleep "$DIAG_WAIT_SEC"

# 3. Run diagnostics while test is (hopefully) still running
echo ""
echo "=== Running recv diagnostics (brokers should be alive) ==="
./scripts/diagnose_recv_bottleneck.sh 2>&1 | tee /tmp/diagnose_recv_output.txt || true

# 4. Wait for throughput test to finish
echo ""
echo "Waiting for throughput test to finish (PID $THROUGHPUT_PID)..."
wait $THROUGHPUT_PID 2>/dev/null || true

echo ""
echo "=== Done ==="
echo "Throughput log: $THROUGHPUT_LOG"
echo "Diagnostic output: /tmp/diagnose_recv_output.txt"
echo ""
echo "Inspect (broker profile may be in throughput log if brokers share same stdout):"
echo "  grep -E \"\\[PublisherPipelineProfile\]|\\[PublishPipelineProfile\]|\\[DrainPayloadToBuffer\]|bandwidth|MB/s|ACK\" $THROUGHPUT_LOG"
echo "  cat /tmp/diagnose_recv_output.txt"
