#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"

TAG="${TAG:-20260328_moscxl_publication}"
SYSTEM="${SYSTEM:?set SYSTEM}"
ORDER="${ORDER:?set ORDER}"
SEQUENCER="${SEQUENCER:?set SEQUENCER}"
NUM_CLIENTS="${NUM_CLIENTS:?set NUM_CLIENTS}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:?set REPLICATION_FACTOR}"
ACK_LEVEL="${ACK_LEVEL:-}"
NUM_TRIALS="${NUM_TRIALS:-3}"
NUM_BROKERS="${NUM_BROKERS:-4}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-16106127360}"
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
TEST_TYPE="${TEST_TYPE:-5}"
THREADS_PER_BROKER="${THREADS_PER_BROKER:-4}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
START_DELAY_SEC="${START_DELAY_SEC:-8}"
LOCAL_CLIENT_NUMA="${LOCAL_CLIENT_NUMA:-0}"
CORFU_SEQUENCER_LOG_HOST="${CORFU_SEQUENCER_LOG_HOST:-c2}"
RUN_TS="$(date -u +%Y%m%dT%H%M%SZ)"
RUN_ID="${RUN_ID:-${RUN_TS}}"
REQUIRE_FIRST_ATTEMPT_PASS="${REQUIRE_FIRST_ATTEMPT_PASS:-0}"

if [[ -z "$ACK_LEVEL" ]]; then
  if [[ "$REPLICATION_FACTOR" == "2" ]]; then
    ACK_LEVEL="2"
  else
    ACK_LEVEL="1"
  fi
fi

case "$NUM_CLIENTS" in
  1) CLIENT_LAYOUT="c4" ;;
  2) CLIENT_LAYOUT="c4,moscxl-local(numa0)" ;;
  3) CLIENT_LAYOUT="c4,moscxl-local(numa0),c3" ;;
  *) CLIENT_LAYOUT="unsupported" ;;
esac

CELL_ID="${SYSTEM}_order${ORDER}_rf${REPLICATION_FACTOR}_n${NUM_CLIENTS}"
RUN_DIR="$PROJECT_ROOT/data/publication/throughput/$TAG/$CELL_ID/run_$RUN_ID"
RAW_DIR="$RUN_DIR/raw"
mkdir -p "$RAW_DIR"

COMMIT="$(git rev-parse HEAD)"

cat > "$RUN_DIR/command.sh" <<EOF
#!/bin/bash
set -euo pipefail
cd "$PROJECT_ROOT"
TAG='$TAG' SYSTEM='$SYSTEM' ORDER='$ORDER' SEQUENCER='$SEQUENCER' NUM_CLIENTS='$NUM_CLIENTS' \\
REPLICATION_FACTOR='$REPLICATION_FACTOR' ACK_LEVEL='$ACK_LEVEL' NUM_TRIALS='$NUM_TRIALS' NUM_BROKERS='$NUM_BROKERS' \\
TOTAL_MESSAGE_SIZE='$TOTAL_MESSAGE_SIZE' MESSAGE_SIZE='$MESSAGE_SIZE' TEST_TYPE='$TEST_TYPE' \\
THREADS_PER_BROKER='$THREADS_PER_BROKER' BROKER_IP='$BROKER_IP' START_DELAY_SEC='$START_DELAY_SEC' \\
LOCAL_CLIENT_NUMA='$LOCAL_CLIENT_NUMA' CORFU_SEQUENCER_LOG_HOST='$CORFU_SEQUENCER_LOG_HOST' bash scripts/publication/run_throughput_cell.sh
EOF
chmod +x "$RUN_DIR/command.sh"

cat > "$RUN_DIR/metadata.env" <<EOF
tag=$TAG
run_id=$RUN_ID
system=$SYSTEM
order=$ORDER
sequencer=$SEQUENCER
num_clients=$NUM_CLIENTS
client_layout=$CLIENT_LAYOUT
replication_factor=$REPLICATION_FACTOR
ack_level=$ACK_LEVEL
num_trials=$NUM_TRIALS
num_brokers=$NUM_BROKERS
total_message_size=$TOTAL_MESSAGE_SIZE
message_size=$MESSAGE_SIZE
test_type=$TEST_TYPE
threads_per_broker=$THREADS_PER_BROKER
broker_host=moscxl
broker_numa=node1
broker_ip=$BROKER_IP
local_client_numa=$LOCAL_CLIENT_NUMA
start_delay_sec=$START_DELAY_SEC
corfu_sequencer_log_host=$CORFU_SEQUENCER_LOG_HOST
require_first_attempt_pass=$REQUIRE_FIRST_ATTEMPT_PASS
commit=$COMMIT
start_time_utc=$RUN_TS
EOF

git status --short > "$RUN_DIR/git_status.txt"

rm -rf "$PROJECT_ROOT/multiclient_logs"
mkdir -p "$PROJECT_ROOT/multiclient_logs"

RUN_LOG="$RUN_DIR/run.log"

