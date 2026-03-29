#!/bin/bash
# run_latency.sh — Embarcadero latency benchmark driver
#
# Supported scenarios:
#   SCENARIO=local  — brokers and client both run on this machine (default)
#   SCENARIO=remote — brokers on this machine; client (publisher) runs on c2 via SSH
#
# Quick examples:
#   ORDERS="0 5" SCENARIO=local  bash scripts/run_latency.sh
#   ORDERS="0 5" SCENARIO=remote REMOTE_CLIENT_HOST=c2 bash scripts/run_latency.sh
#
# NOTE: This script requires recompilation with COLLECT_LATENCY_STATS macro defined.
# Build with:
#   cmake -S . -B build -DCOLLECT_LATENCY_STATS=ON
#   cmake --build build -j
#
# === Latency breakdown: local vs remote (c2→moscxl) ===
#
# Local (single-node, loopback):
#   - avg_send_us ~250 µs/batch (NUMA 1 client, NUMA-local socket buffers)
#   - No NIC serialization latency
#   - Synchronous loopback copy on both send and receive
#
# Remote (c2→moscxl, 100GbE):
#   - Network RTT: ~200–300 ns at wire level
#   - NIC TX + DMA on c2: ~1–5 µs
#   - NIC RX + DMA on moscxl: ~1–5 µs
#   - TCP ACK path (broker → c2): same NIC round-trip
#   - Expected publish latency delta vs local: ~10–50 µs per batch (network overhead)
#     dominated by NIC DMA + PCIe transfer, NOT propagation delay
#   - avg_send_us for remote: ~196 µs/batch (measured) — LOWER than local ~250 µs
#     because NIC DMA is async (broker reads next CXL batch while NIC sends current)
#     whereas loopback requires synchronous CPU copy on both ends
#
# ORDER=0 vs ORDER=5 latency delta:
#   - ORDER=0: no sequencing overhead, direct CXL write
#   - ORDER=5: epoch-based sequencer adds one epoch-interval delay per batch
#     Default epoch interval: kEpochUs = 500 µs  (src/embarlet/topic.h)
#     Override at runtime via EMBARCADERO_EPOCH_US env var (100–5000 µs range)
#   - Expected ORDER=5 tail latency to be higher by ~1× epoch interval (~500 µs)
#     relative to ORDER=0 at the same load

set -euo pipefail

export EMBARCADERO_RUNTIME_MODE="${EMBARCADERO_RUNTIME_MODE:-latency}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
source "$SCRIPT_DIR/lib/broker_lifecycle.sh"

BIN_DIR="$PROJECT_ROOT/build/bin"
DATA_DIR="${DATA_DIR:-$PROJECT_ROOT/data/latency}"

# ---------------------------------------------------------------------------
# Tunable parameters (all overrideable via environment)
# ---------------------------------------------------------------------------
NUM_BROKERS="${NUM_BROKERS:-4}"
TEST_CASE="${TEST_CASE:-2}"
MSG_SIZE="${MSG_SIZE:-1024}"
ACK_LEVEL="${ACK_LEVEL:-1}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-4294967296}"   # 4 GiB
NUM_TRIALS="${NUM_TRIALS:-1}"
RUN_ID="${RUN_ID:-$(date +%Y%m%d_%H%M%S)}"
PLOT_RESULTS="${PLOT_RESULTS:-0}"
TARGET_MBPS="${TARGET_MBPS:-0}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}"
BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-60}"
BROKER_REACHABILITY_TIMEOUT_SEC="${BROKER_REACHABILITY_TIMEOUT_SEC:-20}"
BROKER_REACHABILITY_POLL_SEC="${BROKER_REACHABILITY_POLL_SEC:-1}"
# Give broker heartbeat/control-plane state time to converge after sockets are listening.
BROKER_READY_PROPAGATION_SEC="${BROKER_READY_PROPAGATION_SEC:-4}"
SEQUENCER="${SEQUENCER:-EMBARCADERO}"

