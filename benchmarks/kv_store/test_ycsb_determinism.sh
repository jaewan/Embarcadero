#!/usr/bin/env bash
# Semantic validation for the YCSB distributed-KV evaluation plan
# (docs/experiments/YCSB_DISTRIBUTED_KV_PLAN.md, Section 2/Q2): the whole
# benchmark shares one hardcoded std::mt19937_64 rng(42) with no --seed CLI
# override (kv_bench_main.cc:505), so every system is expected to see the
# identical op/key/value sequence for a given flag set. This test proves that
# empirically by running the identical single-process workload twice, each
# time against a fresh cluster/CXL segment, and comparing an order-independent
# content digest (DistributedKVStore::stateDigest(), a commutative hash over
# every key/value pair) captured by an independent --replica process — a
# stronger signal than comparing op-count summaries, since it also confirms
# every key resolved to the same final value in both runs.
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
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-137438953472}"
BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-120}"
REPLICA_TIMEOUT_SEC="${REPLICA_TIMEOUT_SEC:-60}"

WORKLOAD="${WORKLOAD:-A}"
RECORD_COUNT="${RECORD_COUNT:-2000}"
OPERATION_COUNT="${OPERATION_COUNT:-4000}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/build/results/test_ycsb_determinism_$(date +%Y%m%d_%H%M%S)}"
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

BENCH_COMMON=(
  --sequencer=EMBARCADERO
  --order=5
  --ack=1
  --rf=1
  --workload="$WORKLOAD"
  --zipf_theta=0.99
  --record_count="$RECORD_COUNT"
  --operation_count="$OPERATION_COUNT"
  --value_size=100
  --batch_size=1
  --warmup_ops=0
  --broker_ip=127.0.0.1
)

# run_once RUN_LABEL: fresh cluster, one writer (manage_cluster=0, this
# driver owns cleanup) + one --replica capturing a content digest of the
# resulting store. Prints the digest file path on success.
run_once() {
  local label="$1"
  local run_dir="$OUT_ROOT/$label"
  mkdir -p "$run_dir"

  DRIVER_OWNS_CLUSTER=1
  broker_local_cleanup
  DRIVER_CXL_SHM_NAME="/CXL_KVBASE_${UID}_determinism_${label}_$$_$(date +%s)_${RANDOM}"
  export EMBARCADERO_CXL_SHM_NAME="$DRIVER_CXL_SHM_NAME"
  unlink_kvbase_shm
  sleep 0.5

  echo "[$label] Starting $NUM_BROKERS-broker EMBARCADERO cluster..." >&2
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
    echo "[$label] ERROR: broker not ready" >&2
    return 1
  fi
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
  sleep 4

  echo "[$label] Running writer (workload=$WORKLOAD)..." >&2
  set +e
  "$BIN_DIR/kv_ycsb_bench" "${BENCH_COMMON[@]}" \
    --manage_cluster=0 \
    --run_id="determinism_${label}" --output_dir="$run_dir" \
    >"$run_dir/writer.log" 2>&1
  local writer_rc=$?
  set -e
  if [[ "$writer_rc" -ne 0 ]]; then
    echo "[$label] ERROR: writer exited $writer_rc" >&2
    tail -n 30 "$run_dir/writer.log" >&2
    broker_local_cleanup; DRIVER_OWNS_CLUSTER=0; unlink_kvbase_shm
    return 1
  fi

  local expected_entries
  expected_entries="$(grep -oP 'applied_entries=\K[0-9]+' "$run_dir/writer.log" | tail -1 || true)"
  if [[ -z "$expected_entries" ]]; then
    echo "[$label] ERROR: could not parse applied_entries from writer.log" >&2
    broker_local_cleanup; DRIVER_OWNS_CLUSTER=0; unlink_kvbase_shm
    return 1
  fi

  echo "[$label] Running replica (expected_entries=$expected_entries)..." >&2
  set +e
  "$BIN_DIR/kv_ycsb_bench" "${BENCH_COMMON[@]}" \
    --replica --expected_entries="$expected_entries" \
    --replica_timeout_sec="$REPLICA_TIMEOUT_SEC" \
    --digest_out="$run_dir/digest.txt" \
    >"$run_dir/replica.log" 2>&1
  local replica_rc=$?
  set -e

  broker_local_cleanup
  DRIVER_OWNS_CLUSTER=0
  unlink_kvbase_shm

  if [[ "$replica_rc" -ne 0 ]]; then
    echo "[$label] ERROR: replica exited $replica_rc" >&2
    tail -n 30 "$run_dir/replica.log" >&2
    return 1
  fi
  if ! grep -q "complete=1" "$run_dir/digest.txt" 2>/dev/null; then
    echo "[$label] ERROR: replica did not report complete=1" >&2
    cat "$run_dir/digest.txt" 2>&1 >&2
    return 1
  fi

  echo "[$label] writer ops: $(grep -oP 'kv_bench_main\.cc:\d+\] \KOps:.*' "$run_dir/writer.log" || true)" >&2
  echo "[$label] digest: $(cat "$run_dir/digest.txt")" >&2
  sleep 1
}

run_once run1
run_once run2

DRIVER_DONE=1

digest1="$(cat "$OUT_ROOT/run1/digest.txt")"
digest2="$(cat "$OUT_ROOT/run2/digest.txt")"
ops1="$(grep -oP 'kv_bench_main\.cc:\d+\] \KOps:.*' "$OUT_ROOT/run1/writer.log" || true)"
ops2="$(grep -oP 'kv_bench_main\.cc:\d+\] \KOps:.*' "$OUT_ROOT/run2/writer.log" || true)"

echo ""
echo "run1: $ops1"
echo "run1: $digest1"
echo "run2: $ops2"
echo "run2: $digest2"

fail=0
if [[ -z "$ops1" || -z "$ops2" ]]; then
  echo "FAIL: could not parse an Ops: line from one or both writer logs (vacuous comparison — do not treat matching-empty as a pass)" >&2
  fail=1
elif [[ "$ops1" != "$ops2" ]]; then
  echo "FAIL: op-count breakdown differs between run1 and run2" >&2
  fail=1
fi
if [[ "$digest1" != "$digest2" ]]; then
  echo "FAIL: state digest differs between run1 and run2 (non-deterministic content)" >&2
  fail=1
fi

if [[ "$fail" -eq 0 ]]; then
  echo "PASS: workload=$WORKLOAD is deterministic across independent runs (identical op-counts and identical content digest)"
else
  echo "Full logs kept under $OUT_ROOT"
fi
exit "$fail"
