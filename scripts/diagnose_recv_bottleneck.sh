#!/bin/bash
# Run recv-bottleneck diagnostics: TCP window (ss -ti), syscall count (strace -c), CPU vs wait (mpstat).
# Run this while a throughput test is active (brokers and client running).
# See docs/CRITICAL_ASSESSMENT_RESPONSE.md and docs/RECV_OPTIMIZATION_METHODS.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Broker data port (main publish port from config)
BROKER_PORT="${BROKER_PORT:-1214}"
# How long to run strace/mpstat (seconds)
STRACE_SEC="${STRACE_SEC:-10}"
MPSTAT_SEC="${MPSTAT_SEC:-5}"

echo "=== Recv bottleneck diagnostics (port=$BROKER_PORT) ==="

# 1. Find broker PIDs
BROKER_PIDS=()
while IFS= read -r pid; do
  BROKER_PIDS+=("$pid")
done < <(pgrep -f "embarlet.*--config" 2>/dev/null || true)

if [ ${#BROKER_PIDS[@]} -eq 0 ]; then
  echo "No broker PIDs found (pgrep embarlet). Start a throughput test first, or pass PIDs: BROKER_PID=1234 $0"
  if [ -n "${BROKER_PID:-}" ]; then
    BROKER_PIDS=("$BROKER_PID")
    echo "Using BROKER_PID=$BROKER_PID"
  else
    echo "Skipping PID-based diagnostics. Running TCP window check only."
  fi
else
  echo "Found ${#BROKER_PIDS[@]} broker PID(s): ${BROKER_PIDS[*]}"
fi

# 2. TCP window (ss -ti) — check rcv_space, snd_wnd, wscale
echo ""
echo "--- 1. TCP window (ss -ti for port $BROKER_PORT) ---"
if command -v ss &>/dev/null; then
  ss -ti 2>/dev/null | grep -E ":$BROKER_PORT|State|rcv_space|snd_wnd|bytes_acked|bytes_received" || true
  # Show ESTAB connections involving broker port
  ss -ti state established 2>/dev/null | grep -E ":$BROKER_PORT" -A 5 || true
else
  echo "ss not found, skip TCP window check"
fi

# 3. Syscall count (strace -c) — is recv() dominating?
echo ""
echo "--- 2. Syscall count (strace -c) ---"
if [ ${#BROKER_PIDS[@]} -gt 0 ] && command -v strace &>/dev/null; then
  PID="${BROKER_PIDS[0]}"
  echo "Running strace -c -p $PID for ${STRACE_SEC}s (Ctrl+C to stop early)..."
  timeout "$STRACE_SEC" strace -c -p "$PID" 2>&1 || true
  echo "Check: if recv() is 80%+ of syscalls, syscall overhead may be the bottleneck."
else
  echo "Run manually: strace -c -p <broker_pid>   # Let it run 10s during a test, then Ctrl+C"
fi

# 4. CPU vs I/O wait (mpstat)
echo ""
echo "--- 3. CPU vs wait (mpstat) ---"
if command -v mpstat &>/dev/null; then
  echo "Running mpstat -P ALL 1 $MPSTAT_SEC..."
  mpstat -P ALL 1 "$MPSTAT_SEC" 2>/dev/null || true
  echo "Check: if %idle is high and %iowait is low, CPU is not saturated (wait-for-data hypothesis)."
else
  echo "mpstat not found. Run manually: mpstat -P ALL 1   # Run during test, Ctrl+C after a few seconds"
fi

echo ""
echo "=== Next steps ==="
echo "1. Check broker logs for [PublishPipelineProfile] and [DrainPayloadToBuffer] Avg recv calls per batch."
echo "2. If recv calls per batch is 20+, kernel may be returning small chunks (tune SO_RCVBUF / net.core.rmem_max)."
echo "3. If strace shows recv() dominating and CPU is high, consider io_uring. If CPU is low, bottleneck is likely sender/TCP window."
