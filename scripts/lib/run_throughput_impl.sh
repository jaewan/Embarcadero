#!/bin/bash
# Shared implementation for local and remote throughput launchers.

SCRIPT_DIR="${SCRIPT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
source "$SCRIPT_DIR/lib/broker_lifecycle.sh"
cd "$PROJECT_ROOT"

THROUGHPUT_SCRIPT_MODE="${THROUGHPUT_SCRIPT_MODE:-auto}"
case "$THROUGHPUT_SCRIPT_MODE" in
  remote)
    if [ -z "${REMOTE_BROKER_HOST:-}" ]; then
      echo "ERROR: run_throughput.sh is the remote-client launcher. Set REMOTE_BROKER_HOST." >&2
      echo "       For local runs, use scripts/singlenode_run_throughput.sh." >&2
      exit 2
    fi
    if [ -z "${EMBARCADERO_HEAD_ADDR:-}" ]; then
      echo "ERROR: remote mode requires EMBARCADERO_HEAD_ADDR to point at the broker node IP." >&2
      exit 2
    fi
    ;;
  single)
    unset REMOTE_BROKER_HOST
    unset REMOTE_PROJECT_ROOT
    unset REMOTE_BUILD_BIN
    ;;
  auto)
    ;;
  *)
    echo "ERROR: unknown THROUGHPUT_SCRIPT_MODE=$THROUGHPUT_SCRIPT_MODE" >&2
    exit 2
    ;;
esac

# --- Configuration ---
NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-1}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-8589934592}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TEST_TYPE=${TEST_TYPE:-1}
ORDER=${ORDER:-5}
ACK=${ACK:-1}
REPLICATION_FACTOR=${REPLICATION_FACTOR:-0}
SEQUENCER=${SEQUENCER:-EMBARCADERO}
if [[ "$SEQUENCER" == "CORFU" && "$ORDER" != "2" ]]; then
  echo "ERROR: CORFU is restricted to ORDER=2 in this implementation (got ORDER=$ORDER)."
  exit 1
fi
if [ -z "${THREADS_PER_BROKER:-}" ]; then
  if [ "$NUM_BROKERS" = "1" ]; then
    THREADS_PER_BROKER=1
  elif [ "$TOTAL_MESSAGE_SIZE" -le $((1024 * 1024 * 1024)) ]; then
    THREADS_PER_BROKER=3
  else
    THREADS_PER_BROKER=4
  fi
fi
QUIET=${QUIET:-0}
TRIAL_MAX_ATTEMPTS=${TRIAL_MAX_ATTEMPTS:-3}
BROKER_READY_TIMEOUT_SEC=${BROKER_READY_TIMEOUT_SEC:-60}
BROKER_POLL_INTERVAL_SEC=${BROKER_POLL_INTERVAL_SEC:-0.1}
PRESERVE_REMOTE_BROKERS=${PRESERVE_REMOTE_BROKERS:-0}

# --- Environment Setup ---
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
export EMBARCADERO_CXL_ZERO_MODE=${EMBARCADERO_CXL_ZERO_MODE:-full}
export EMBARCADERO_RUNTIME_MODE=${EMBARCADERO_RUNTIME_MODE:-throughput}
export EMBARCADERO_REPLICATION_FACTOR=${EMBARCADERO_REPLICATION_FACTOR:-$REPLICATION_FACTOR}
if [ -z "${EMBARCADERO_CXL_SHM_NAME:-}" ]; then
  export EMBARCADERO_CXL_SHM_NAME="/CXL_SHARED_EXPERIMENT_${UID}"
  shm_unlink "$EMBARCADERO_CXL_SHM_NAME" 2>/dev/null || true
  rm -f "/dev/shm${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
fi

throughput_test_supports_head_addr() {
  strings ./throughput_test 2>/dev/null | grep -q 'head_addr'
}

have_numactl() {
  command -v numactl >/dev/null 2>&1
}