set +e
env \
  NUM_CLIENTS="$NUM_CLIENTS" \
  NUM_BROKERS="$NUM_BROKERS" \
  NUM_TRIALS="$NUM_TRIALS" \
  TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE" \
  MESSAGE_SIZE="$MESSAGE_SIZE" \
  THREADS_PER_BROKER="$THREADS_PER_BROKER" \
  TEST_TYPE="$TEST_TYPE" \
  ORDER="$ORDER" \
  ACK="$ACK_LEVEL" \
  REPLICATION_FACTOR="$REPLICATION_FACTOR" \
  SEQUENCER="$SEQUENCER" \
  EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
  START_DELAY_SEC="$START_DELAY_SEC" \
  LOCAL_CLIENT_NUMA="$LOCAL_CLIENT_NUMA" \
  REMOTE_CORFU_SEQUENCER_HOST="${REMOTE_CORFU_SEQUENCER_HOST:-}" \
  REMOTE_CORFU_BUILD_BIN="${REMOTE_CORFU_BUILD_BIN:-}" \
  bash scripts/run_multiclient.sh \
  2>&1 | tee "$RUN_LOG"
RUN_STATUS=${PIPESTATUS[0]}
set -e

cp -a "$PROJECT_ROOT/multiclient_logs/." "$RAW_DIR/" 2>/dev/null || true
cp -a build/bin/broker_*.log "$RAW_DIR/" 2>/dev/null || true
cp -a /tmp/broker_*.log "$RAW_DIR/" 2>/dev/null || true
scp -o StrictHostKeyChecking=no "${CORFU_SEQUENCER_LOG_HOST}:/tmp/corfu_sequencer.log" \
  "$RAW_DIR/${CORFU_SEQUENCER_LOG_HOST}_corfu_sequencer.log" >/dev/null 2>&1 || true

SUMMARY_CSV="$RUN_DIR/summary.csv"
echo "system,order,sequencer,num_clients,client_layout,replication_factor,run_idx,status,throughput_gbps,throughput_overlap_gbps,overlap_window_ms,timeseries_clients,attempts_used,first_attempt_pass,artifact_dir,commit" > "$SUMMARY_CSV"
ATTEMPT_SUMMARY_FILE="$RAW_DIR/attempt_summary.csv"
OVERLAP_SUMMARY_FILE="$RAW_DIR/overlap_summary.csv"

for trial in $(seq 1 "$NUM_TRIALS"); do
  trial_dir="$RUN_DIR/trial_$trial"
  mkdir -p "$trial_dir"
  trial_status="ok"
  total_mbs="0"
  copied_any=0
  for host in c4 local c3; do
    log_file="$RAW_DIR/trial${trial}_${host}.log"
    if [[ ! -f "$log_file" ]]; then
      continue
    fi
    copied_any=1
    cp "$log_file" "$trial_dir/"
    bw_val="$(grep -i 'bandwidth:' "$log_file" | tail -1 | grep -oiP 'bandwidth:\s*\K[0-9]+(\.[0-9]+)?' || true)"
    if [[ "$bw_val" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
      total_mbs="$(awk "BEGIN {printf \"%.6f\", $total_mbs + $bw_val}")"
    else
      trial_status="failed"
    fi
  done
  if [[ "$copied_any" -eq 0 ]]; then
    trial_status="missing_logs"
  fi
  throughput_gbps="$(awk "BEGIN {printf \"%.6f\", $total_mbs * 8 / 1024}")"
  overlap_gbps=""
  overlap_window_ms=""
  timeseries_clients=""
  if [[ -f "$OVERLAP_SUMMARY_FILE" ]]; then
    overlap_gbps="$(awk -F',' -v t="$trial" 'NR>1 && $1==t {print $2; exit}' "$OVERLAP_SUMMARY_FILE")"
    overlap_window_ms="$(awk -F',' -v t="$trial" 'NR>1 && $1==t {print $3; exit}' "$OVERLAP_SUMMARY_FILE")"
    timeseries_clients="$(awk -F',' -v t="$trial" 'NR>1 && $1==t {print $4; exit}' "$OVERLAP_SUMMARY_FILE")"
  fi
  attempts_used=""
  first_attempt_pass=""
  if [[ -f "$ATTEMPT_SUMMARY_FILE" ]]; then
    attempts_used="$(awk -F',' -v t="$trial" '$1==t && $3=="success"{print $2; exit}' "$ATTEMPT_SUMMARY_FILE")"
    if [[ -z "$attempts_used" ]]; then
      attempts_used="$(awk -F',' -v t="$trial" '$1==t && $2+0>max{max=$2+0} END{if(max>0) print max}' "$ATTEMPT_SUMMARY_FILE")"
    fi
    if [[ -n "$attempts_used" ]]; then
      if [[ "$attempts_used" == "1" ]]; then
        first_attempt_pass="1"
      else
        first_attempt_pass="0"
      fi
    fi
  fi
  echo "$SYSTEM,$ORDER,$SEQUENCER,$NUM_CLIENTS,\"$CLIENT_LAYOUT\",$REPLICATION_FACTOR,$trial,$trial_status,$throughput_gbps,$overlap_gbps,$overlap_window_ms,$timeseries_clients,$attempts_used,$first_attempt_pass,$trial_dir,$COMMIT" >> "$SUMMARY_CSV"
done

if [[ "$REQUIRE_FIRST_ATTEMPT_PASS" == "1" ]]; then
  if awk -F',' 'NR>1 && $14 != "1" {found=1} END{exit !found}' "$SUMMARY_CSV"; then
    echo "ERROR: first-attempt reliability gate failed (REQUIRE_FIRST_ATTEMPT_PASS=1)." >&2
    RUN_STATUS=1
  fi
fi

printf 'run_status=%s\nend_time_utc=%s\n' "$RUN_STATUS" "$(date -u +%Y%m%dT%H%M%SZ)" >> "$RUN_DIR/metadata.env"
exit "$RUN_STATUS"
