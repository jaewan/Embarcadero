#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

LOAD_POINTS_MBPS="${LOAD_POINTS_MBPS:-100 250 500 1000 2000}"
PACING_MODE="${PACING_MODE:-open_loop}"
BENCHMARK_TAG="${BENCHMARK_TAG:-$(date -u +%Y%m%d)_latency_vs_load}"
RUN_ID="${RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
SEQUENCER="${SEQUENCER:-EMBARCADERO}"
ORDER="${ORDER:-0}"
ACK_LEVEL="${ACK_LEVEL:-1}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}"
MSG_SIZE="${MSG_SIZE:-1024}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-134217728}"
NUM_TRIALS="${NUM_TRIALS:-3}"
# [[EVAL WARM-UP]] Leading (cold) trials excluded from the per-point mean (raw preserved). The first
# trial after a fresh cluster start is deterministically slow on this testbed; see
# docs/experiments/rf_throughput_warmup_artifact.md. Set 0 to disable; use NUM_TRIALS >= WARMUP_TRIALS+3.
WARMUP_TRIALS="${WARMUP_TRIALS:-1}"
NUM_BROKERS="${NUM_BROKERS:-4}"
SCENARIO="${SCENARIO:-local}"
SYSTEM_LABEL="${SYSTEM_LABEL:-${SEQUENCER}_order${ORDER}_ack${ACK_LEVEL}_rf${REPLICATION_FACTOR}}"
OUT_BASE="${OUT_BASE:-$PROJECT_ROOT/data/latency_vs_load}"
CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-full}"
CXL_MAP_POPULATE="${EMBARCADERO_CXL_MAP_POPULATE:-1}"
HUGETLB="${EMBAR_USE_HUGETLB:-1}"

case "$PACING_MODE" in
  open_loop)
    LATENCY_MODES="burst"
    ;;
  steady)
    LATENCY_MODES="steady"
    ;;
  *)
    echo "Unsupported PACING_MODE='$PACING_MODE'. Use 'open_loop' or 'steady'." >&2
    exit 1
    ;;
esac

RUN_DIR="$OUT_BASE/$BENCHMARK_TAG/$SYSTEM_LABEL/run_$RUN_ID"
if [[ -e "$RUN_DIR" ]]; then
  echo "Refusing to overwrite existing run directory: $RUN_DIR" >&2
  exit 1
fi
mkdir -p "$RUN_DIR"

cat > "$RUN_DIR/benchmark_contract.md" <<EOF
# Latency-vs-load contract

