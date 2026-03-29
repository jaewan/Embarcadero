#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"

TAG="${TAG:-20260328_moscxl_publication}"
SYSTEM="${SYSTEM:?set SYSTEM}"
ORDER="${ORDER:?set ORDER}"
SEQUENCER="${SEQUENCER:?set SEQUENCER}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:?set REPLICATION_FACTOR}"
NUM_TRIALS="${NUM_TRIALS:-3}"
NUM_BROKERS="${NUM_BROKERS:-4}"
MSG_SIZE="${MSG_SIZE:-1024}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-4294967296}"
ACK_LEVEL="${ACK_LEVEL:-}"
MODES="${MODES:-steady}"
PUBLISHER_HOST="${PUBLISHER_HOST:-c4}"
BROKER_HOST="${BROKER_HOST:-moscxl}"
BROKER_LISTEN_ADDR="${BROKER_LISTEN_ADDR:-10.10.10.10}"
# Corfu gRPC sequencer log fetch (publication default: c2; override if your layout host differs).
CORFU_SEQUENCER_LOG_HOST="${CORFU_SEQUENCER_LOG_HOST:-c2}"
LAZYLOG_SEQUENCER_LOG_HOST="${LAZYLOG_SEQUENCER_LOG_HOST:-c3}"
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

CELL_ID="${SYSTEM}_order${ORDER}_rf${REPLICATION_FACTOR}"
RUN_DIR="$PROJECT_ROOT/data/publication/latency/$TAG/$CELL_ID/run_$RUN_ID"
RAW_DATA_DIR="$RUN_DIR/rawdata"
mkdir -p "$RAW_DATA_DIR"

COMMIT="$(git rev-parse HEAD)"

cat > "$RUN_DIR/command.sh" <<EOF
#!/bin/bash
set -euo pipefail
cd "$PROJECT_ROOT"
TAG='$TAG' SYSTEM='$SYSTEM' ORDER='$ORDER' SEQUENCER='$SEQUENCER' REPLICATION_FACTOR='$REPLICATION_FACTOR' \\
NUM_TRIALS='$NUM_TRIALS' NUM_BROKERS='$NUM_BROKERS' MSG_SIZE='$MSG_SIZE' TOTAL_MESSAGE_SIZE='$TOTAL_MESSAGE_SIZE' \\
ACK_LEVEL='$ACK_LEVEL' MODES='$MODES' PUBLISHER_HOST='$PUBLISHER_HOST' BROKER_HOST='$BROKER_HOST' \\
BROKER_LISTEN_ADDR='$BROKER_LISTEN_ADDR' CORFU_SEQUENCER_LOG_HOST='$CORFU_SEQUENCER_LOG_HOST' \\
LAZYLOG_SEQUENCER_LOG_HOST='$LAZYLOG_SEQUENCER_LOG_HOST' \\
bash scripts/publication/run_latency_cell.sh
EOF
chmod +x "$RUN_DIR/command.sh"

cat > "$RUN_DIR/metadata.env" <<EOF
tag=$TAG
run_id=$RUN_ID
system=$SYSTEM
order=$ORDER
sequencer=$SEQUENCER
replication_factor=$REPLICATION_FACTOR
num_trials=$NUM_TRIALS
num_brokers=$NUM_BROKERS
msg_size=$MSG_SIZE
total_message_size=$TOTAL_MESSAGE_SIZE
ack_level=$ACK_LEVEL
modes=$MODES
publisher_host=$PUBLISHER_HOST
broker_host=$BROKER_HOST
broker_listen_addr=$BROKER_LISTEN_ADDR
corfu_sequencer_log_host=$CORFU_SEQUENCER_LOG_HOST
lazylog_sequencer_log_host=$LAZYLOG_SEQUENCER_LOG_HOST
require_first_attempt_pass=$REQUIRE_FIRST_ATTEMPT_PASS
commit=$COMMIT
start_time_utc=$RUN_TS
EOF

git status --short > "$RUN_DIR/git_status.txt"

