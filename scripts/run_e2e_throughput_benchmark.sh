#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

BENCHMARK_TAG="${BENCHMARK_TAG:-$(date -u +%Y%m%d)_e2e_throughput}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
SCENARIO="${SCENARIO:-local}"
SEQUENCER="${SEQUENCER:-EMBARCADERO}"
ORDER="${ORDER:-0}"
ACK="${ACK:-1}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}"
NUM_BROKERS="${NUM_BROKERS:-1}"
NUM_TRIALS="${NUM_TRIALS:-3}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-134217728}"
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
THREADS_PER_BROKER="${THREADS_PER_BROKER:-4}"
SYSTEM_LABEL="${SYSTEM_LABEL:-${SEQUENCER}_order${ORDER}_ack${ACK}_rf${REPLICATION_FACTOR}}"
OUT_BASE="${OUT_BASE:-$PROJECT_ROOT/data/e2e_throughput}"

RUN_DIR="$OUT_BASE/$BENCHMARK_TAG/$SYSTEM_LABEL/run_$RUN_ID"
if [[ -e "$RUN_DIR" ]]; then
  echo "Refusing to overwrite existing run directory: $RUN_DIR" >&2
  exit 1
fi
mkdir -p "$RUN_DIR"

cat > "$RUN_DIR/benchmark_contract.md" <<EOF
# End-to-end throughput contract

- Entrypoint: \`scripts/run_e2e_throughput_benchmark.sh\`
- Benchmark mode: \`throughput_test -t 1\` (end-to-end throughput)
- Measured outputs: publish goodput and end-to-end goodput in MB/s from \`throughput_benchmark_summary.csv\`
- Raw artifacts: one deterministic directory per trial containing the client run log and the throughput summary
- Scope: this path is single-benchmark, publication-oriented harnessing for end-to-end throughput; it does not replace the existing multiclient publish-throughput matrix
EOF

cat > "$RUN_DIR/metadata.env" <<EOF
benchmark_tag=$BENCHMARK_TAG
run_id=$RUN_ID
system_label=$SYSTEM_LABEL
scenario=$SCENARIO
sequencer=$SEQUENCER
order=$ORDER
ack=$ACK
replication_factor=$REPLICATION_FACTOR
num_brokers=$NUM_BROKERS
num_trials=$NUM_TRIALS
total_message_size=$TOTAL_MESSAGE_SIZE
message_size=$MESSAGE_SIZE
threads_per_broker=$THREADS_PER_BROKER
commit=$(git rev-parse HEAD)
start_time_utc=$(date -u +%Y%m%dT%H%M%SZ)
EOF

cat > "$RUN_DIR/command.sh" <<EOF
#!/bin/bash
set -euo pipefail
cd "$PROJECT_ROOT"
BENCHMARK_TAG='$BENCHMARK_TAG' RUN_ID='$RUN_ID' SCENARIO='$SCENARIO' SEQUENCER='$SEQUENCER' \\
ORDER='$ORDER' ACK='$ACK' REPLICATION_FACTOR='$REPLICATION_FACTOR' NUM_BROKERS='$NUM_BROKERS' \\
NUM_TRIALS='$NUM_TRIALS' TOTAL_MESSAGE_SIZE='$TOTAL_MESSAGE_SIZE' MESSAGE_SIZE='$MESSAGE_SIZE' \\
THREADS_PER_BROKER='$THREADS_PER_BROKER' SYSTEM_LABEL='$SYSTEM_LABEL' OUT_BASE='$OUT_BASE' \\
bash scripts/run_e2e_throughput_benchmark.sh
EOF
chmod +x "$RUN_DIR/command.sh"

if [[ "$SCENARIO" == "remote" ]]; then
  THROUGHPUT_SCRIPT="scripts/run_throughput.sh"
else
  THROUGHPUT_SCRIPT="scripts/singlenode_run_throughput.sh"
fi

env \
  THROUGHPUT_ARTIFACT_BASE_DIR="$RUN_DIR" \
  NUM_BROKERS="$NUM_BROKERS" \
  NUM_TRIALS="$NUM_TRIALS" \
  TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE" \
  MESSAGE_SIZE="$MESSAGE_SIZE" \
  THREADS_PER_BROKER="$THREADS_PER_BROKER" \
  TEST_TYPE="1" \
  ORDER="$ORDER" \
  ACK="$ACK" \
  REPLICATION_FACTOR="$REPLICATION_FACTOR" \
  SEQUENCER="$SEQUENCER" \
  bash "$THROUGHPUT_SCRIPT" 2>&1 | tee "$RUN_DIR/driver.log"

python3 scripts/aggregate_e2e_throughput.py \
  --input-run-dir "$RUN_DIR" \
  --trial-output "$RUN_DIR/trial_results.csv" \
  --summary-output "$RUN_DIR/summary.csv"

printf 'end_time_utc=%s\n' "$(date -u +%Y%m%dT%H%M%SZ)" >> "$RUN_DIR/metadata.env"

echo "End-to-end throughput benchmark complete."
echo "Run directory: $RUN_DIR"
