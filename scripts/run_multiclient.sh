#!/bin/bash
# scripts/run_multiclient.sh
#
# Multi-client throughput orchestration for Embarcadero.
# Starts brokers locally, then launches N physical clients in parallel with a
# synchronized (future-timestamp) barrier start.
#
# Client roster (order defines who is added at each NUM_CLIENTS level):
#   NUM_CLIENTS=1  → c4              (NUMA 1)
#   NUM_CLIENTS=2  → c4, c3          (NUMA 1, 1)
#   NUM_CLIENTS=3  → c4, c3, local   (NUMA 1, 1, 0)
#   NUM_CLIENTS=4  → c4, local, c3, c2
#   NUM_CLIENTS=5  → c4, local, c3, c2, c1
#
# All configuration via environment variables:
#   NUM_CLIENTS, NUM_BROKERS, NUM_TRIALS, TRIAL_MAX_ATTEMPTS
#   TOTAL_MESSAGE_SIZE, MESSAGE_SIZE, THREADS_PER_BROKER
#   TEST_TYPE, ORDER, ACK, REPLICATION_FACTOR, SEQUENCER
#   EMBARCADERO_HEAD_ADDR (broker IP, default 10.10.10.10)
#   EMBARCADERO_ORDER0_FAST_PATH, EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES,
#   EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE, EMBARCADERO_BATCH_SIZE,
#   EMBARCADERO_CLIENT_PUB_BATCH_KB, EMBARCADERO_NETWORK_IO_THREADS,
#   EMBARCADERO_ORDER5_HOME_BROKERS
#
# Example:
#   NUM_CLIENTS=3 NUM_BROKERS=4 MESSAGE_SIZE=8192 scripts/run_multiclient.sh

set -euo pipefail

# ---------------------------------------------------------------------------
# Cluster topology — order determines activation sequence
# "local" means this broker machine (where brokers run); everything else is SSH
# ---------------------------------------------------------------------------
declare -a CLIENT_HOSTS=( "c4"  "c3"  "local" "c2"  "c1"  )
declare -a CLIENT_NUMAS=( "1"   "1"   "0"     "1"   "1"   )
MAX_CLIENTS=${#CLIENT_HOSTS[@]}

# ---------------------------------------------------------------------------
# Configuration (all overrideable via environment)
# ---------------------------------------------------------------------------
NUM_CLIENTS=${NUM_CLIENTS:-1}
NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-3}
TRIAL_MAX_ATTEMPTS=${TRIAL_MAX_ATTEMPTS:-3}

# Total bytes across ALL clients combined; divided equally per client
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-8589934592}   # 8 GiB default
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-4}

TEST_TYPE=${TEST_TYPE:-5}          # 5 = publish-only
ORDER=${ORDER:-0}
ACK=${ACK:-1}
REPLICATION_FACTOR=${REPLICATION_FACTOR:-0}
SEQUENCER=${SEQUENCER:-EMBARCADERO}

BROKER_READY_TIMEOUT_SEC=${BROKER_READY_TIMEOUT_SEC:-60}
# Extra lead time given to SSH connections and clock-sync settling
START_DELAY_SEC=${START_DELAY_SEC:-8}
QUIET=${QUIET:-0}

# Performance knobs (set to empty string to disable)
EMBARCADERO_ORDER0_FAST_PATH=${EMBARCADERO_ORDER0_FAST_PATH:-1}
EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES=${EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES:-524288}
EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE=${EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE:-1}
EMBARCADERO_BATCH_SIZE=${EMBARCADERO_BATCH_SIZE:-524288}
EMBARCADERO_CLIENT_PUB_BATCH_KB=${EMBARCADERO_CLIENT_PUB_BATCH_KB:-512}
EMBARCADERO_NETWORK_IO_THREADS=${EMBARCADERO_NETWORK_IO_THREADS:-4}
EMBARCADERO_ORDER5_HOME_BROKERS=${EMBARCADERO_ORDER5_HOME_BROKERS:-}
CLIENT_LD_LIBRARY_PATH=${LD_LIBRARY_PATH:-}

