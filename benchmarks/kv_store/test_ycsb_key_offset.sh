#!/usr/bin/env bash
# Semantic validation for the YCSB distributed-KV evaluation plan
# (docs/experiments/YCSB_DISTRIBUTED_KV_PLAN.md, Section 6): confirms that
# two concurrent kv_ycsb_bench processes sharing one topic with disjoint
# --key_offset ranges under a standard workload (WORKLOAD=A|F, default A) do
# not duplicate the load, do not read into each other's range, and both
# drain cleanly. This is the exact load-coordination model the multi-client
# YCSB matrix depends on. WORKLOAD=F additionally exercises the RMW dispatch
# path under the matrix's single-writer-per-key placement.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BIN_DIR="${BIN_DIR:-$PROJECT_ROOT/build/bin}"
source "$PROJECT_ROOT/scripts/lib/broker_lifecycle.sh"

export PROJECT_ROOT
broker_init_paths

NUM_BROKERS="${NUM_BROKERS:-4}"
export NUM_BROKERS EMBARCADERO_NUM_BROKERS="$NUM_BROKERS"
export EMBARCADERO_REPLICATION_FACTOR="${EMBARCADERO_REPLICATION_FACTOR:-1}"
export EMBARCADERO_RUNTIME_MODE="${EMBARCADERO_RUNTIME_MODE:-throughput}"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-0}"
export EMBARCADERO_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-127.0.0.1}"
# Match run_smr_fifo_eval.sh's default: 4 brokers x 8GB segment_size need
# headroom well beyond config/embarcadero.yaml's small default cxl.size, or
# topic creation exhausts segments and brokers crash (max_topics check-fail).
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-137438953472}"
BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-120}"

WORKLOAD="${WORKLOAD:-A}"
RECORD_COUNT_PER_SESSION="${RECORD_COUNT_PER_SESSION:-2000}"
OPERATION_COUNT_PER_SESSION="${OPERATION_COUNT_PER_SESSION:-4000}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/build/results/test_ycsb_key_offset_${WORKLOAD}_$(date +%Y%m%d_%H%M%S)}"
mkdir -p "$OUT_ROOT"

for name in embarlet throughput_test corfu_global_sequencer lazylog_global_sequencer scalog_global_sequencer; do
  while IFS= read -r pid; do
    [[ -n "$pid" ]] || continue
    state="$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null || true)"
    if [[ -n "$state" && "$state" != "Z" ]]; then
      echo "ERROR: live $name pid=$pid exists; refusing host-wide cleanup" >&2
      exit 1
    fi
  done < <(pgrep -x "$name" 2>/dev/null || true)
done

DRIVER_DONE=0
DRIVER_OWNS_CLUSTER=0
DRIVER_CXL_SHM_NAME=""
# Single source of truth for the scoped-deletion guard, used by both the
# abnormal-exit trap and the normal end-of-script teardown, so neither path
# can drift into an unscoped rm -f.
unlink_kvbase_shm() {
  [[ -n "$DRIVER_CXL_SHM_NAME" ]] || return 0
  case "$DRIVER_CXL_SHM_NAME" in
    /CXL_KVBASE_"${UID}"_*) rm -f "/dev/shm/${DRIVER_CXL_SHM_NAME#/}" 2>/dev/null || true ;;
    *) echo "ERROR: refusing to unlink unexpected SHM name: $DRIVER_CXL_SHM_NAME" >&2 ;;
  esac
}
cleanup() {
  [[ "$DRIVER_DONE" == 1 ]] && return 0
  [[ "$DRIVER_OWNS_CLUSTER" == 1 ]] && broker_local_cleanup
  unlink_kvbase_shm
}
trap cleanup EXIT

DRIVER_OWNS_CLUSTER=1
broker_local_cleanup
DRIVER_CXL_SHM_NAME="/CXL_KVBASE_${UID}_keyoffset_$$_$(date +%s)_${RANDOM}"
export EMBARCADERO_CXL_SHM_NAME="$DRIVER_CXL_SHM_NAME"
unlink_kvbase_shm
sleep 0.5

