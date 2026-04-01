#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"

TAG="${TAG:-20260402_publication_matrix_appendable_tp}"
RUN_TS="${RUN_TS:-$(date -u +%Y%m%dT%H%M%SZ)}"
NUM_TRIALS="${NUM_TRIALS:-1}"
NUM_BROKERS="${NUM_BROKERS:-4}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-16106127360}"
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
TEST_TYPE="${TEST_TYPE:-5}"
THREADS_PER_BROKER="${THREADS_PER_BROKER:-4}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
START_DELAY_SEC="${START_DELAY_SEC:-8}"
LOCAL_CLIENT_NUMA="${LOCAL_CLIENT_NUMA:-0}"
TRIAL_MAX_ATTEMPTS="${TRIAL_MAX_ATTEMPTS:-1}"
REQUIRE_FIRST_ATTEMPT_PASS="${REQUIRE_FIRST_ATTEMPT_PASS:-1}"
REMOTE_SEQ_HOST="${REMOTE_SEQ_HOST:-c2}"
REMOTE_SEQ_BUILD_BIN="${REMOTE_SEQ_BUILD_BIN:-/home/domin/Embarcadero/build/bin}"
REMOTE_SEQ_IP="${REMOTE_SEQ_IP:-10.10.10.144}"
EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-120}"
SKIP_EXISTING_CELLS="${SKIP_EXISTING_CELLS:-1}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-1}"
ACK_LEVEL="${ACK_LEVEL:-}"

declare -a NUM_CLIENT_VALUES=(1 2 3)
failures=0

run_cell() {
  local system="$1"
  local order="$2"
  local sequencer="$3"
  local num_clients="$4"
  local run_id="${RUN_TS}_${system}_o${order}_n${num_clients}"
  local cell_id="${system}_order${order}_rf${REPLICATION_FACTOR}_n${num_clients}"
  local run_dir="$PROJECT_ROOT/data/publication/throughput/$TAG/$cell_id/run_$run_id"
  local cell_root="$PROJECT_ROOT/data/publication/throughput/$TAG/$cell_id"

  if [[ "$SKIP_EXISTING_CELLS" == "1" ]] && find "$cell_root" -mindepth 2 -maxdepth 2 -name summary.csv -print -quit 2>/dev/null | grep -q .; then
    printf '\n=== %s order=%s n=%s skipped (existing summary under %s) ===\n' \
      "$sequencer" "$order" "$num_clients" "$cell_root"
    python3 scripts/publication/summarize_publication_results.py throughput "$TAG" || true
    return 0
  fi

  local effective_ack_level="$ACK_LEVEL"
  if [[ -z "$effective_ack_level" ]]; then
    if [[ "$REPLICATION_FACTOR" -gt 1 ]]; then
      effective_ack_level=2
    else
      effective_ack_level=1
    fi
  fi

  local -a env_args=(
    "TAG=$TAG"
    "RUN_ID=$run_id"
    "SYSTEM=$system"
    "ORDER=$order"
    "SEQUENCER=$sequencer"
    "NUM_CLIENTS=$num_clients"
    "REPLICATION_FACTOR=$REPLICATION_FACTOR"
    "ACK_LEVEL=$effective_ack_level"
    "NUM_TRIALS=$NUM_TRIALS"
    "NUM_BROKERS=$NUM_BROKERS"
    "TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE"
    "MESSAGE_SIZE=$MESSAGE_SIZE"
    "TEST_TYPE=$TEST_TYPE"
    "THREADS_PER_BROKER=$THREADS_PER_BROKER"
    "BROKER_IP=$BROKER_IP"
    "START_DELAY_SEC=$START_DELAY_SEC"
    "LOCAL_CLIENT_NUMA=$LOCAL_CLIENT_NUMA"
    "TRIAL_MAX_ATTEMPTS=$TRIAL_MAX_ATTEMPTS"
    "REQUIRE_FIRST_ATTEMPT_PASS=$REQUIRE_FIRST_ATTEMPT_PASS"
    "CORFU_SEQUENCER_LOG_HOST=$REMOTE_SEQ_HOST"
    "LAZYLOG_SEQUENCER_LOG_HOST=$REMOTE_SEQ_HOST"
    "SCALOG_SEQUENCER_LOG_HOST=$REMOTE_SEQ_HOST"
  )

  case "$sequencer" in
    CORFU)
      env_args+=(
        "REMOTE_CORFU_SEQUENCER_HOST=$REMOTE_SEQ_HOST"
        "REMOTE_CORFU_BUILD_BIN=$REMOTE_SEQ_BUILD_BIN"
        "EMBARCADERO_CORFU_SEQ_IP=$REMOTE_SEQ_IP"
        "EMBARCADERO_ACK_TIMEOUT_SEC=$EMBARCADERO_ACK_TIMEOUT_SEC"
      )
      ;;
    LAZYLOG)
      env_args+=(
        "REMOTE_LAZYLOG_SEQUENCER_HOST=$REMOTE_SEQ_HOST"
        "REMOTE_LAZYLOG_BUILD_BIN=$REMOTE_SEQ_BUILD_BIN"
        "EMBARCADERO_LAZYLOG_SEQ_IP=$REMOTE_SEQ_IP"
        "EMBARCADERO_ACK_TIMEOUT_SEC=$EMBARCADERO_ACK_TIMEOUT_SEC"
      )
      ;;
    SCALOG)
      env_args+=(
        "REMOTE_SCALOG_SEQUENCER_HOST=$REMOTE_SEQ_HOST"
        "REMOTE_SCALOG_BUILD_BIN=$REMOTE_SEQ_BUILD_BIN"
        "EMBARCADERO_SCALOG_SEQ_IP=$REMOTE_SEQ_IP"
        "EMBARCADERO_ACK_TIMEOUT_SEC=$EMBARCADERO_ACK_TIMEOUT_SEC"
      )
      ;;
  esac

  printf '\n=== %s order=%s rf=%s ack=%s n=%s run_id=%s ===\n' \
    "$sequencer" "$order" "$REPLICATION_FACTOR" "$effective_ack_level" "$num_clients" "$run_id"
  local rc=0
  env "${env_args[@]}" bash scripts/publication/run_throughput_cell.sh || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    echo "Cell failed: sequencer=$sequencer order=$order num_clients=$num_clients run_dir=$run_dir rc=$rc" >&2
    failures=$((failures + 1))
  fi
  python3 scripts/publication/summarize_publication_results.py throughput "$TAG" || true
}

for n in "${NUM_CLIENT_VALUES[@]}"; do
  run_cell embarcadero 0 EMBARCADERO "$n"
done

for n in "${NUM_CLIENT_VALUES[@]}"; do
  run_cell embarcadero 5 EMBARCADERO "$n"
done

for n in "${NUM_CLIENT_VALUES[@]}"; do
  run_cell corfu 2 CORFU "$n"
done

for n in "${NUM_CLIENT_VALUES[@]}"; do
  run_cell lazylog 2 LAZYLOG "$n"
done

for n in "${NUM_CLIENT_VALUES[@]}"; do
  run_cell scalog 1 SCALOG "$n"
done

echo
echo "Matrix complete. Aggregate summary:"
echo "  $PROJECT_ROOT/data/publication/throughput/$TAG/throughput_summary.csv"
if [[ "$failures" -ne 0 ]]; then
  echo "Matrix completed with $failures failed cell(s)." >&2
  exit 1
fi