RUN_LOG="$RUN_DIR/run.log"
set +e
env \
  DATA_DIR="$RAW_DATA_DIR" \
  RUN_ID="$RUN_ID" \
  SCENARIO="remote" \
  ORDERS="$ORDER" \
  MODES="$MODES" \
  SEQUENCER="$SEQUENCER" \
  NUM_TRIALS="$NUM_TRIALS" \
  NUM_BROKERS="$NUM_BROKERS" \
  MSG_SIZE="$MSG_SIZE" \
  TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE" \
  ACK_LEVEL="$ACK_LEVEL" \
  REPLICATION_FACTOR="$REPLICATION_FACTOR" \
  REMOTE_CLIENT_HOST="$PUBLISHER_HOST" \
  BROKER_LISTEN_ADDR="$BROKER_LISTEN_ADDR" \
  REMOTE_CORFU_SEQUENCER_HOST="${REMOTE_CORFU_SEQUENCER_HOST:-}" \
  REMOTE_CORFU_BUILD_BIN="${REMOTE_CORFU_BUILD_BIN:-}" \
  REMOTE_LAZYLOG_SEQUENCER_HOST="${REMOTE_LAZYLOG_SEQUENCER_HOST:-}" \
  REMOTE_LAZYLOG_BUILD_BIN="${REMOTE_LAZYLOG_BUILD_BIN:-}" \
  EMBARCADERO_LAZYLOG_SEQ_IP="${EMBARCADERO_LAZYLOG_SEQ_IP:-}" \
  EMBARCADERO_LAZYLOG_SEQ_PORT="${EMBARCADERO_LAZYLOG_SEQ_PORT:-}" \
  bash scripts/run_latency.sh \
  2>&1 | tee "$RUN_LOG"
RUN_STATUS=${PIPESTATUS[0]}
set -e

cp -a build/bin/broker_*.log "$RUN_DIR/" 2>/dev/null || true
scp -o StrictHostKeyChecking=no "${CORFU_SEQUENCER_LOG_HOST}:/tmp/corfu_sequencer.log" \
  "$RUN_DIR/${CORFU_SEQUENCER_LOG_HOST}_corfu_sequencer.log" >/dev/null 2>&1 || true
scp -o StrictHostKeyChecking=no "${LAZYLOG_SEQUENCER_LOG_HOST}:/tmp/lazylog_sequencer.log" \
  "$RUN_DIR/${LAZYLOG_SEQUENCER_LOG_HOST}_lazylog_sequencer.log" >/dev/null 2>&1 || true

SUMMARY_CSV="$RUN_DIR/summary.csv"
echo "system,order,sequencer,replication_factor,publisher_host,broker_host,run_idx,status,p50_us,p95_us,p99_us,max_us,attempts_used,first_attempt_pass,artifact_dir,commit" > "$SUMMARY_CSV"

find "$RAW_DATA_DIR" -type f -name latency_stats.csv | sort | while read -r stats; do
  trial_dir="$(dirname "$stats")"
  trial_num="$(basename "$trial_dir" | sed -n 's/.*_trial\([0-9][0-9]*\)$/\1/p')"
  if [[ -z "$trial_num" ]]; then
    continue
  fi
  row="$(awk -F',' '$13=="publish_to_deliver_latency"{print $3","$5","$6","$8; exit}' "$stats")"
  if [[ -n "$row" ]]; then
    IFS=',' read -r p50 p95 p99 maxv <<< "$row"
    status="ok"
  else
    p50=""
    p95=""
    p99=""
    maxv=""
    status="failed"
  fi
  echo "$SYSTEM,$ORDER,$SEQUENCER,$REPLICATION_FACTOR,$PUBLISHER_HOST,$BROKER_HOST,$trial_num,$status,$p50,$p95,$p99,$maxv,1,1,$trial_dir,$COMMIT" >> "$SUMMARY_CSV"
done

if [[ "$REQUIRE_FIRST_ATTEMPT_PASS" == "1" ]]; then
  if awk -F',' 'NR>1 && $14 != "1" {found=1} END{exit !found}' "$SUMMARY_CSV"; then
    echo "ERROR: first-attempt reliability gate failed (REQUIRE_FIRST_ATTEMPT_PASS=1)." >&2
    RUN_STATUS=1
  fi
fi

printf 'run_status=%s\nend_time_utc=%s\n' "$RUN_STATUS" "$(date -u +%Y%m%dT%H%M%SZ)" >> "$RUN_DIR/metadata.env"
exit "$RUN_STATUS"
