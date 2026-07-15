#!/usr/bin/env bash
# SMR-FIFO end-to-end eval (paper Q3, tab:kv-pipelined).
#
# Runs the shared-log KV store in --fifo_valid mode: one client session issues
# pipelined, versioned overwrites that stripe across all brokers; the run is
# Valid iff every key's final value is the LAST version submitted for it
# (session FIFO), store size stays record_count, and applied == published.
#
# Matrix: sequencers x {pipe, serialize} x trials.
#   pipe      : sync_interval=0  (max pipeline; Embar's claimed setting)
#   serialize : sync_interval=1, stop-and-wait on the ACK barrier — the
#               "restore FIFO without native holds" fairness row for
#               write-before-order baselines.
#
# A striped-Scalog/LazyLog Pipe run that fails session-FIFO validation is a
# RESULT (Appendix app:scalog-fifo), not a harness bug — do not "fix" it by
# importing Embar-style holds (docs/baselines/porting_rule.md).
#
# Cluster lifecycle mirrors run_kv_baseline_compare.sh (same bring-up, same
# knobs) so Pipe rows here are comparable to that driver's Phase 1.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$PROJECT_ROOT/build/bin}"
source "$PROJECT_ROOT/scripts/lib/broker_lifecycle.sh"

export PROJECT_ROOT
broker_init_paths

NUM_BROKERS="${NUM_BROKERS:-4}"
export NUM_BROKERS EMBARCADERO_NUM_BROKERS="$NUM_BROKERS"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-1}"
export REPLICATION_FACTOR EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"
export EMBARCADERO_RUNTIME_MODE="${EMBARCADERO_RUNTIME_MODE:-throughput}"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
# Default config (64GB CXL, 8GB segments) leaves a ~30GB segment region = 3
# segments, but every broker needs one segment for the KV topic: with 4 brokers
# the last one crash-loops on "CXL memory exhausted". 128GB keeps 4 brokers +
# headroom; zero_mode=metadata keeps the shm mapping sparse.
export EMBARCADERO_CXL_SIZE="${KV_BENCH_CXL_SIZE_BYTES:-137438953472}"
export EMBARCADERO_SCALOG_SEQ_IP="${EMBARCADERO_SCALOG_SEQ_IP:-127.0.0.1}"
export EMBARCADERO_LAZYLOG_SEQ_IP="${EMBARCADERO_LAZYLOG_SEQ_IP:-127.0.0.1}"
export EMBARCADERO_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-127.0.0.1}"
export SCALOG_CXL_MODE="${SCALOG_CXL_MODE:-1}"
export LAZYLOG_CXL_MODE="${LAZYLOG_CXL_MODE:-1}"
export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-0}"

SMR_FIFO_BROKER_IP="${SMR_FIFO_BROKER_IP:-127.0.0.1}"
# Paper caption knobs (tab:kv-pipelined): 500K keys, 50K warmup, 500K overwrites.
SMR_FIFO_RECORD_COUNT="${SMR_FIFO_RECORD_COUNT:-500000}"
SMR_FIFO_OPERATION_COUNT="${SMR_FIFO_OPERATION_COUNT:-500000}"
SMR_FIFO_WARMUP_OPS="${SMR_FIFO_WARMUP_OPS:-50000}"
# Serialize mode is stop-and-wait (can be ~1000x slower); use fewer ops and
# compare RATES. Slowdown is computed from ops/s, not wall time.
SMR_FIFO_SERIALIZE_OPS="${SMR_FIFO_SERIALIZE_OPS:-20000}"
SMR_FIFO_SERIALIZE_BARRIER="${SMR_FIFO_SERIALIZE_BARRIER:-ack}"
SMR_FIFO_VALUE_SIZE="${SMR_FIFO_VALUE_SIZE:-100}"
SMR_FIFO_ACK="${SMR_FIFO_ACK:-1}"
SMR_FIFO_RF="${SMR_FIFO_RF:-1}"
# Striping note: the publisher round-robins sealed batches across ALL brokers
# regardless of pub_threads (src/client/queue_buffer.cc SealCurrentAndAdvance),
# so pub_threads=1 still stripes. Keep it matched across sequencers.
SMR_FIFO_PUB_THREADS="${SMR_FIFO_PUB_THREADS:-1}"
SMR_FIFO_NUM_TRIALS="${SMR_FIFO_NUM_TRIALS:-3}"
SMR_FIFO_SEQUENCERS="${SMR_FIFO_SEQUENCERS:-EMBARCADERO CORFU SCALOG LAZYLOG}"
SMR_FIFO_MODES="${SMR_FIFO_MODES:-pipe serialize}"
SMR_FIFO_LOG_LEVEL="${SMR_FIFO_LOG_LEVEL:-0}"
BENCH_TIMEOUT_SEC="${BENCH_TIMEOUT_SEC:-900}"
BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-120}"

OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/build/results/smr_fifo_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT_ROOT"

# Only remove THIS driver's shm segments (CXL_KVBASE_<uid>_*). A wildcard
# /dev/shm/CXL_* rm would unlink segments belonging to concurrently running
# harnesses (moscxl is a shared box).
kv_bench_unlink_kvbase_shm() {
  rm -f /dev/shm/CXL_KVBASE_"${UID}"_* 2>/dev/null || true
}

# broker_local_cleanup kills embarlet/sequencer processes BY NAME, host-wide.
# Refuse to start on top of someone else's live cluster instead of nuking it.
assert_broker_ports_free() {
  # Grace period covers this driver's own slow teardown (tmpfs unmap can hold
  # the port briefly between trials).
  local i
  for i in $(seq 1 15); do
    if ! ss -H -ltn 'sport = :1214' 2>/dev/null | grep -q .; then
      return 0
    fi
    sleep 1
  done
  echo "ERROR: port 1214 in use — another broker cluster is running on this host." >&2
  echo "       Refusing to start (cleanup here would kill that cluster)." >&2
  ss -ltnp 2>/dev/null | grep -E ':121[4-7]' >&2 || true
  return 1
}

# shellcheck disable=SC2206
if command -v numactl >/dev/null 2>&1 && numactl -H 2>/dev/null | grep -qE '^node 1 cpus:'; then
  if numactl -H 2>/dev/null | grep -qE '^node 2 cpus:'; then
    EMBARLET_NUMA_ARR=(numactl --cpunodebind=1 --membind=1,2)
  else
    EMBARLET_NUMA_ARR=(numactl --cpunodebind=1 --membind=1)
  fi
else
  EMBARLET_NUMA_ARR=()
fi

wait_scalog_port() {
  local port="${EMBARCADERO_SCALOG_SEQ_PORT:-50051}"
  local i
  for i in $(seq 1 100); do
    if ss -H -ltn "sport = :$port" 2>/dev/null | grep -q .; then
      return 0
    fi
    sleep 0.1
  done
  echo "ERROR: Scalog sequencer did not bind :$port" >&2
  tail -n 40 /tmp/scalog_sequencer.log >&2 || true
  return 1
}