# ---------------------------------------------------------------------------
# Derived paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_BIN="$PROJECT_ROOT/build/bin"
BROKER_CONFIG="${BROKER_CONFIG:-config/embarcadero.yaml}"
CLIENT_CONFIG="${CLIENT_CONFIG:-config/client.yaml}"
BROKER_CONFIG_ABS="$PROJECT_ROOT/$BROKER_CONFIG"
CLIENT_CONFIG_ABS="$PROJECT_ROOT/$CLIENT_CONFIG"
LOG_DIR="$PROJECT_ROOT/multiclient_logs"

BROKER_IP="${EMBARCADERO_HEAD_ADDR:-10.10.10.10}"
export EMBARCADERO_CXL_SHM_NAME="${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}"

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
if ! [[ "$NUM_CLIENTS" =~ ^[1-9][0-9]*$ ]] || [ "$NUM_CLIENTS" -gt "$MAX_CLIENTS" ]; then
    echo "ERROR: NUM_CLIENTS must be 1–${MAX_CLIENTS}, got '${NUM_CLIENTS}'" >&2
    echo "Usage: NUM_CLIENTS=<1-${MAX_CLIENTS}> $0" >&2
    exit 1
fi

if [[ "$SEQUENCER" == "CORFU" ]] && [[ "$ORDER" != "2" ]]; then
    echo "ERROR: CORFU sequencer requires ORDER=2 (got ORDER=$ORDER)" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Source lifecycle helpers (provides broker_local_wait_for_cluster, etc.)
# broker_is_remote_mode() tests for REMOTE_BROKER_HOST; unset it so we stay local
# ---------------------------------------------------------------------------
unset REMOTE_BROKER_HOST
export PROJECT_ROOT
source "$SCRIPT_DIR/lib/broker_lifecycle.sh"
broker_init_paths

EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"
[[ "$SEQUENCER" == "CORFU" ]] && EMBARLET_NUMA_BIND=""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log() { [ "$QUIET" != "1" ] && echo "$*"; }

shm_cleanup() {
    shm_unlink "${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
    rm -f "/dev/shm${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
}

cleanup() {
    log "Cleaning up..."
    broker_local_cleanup
    shm_cleanup
    # Kill remote client processes (ignore failures)
    for (( _i=0; _i<${NUM_CLIENTS:-0}; _i++ )); do
        _h="${CLIENT_HOSTS[$_i]}"
        if [[ "$_h" != "local" ]]; then
            ssh "$_h" "pkill -9 -f throughput_test 2>/dev/null; true" 2>/dev/null || true
        fi
    done
}
trap cleanup EXIT

start_brokers() {
    log "Resetting previous broker state..."
    pkill -9 -f "./embarlet" >/dev/null 2>&1 || true
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    shm_cleanup

    cd "$BUILD_BIN"

    export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}"
    export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-full}"
    export EMBARCADERO_RUNTIME_MODE="throughput"
    export EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"
    export EMBARCADERO_HEAD_ADDR="$BROKER_IP"
    export EMBARCADERO_ORDER0_FAST_PATH
    export EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES

    log "Starting $NUM_BROKERS broker(s) with NUMA bind: '${EMBARLET_NUMA_BIND}'"
    if [[ "$SEQUENCER" == "CORFU" ]]; then
        ./corfu_global_sequencer > /tmp/corfu_sequencer.log 2>&1 &
    fi

    # shellcheck disable=SC2086
    $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" --head "--${SEQUENCER}" \
        > /tmp/broker_0.log 2>&1 &
    for (( i=1; i<NUM_BROKERS; i++ )); do
        # shellcheck disable=SC2086
        $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" \
            > /tmp/broker_"$i".log 2>&1 &
    done

    log "Waiting for $NUM_BROKERS broker(s) to become ready (timeout: ${BROKER_READY_TIMEOUT_SEC}s)..."
    if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS"; then
        echo "ERROR: Brokers did not become ready within ${BROKER_READY_TIMEOUT_SEC}s" >&2
        cat /tmp/broker_0.log >&2
        return 1
    fi
    # Clear ready sentinels so the next trial's wait starts from a clean state
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    log "All $NUM_BROKERS brokers ready."
}

