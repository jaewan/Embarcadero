#!/bin/bash
set -euo pipefail

# Validate ORDER=5 throughput and fail fast on regressions.
# Defaults target the standard 10GB / 4 broker throughput benchmark.

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT/build/bin"

NUM_TRIALS="${NUM_TRIALS:-3}"
NUM_BROKERS="${NUM_BROKERS:-4}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-10737418240}"
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
TEST_TYPE="${TEST_TYPE:-5}"
ACK="${ACK:-1}"
ORDER="${ORDER:-5}"
THRESHOLD_MBPS="${THRESHOLD_MBPS:-9000}"

if [[ "$ORDER" != "5" ]]; then
  echo "ERROR: validate_order5_throughput.sh expects ORDER=5 (got ORDER=$ORDER)" >&2
  exit 2
fi

LOG_FILE="/tmp/order5_validate_$$.log"

ORDER="$ORDER" ACK="$ACK" NUM_BROKERS="$NUM_BROKERS" TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE" \
MESSAGE_SIZE="$MESSAGE_SIZE" TEST_TYPE="$TEST_TYPE" NUM_TRIALS="$NUM_TRIALS" QUIET=1 \
  bash ../../scripts/run_throughput.sh > "$LOG_FILE" 2>&1

mapfile -t vals < <(sed -n 's/.*Bandwidth: \([0-9][0-9.]*\) MB\/s.*/\1/p' "$LOG_FILE")
if [[ "${#vals[@]}" -eq 0 ]]; then
  echo "ERROR: No bandwidth values found in benchmark output." >&2
  tail -n 80 "$LOG_FILE" >&2
  exit 1
fi

avg=$(printf '%s\n' "${vals[@]}" | awk '{sum+=$1; n++} END { if (n>0) printf("%.2f", sum/n); }')

printf 'ORDER=5 throughput trials (MB/s): %s\n' "$(IFS=', '; echo "${vals[*]}")"
printf 'ORDER=5 throughput average (MB/s): %s\n' "$avg"
printf 'Threshold (MB/s): %s\n' "$THRESHOLD_MBPS"

python3 - <<'PY' "$avg" "$THRESHOLD_MBPS"
import sys
avg=float(sys.argv[1])
threshold=float(sys.argv[2])
sys.exit(0 if avg >= threshold else 1)
PY

if [[ $? -ne 0 ]]; then
  echo "FAIL: ORDER=5 throughput regression detected (avg ${avg} < threshold ${THRESHOLD_MBPS})." >&2
  exit 1
fi

echo "PASS: ORDER=5 throughput is within expected range."