start_cluster() {
  local seq="$1"
  assert_broker_ports_free || return 1
  broker_local_cleanup
  kv_bench_unlink_kvbase_shm
  sleep 0.5

  export EMBARCADERO_CXL_SHM_NAME="/CXL_KVBASE_${UID}_${seq}_$$_$(date +%s)_${RANDOM}"

  local -a run_env=(
    env
    "REPLICATION_FACTOR=$REPLICATION_FACTOR"
    "EMBARCADERO_REPLICATION_FACTOR=$REPLICATION_FACTOR"
    "NUM_BROKERS=$NUM_BROKERS"
    "EMBARCADERO_NUM_BROKERS=$NUM_BROKERS"
    "EMBARCADERO_CXL_SHM_NAME=$EMBARCADERO_CXL_SHM_NAME"
    "EMBARCADERO_CXL_ZERO_MODE=$EMBARCADERO_CXL_ZERO_MODE"
    "EMBAR_USE_HUGETLB=$EMBAR_USE_HUGETLB"
    "EMBARCADERO_HEAD_ADDR=$EMBARCADERO_HEAD_ADDR"
  )
  if [[ -n "${EMBARCADERO_CXL_SIZE:-}" ]]; then
    run_env+=("EMBARCADERO_CXL_SIZE=$EMBARCADERO_CXL_SIZE")
  fi
  run_env+=("EMBARCADERO_RUNTIME_MODE=${KV_BENCH_BROKER_RUNTIME_MODE:-latency}")
  if [[ "$seq" == "SCALOG" ]]; then
    run_env+=("SCALOG_CXL_MODE=${SCALOG_CXL_MODE:-1}")
  fi
  if [[ "$seq" == "LAZYLOG" ]]; then
    run_env+=("LAZYLOG_CXL_MODE=${LAZYLOG_CXL_MODE:-1}")
    run_env+=("EMBARCADERO_LAZYLOG_SEQ_IP=$EMBARCADERO_LAZYLOG_SEQ_IP")
  fi

  if [[ "$seq" == "SCALOG" ]]; then
    "$BIN_DIR/scalog_global_sequencer" >>/tmp/scalog_sequencer.log 2>&1 &
    wait_scalog_port
  elif [[ "$seq" == "LAZYLOG" ]]; then
    "$BIN_DIR/lazylog_global_sequencer" >>/tmp/lazylog_sequencer.log 2>&1 &
    sleep 0.5
  elif [[ "$seq" == "CORFU" ]]; then
    "$BIN_DIR/corfu_global_sequencer" >>/tmp/corfu_sequencer.log 2>&1 &
    sleep 0.3
  fi

  local -a pids=()
  echo "Starting head --$seq..."
  "${run_env[@]}" "${EMBARLET_NUMA_ARR[@]}" "$BIN_DIR/embarlet" \
    --config "$BROKER_CONFIG_ABS" --head --"$seq" \
    >"$BIN_DIR/broker_0.log" 2>&1 &
  pids+=("$!")

  local i
  for ((i = 1; i < NUM_BROKERS; i++)); do
    echo "Starting broker $i --$seq..."
    "${run_env[@]}" "${EMBARLET_NUMA_ARR[@]}" "$BIN_DIR/embarlet" \
      --config "$BROKER_CONFIG_ABS" --"$seq" \
      >"$BIN_DIR/broker_${i}.log" 2>&1 &
    pids+=("$!")
  done

  if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS" "${pids[@]}"; then
    echo "ERROR: brokers not ready for $seq" >&2
    return 1
  fi
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  sleep "${BROKER_READY_PROPAGATION_SEC:-4}"
}

# The EXIT trap exists for aborted runs. broker_local_cleanup kills embarlet/
# sequencer processes host-wide BY NAME, so an unconditional trap firing after
# a normal run (where each trial already cleaned up) can murder a cluster some
# NEWER driver invocation just started. Skip it once all trials completed.
DRIVER_DONE=0
cleanup() { [[ "$DRIVER_DONE" == 1 ]] || broker_local_cleanup; }
trap cleanup EXIT

# run_one SEQ MODE TRIAL
run_one() {
  local seq="$1" mode="$2" trial="$3"
  local sync_iv=0 sync_barrier=apply ops="$SMR_FIFO_OPERATION_COUNT"
  if [[ "$mode" == "serialize" ]]; then
    sync_iv=1
    sync_barrier="$SMR_FIFO_SERIALIZE_BARRIER"
    ops="$SMR_FIFO_SERIALIZE_OPS"
  fi
  local _bench_to="$BENCH_TIMEOUT_SEC"
  [[ "$seq" == "SCALOG" ]] && _bench_to="${BENCH_TIMEOUT_SCALOG:-1800}"

  echo "======== $seq  $mode  trial $trial  (sync_interval=$sync_iv barrier=$sync_barrier ops=$ops) ========"
  if ! start_cluster "$seq"; then
    echo "ERROR: skipping $seq $mode trial $trial (cluster did not start)" >&2
    broker_local_cleanup; sleep 2; return 1
  fi
  local bench_status=0
  set +e
  timeout "$_bench_to" env "EMBAR_USE_HUGETLB=$EMBAR_USE_HUGETLB" \
    "EMBARCADERO_RUNTIME_MODE=${KV_BENCH_CLIENT_RUNTIME_MODE:-latency}" \
    "${EMBARLET_NUMA_ARR[@]}" "$BIN_DIR/kv_ycsb_bench" \
    --sequencer="$seq" \
    --order=-1 \
    --fifo_valid \
    --record_count="$SMR_FIFO_RECORD_COUNT" \
    --operation_count="$ops" \
    --warmup_ops="$SMR_FIFO_WARMUP_OPS" \
    --value_size="$SMR_FIFO_VALUE_SIZE" \
    --batch_size=1 \
    --ack="$SMR_FIFO_ACK" \
    --rf="$SMR_FIFO_RF" \
    --pub_threads="$SMR_FIFO_PUB_THREADS" \
    --sync_interval="$sync_iv" \
    --sync_barrier="$sync_barrier" \
    --log_level="$SMR_FIFO_LOG_LEVEL" \
    --broker_ip="$SMR_FIFO_BROKER_IP" \
    --output_dir="$OUT_ROOT" \
    --run_id="${mode}_trial${trial}" \
    2>&1 | tee "$OUT_ROOT/${seq}_${mode}_trial${trial}.log"
  bench_status=${PIPESTATUS[0]}
  set -e
  broker_local_cleanup
  kv_bench_unlink_kvbase_shm
  sleep "${BROKER_SEQ_GAP_SEC:-3}"
  if [[ "$bench_status" -ne 0 ]]; then
    # A Valid=NO run exits nonzero by design; the row is still written and is a
    # result (e.g. striped Scalog Pipe). Only a missing summary.csv is an error.
    echo "NOTE: $seq $mode trial $trial exited $bench_status (Valid=NO or failure)" >&2
    return "$bench_status"
  fi
}

