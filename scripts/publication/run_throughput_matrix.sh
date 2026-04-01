#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"

TAG="${TAG:-publication_matrix_current}"
RUN_TS="${RUN_TS:-$(date -u +%Y%m%dT%H%M%SZ)}"
NUM_TRIALS="${NUM_TRIALS:-1}"
NUM_BROKERS="${NUM_BROKERS:-4}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-16106127360}"
MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
TEST_TYPE="${TEST_TYPE:-5}"
THREADS_PER_BROKER="${THREADS_PER_BROKER:-4}"
ORDER5_THREADS_PER_BROKER="${ORDER5_THREADS_PER_BROKER:-1}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
START_DELAY_SEC="${START_DELAY_SEC:-8}"
LOCAL_CLIENT_NUMA="${LOCAL_CLIENT_NUMA:-0}"
TRIAL_MAX_ATTEMPTS="${TRIAL_MAX_ATTEMPTS:-1}"
REQUIRE_FIRST_ATTEMPT_PASS="${REQUIRE_FIRST_ATTEMPT_PASS:-1}"
REMOTE_SEQ_HOST="${REMOTE_SEQ_HOST:-c2}"
REMOTE_SEQ_BUILD_BIN="${REMOTE_SEQ_BUILD_BIN:-/home/domin/Embarcadero/build/bin}"
REMOTE_SEQ_IP="${REMOTE_SEQ_IP:-10.10.10.144}"
REMOTE_CORFU_SEQ_PORT="${REMOTE_CORFU_SEQ_PORT:-50052}"
REMOTE_LAZYLOG_SEQ_PORT="${REMOTE_LAZYLOG_SEQ_PORT:-50061}"
REMOTE_SCALOG_SEQ_PORT="${REMOTE_SCALOG_SEQ_PORT:-50051}"
EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-120}"
SKIP_EXISTING_CELLS="${SKIP_EXISTING_CELLS:-1}"
ACK_LEVEL="${ACK_LEVEL:-}"
MATRIX_SYSTEMS="${MATRIX_SYSTEMS:-embarcadero0 embarcadero5 corfu lazylog scalog}"
MATRIX_NUM_CLIENT_VALUES="${MATRIX_NUM_CLIENT_VALUES:-1 2 3}"
EMBARCADERO_RF_VALUES="${EMBARCADERO_RF_VALUES:-1}"
EMBARCADERO_ORDER5_RF_VALUES="${EMBARCADERO_ORDER5_RF_VALUES:-1}"
BASELINE_RF_VALUES="${BASELINE_RF_VALUES:-1 2}"
MIXED_CLIENT_HOSTS_1="${MIXED_CLIENT_HOSTS_1:-c4}"
MIXED_CLIENT_NUMAS_1="${MIXED_CLIENT_NUMAS_1:-1}"
MIXED_CLIENT_HOSTS_2="${MIXED_CLIENT_HOSTS_2:-c4,c3}"
MIXED_CLIENT_NUMAS_2="${MIXED_CLIENT_NUMAS_2:-1,1}"
MIXED_CLIENT_HOSTS_3="${MIXED_CLIENT_HOSTS_3:-c4,c3,local}"
MIXED_CLIENT_NUMAS_3="${MIXED_CLIENT_NUMAS_3:-1,1,0}"
REMOTE_CLIENT_HOSTS_1="${REMOTE_CLIENT_HOSTS_1:-c4}"
REMOTE_CLIENT_NUMAS_1="${REMOTE_CLIENT_NUMAS_1:-1}"
REMOTE_CLIENT_HOSTS_2="${REMOTE_CLIENT_HOSTS_2:-c4,c3}"
REMOTE_CLIENT_NUMAS_2="${REMOTE_CLIENT_NUMAS_2:-1,1}"
REMOTE_CLIENT_HOSTS_3="${REMOTE_CLIENT_HOSTS_3:-c4,c3,c2}"
REMOTE_CLIENT_NUMAS_3="${REMOTE_CLIENT_NUMAS_3:-1,1,1}"