# Orders to benchmark: space-separated list, e.g. "0 5"
read -r -a ORDERS_ARR <<< "${ORDERS:-0 5}"

# Modes: steady (--steady_rate) or burst (no flag)
read -r -a modes <<< "${MODES:-steady}"

# Scenario: local or remote
SCENARIO="${SCENARIO:-local}"

if [[ "$SEQUENCER" == "LAZYLOG" ]]; then
  for __order in "${ORDERS_ARR[@]}"; do
    if [[ "$__order" != "2" ]]; then
      echo "ERROR: LAZYLOG baseline requires ORDER=2 (got ORDER=${__order})" >&2
      exit 1
    fi
  done
fi

# Remote client settings (SCENARIO=remote only)
# REMOTE_CLIENT_HOST: SSH destination for the publisher (e.g. "c4" or "user@10.10.10.20")
REMOTE_CLIENT_HOST="${REMOTE_CLIENT_HOST:-c2}"
REMOTE_CLIENT_BIN_DIR="${REMOTE_CLIENT_BIN_DIR:-~/Embarcadero/build/bin}"
# IP address the remote client uses to reach the broker on this machine
BROKER_LISTEN_ADDR="${BROKER_LISTEN_ADDR:-10.10.10.10}"
# Optional separate Corfu sequencer host for Corfu latency runs.
REMOTE_CORFU_SEQUENCER_HOST="${REMOTE_CORFU_SEQUENCER_HOST:-}"
REMOTE_CORFU_BUILD_BIN="${REMOTE_CORFU_BUILD_BIN:-}"
REMOTE_LAZYLOG_SEQUENCER_HOST="${REMOTE_LAZYLOG_SEQUENCER_HOST:-}"
REMOTE_LAZYLOG_BUILD_BIN="${REMOTE_LAZYLOG_BUILD_BIN:-}"
EMBARCADERO_LAZYLOG_SEQ_IP="${EMBARCADERO_LAZYLOG_SEQ_IP:-}"
EMBARCADERO_LAZYLOG_SEQ_PORT="${EMBARCADERO_LAZYLOG_SEQ_PORT:-}"

# NUMA: CPUs on compute node 1; membind includes node 2 when CXL is a **zero-core** NUMA node (see cxl_manager.cc mbind).
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
# CORFU: same default as run_multiclient — do not wrap embarlet in numactl unless you set EMBARLET_NUMA_BIND_CORFU.
if [[ "$SEQUENCER" == "CORFU" ]]; then
  EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND_CORFU-}"
fi

# ---------------------------------------------------------------------------
# Initialise paths (sets BROKER_CONFIG_ABS, CLIENT_CONFIG_ABS, etc.)
# ---------------------------------------------------------------------------
broker_init_paths

# ---------------------------------------------------------------------------
# Client NUMA binding
#   local  → NUMA 1 (co-locate with broker; avoids cross-NUMA socket buffer penalty)
#   remote → empty (numactl runs on the remote machine, not here)
# ---------------------------------------------------------------------------
if [ -z "${CLIENT_NUMA_BIND+x}" ]; then
  if [[ "$SCENARIO" == "remote" ]]; then
    CLIENT_NUMA_BIND=""
  else
    # Single-node: CPUs on node 1 (match brokers). membind=1,2 so policy allows the CXL NUMA node
    # (often node 2, no CPUs) used by head mbind; keeps heap preferring DRAM while shared CXL maps correctly.
    CLIENT_NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"
  fi
fi