local_numa_node_count() {
  if ! have_numactl; then
    printf '0\n'
    return
  fi
  numactl --hardware 2>/dev/null | awk '/^available:/ {print $2; exit}'
}

detect_numa_node_for_head_addr() {
  local head_addr="$1"
  local route_line iface numa_node

  if [ -z "$head_addr" ] || [ "$head_addr" = "127.0.0.1" ] || [ "$head_addr" = "localhost" ]; then
    return 1
  fi

  route_line=$(ip -o route get "$head_addr" 2>/dev/null | head -1) || return 1
  iface=$(awk '{for (i = 1; i <= NF; i++) if ($i == "dev") {print $(i + 1); exit}}' <<<"$route_line")
  if [ -z "$iface" ] || [ ! -r "/sys/class/net/$iface/device/numa_node" ]; then
    return 1
  fi

  numa_node=$(cat "/sys/class/net/$iface/device/numa_node" 2>/dev/null || true)
  if [[ ! "$numa_node" =~ ^-?[0-9]+$ ]] || [ "$numa_node" -lt 0 ]; then
    return 1
  fi

  printf '%s %s\n' "$numa_node" "$iface"
}

default_client_numa_bind() {
  local head_addr="$1"
  local numa_nodes detection detected_node detected_iface

  if ! have_numactl; then
    printf '%s\n' ""
    return
  fi

  numa_nodes=$(local_numa_node_count)
  if [[ ! "$numa_nodes" =~ ^[0-9]+$ ]] || [ "$numa_nodes" -le 1 ]; then
    printf '%s\n' ""
    return
  fi

  if detection=$(detect_numa_node_for_head_addr "$head_addr"); then
    read -r detected_node detected_iface <<<"$detection"
    if [ -n "$detected_node" ] && [ -n "$detected_iface" ]; then
      echo "Client NUMA auto-bind: head_addr=$head_addr routes via $detected_iface on NUMA node $detected_node" >&2
      printf 'numactl --cpunodebind=%s --membind=%s\n' "$detected_node" "$detected_node"
      return
    fi
  fi

  # Single-node fallback: bind client to NUMA 1, same node as the broker (broker_lifecycle.sh
  # always uses --cpunodebind=1).  Broker creates loopback socket buffers on NUMA 1; if the
  # client is on NUMA 0 its recv() must cross NUMA to read those buffers, doubling avg_send_us
  # (~500 µs vs ~250 µs) and halving single-node subscribe throughput (~3 GB/s vs ~7 GB/s).
  echo "Client NUMA auto-bind: single-node loopback → cpunodebind=1 (co-locate with broker)" >&2
  printf '%s\n' "numactl --cpunodebind=1 --membind=1"
}

resolve_client_head_addr() {
  local head_addr="${EMBARCADERO_HEAD_ADDR:-${REMOTE_HEAD_ADDR:-127.0.0.1}}"
  if broker_is_remote_mode && broker_remote_host_is_local; then
    if [ "$head_addr" != "127.0.0.1" ]; then
      echo "Remote broker host resolves to the local machine; using 127.0.0.1 for client/broker connections to match single-node behavior."
    fi
    head_addr="127.0.0.1"
  fi
  printf '%s\n' "$head_addr"
}

cd build/bin || { echo "Error: build/bin not found"; exit 1; }
broker_init_paths

cleanup() {
  [ "$QUIET" != "1" ] && echo "Cleaning up..."
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  pkill -9 -f "corfu_global_sequencer" >/dev/null 2>&1 || true
  if ! broker_is_remote_mode || [ "$PRESERVE_REMOTE_BROKERS" != "1" ]; then
    broker_cleanup
  fi
  shm_unlink "$EMBARCADERO_CXL_SHM_NAME" 2>/dev/null || true
  rm -f "/dev/shm${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
}


fresh_start_cluster() {
  [ "$QUIET" != "1" ] && echo "Resetting brokers for a fresh trial start..."
  cleanup
}