echo "Starting $NUM_BROKERS-broker EMBARCADERO cluster..."
broker_pids=()
env EMBARCADERO_RUNTIME_MODE="$EMBARCADERO_RUNTIME_MODE" \
    "$BIN_DIR/embarlet" --config "$BROKER_CONFIG_ABS" --head --EMBARCADERO \
    >"$BIN_DIR/broker_0.log" 2>&1 &
broker_pids+=("$!")
for ((i = 1; i < NUM_BROKERS; i++)); do
  env EMBARCADERO_RUNTIME_MODE="$EMBARCADERO_RUNTIME_MODE" \
      "$BIN_DIR/embarlet" --config "$BROKER_CONFIG_ABS" --EMBARCADERO \
      >"$BIN_DIR/broker_${i}.log" 2>&1 &
  broker_pids+=("$!")
done

if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS" "${broker_pids[@]}"; then
  echo "ERROR: broker not ready" >&2
  exit 1
fi
rm -f /tmp/embarlet_*_ready 2>/dev/null || true
sleep 4

BENCH_COMMON=(
  --sequencer=EMBARCADERO
  --order=5
  --ack=1
  --rf=1
  --workload="$WORKLOAD"
  --zipf_theta=0.99
  --record_count="$RECORD_COUNT_PER_SESSION"
  --operation_count="$OPERATION_COUNT_PER_SESSION"
  --value_size=100
  --batch_size=1
  --warmup_ops=0
  --shared_topic
  --broker_ip=127.0.0.1
)

echo "Launching workload=$WORKLOAD session 0 (key_offset=0, manage_cluster=1) and session 1 (key_offset=$RECORD_COUNT_PER_SESSION, manage_cluster=0)..."
set +e
"$BIN_DIR/kv_ycsb_bench" "${BENCH_COMMON[@]}" \
  --key_offset=0 --manage_cluster=1 \
  --run_id=keyoffset_s0 --output_dir="$OUT_ROOT" \
  >"$OUT_ROOT/session0.log" 2>&1 &
pid0=$!

sleep 2

"$BIN_DIR/kv_ycsb_bench" "${BENCH_COMMON[@]}" \
  --key_offset="$RECORD_COUNT_PER_SESSION" --manage_cluster=0 \
  --run_id=keyoffset_s1 --output_dir="$OUT_ROOT" \
  >"$OUT_ROOT/session1.log" 2>&1 &
pid1=$!

wait "$pid0"; rc0=$?
wait "$pid1"; rc1=$?
set -e

DRIVER_DONE=1
broker_local_cleanup
DRIVER_OWNS_CLUSTER=0
unlink_kvbase_shm

echo ""
echo "=== session0.log tail ==="
tail -n 25 "$OUT_ROOT/session0.log"
echo "=== session1.log tail ==="
tail -n 25 "$OUT_ROOT/session1.log"

fail=0
if [[ "$rc0" -ne 0 ]]; then
  echo "FAIL: session 0 (key_offset=0) exited $rc0" >&2
  fail=1
fi
if [[ "$rc1" -ne 0 ]]; then
  echo "FAIL: session 1 (key_offset=$RECORD_COUNT_PER_SESSION) exited $rc1" >&2
  fail=1
fi
# Do NOT grep for a bare "error" substring here: the client legitimately logs
# glog E-level lines for recoverable conditions (e.g. the cluster-status side
# channel timing out and retrying while the actual publish path keeps
# working) and still reaches a fully valid result. exit code + valid=YES
# below are the authoritative signals; main() returns
# runBenchmark(cfg) ? 0 : 1, and kv_bench_main.cc's fail_check() is what sets
# run_valid=false on any real mismatch, so nothing weaker is needed.
for f in "$OUT_ROOT/session0.log" "$OUT_ROOT/session1.log"; do
  if ! grep -q "valid=YES" "$f"; then
    echo "FAIL: $f did not report valid=YES" >&2
    fail=1
  fi
done

if [[ "$fail" -eq 0 ]]; then
  echo "PASS: both disjoint key_offset sessions (workload=$WORKLOAD) completed and drained cleanly (no cross-range errors)"
else
  echo "Full logs kept under $OUT_ROOT"
fi
exit "$fail"