# ---------------------------------------------------------------------------
# Derived run-time values
# ---------------------------------------------------------------------------
LOAD_PER_CLIENT=$(( TOTAL_MESSAGE_SIZE / NUM_CLIENTS ))

echo "================================================================"
echo "  Embarcadero Multi-Client Throughput Benchmark"
echo "================================================================"
printf "  %-32s %s\n" "NUM_CLIENTS:"                   "$NUM_CLIENTS  (${CLIENT_HOSTS[*]:0:$NUM_CLIENTS})"
printf "  %-32s %s\n" "NUM_BROKERS:"                   "$NUM_BROKERS"
printf "  %-32s %s\n" "NUM_TRIALS:"                    "$NUM_TRIALS"
printf "  %-32s %s\n" "SEQUENCER / ORDER:"             "$SEQUENCER / $ORDER"
printf "  %-32s %s\n" "ACK / REPLICATION_FACTOR:"      "$ACK / $REPLICATION_FACTOR"
printf "  %-32s %s\n" "MESSAGE_SIZE:"                  "$MESSAGE_SIZE B"
printf "  %-32s %s\n" "TOTAL_MESSAGE_SIZE:"            "$TOTAL_MESSAGE_SIZE B  ($(( TOTAL_MESSAGE_SIZE / 1024 / 1024 )) MiB)"
printf "  %-32s %s\n" "LOAD_PER_CLIENT:"               "$LOAD_PER_CLIENT B  ($(( LOAD_PER_CLIENT / 1024 / 1024 )) MiB)"
printf "  %-32s %s\n" "THREADS_PER_BROKER:"            "$THREADS_PER_BROKER"
printf "  %-32s %s\n" "BROKER_IP:"                     "$BROKER_IP"
printf "  %-32s %s\n" "START_DELAY_SEC:"               "$START_DELAY_SEC"
printf "  %-32s %s\n" "ORDER0_FAST_PATH:"              "$EMBARCADERO_ORDER0_FAST_PATH"
printf "  %-32s %s\n" "PAYLOAD_SEND_CHUNK_BYTES:"      "$EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES"
printf "  %-32s %s\n" "ENABLE_PAYLOAD_MSG_MORE:"       "$EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE"
printf "  %-32s %s\n" "BATCH_SIZE:"                    "$EMBARCADERO_BATCH_SIZE"
printf "  %-32s %s\n" "CLIENT_PUB_BATCH_KB:"           "$EMBARCADERO_CLIENT_PUB_BATCH_KB"
printf "  %-32s %s\n" "ORDER5_HOME_BROKERS:"           "${EMBARCADERO_ORDER5_HOME_BROKERS:-"(unset)"}"
echo "================================================================"

mkdir -p "$LOG_DIR"
overall_status=0