wait_for_brokers() {
  local timeout=$1
  local expected_count=$2
  broker_wait_for_cluster "$timeout" "$expected_count"
}

inspect_replication_layout() {
  if [ "${REPLICATION_FACTOR:-0}" -le 1 ]; then
    return
  fi
  local -a repl_dirs=()
  if [ -n "${EMBARCADERO_REPLICA_DISK_DIRS:-}" ]; then
    IFS=',' read -r -a repl_dirs <<< "${EMBARCADERO_REPLICA_DISK_DIRS}"
  else
    local repl_root="$PROJECT_ROOT/.Replication"
    if [ ! -d "$repl_root" ]; then
      echo "WARNING: replication root '$repl_root' not found; cannot verify multi-disk placement."
      return
    fi
    mapfile -t repl_dirs < <(find "$repl_root" -maxdepth 1 -type d -name 'disk*' | sort)
  fi

  if [ "${#repl_dirs[@]}" -lt 2 ]; then
    echo "WARNING: fewer than 2 replication directories configured; real multi-disk striping is unlikely."
    return
  fi

  local -A seen_devs=()
  local -A dir_to_dev=()
  local d
  for d in "${repl_dirs[@]}"; do
    local dev
    dev=$(df -P "$d" 2>/dev/null | awk 'NR==2 {print $1}')
    if [ -n "$dev" ]; then
      seen_devs["$dev"]=1
      dir_to_dev["$d"]="$dev"
    fi
  done

  if [ "${#seen_devs[@]}" -lt 2 ]; then
    echo "WARNING: replication directories share one backing device; ACK2 replication throughput is placement-limited."
    for d in "${repl_dirs[@]}"; do
      echo "  $d -> ${dir_to_dev[$d]:-unknown}"
    done
  fi
}

start_cluster() {
  local start_ts
  start_ts=$(date +%s)

  if broker_is_remote_mode; then
    echo "Broker mode: remote host=$REMOTE_BROKER_HOST project_root=$REMOTE_PROJECT_ROOT build_bin=$REMOTE_BUILD_BIN"
    echo "Remote head address for clients: ${EMBARCADERO_HEAD_ADDR:-$REMOTE_HEAD_ADDR}"
    if ! broker_ensure_cluster "$NUM_BROKERS" "$BROKER_READY_TIMEOUT_SEC" "$SEQUENCER"; then
      echo "ERROR: remote broker startup failed on $REMOTE_BROKER_HOST" >&2
      return 1
    fi
    local elapsed
    elapsed=$(( $(date +%s) - start_ts ))
    local ready_count
    ready_count="$(broker_count_ready)"
    echo "Brokers ready on $REMOTE_BROKER_HOST: ${ready_count}/${NUM_BROKERS} after ${elapsed}s"
    return 0
  fi

  if [[ "$SEQUENCER" == "CORFU" ]]; then
    ./corfu_global_sequencer > /tmp/corfu_sequencer.log 2>&1 &
  fi

  $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" --head --$SEQUENCER > broker_0.log 2>&1 &
  for ((i=1; i<NUM_BROKERS; i++)); do
    $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" > broker_"$i".log 2>&1 &
  done

  if ! wait_for_brokers "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS"; then
    echo "ERROR: Brokers failed to start"
    return 1
  fi
  rm -f /tmp/embarlet_*_ready
  return 0
}

overall_status=0
fresh_start_cluster
inspect_replication_layout

CLIENT_HEAD_ADDR="$(resolve_client_head_addr)"
if broker_is_remote_mode && broker_remote_host_is_local; then
  REMOTE_HEAD_ADDR="$CLIENT_HEAD_ADDR"
