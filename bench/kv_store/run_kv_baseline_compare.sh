#!/usr/bin/env bash
# Start local brokers + global sequencer (when needed), run kv_ycsb_bench per sequencer, tear down.
# Same workload args across sequencers for apples-to-apples comparison.
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
# Default client/broker runtime for general scripts; this driver overrides per sequencer below.
export EMBARCADERO_RUNTIME_MODE="${EMBARCADERO_RUNTIME_MODE:-throughput}"
# Full-region memset (~config cxl.size) can SIGBUS on shm-backed / mis-bound CXL; metadata is enough for benchmarks.
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
# Optional: shrink CXL mapping on tiny /dev/shm hosts (must match across all brokers). Prefer cleaning stale /dev/shm/CXL_* first.
if [[ -n "${KV_BENCH_CXL_SIZE_BYTES:-}" ]]; then
  export EMBARCADERO_CXL_SIZE="$KV_BENCH_CXL_SIZE_BYTES"
fi
# Fresh SHM per cluster start (do not reuse stale EMBARCADERO_CXL_SHM_NAME from the shell).
# Set KV_BENCH_FIXED_CXL_SHM_NAME only if you intentionally want one segment for all steps.
export EMBARCADERO_SCALOG_SEQ_IP="${EMBARCADERO_SCALOG_SEQ_IP:-127.0.0.1}"
export EMBARCADERO_LAZYLOG_SEQ_IP="${EMBARCADERO_LAZYLOG_SEQ_IP:-127.0.0.1}"
# Client connects here; broker bind/advertise via EMBARCADERO_HEAD_ADDR (inherit or set below).
export EMBARCADERO_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-127.0.0.1}"
KV_BENCH_BROKER_IP="${KV_BENCH_BROKER_IP:-127.0.0.1}"
KV_BENCH_RECORD_COUNT="${KV_BENCH_RECORD_COUNT:-100000}"
KV_BENCH_OPERATION_COUNT="${KV_BENCH_OPERATION_COUNT:-500000}"
KV_BENCH_WARMUP_OPS="${KV_BENCH_WARMUP_OPS:-10000}"
KV_BENCH_PUB_THREADS="${KV_BENCH_PUB_THREADS:-3}"
KV_BENCH_LOG_LEVEL="${KV_BENCH_LOG_LEVEL:-0}"
KV_BENCH_ENABLE_LATENCY="${KV_BENCH_ENABLE_LATENCY:-1}"
KV_BENCH_NUM_TRIALS="${KV_BENCH_NUM_TRIALS:-3}"
export SCALOG_CXL_MODE="${SCALOG_CXL_MODE:-1}"
export LAZYLOG_CXL_MODE="${LAZYLOG_CXL_MODE:-1}"
# Four brokers × multi-GB mmap per start; hugepage pools can trigger SIGBUS under rapid restart.
export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-0}"
# Each sequencer start uses a fresh EMBARCADERO_CXL_SHM_NAME under /dev/shm; without unlink,
# back-to-back runs fill tmpfs (SIGBUS on memset) long before ftruncate fails.
KV_BENCH_REMOVE_STALE_KVBASE_SHM="${KV_BENCH_REMOVE_STALE_KVBASE_SHM:-1}"
kv_bench_unlink_kvbase_shm() {
  [[ "${KV_BENCH_REMOVE_STALE_KVBASE_SHM}" == "1" ]] || return 0
  rm -f /dev/shm/CXL_KVBASE_"${UID}"_* 2>/dev/null || true
  rm -f /dev/shm/CXL_* 2>/dev/null || true
}

BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-120}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/build/results/kv_baseline_compare_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT_ROOT"

_avail_kb="$(df -Pk /dev/shm 2>/dev/null | awk 'NR==2 {print $4}')"
if [[ -n "${_avail_kb:-}" && "${_avail_kb}" -lt 524288 ]]; then
  echo "WARNING: /dev/shm has <512MiB free (${_avail_kb} KiB). Stale CXL_* segments cause SIGBUS; remove them before benchmarking." >&2
fi
unset _avail_kb

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
  broker_local_cleanup
  kv_bench_unlink_kvbase_shm
  sleep 0.5

  if [[ -n "${KV_BENCH_FIXED_CXL_SHM_NAME:-}" ]]; then
    export EMBARCADERO_CXL_SHM_NAME="$KV_BENCH_FIXED_CXL_SHM_NAME"
  else
    export EMBARCADERO_CXL_SHM_NAME="/CXL_KVBASE_${UID}_${seq}_$$_$(date +%s)_${RANDOM}"
  fi

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

  local -a embarlet_bind=("${EMBARLET_NUMA_ARR[@]}")

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
  "${run_env[@]}" "${embarlet_bind[@]}" "$BIN_DIR/embarlet" \
    --config "$BROKER_CONFIG_ABS" --head --"$seq" \
    >"$BIN_DIR/broker_0.log" 2>&1 &
  pids+=("$!")

  local i
  for ((i = 1; i < NUM_BROKERS; i++)); do
    echo "Starting broker $i --$seq..."
    "${run_env[@]}" "${embarlet_bind[@]}" "$BIN_DIR/embarlet" \
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

cleanup() { broker_local_cleanup; }
trap cleanup EXIT

