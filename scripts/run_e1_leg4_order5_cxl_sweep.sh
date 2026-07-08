#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

TAG="${TAG:-$(date -u +%Y%m%d)_e1_leg4_order5_cxl}"
RUN_TS="${RUN_TS:-$(date -u +%Y%m%dT%H%M%SZ)}"
MSG_SIZES="${MSG_SIZES:-128 512 1024 4096 16384}"
NUM_TRIALS="${NUM_TRIALS:-3}"
NUM_BROKERS="${NUM_BROKERS:-4}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-8589934592}"
THREADS_PER_BROKER="${THREADS_PER_BROKER:-1}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
CLIENT_HOSTS_CSV="${CLIENT_HOSTS_CSV:-c4}"
CLIENT_NUMAS_CSV="${CLIENT_NUMAS_CSV:-1}"
TESTBED_LOCK="${TESTBED_LOCK:-$HOME/Embarcadero-sessions/testbed.lock}"
ACTIVITY_LOG="${ACTIVITY_LOG:-$HOME/Embarcadero-sessions/activity.log}"
RAW_DOC_ROOT="${RAW_DOC_ROOT:-$PROJECT_ROOT/docs/experiments/e1_leg4_raw/$TAG}"

mkdir -p "$(dirname "$TESTBED_LOCK")" "$(dirname "$ACTIVITY_LOG")" "$RAW_DOC_ROOT"

exec {LOCK_FD}>"$TESTBED_LOCK"
flock "$LOCK_FD"

printf '%s START e1_leg4_order5_cxl tag=%s commit=%s\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$TAG" "$(git rev-parse HEAD)" >> "$ACTIVITY_LOG"
trap 'printf "%s END e1_leg4_order5_cxl tag=%s status=%s\n" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$TAG" "$?" >> "$ACTIVITY_LOG"' EXIT

for msg_size in $MSG_SIZES; do
  run_id="${RUN_TS}_msg${msg_size}"
  echo "=== E1 leg-4 Embarcadero-on-CXL ORDER=5 msg=${msg_size}B run_id=${run_id} ==="
  env \
    EMBARCADERO_DISABLE_PATTERN_KILL=1 \
    TAG="$TAG" \
    RUN_ID="$run_id" \
    SYSTEM="embarcadero" \
    ORDER="5" \
    SEQUENCER="EMBARCADERO" \
    NUM_CLIENTS="1" \
    REPLICATION_FACTOR="1" \
    ACK_LEVEL="1" \
    NUM_TRIALS="$NUM_TRIALS" \
    NUM_BROKERS="$NUM_BROKERS" \
    TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE" \
    MESSAGE_SIZE="$msg_size" \
    TEST_TYPE="5" \
    THREADS_PER_BROKER="$THREADS_PER_BROKER" \
    BROKER_IP="$BROKER_IP" \
    CLIENT_HOSTS_CSV="$CLIENT_HOSTS_CSV" \
    CLIENT_NUMAS_CSV="$CLIENT_NUMAS_CSV" \
    REQUIRE_FIRST_ATTEMPT_PASS="${REQUIRE_FIRST_ATTEMPT_PASS:-1}" \
    TRIAL_MAX_ATTEMPTS="${TRIAL_MAX_ATTEMPTS:-1}" \
    bash scripts/publication/run_throughput_cell.sh
done

SUMMARY_OUT="$RAW_DOC_ROOT/e1_leg4_trial_rows.csv"
echo "message_size,system,order,sequencer,num_clients,client_layout,replication_factor,run_idx,status,throughput_gbps,throughput_overlap_gbps,overlap_window_ms,timeseries_clients,attempts_used,first_attempt_pass,artifact_dir,commit" > "$SUMMARY_OUT"

for msg_size in $MSG_SIZES; do
  while IFS= read -r summary; do
    run_dir="$(dirname "$summary")"
    doc_dir="$RAW_DOC_ROOT/msg${msg_size}/$(basename "$run_dir")"
    mkdir -p "$doc_dir"
    cp "$summary" "$doc_dir/summary.csv"
    cp "$run_dir/metadata.env" "$doc_dir/metadata.env" 2>/dev/null || true
    awk -F',' -v msg="$msg_size" 'NR > 1 {print msg "," $0}' "$summary" >> "$SUMMARY_OUT"
  done < <(find "$PROJECT_ROOT/data/publication/throughput/$TAG/embarcadero_order5_rf1_n1" -path "*/run_${RUN_TS}_msg${msg_size}/summary.csv" -print 2>/dev/null | sort)
done

python3 - "$SUMMARY_OUT" "$RAW_DOC_ROOT/e1_leg4_summary.csv" <<'PY'
import csv
import statistics
import sys

inp, outp = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(inp, newline="")))
groups = {}
for row in rows:
    if row.get("status") != "ok":
        continue
    groups.setdefault(row["message_size"], []).append(float(row["throughput_overlap_gbps"] or row["throughput_gbps"]))

with open(outp, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["message_size", "trials", "median_gbps", "min_gbps", "max_gbps", "median_msgs_per_s"])
    for msg in sorted(groups, key=lambda x: int(x)):
        vals = groups[msg]
        med = statistics.median(vals)
        w.writerow([msg, len(vals), f"{med:.6f}", f"{min(vals):.6f}", f"{max(vals):.6f}", f"{med * (1024**3) / int(msg):.3f}"])
PY

echo "E1 leg-4 raw tree: $RAW_DOC_ROOT"
echo "E1 leg-4 summary:  $RAW_DOC_ROOT/e1_leg4_summary.csv"