# ---------------------------------------------------------------------------
# Trial loop
# ---------------------------------------------------------------------------
for (( trial=1; trial<=NUM_TRIALS; trial++ )); do
    echo ""
    echo "=== Trial $trial / $NUM_TRIALS  ($SEQUENCER  Order=$ORDER  Brokers=$NUM_BROKERS  Clients=$NUM_CLIENTS  msg=${MESSAGE_SIZE}B) ==="
    trial_success=0

    for (( attempt=1; attempt<=TRIAL_MAX_ATTEMPTS; attempt++ )); do
        log "  Attempt $attempt / $TRIAL_MAX_ATTEMPTS"

        if ! start_brokers; then
            echo "ERROR: broker startup failed on trial $trial attempt $attempt" >&2
            cleanup
            continue
        fi

        # ------------------------------------------------------------------
        # Compute synchronized barrier timestamp (NTP-synced wall clock)
        # All clients spin-wait until this exact millisecond.
        # ------------------------------------------------------------------
        START_TIME_MS=$(( $(date +%s%3N) + START_DELAY_SEC * 1000 ))
        log "  Barrier start time: ${START_TIME_MS} ms  (T+${START_DELAY_SEC}s)"

        # ------------------------------------------------------------------
        # Launch all clients in parallel
        # ------------------------------------------------------------------
        declare -a CLIENT_PIDS=()
        declare -a CLIENT_LOGS=()

        for (( i=0; i<NUM_CLIENTS; i++ )); do
            host="${CLIENT_HOSTS[$i]}"
            numa="${CLIENT_NUMAS[$i]}"
            log_file="$LOG_DIR/trial${trial}_${host}.log"
            CLIENT_LOGS+=( "$log_file" )

            # Build the command that will execute on the remote (or local) shell.
            # We use export statements so every env var is properly set regardless
            # of the remote shell's inherited environment.
            EXEC_CMD="$(cat <<ENDINNERSCRIPT
set -e
export EMBARCADERO_HEAD_ADDR=$BROKER_IP
export EMBARCADERO_CXL_SHM_NAME=$EMBARCADERO_CXL_SHM_NAME
export EMBARCADERO_CXL_ZERO_MODE=${EMBARCADERO_CXL_ZERO_MODE:-full}
export EMBARCADERO_RUNTIME_MODE=throughput
export EMBARCADERO_REPLICATION_FACTOR=$REPLICATION_FACTOR
export EMBARCADERO_ORDER0_FAST_PATH=$EMBARCADERO_ORDER0_FAST_PATH
export EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES=$EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES
export EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE=$EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE
export EMBARCADERO_BATCH_SIZE=$EMBARCADERO_BATCH_SIZE
export EMBARCADERO_CLIENT_PUB_BATCH_KB=$EMBARCADERO_CLIENT_PUB_BATCH_KB
export EMBARCADERO_NETWORK_IO_THREADS=$EMBARCADERO_NETWORK_IO_THREADS
export EMBARCADERO_ORDER5_HOME_BROKERS=$EMBARCADERO_ORDER5_HOME_BROKERS
export LD_LIBRARY_PATH=$CLIENT_LD_LIBRARY_PATH
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
cd $BUILD_BIN
# Spin-wait until the synchronized barrier millisecond (requires NTP-synced clocks)
while [ \$(date +%s%3N) -lt $START_TIME_MS ]; do sleep 0.0005; done
numactl --cpunodebind=$numa --membind=$numa \\
    ./throughput_test \\
    --config $CLIENT_CONFIG_ABS \\
    -n $THREADS_PER_BROKER \\
    -m $MESSAGE_SIZE \\
    -s $LOAD_PER_CLIENT \\
    -t $TEST_TYPE \\
    -o $ORDER \\
    -a $ACK \\
    -r $REPLICATION_FACTOR \\
    --sequencer $SEQUENCER \\
    --head_addr $BROKER_IP \\
    -l 0