read -r -a NUM_CLIENT_VALUES <<< "$MATRIX_NUM_CLIENT_VALUES"
failures=0

run_cell() {
  local system="$1"
  local order="$2"
  local sequencer="$3"
  local num_clients="$4"
  local replication_factor="$5"
  local threads_per_broker="$6"
  local client_hosts_csv="$7"
  local client_numas_csv="$8"
  local run_id="${RUN_TS}_${system}_o${order}_n${num_clients}"
  local cell_id="${system}_order${order}_rf${replication_factor}_n${num_clients}"
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
    if [[ "$replication_factor" -gt 1 ]]; then
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
    "REPLICATION_FACTOR=$replication_factor"
    "ACK_LEVEL=$effective_ack_level"
    "NUM_TRIALS=$NUM_TRIALS"
    "NUM_BROKERS=$NUM_BROKERS"
    "TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE"
    "MESSAGE_SIZE=$MESSAGE_SIZE"
    "TEST_TYPE=$TEST_TYPE"
    "THREADS_PER_BROKER=$threads_per_broker"
    "BROKER_IP=$BROKER_IP"
    "START_DELAY_SEC=$START_DELAY_SEC"
    "LOCAL_CLIENT_NUMA=$LOCAL_CLIENT_NUMA"
    "TRIAL_MAX_ATTEMPTS=$TRIAL_MAX_ATTEMPTS"
    "REQUIRE_FIRST_ATTEMPT_PASS=$REQUIRE_FIRST_ATTEMPT_PASS"
    "CLIENT_HOSTS_CSV=$client_hosts_csv"
    "CLIENT_NUMAS_CSV=$client_numas_csv"
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
        "EMBARCADERO_CORFU_SEQ_PORT=$REMOTE_CORFU_SEQ_PORT"
        "EMBARCADERO_ACK_TIMEOUT_SEC=$EMBARCADERO_ACK_TIMEOUT_SEC"
      )
      ;;
    LAZYLOG)
      env_args+=(
        "REMOTE_LAZYLOG_SEQUENCER_HOST=$REMOTE_SEQ_HOST"
        "REMOTE_LAZYLOG_BUILD_BIN=$REMOTE_SEQ_BUILD_BIN"
        "EMBARCADERO_LAZYLOG_SEQ_IP=$REMOTE_SEQ_IP"
        "EMBARCADERO_LAZYLOG_SEQ_PORT=$REMOTE_LAZYLOG_SEQ_PORT"
        "EMBARCADERO_ACK_TIMEOUT_SEC=$EMBARCADERO_ACK_TIMEOUT_SEC"
      )
      ;;
    SCALOG)
      env_args+=(
        "REMOTE_SCALOG_SEQUENCER_HOST=$REMOTE_SEQ_HOST"
        "REMOTE_SCALOG_BUILD_BIN=$REMOTE_SEQ_BUILD_BIN"
        "EMBARCADERO_SCALOG_SEQ_IP=$REMOTE_SEQ_IP"
        "EMBARCADERO_SCALOG_SEQ_PORT=$REMOTE_SCALOG_SEQ_PORT"
        "EMBARCADERO_ACK_TIMEOUT_SEC=$EMBARCADERO_ACK_TIMEOUT_SEC"
      )
      ;;
  esac

  printf '\n=== %s order=%s rf=%s ack=%s n=%s run_id=%s ===\n' \
    "$sequencer" "$order" "$replication_factor" "$effective_ack_level" "$num_clients" "$run_id"
  local rc=0
  env "${env_args[@]}" bash scripts/publication/run_throughput_cell.sh || rc=$?
  if [[ "$rc" -ne 0 ]]; then
    echo "Cell failed: sequencer=$sequencer order=$order num_clients=$num_clients run_dir=$run_dir rc=$rc" >&2
    failures=$((failures + 1))
  fi
  python3 scripts/publication/summarize_publication_results.py throughput "$TAG" || true
}

