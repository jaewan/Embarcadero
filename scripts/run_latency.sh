#!/bin/bash
# run_latency.sh — Embarcadero latency benchmark driver
#
# Supported scenarios:
#   SCENARIO=local  — brokers and client both run on this machine (default)
#   SCENARIO=remote — brokers on this machine; client (publisher) runs on c4 via SSH
#
# Quick examples:
#   ORDERS="0 5" SCENARIO=local  bash scripts/run_latency.sh
#   ORDERS="0 5" SCENARIO=remote REMOTE_CLIENT_HOST=c4 bash scripts/run_latency.sh
#
# NOTE: This script requires recompilation with COLLECT_LATENCY_STATS macro defined.
# Build with:
#   cmake -S . -B build -DCOLLECT_LATENCY_STATS=ON
#   cmake --build build -j
#
# === Latency breakdown: local vs remote (c4→moscxl) ===
#
# Local (single-node, loopback):
#   - avg_send_us ~250 µs/batch (NUMA 1 client, NUMA-local socket buffers)
#   - No NIC serialization latency
#   - Synchronous loopback copy on both send and receive
#
# Remote (c4→moscxl, 100GbE):
#   - Network RTT: ~200–300 ns at wire level
#   - NIC TX + DMA on c4: ~1–5 µs
#   - NIC RX + DMA on moscxl: ~1–5 µs
#   - TCP ACK path (broker → c4): same NIC round-trip
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
SEQUENCER="${SEQUENCER:-EMBARCADERO}"

# Orders to benchmark: space-separated list, e.g. "0 5"
read -r -a ORDERS_ARR <<< "${ORDERS:-0 5}"

# Modes: steady (--steady_rate) or burst (no flag)
read -r -a modes <<< "${MODES:-steady}"

# Scenario: local or remote
SCENARIO="${SCENARIO:-local}"

# Remote client settings (SCENARIO=remote only)
# REMOTE_CLIENT_HOST: SSH destination for the publisher (e.g. "c4" or "user@10.10.10.20")
REMOTE_CLIENT_HOST="${REMOTE_CLIENT_HOST:-c4}"
REMOTE_CLIENT_BIN_DIR="${REMOTE_CLIENT_BIN_DIR:-~/Embarcadero/build/bin}"
# IP address the remote client uses to reach the broker on this machine
BROKER_LISTEN_ADDR="${BROKER_LISTEN_ADDR:-10.10.10.10}"

# NUMA binding for broker processes (always NUMA 1 for CXL locality)
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"

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
    # Single-node loopback: bind to NUMA 1 to match broker socket buffer locality.
    # Binding to NUMA 0 doubles avg_send_us (~500 µs vs ~250 µs) and halves subscribe
    # throughput because the kernel loopback copy crosses NUMA.
    CLIENT_NUMA_BIND="numactl --cpunodebind=1 --membind=1"
  fi
fi

# ---------------------------------------------------------------------------
# Cleanup helpers
# ---------------------------------------------------------------------------
cleanup() {
  pkill -9 -f "./embarlet"      >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  rm -f /tmp/embarlet_*_ready   >/dev/null 2>&1 || true
}

trap cleanup EXIT

# ---------------------------------------------------------------------------
# Broker startup (local only — uses ready-file mechanism)
# ---------------------------------------------------------------------------
start_local_brokers() {
  local order="$1"
  local seq="$2"

  cleanup

  echo "Starting head broker (order=$order sequencer=$seq)..."
  $EMBARLET_NUMA_BIND "$BIN_DIR/embarlet" \
    --config "$BROKER_CONFIG_ABS" \
    --head \
    --"$seq" \
    > "$BIN_DIR/broker_0.log" 2>&1 &

  for ((i=1; i<NUM_BROKERS; i++)); do
    echo "Starting broker $i..."
    $EMBARLET_NUMA_BIND "$BIN_DIR/embarlet" \
      --config "$BROKER_CONFIG_ABS" \
      > "$BIN_DIR/broker_${i}.log" 2>&1 &
  done

  if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS"; then
    echo "ERROR: Brokers failed to reach ready state within ${BROKER_READY_TIMEOUT_SEC}s" >&2
    return 1
  fi
  rm -f /tmp/embarlet_*_ready
  echo "All $NUM_BROKERS brokers ready."
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
    --record_results
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

  echo "[Trial $trial/$NUM_TRIALS] scenario=$SCENARIO mode=$mode sequencer=$seq order=$order"

  # Clean up any leftover CSV files from a previous run
  (cd "$BIN_DIR" && rm -f cdf_latency_us.csv latency_stats.csv pub_cdf_latency_us.csv pub_latency_stats.csv)

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

  if [[ "$SCENARIO" == "remote" ]]; then
    # Run the publisher on the remote client machine (c4).
    # The remote bin dir must have a throughput_test binary and the client config.
    echo "Launching remote client on $REMOTE_CLIENT_HOST (publisher → $head_addr)..."
    # Reconstruct the command as a single shell-quoted string for SSH.
    local quoted_cmd
    quoted_cmd="cd ${REMOTE_CLIENT_BIN_DIR} && "
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

  # Collect artefacts
  local TRIAL_DIR
  TRIAL_DIR="$DATA_DIR/$mode/$RUN_ID/${seq}_order${order}_ack${ACK_LEVEL}_msg${MSG_SIZE}_bytes${TOTAL_MESSAGE_SIZE}_trial${trial}"
  mkdir -p "$TRIAL_DIR"

  for f in cdf_latency_us.csv latency_stats.csv pub_cdf_latency_us.csv pub_latency_stats.csv; do
    local src="$BIN_DIR/$f"
    if [[ -f "$src" ]]; then
      mv "$src" "$TRIAL_DIR/$f"
    fi
  done

  cat > "$TRIAL_DIR/run_metadata.txt" <<EOF
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