ENDINNERSCRIPT
)"

            log "  Launching client[$i] host=$host  NUMA=$numa  log=$log_file"
            if [[ "$host" == "local" ]]; then
                bash -c "$EXEC_CMD" > "$log_file" 2>&1 &
            else
                ssh "$host" "$EXEC_CMD" > "$log_file" 2>&1 &
            fi
            CLIENT_PIDS+=( $! )
        done

        # ------------------------------------------------------------------
        # Wait for all clients to finish
        # ------------------------------------------------------------------
        log "  Waiting for ${#CLIENT_PIDS[@]} client(s)..."
        all_ok=1
        for pid in "${CLIENT_PIDS[@]}"; do
            wait "$pid" || all_ok=0
        done

        # ------------------------------------------------------------------
        # Validate: every log must contain a "Bandwidth:" line
        # ------------------------------------------------------------------
        logs_ok=1
        for log_file in "${CLIENT_LOGS[@]}"; do
            if ! grep -qi "bandwidth:" "$log_file" 2>/dev/null; then
                echo "WARNING: no Bandwidth line in $log_file" >&2
                tail -n 15 "$log_file" >&2
                logs_ok=0
            fi
        done

        if [[ "$all_ok" -eq 1 && "$logs_ok" -eq 1 ]]; then
            trial_success=1
            break
        fi

        echo "WARNING: trial $trial attempt $attempt incomplete — retrying..." >&2
        # Kill any surviving clients before re-attempting
        for pid in "${CLIENT_PIDS[@]}"; do
            kill "$pid" 2>/dev/null || true
        done
        cleanup
        # brief pause so the broker port is released
        sleep 2
    done

    # ------------------------------------------------------------------
    # Per-trial summary
    # ------------------------------------------------------------------
    echo ""
    echo "--- Trial $trial Results ---"
    total_bw_mbs=0
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        log_file="$LOG_DIR/trial${trial}_${host}.log"
        bw_line=$(grep -i "bandwidth:" "$log_file" 2>/dev/null | tail -1 || echo "")
        # glog prefix: "I0326 HH:MM:SS PID file:line] Bandwidth: VALUE UNIT"
        # extract the number that follows "Bandwidth:" regardless of prefix fields
        bw_val=$(echo "$bw_line" | grep -oiP 'bandwidth:\s*\K[0-9]+(\.[0-9]+)?' || true)
        bw_unit=$(echo "$bw_line" | awk '{for(i=1;i<NF;i++) if($i=="Bandwidth:") {print $(i+2); exit}}' || true)
        printf "  %-8s → %s %s\n" "$host" "${bw_val:-N/A}" "${bw_unit:-}"
        if [[ "$bw_val" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            total_bw_mbs=$(awk "BEGIN {printf \"%.2f\", $total_bw_mbs + $bw_val}")
        fi
    done
    total_gbs=$(awk "BEGIN {printf \"%.3f\", $total_bw_mbs / 1024}")
    echo "  ────────────────────────────────────────"
    printf "  %-8s → %s MB/s  (%s GB/s)\n" "TOTAL" "$total_bw_mbs" "$total_gbs"
    echo "  (naive sum of per-client averages; see OSDI/SOSP note below)"

    if [[ "$trial_success" -ne 1 ]]; then
        echo "ERROR: Trial $trial failed after $TRIAL_MAX_ATTEMPTS attempts." >&2
        overall_status=1
    fi

    cleanup
done

# ---------------------------------------------------------------------------
# Grand summary across all trials
# ---------------------------------------------------------------------------
echo ""
echo "================================================================"
echo "  Grand Summary  (naive aggregate MB/s per trial)"
echo "================================================================"
for (( trial=1; trial<=NUM_TRIALS; trial++ )); do
    total=0
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        log_file="$LOG_DIR/trial${trial}_${host}.log"
        bw=$(grep -i "bandwidth:" "$log_file" 2>/dev/null | tail -1 | grep -oiP 'bandwidth:\s*\K[0-9]+(\.[0-9]+)?' || true)
        if [[ "$bw" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            total=$(awk "BEGIN {printf \"%.2f\", $total + $bw}")
        fi
    done
    gbs=$(awk "BEGIN {printf \"%.3f\", $total / 1024}")
    printf "  Trial %-3d:  %9s MB/s   (%s GB/s)\n" "$trial" "$total" "$gbs"
done
echo "================================================================"
echo ""
echo "  Logs: $LOG_DIR/"
echo ""
echo "  RIGOROUS AGGREGATION NOTE (OSDI/SOSP method):"
echo "  The numbers above are naive sums of each client's average.  For"
echo "  peer-reviewable results, determine the overlapping measurement"
echo "  window from time-series CSVs (one per client) and divide total"
echo "  bytes sent inside that window by its duration."
echo "================================================================"

exit "$overall_status"