publication_layout_csv() {
  local mode="$1"
  local n="$2"
  case "$mode:$n" in
    mixed:1) printf '%s\n' "$MIXED_CLIENT_HOSTS_1|$MIXED_CLIENT_NUMAS_1" ;;
    mixed:2) printf '%s\n' "$MIXED_CLIENT_HOSTS_2|$MIXED_CLIENT_NUMAS_2" ;;
    mixed:3) printf '%s\n' "$MIXED_CLIENT_HOSTS_3|$MIXED_CLIENT_NUMAS_3" ;;
    remote:1) printf '%s\n' "$REMOTE_CLIENT_HOSTS_1|$REMOTE_CLIENT_NUMAS_1" ;;
    remote:2) printf '%s\n' "$REMOTE_CLIENT_HOSTS_2|$REMOTE_CLIENT_NUMAS_2" ;;
    remote:3) printf '%s\n' "$REMOTE_CLIENT_HOSTS_3|$REMOTE_CLIENT_NUMAS_3" ;;
    *)
      echo "unsupported publication layout mode=$mode num_clients=$n" >&2
      return 1
      ;;
  esac
}

run_matrix_group() {
  local system_key="$1"
  local rf_values="$2"
  local layout_mode="$3"
  local default_threads="$4"
  local system=""
  local order=""
  local sequencer=""

  case "$system_key" in
    embarcadero0)
      system="embarcadero"
      order="0"
      sequencer="EMBARCADERO"
      ;;
    embarcadero5)
      system="embarcadero"
      order="5"
      sequencer="EMBARCADERO"
      ;;
    corfu)
      system="corfu"
      order="2"
      sequencer="CORFU"
      ;;
    lazylog)
      system="lazylog"
      order="2"
      sequencer="LAZYLOG"
      ;;
    scalog)
      system="scalog"
      order="1"
      sequencer="SCALOG"
      ;;
    *)
      echo "unsupported MATRIX_SYSTEMS entry: $system_key" >&2
      return 1
      ;;
  esac

  local -a rf_list=()
  read -r -a rf_list <<< "$rf_values"
  local rf n layout client_hosts client_numas threads
  for rf in "${rf_list[@]}"; do
    for n in "${NUM_CLIENT_VALUES[@]}"; do
      layout="$(publication_layout_csv "$layout_mode" "$n")" || return 1
      client_hosts="${layout%%|*}"
      client_numas="${layout#*|}"
      threads="$default_threads"
      if [[ "$system_key" == "embarcadero5" ]]; then
        threads="$ORDER5_THREADS_PER_BROKER"
      fi
      run_cell "$system" "$order" "$sequencer" "$n" "$rf" "$threads" "$client_hosts" "$client_numas"
    done
  done
}

for system_key in $MATRIX_SYSTEMS; do
  case "$system_key" in
    embarcadero0)
      run_matrix_group "$system_key" "$EMBARCADERO_RF_VALUES" mixed "$THREADS_PER_BROKER"
      ;;
    embarcadero5)
      run_matrix_group "$system_key" "$EMBARCADERO_ORDER5_RF_VALUES" remote "$ORDER5_THREADS_PER_BROKER"
      ;;
    corfu|lazylog|scalog)
      run_matrix_group "$system_key" "$BASELINE_RF_VALUES" mixed "$THREADS_PER_BROKER"
      ;;
    *)
      echo "unsupported MATRIX_SYSTEMS entry: $system_key" >&2
      exit 1
      ;;
  esac
done

echo
echo "Matrix complete. Aggregate summary:"
echo "  $PROJECT_ROOT/data/publication/throughput/$TAG/throughput_summary.csv"
if [[ "$failures" -ne 0 ]]; then
  echo "Matrix completed with $failures failed cell(s)." >&2
  exit 1
fi