BENCH_COMMON_BASE=(
  --record_count="$KV_BENCH_RECORD_COUNT"
  --operation_count="$KV_BENCH_OPERATION_COUNT"
  --value_size=100
  --batch_size=1
  --write_ratio=1
  --warmup_ops="$KV_BENCH_WARMUP_OPS"
  --ack=1
  --rf=1
  --log_level="$KV_BENCH_LOG_LEVEL"
  --broker_ip="$KV_BENCH_BROKER_IP"
  --pub_threads="$KV_BENCH_PUB_THREADS"
  --output_dir="$OUT_ROOT"
)
if [[ "$KV_BENCH_ENABLE_LATENCY" == "1" ]]; then
  BENCH_COMMON_BASE+=(--latency)
fi
BENCH_TIMEOUT_SEC="${BENCH_TIMEOUT_SEC:-600}"
KV_BENCH_SEQUENCERS="${KV_BENCH_SEQUENCERS:-EMBARCADERO CORFU SCALOG LAZYLOG}"
# Set to 1 to run the FIFO-cost comparison (Phase 2).
KV_BENCH_RUN_FIFO_COMPARISON="${KV_BENCH_RUN_FIFO_COMPARISON:-0}"

# shellcheck disable=SC2206
read -r -a KV_BENCH_SEQ_ARR <<< "$KV_BENCH_SEQUENCERS"

# run_one_config SEQ TRIAL SYNC_INTERVAL SYNC_BARRIER TAG
#   TAG is appended to run_id and log name for identification.
run_one_config() {
  local seq="$1" trial="$2" sync_iv="$3" sync_barrier="$4" tag="$5"
  local _bench_to="$BENCH_TIMEOUT_SEC"
  [[ "$seq" == "SCALOG" ]] && _bench_to="${BENCH_TIMEOUT_SCALOG:-1200}"

  echo "======== $seq  trial $trial  $tag  (sync_interval=$sync_iv sync_barrier=$sync_barrier) ========"
  if ! start_cluster "$seq"; then
    echo "ERROR: skipping $seq trial $trial $tag (cluster did not start)" >&2
    broker_local_cleanup; sleep 2; return 1
  fi
  local -a bench_env=(
    env "EMBAR_USE_HUGETLB=$EMBAR_USE_HUGETLB"
    "EMBARCADERO_RUNTIME_MODE=${KV_BENCH_CLIENT_RUNTIME_MODE:-latency}"
  )
  local bench_status=0
  set +e
  timeout "$_bench_to" "${bench_env[@]}" \
    "${EMBARLET_NUMA_ARR[@]}" "$BIN_DIR/kv_ycsb_bench" \
    --sequencer="$seq" \
    --order=-1 \
    --sync_interval="$sync_iv" \
    --sync_barrier="$sync_barrier" \
    "${BENCH_COMMON_BASE[@]}" \
    --run_id="${tag}_trial${trial}" \
    2>&1 | tee "$OUT_ROOT/${seq}_${tag}_trial${trial}_bench.log"
  bench_status=${PIPESTATUS[0]}
  set -e
  broker_local_cleanup
  kv_bench_unlink_kvbase_shm
  sleep "${BROKER_SEQ_GAP_SEC:-3}"
  if [[ "$bench_status" -ne 0 ]]; then
    echo "ERROR: benchmark failed for $seq trial $trial $tag (exit=$bench_status)" >&2
    return "$bench_status"
  fi
}

failures=0
declare -a failed_runs=()

# ── Phase 1: Pipelined throughput (sync_interval=0), each system's native ordering ──
echo "═══════════════════════════════════════════════════════"
echo "  Phase 1: Pipelined throughput (sync_interval=0)"
echo "═══════════════════════════════════════════════════════"
for SEQ in "${KV_BENCH_SEQ_ARR[@]}"; do
  for TRIAL in $(seq 1 "$KV_BENCH_NUM_TRIALS"); do
    if ! run_one_config "$SEQ" "$TRIAL" 0 apply "pipelined"; then
      failures=$((failures + 1))
      failed_runs+=("${SEQ}:pipelined:trial${TRIAL}")
    fi
  done
done

# ── Phase 2: Cost of per-client FIFO ordering ──
# Embarcadero ORDER=5 provides FIFO natively via the hold buffer, so it keeps
# sync_interval=0. Baselines serialize at sync_interval=1 and gate on ACK, so
# batch N+1 is issued only after batch N completes the ordering pipeline.
if [[ "$KV_BENCH_RUN_FIFO_COMPARISON" == "1" ]]; then
  echo ""
  echo "═══════════════════════════════════════════════════════"
  echo "  Phase 2: Per-client FIFO ordering cost"
  echo "═══════════════════════════════════════════════════════"
  for SEQ in "${KV_BENCH_SEQ_ARR[@]}"; do
    if [[ "$SEQ" == "EMBARCADERO" ]]; then
      sync_iv=0  # hold buffer provides FIFO; no serialization needed
      sync_barrier=apply
    else
      sync_iv=1
      sync_barrier=ack  # stop-and-wait on ACK: one batch must complete before the next issues
    fi
    for TRIAL in $(seq 1 "$KV_BENCH_NUM_TRIALS"); do
      if ! run_one_config "$SEQ" "$TRIAL" "$sync_iv" "$sync_barrier" "fifo"; then
        failures=$((failures + 1))
        failed_runs+=("${SEQ}:fifo:trial${TRIAL}")
      fi
    done
  done
fi

echo ""
echo "Done. Results under $OUT_ROOT"
ls -la "$OUT_ROOT"
if [[ "$failures" -ne 0 ]]; then
  echo "FAILED RUNS: ${failed_runs[*]}" >&2
  exit 1
fi