- Controlled variable: publisher offered load in MB/s via \`throughput_test --target_mbps\`.
- Pacing semantics: \`PACING_MODE=open_loop\` maps to the existing latency harness \`burst\` mode, meaning no extra pause injection beyond the target-load scheduler. \`PACING_MODE=steady\` maps to \`--steady_rate\` and must not be mixed with open-loop results in one figure.
- Measured latency: end-to-end \`publish_to_deliver_latency\` in \`delivery_latency_stats.csv\`; receive-side wire-arrival \`publish_to_receive_latency\` in \`latency_stats.csv\`; publisher batch latency metrics in \`pub_latency_stats.csv\`.
- Measured throughput: achieved offered load, publish goodput, and end-to-end goodput from \`latency_benchmark_summary.csv\`.
- Raw artifacts: every target/trial keeps its original CSV files and run log under a deterministic point directory.
- Failure policy: the script stops on the first failed target to avoid silently publishing partial or incompatible sweeps.
EOF

cat > "$RUN_DIR/metadata.env" <<EOF
benchmark_tag=$BENCHMARK_TAG
run_id=$RUN_ID
system_label=$SYSTEM_LABEL
sequencer=$SEQUENCER
order=$ORDER
ack_level=$ACK_LEVEL
replication_factor=$REPLICATION_FACTOR
message_size=$MSG_SIZE
total_message_size=$TOTAL_MESSAGE_SIZE
num_trials=$NUM_TRIALS
num_brokers=$NUM_BROKERS
scenario=$SCENARIO
pacing_mode=$PACING_MODE
load_points_mbps=$LOAD_POINTS_MBPS
cxl_zero_mode=$CXL_ZERO_MODE
cxl_map_populate=$CXL_MAP_POPULATE
hugetlb=$HUGETLB
start_time_utc=$(date -u +%Y%m%dT%H%M%SZ)
commit=$(git rev-parse HEAD)
EOF

cat > "$RUN_DIR/command.sh" <<EOF
#!/bin/bash
set -euo pipefail
cd "$PROJECT_ROOT"
LOAD_POINTS_MBPS='$LOAD_POINTS_MBPS' PACING_MODE='$PACING_MODE' BENCHMARK_TAG='$BENCHMARK_TAG' RUN_ID='$RUN_ID' \\
SEQUENCER='$SEQUENCER' ORDER='$ORDER' ACK_LEVEL='$ACK_LEVEL' REPLICATION_FACTOR='$REPLICATION_FACTOR' \\
MSG_SIZE='$MSG_SIZE' TOTAL_MESSAGE_SIZE='$TOTAL_MESSAGE_SIZE' NUM_TRIALS='$NUM_TRIALS' NUM_BROKERS='$NUM_BROKERS' \\
SCENARIO='$SCENARIO' SYSTEM_LABEL='$SYSTEM_LABEL' OUT_BASE='$OUT_BASE' \\
EMBARCADERO_CXL_ZERO_MODE='$CXL_ZERO_MODE' EMBARCADERO_CXL_MAP_POPULATE='$CXL_MAP_POPULATE' EMBAR_USE_HUGETLB='$HUGETLB' \\
bash scripts/run_latency_vs_load.sh
EOF
chmod +x "$RUN_DIR/command.sh"

POINT_INDEX=0
for target in $LOAD_POINTS_MBPS; do
  POINT_INDEX=$((POINT_INDEX + 1))
  safe_target="${target//./p}"
  POINT_DIR="$RUN_DIR/points/$(printf "%03d" "$POINT_INDEX")_${safe_target}mbps"
  RAW_DIR="$POINT_DIR/raw"
  mkdir -p "$POINT_DIR"

  echo "========================================================" | tee "$POINT_DIR/driver.log"
  echo "Latency-vs-load point $POINT_INDEX target=${target} MB/s" | tee -a "$POINT_DIR/driver.log"
  echo "Output: $POINT_DIR" | tee -a "$POINT_DIR/driver.log"
  echo "========================================================" | tee -a "$POINT_DIR/driver.log"

  env \
    DATA_DIR="$RAW_DIR" \
    RUN_ID="target_${safe_target}mbps" \
    TARGET_MBPS="$target" \
    ORDERS="$ORDER" \
    MODES="$LATENCY_MODES" \
    SEQUENCER="$SEQUENCER" \
    ACK_LEVEL="$ACK_LEVEL" \
    REPLICATION_FACTOR="$REPLICATION_FACTOR" \
    MSG_SIZE="$MSG_SIZE" \
    TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE" \
    NUM_TRIALS="$NUM_TRIALS" \
    NUM_BROKERS="$NUM_BROKERS" \
    SCENARIO="$SCENARIO" \
    EMBARCADERO_CXL_ZERO_MODE="$CXL_ZERO_MODE" \
    EMBARCADERO_CXL_MAP_POPULATE="$CXL_MAP_POPULATE" \
    EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-}" \
    EMBAR_USE_HUGETLB="$HUGETLB" \
    BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-120}" \
    BROKER_REACHABILITY_TIMEOUT_SEC="${BROKER_REACHABILITY_TIMEOUT_SEC:-20}" \
    CLIENT_LD_LIBRARY_PATH="${CLIENT_LD_LIBRARY_PATH:-}" \
    EMBAR_ORDER5_EPOCH_US="${EMBAR_ORDER5_EPOCH_US:-}" \
    EMBARCADERO_CLIENT_LINGER_US="${EMBARCADERO_CLIENT_LINGER_US:-}" \
    bash scripts/run_latency.sh 2>&1 | tee -a "$POINT_DIR/driver.log"

  python3 scripts/aggregate_latency_vs_load.py \
    --input-run-dir "$POINT_DIR" \
    --trial-output "$POINT_DIR/trial_results.csv" \
    --summary-output "$POINT_DIR/summary.csv" \
    --warmup-trials "$WARMUP_TRIALS"
done

python3 scripts/aggregate_latency_vs_load.py \
  --input-run-dir "$RUN_DIR" \
  --trial-output "$RUN_DIR/trial_results.csv" \
  --summary-output "$RUN_DIR/summary.csv" \
  --warmup-trials "$WARMUP_TRIALS"

printf 'end_time_utc=%s\n' "$(date -u +%Y%m%dT%H%M%SZ)" >> "$RUN_DIR/metadata.env"

echo "Latency-vs-load run complete."
echo "Run directory: $RUN_DIR"
echo "Trial rows:    $RUN_DIR/trial_results.csv"
echo "Summary rows:  $RUN_DIR/summary.csv"