# ---------------------------------------------------------------------------
# Cleanup helpers
# ---------------------------------------------------------------------------
cleanup() {
  pkill -9 -f "./embarlet"      >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  if [[ "$SEQUENCER" == "CORFU" && -n "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
    broker_remote_corfu_stop || true
  elif [[ "$SEQUENCER" == "LAZYLOG" && -n "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
    broker_remote_lazylog_stop || true
  fi
  rm -f /tmp/embarlet_*_ready   >/dev/null 2>&1 || true
}

trap cleanup EXIT

# ---------------------------------------------------------------------------
# Readiness hardening: verify broker data ports are reachable from client host(s)
# ---------------------------------------------------------------------------
probe_tcp_from_host() {
  local host="$1"
  local ip="$2"
  local port="$3"
  local probe_cmd="timeout 1 bash -lc '</dev/tcp/$ip/$port' >/dev/null 2>&1"
  if [[ "$host" == "local" ]]; then
    eval "$probe_cmd"
  else
    ssh -o StrictHostKeyChecking=no "$host" "$probe_cmd" >/dev/null 2>&1
  fi
}

wait_for_broker_reachability() {
  local target_ip="$1"
  shift
  local -a hosts=("$@")
  local deadline=$(( $(date +%s) + BROKER_REACHABILITY_TIMEOUT_SEC ))

  while (( $(date +%s) < deadline )); do
    local all_ok=1
    local missing=""
    local host
    local i

    for host in "${hosts[@]}"; do
      for ((i=0; i<NUM_BROKERS; i++)); do
        local port=$((1214 + i))
        if ! probe_tcp_from_host "$host" "$target_ip" "$port"; then
          all_ok=0
          missing+=" ${host}:${target_ip}:${port}"
        fi
      done
    done

    if [[ "$all_ok" -eq 1 ]]; then
      return 0
    fi

    echo "Waiting for client-side broker reachability:${missing}" >&2
    sleep "$BROKER_REACHABILITY_POLL_SEC"
  done

  return 1
}

# ---------------------------------------------------------------------------
# Broker startup (local only — uses ready-file mechanism)
# ---------------------------------------------------------------------------
start_local_brokers() {
  local order="$1"
  local seq="$2"

  cleanup
  export EMBARCADERO_NUM_BROKERS="$NUM_BROKERS"

  if [[ "$seq" == "CORFU" && -n "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
    export REMOTE_CORFU_BUILD_BIN="${REMOTE_CORFU_BUILD_BIN:-$BIN_DIR}"
    echo "Starting remote Corfu sequencer on $REMOTE_CORFU_SEQUENCER_HOST..."
    if ! broker_remote_corfu_start; then
      echo "ERROR: failed to start remote Corfu sequencer on $REMOTE_CORFU_SEQUENCER_HOST" >&2
      return 1
    fi
    sleep 1
  elif [[ "$seq" == "LAZYLOG" && -n "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
    export REMOTE_LAZYLOG_BUILD_BIN="${REMOTE_LAZYLOG_BUILD_BIN:-$BIN_DIR}"
    echo "Starting remote LazyLog sequencer on $REMOTE_LAZYLOG_SEQUENCER_HOST..."
    if ! broker_remote_lazylog_start; then
      echo "ERROR: failed to start remote LazyLog sequencer on $REMOTE_LAZYLOG_SEQUENCER_HOST" >&2
      return 1
    fi
    sleep 1
  fi

  # For remote scenario the broker must bind to the external NIC so the remote client
  # can reach it.  EMBARCADERO_HEAD_ADDR overrides the default 127.0.0.1 listen address.
  local broker_env=""
  broker_env+="REPLICATION_FACTOR=$REPLICATION_FACTOR "
  broker_env+="EMBARCADERO_REPLICATION_FACTOR=$REPLICATION_FACTOR "
  broker_env+="NUM_BROKERS=$NUM_BROKERS "
  broker_env+="EMBARCADERO_NUM_BROKERS=$NUM_BROKERS "
  if [[ "$seq" == "LAZYLOG" ]]; then
    broker_env+="LAZYLOG_CXL_MODE=1 "
  fi
  if [[ "$SCENARIO" == "remote" ]]; then
    broker_env+="EMBARCADERO_HEAD_ADDR=$BROKER_LISTEN_ADDR "
  fi
  if [[ "$seq" == "LAZYLOG" && -n "$EMBARCADERO_LAZYLOG_SEQ_IP" ]]; then
    broker_env+="EMBARCADERO_LAZYLOG_SEQ_IP=$EMBARCADERO_LAZYLOG_SEQ_IP "
  fi
  if [[ "$seq" == "LAZYLOG" && -n "$EMBARCADERO_LAZYLOG_SEQ_PORT" ]]; then
    broker_env+="EMBARCADERO_LAZYLOG_SEQ_PORT=$EMBARCADERO_LAZYLOG_SEQ_PORT "
  fi

  echo "Starting head broker (order=$order sequencer=$seq)..."
  env $broker_env $EMBARLET_NUMA_BIND "$BIN_DIR/embarlet" \
    --config "$BROKER_CONFIG_ABS" \
    --head \
    --"$seq" \
    > "$BIN_DIR/broker_0.log" 2>&1 &

  for ((i=1; i<NUM_BROKERS; i++)); do
    echo "Starting broker $i..."
    env $broker_env $EMBARLET_NUMA_BIND "$BIN_DIR/embarlet" \
      --config "$BROKER_CONFIG_ABS" \
      > "$BIN_DIR/broker_${i}.log" 2>&1 &
  done

  if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS"; then
    echo "ERROR: Brokers failed to reach ready state within ${BROKER_READY_TIMEOUT_SEC}s" >&2
    return 1
  fi
  rm -f /tmp/embarlet_*_ready

  local reachability_ip
  local -a reachability_hosts
  if [[ "$SCENARIO" == "remote" ]]; then
    reachability_ip="$BROKER_LISTEN_ADDR"
    reachability_hosts=("$REMOTE_CLIENT_HOST")
  else
    reachability_ip="${EMBARCADERO_HEAD_ADDR:-127.0.0.1}"
    reachability_hosts=("local")
  fi

  if ! wait_for_broker_reachability "$reachability_ip" "${reachability_hosts[@]}"; then
    echo "ERROR: Brokers are not reachable from client host(s) within ${BROKER_REACHABILITY_TIMEOUT_SEC}s" >&2
    return 1
  fi

  if [[ "$BROKER_READY_PROPAGATION_SEC" -gt 0 ]]; then
    echo "Waiting ${BROKER_READY_PROPAGATION_SEC}s for cluster state propagation..."
    sleep "$BROKER_READY_PROPAGATION_SEC"
  fi
  echo "All $NUM_BROKERS brokers ready and reachable."
}

# ---------------------------------------------------------------------------
# Client command builder
# ---------------------------------------------------------------------------
build_client_cmd() {
  local head_addr="$1"
  local order="$2"
  local seq="$3"
  local mode="$4"

  local cmd=(
    ./throughput_test
    --config "$CLIENT_CONFIG_ABS"
    --head_addr "$head_addr"
    -m "$MSG_SIZE"
    -s "$TOTAL_MESSAGE_SIZE"
    -t "$TEST_CASE"
    -o "$order"
    -a "$ACK_LEVEL"
    --sequencer "$seq"
    -r "$REPLICATION_FACTOR"
    --target_mbps "$TARGET_MBPS"
  )
  if [[ "$mode" == "steady" ]]; then
    cmd+=(--steady_rate)
  fi

  printf '%s\n' "${cmd[@]}"
}

# ---------------------------------------------------------------------------
# Run a single trial
# ---------------------------------------------------------------------------
run_trial() {
  local mode="$1"
  local order="$2"
  local seq="$3"
  local trial="$4"
  local TRIAL_DIR
  TRIAL_DIR="$DATA_DIR/$mode/$RUN_ID/${seq}_order${order}_ack${ACK_LEVEL}_msg${MSG_SIZE}_bytes${TOTAL_MESSAGE_SIZE}_trial${trial}"
  mkdir -p "$TRIAL_DIR/brokers"

  local RUN_LOG="$TRIAL_DIR/run.log"
  local CLIENT_CMD_FILE="$TRIAL_DIR/client_command.txt"
  local RUN_METADATA="$TRIAL_DIR/run_metadata.txt"

  exec 3>&1 4>&2
  trap 'exec 1>&3 2>&4; exec 3>&- 4>&-; trap - RETURN' RETURN
  exec > >(tee "$RUN_LOG") 2>&1

  echo "[Trial $trial/$NUM_TRIALS] scenario=$SCENARIO mode=$mode sequencer=$seq order=$order"

  # Clean up any leftover CSV files from a previous run
  (cd "$BIN_DIR" && rm -f cdf_latency_us.csv latency_stats.csv pub_cdf_latency_us.csv pub_latency_stats.csv)
  if [[ "$SCENARIO" == "remote" ]]; then
    ssh -o StrictHostKeyChecking=no "$REMOTE_CLIENT_HOST" \
      "cd ${REMOTE_CLIENT_BIN_DIR} && rm -f cdf_latency_us.csv latency_stats.csv pub_cdf_latency_us.csv pub_latency_stats.csv" 2>/dev/null || true
  fi

  # Start brokers (local; for remote-client scenario the broker still lives here)
  if ! start_local_brokers "$order" "$seq"; then
    echo "ERROR: broker startup failed" >&2
    return 1
  fi

  # Determine head address the client will connect to
  local head_addr
  if [[ "$SCENARIO" == "remote" ]]; then
    head_addr="$BROKER_LISTEN_ADDR"
  else
    head_addr="${EMBARCADERO_HEAD_ADDR:-127.0.0.1}"
  fi

  # Build client command array (one arg per line for easy SSH quoting)
  local -a raw_cmd
  mapfile -t raw_cmd < <(build_client_cmd "$head_addr" "$order" "$seq" "$mode")
  printf '%q ' "${raw_cmd[@]}" > "$CLIENT_CMD_FILE"
  printf '\n' >> "$CLIENT_CMD_FILE"

  if [[ "$SCENARIO" == "remote" ]]; then
    # Run the publisher on the remote client machine (c4).
    # The remote bin dir must have a throughput_test binary and the client config.
    echo "Launching remote client on $REMOTE_CLIENT_HOST (publisher → $head_addr)..."
    # Reconstruct the command as a single shell-quoted string for SSH.
    local quoted_cmd
    quoted_cmd="cd ${REMOTE_CLIENT_BIN_DIR} && "
    quoted_cmd+="export EMBARCADERO_RUNTIME_MODE=${EMBARCADERO_RUNTIME_MODE} && "
    quoted_cmd+="export NUM_BROKERS=${NUM_BROKERS} && "
    quoted_cmd+="export EMBARCADERO_NUM_BROKERS=${NUM_BROKERS} && "
    if [[ -n "${EMBARCADERO_CORFU_SEQ_IP:-}" ]]; then
      quoted_cmd+="export EMBARCADERO_CORFU_SEQ_IP=${EMBARCADERO_CORFU_SEQ_IP} && "
    fi
    if [[ -n "${EMBARCADERO_CORFU_SEQ_PORT:-}" ]]; then
      quoted_cmd+="export EMBARCADERO_CORFU_SEQ_PORT=${EMBARCADERO_CORFU_SEQ_PORT} && "
    fi
    if [[ -n "${EMBARCADERO_LAZYLOG_SEQ_IP:-}" ]]; then
      quoted_cmd+="export EMBARCADERO_LAZYLOG_SEQ_IP=${EMBARCADERO_LAZYLOG_SEQ_IP} && "
    fi
    if [[ -n "${EMBARCADERO_LAZYLOG_SEQ_PORT:-}" ]]; then
      quoted_cmd+="export EMBARCADERO_LAZYLOG_SEQ_PORT=${EMBARCADERO_LAZYLOG_SEQ_PORT} && "
    fi
    if [[ -n "$CLIENT_NUMA_BIND" ]]; then
      quoted_cmd+="$CLIENT_NUMA_BIND "
    fi
    quoted_cmd+="${raw_cmd[*]}"

    # Run remotely; pipe output back so it appears in local logs.
    ssh -o StrictHostKeyChecking=no "$REMOTE_CLIENT_HOST" bash -c "'$quoted_cmd'" 2>&1
    local client_status=${PIPESTATUS[0]}
  else
    # Run locally
    (
      cd "$BIN_DIR"
      $CLIENT_NUMA_BIND "${raw_cmd[@]}" 2>&1
    )
    local client_status=$?
  fi

  if [[ "$client_status" -ne 0 ]]; then
    echo "WARNING: throughput_test exited with status $client_status" >&2
  fi

  if [[ "$SCENARIO" == "remote" ]]; then
    # CSV files were written on the remote client; scp them back.
    for f in cdf_latency_us.csv latency_stats.csv pub_cdf_latency_us.csv pub_latency_stats.csv; do
      scp -o StrictHostKeyChecking=no \
          "${REMOTE_CLIENT_HOST}:${REMOTE_CLIENT_BIN_DIR}/$f" \
          "$TRIAL_DIR/$f" 2>/dev/null || true
    done
  else
    for f in cdf_latency_us.csv latency_stats.csv pub_cdf_latency_us.csv pub_latency_stats.csv; do
      local src="$BIN_DIR/$f"
      if [[ -f "$src" ]]; then
        mv "$src" "$TRIAL_DIR/$f"
      fi
    done
  fi

  for broker_log in "$BIN_DIR"/broker_*.log; do
    if [[ -f "$broker_log" ]]; then
      cp "$broker_log" "$TRIAL_DIR/brokers/"
    fi
  done

  cat > "$RUN_METADATA" <<EOF
run_id=$RUN_ID
scenario=$SCENARIO
mode=$mode
trial=$trial
sequencer=$seq
order=$order
ack_level=$ACK_LEVEL
message_size=$MSG_SIZE
total_message_size=$TOTAL_MESSAGE_SIZE
num_brokers=$NUM_BROKERS
test_case=$TEST_CASE
replication_factor=$REPLICATION_FACTOR
runtime_mode=$EMBARCADERO_RUNTIME_MODE
target_mbps=$TARGET_MBPS
broker_head_addr=$head_addr
run_log=$RUN_LOG
client_command_file=$CLIENT_CMD_FILE
EOF

  echo "Saved trial artefacts to: $TRIAL_DIR"
  return 0
}

# ---------------------------------------------------------------------------
# Main loop
# ---------------------------------------------------------------------------
mkdir -p "$DATA_DIR"

echo "========================================================"
echo "Embarcadero latency benchmark"
echo "  RUN_ID   : $RUN_ID"
echo "  SCENARIO : $SCENARIO"
echo "  ORDERS   : ${ORDERS_ARR[*]}"
echo "  MODES    : ${modes[*]}"
echo "  SEQUENCER: $SEQUENCER"
echo "  MSG_SIZE : $MSG_SIZE"
echo "  ACK_LEVEL: $ACK_LEVEL"
echo "  REPL_FACT: $REPLICATION_FACTOR"
echo "  NUM_TRIAL: $NUM_TRIALS"
echo "========================================================"

for mode in "${modes[@]}"; do
  for order in "${ORDERS_ARR[@]}"; do
    echo "--------------------------------------------------------"
    echo "mode=$mode  order=$order  sequencer=$SEQUENCER"
    echo "--------------------------------------------------------"
    for trial in $(seq 1 "$NUM_TRIALS"); do
      if ! run_trial "$mode" "$order" "$SEQUENCER" "$trial"; then
        echo "ERROR: trial $trial failed for mode=$mode order=$order" >&2
        cleanup
        exit 1
      fi
      cleanup
    done
  done
done

if [[ "$PLOT_RESULTS" == "1" ]]; then
  pushd "$DATA_DIR" >/dev/null
  python3 plot_latency.py latency
  popd >/dev/null
fi

echo "All latency experiments finished. Results under: $DATA_DIR"