fi
export EMBARCADERO_HEAD_ADDR="$CLIENT_HEAD_ADDR"
# Remote brokers are started over SSH; REMOTE_BROKER_HOST is often an SSH alias, not the dataplane IP.
# broker_remote_launch passes REMOTE_HEAD_ADDR to embarlet as EMBARCADERO_HEAD_ADDR; default it to the
# same address clients use so publish and ACK paths match publication / multiclient runs.
if broker_is_remote_mode && [ -z "${REMOTE_HEAD_ADDR:-}" ]; then
  export REMOTE_HEAD_ADDR="$CLIENT_HEAD_ADDR"
fi

EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-$(default_client_numa_bind "$CLIENT_HEAD_ADDR")}"
if [[ "$SEQUENCER" == "CORFU" ]]; then
  EMBARLET_NUMA_BIND=""
  CLIENT_NUMA_BIND=""
fi

for ((trial=1; trial<=NUM_TRIALS; trial++)); do
  echo "=== Trial $trial ($SEQUENCER Order $ORDER, $NUM_BROKERS brokers, msg=$MESSAGE_SIZE) ==="
  trial_success=0
  for ((attempt=1; attempt<=TRIAL_MAX_ATTEMPTS; attempt++)); do
    [ "$QUIET" != "1" ] && echo "Trial $trial attempt $attempt/$TRIAL_MAX_ATTEMPTS"
    fresh_start_cluster
    if ! start_cluster; then
      overall_status=1
      cleanup
      continue
    fi

    echo "Running throughput test (type $TEST_TYPE)..."
    TRIAL_LOG="$(mktemp /tmp/throughput_trial_${trial}_${attempt}_XXXX.log)"
    client_cmd=(
      ./throughput_test
      --config "$CLIENT_CONFIG_ABS"
      -n "$THREADS_PER_BROKER"
      -m "$MESSAGE_SIZE"
      -s "$TOTAL_MESSAGE_SIZE"
      -t "$TEST_TYPE"
      -o "$ORDER"
      -a "$ACK"
      --sequencer "$SEQUENCER"
      -l 0
      -r "$REPLICATION_FACTOR"
    )
    if throughput_test_supports_head_addr; then
      client_cmd+=(--head_addr "$CLIENT_HEAD_ADDR")
    else
      echo "WARNING: throughput_test binary does not advertise --head_addr; relying on EMBARCADERO_HEAD_ADDR environment fallback."
    fi
    if broker_is_remote_mode; then
      echo "Benchmark start timestamp: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      $CLIENT_NUMA_BIND "${client_cmd[@]}" 2>&1 | tee "$TRIAL_LOG"
    else
      echo "Benchmark start timestamp: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
      $CLIENT_NUMA_BIND "${client_cmd[@]}" 2>&1 | tee "$TRIAL_LOG"
    fi
    cmd_status=${PIPESTATUS[0]}

    if [[ "$TEST_TYPE" == "2" ]]; then
      if [[ "$cmd_status" -eq 0 ]] &&
         grep -q "End-to-end completed in" "$TRIAL_LOG" &&
         grep -q "Latency accounting:" "$TRIAL_LOG" &&
         ! grep -q -E "Exception during latency test|Subscriber::Poll timeout|Subscriber poll timeout" "$TRIAL_LOG"; then
        trial_success=1
        rm -f "$TRIAL_LOG"
        cleanup
        break
      fi
    else
      if grep -qi "bandwidth:" "$TRIAL_LOG"; then
        trial_success=1
        rm -f "$TRIAL_LOG"
        cleanup
        break
      fi
    fi

    echo "WARNING: Trial $trial attempt $attempt did not produce bandwidth output (exit=$cmd_status)."
    if grep -q "Server returned failure when creating topic" "$TRIAL_LOG"; then
      echo "WARNING: Detected topic-creation race; retrying trial."
    fi
    tail -n 20 "$TRIAL_LOG"
    rm -f "$TRIAL_LOG"
    cleanup
  done

  if [ "$trial_success" -ne 1 ]; then
    echo "ERROR: Trial $trial failed after $TRIAL_MAX_ATTEMPTS attempts."
    overall_status=1
  fi
done

echo "Done."
exit "$overall_status"