# shellcheck disable=SC2206
read -r -a SEQ_ARR <<< "$SMR_FIFO_SEQUENCERS"
# shellcheck disable=SC2206
read -r -a MODE_ARR <<< "$SMR_FIFO_MODES"

declare -a invalid_runs=()
declare -a broken_runs=()
for SEQ in "${SEQ_ARR[@]}"; do
  for MODE in "${MODE_ARR[@]}"; do
    for TRIAL in $(seq 1 "$SMR_FIFO_NUM_TRIALS"); do
      if ! run_one "$SEQ" "$MODE" "$TRIAL"; then
        if compgen -G "$OUT_ROOT/${SEQ}_*_${MODE}_trial${TRIAL}/summary.csv" >/dev/null; then
          invalid_runs+=("${SEQ}:${MODE}:trial${TRIAL}")
        else
          broken_runs+=("${SEQ}:${MODE}:trial${TRIAL}")
        fi
      fi
    done
  done
done

# ── Aggregate per-run summary.csv rows into one file with mode/trial columns ──
AGG="$OUT_ROOT/summary.csv"
first=1
for f in "$OUT_ROOT"/*/summary.csv; do
  [[ -e "$f" ]] || continue
  run_dir="$(basename "$(dirname "$f")")"
  # run_id suffix is "<mode>_trial<N>"
  mode="$(sed -E 's/.*_(pipe|serialize)_trial[0-9]+$/\1/' <<<"$run_dir")"
  trial="$(sed -E 's/.*_trial([0-9]+)$/\1/' <<<"$run_dir")"
  if [[ "$first" == 1 ]]; then
    echo "mode,trial,$(head -1 "$f")" > "$AGG"
    first=0
  fi
  echo "$mode,$trial,$(tail -n +2 "$f")" >> "$AGG"
done

if [[ -f "$AGG" ]]; then
  echo ""
  echo "Aggregated: $AGG"
  # Markdown snippet + plots (plots skipped if matplotlib is unavailable)
  python3 "$PROJECT_ROOT/scripts/plot_smr_fifo.py" \
    --csv "$AGG" \
    --outdir "$OUT_ROOT" \
    --markdown "$OUT_ROOT/paper_snippet.md" || true
  [[ -f "$OUT_ROOT/paper_snippet.md" ]] && cat "$OUT_ROOT/paper_snippet.md"
fi

DRIVER_DONE=1
echo ""
echo "Done. Results under $OUT_ROOT"
if [[ ${#invalid_runs[@]} -gt 0 ]]; then
  echo "VALID=NO RUNS (results, inspect failed_checks column): ${invalid_runs[*]}"
fi
if [[ ${#broken_runs[@]} -gt 0 ]]; then
  echo "BROKEN RUNS (no summary written): ${broken_runs[*]}" >&2
  exit 1
fi
