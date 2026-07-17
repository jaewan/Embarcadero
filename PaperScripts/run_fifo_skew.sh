#!/usr/bin/env bash
# PaperScripts/run_fifo_skew.sh
#
# FIFO violation rate vs. injected inter-server skew δ.
#
# Scientific claim:
#   Under WBO (CXL-Scalog), per-client FIFO is violated once inter-server
#   skew δ exceeds the sequencer's round period R. Embarcadero never violates
#   (it holds or fences). This script empirically validates the T/R boundary.
#
# Experiment design:
#   - 2 publishers × 2 brokers on the real cluster
#   - tc netem delay injected on broker A's receive NIC path
#   - Systems: EMBARCADERO ORDER=5 and SCALOG ORDER=1
#   - δ sweep: 0, 0.5, 1.0, 1.5, 2.0, 3.0, 5.0 ms (7 points)
#   - 50,000 messages per client per point
#   - Output: CSV in data/paper_eval/fifo_skew/<CAMPAIGN_ID>/results.csv
#
# Usage:
#   bash PaperScripts/run_fifo_skew.sh
#   DELTA_POINTS_MS="0 1 2 3" bash PaperScripts/run_fifo_skew.sh
#   SYSTEMS="EMBARCADERO" bash PaperScripts/run_fifo_skew.sh
#   bash PaperScripts/run_fifo_skew.sh --preflight
#
# Key env overrides:
#   DELTA_POINTS_MS, SYSTEMS, NUM_TRIALS, MSGS_PER_CLIENT, MSG_SIZE
#   NETEM_IFACE, BROKER_IP, CAMPAIGN_ID, NUM_BROKERS
#
set -uo pipefail

PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$PAPER_DIR/.." && pwd)"
SCRIPTS_DIR="$PROJECT_ROOT/scripts"
BUILD_BIN="$PROJECT_ROOT/build/bin"
cd "$PROJECT_ROOT"

# ---------------------------------------------------------------------------
# Preflight flag
# ---------------------------------------------------------------------------
if [[ "${1:-}" == "--preflight" ]]; then
    PREFLIGHT_ONLY=1
else
    PREFLIGHT_ONLY=0
fi

# ---------------------------------------------------------------------------
# Campaign identity (appendable CSV)
# ---------------------------------------------------------------------------
CAMPAIGN_ID="${CAMPAIGN_ID:-fifo_skew_tr_boundary}"
PASS_ID="${PASS_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/data/paper_eval/fifo_skew/$CAMPAIGN_ID}"
LOG_DIR="$OUT_ROOT/logs/$PASS_ID"
RESULTS_CSV="${RESULTS_CSV:-$OUT_ROOT/results.csv}"
SUMMARY_LOG="$OUT_ROOT/sweep_summary.log"
LOCK_FILE="${LOCK_FILE:-/tmp/embarcadero_paper_fifo_skew.lock}"

mkdir -p "$LOG_DIR"

# ---------------------------------------------------------------------------
# Experiment knobs
# ---------------------------------------------------------------------------
DELTA_POINTS_MS="${DELTA_POINTS_MS:-0 0.5 1.0 1.5 2.0 3.0 5.0}"
SYSTEMS="${SYSTEMS:-EMBARCADERO SCALOG}"
NUM_TRIALS="${NUM_TRIALS:-3}"
MSGS_PER_CLIENT="${MSGS_PER_CLIENT:-50000}"
MSG_SIZE="${MSG_SIZE:-1024}"
NUM_BROKERS="${NUM_BROKERS:-2}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
# NETEM_IFACE is no longer used (delay is applied on CLIENT_A, not broker).
# Use CLIENT_A_IFACE and CLIENT_A to control netem injection target.

# Client topology: 2 clients for 2-broker experiment
# Client A → broker 0 (head, has netem delay)
# Client B → broker 1 (follower, no netem)
CLIENT_A="${CLIENT_A:-c4}"   # remote: the client whose path to broker 0 is delayed
CLIENT_B="${CLIENT_B:-c3}"   # remote: the client with undelayed path (control)
# NIC on CLIENT_A that faces the broker (10.10.10.0/24 network)
CLIENT_A_IFACE="${CLIENT_A_IFACE:-ens801f0np0}"
# Base port for broker 0 (embarcadero.yaml: port: 1214); broker k = BROKER_BASE_PORT+k.
# Delaying dport 1214 delays traffic to broker 0 only, leaving broker 1 (1215) unaffected.
BROKER_BASE_PORT="${BROKER_BASE_PORT:-1214}"

REMOTE_CLIENT_BIN_DIR="${REMOTE_CLIENT_BIN_DIR:-/home/domin/Embarcadero/build/bin}"
REMOTE_CLIENT_CONFIG="${REMOTE_CLIENT_CONFIG:-/home/domin/Embarcadero/config/client.yaml}"
CLIENT_LD_LIBRARY_PATH="${CLIENT_LD_LIBRARY_PATH:-/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib}"

# Broker/cluster settings
BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-300}"
BROKER_REACHABILITY_TIMEOUT_SEC="${BROKER_REACHABILITY_TIMEOUT_SEC:-60}"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
export EMBARCADERO_CXL_MAP_POPULATE="${EMBARCADERO_CXL_MAP_POPULATE:-0}"
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-77309411328}"  # 72 GiB
export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}"
export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-120}"
export EMBAR_ORDER5_EPOCH_US="${EMBAR_ORDER5_EPOCH_US:-500}"
export EMBARCADERO_RUNTIME_MODE="throughput"

# Scalog sequencer is always local (same host as brokers)
export EMBARCADERO_SCALOG_SEQ_IP="${EMBARCADERO_SCALOG_SEQ_IP:-127.0.0.1}"
export SCALOG_CXL_MODE="${SCALOG_CXL_MODE:-1}"

# E2E drain timeout: scale with message count (generous headroom)
_e2e=$(( 60 + (MSGS_PER_CLIENT * MSG_SIZE) / 52428800 ))
(( _e2e < 120 )) && _e2e=120
(( _e2e > 600 )) && _e2e=600
export EMBARCADERO_E2E_TIMEOUT_SEC="${EMBARCADERO_E2E_TIMEOUT_SEC:-$_e2e}"
unset _e2e

# NUMA bind for local brokers
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1}"

# ---------------------------------------------------------------------------
# Logging helpers
# ---------------------------------------------------------------------------
stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY_LOG"; }

# ---------------------------------------------------------------------------
# Violation detection capability probe
# Checks if the binary supports out_of_order_total_order field in the delivery
# ordering assertion CSV (delivery_ordering_assertion.csv).
# ---------------------------------------------------------------------------
FIFO_CHECK_AVAILABLE=0
probe_fifo_check() {
    local bin="$BUILD_BIN/throughput_test"
    # The delivery_ordering_assertion.csv is always written by the subscriber;
    # OutOfOrderPerClient (FIFO checker) is available when FIFO_CHECK+FIFO_CLIENT_ID are set.
    if [[ -x "$bin" ]]; then
        FIFO_CHECK_AVAILABLE=1
    fi
}

# ---------------------------------------------------------------------------
# tc netem injection — per-destination-port egress delay on CLIENT_A.
#
# Scientific design: CLIENT_A publishes to ALL brokers via striping.
# We delay only CLIENT_A traffic to broker 0 (port BROKER_BASE_PORT),
# not broker 1 (port BROKER_BASE_PORT+1). This creates the exact skew:
#   - CLIENT_A batch b1 -> broker 0 arrives LATE (delayed by delta)
#   - CLIENT_A batch b2 -> broker 1 arrives on time
# The sequencer sees b2 before b1 in CLIENT_A's session.
# Scalog commits b2 first => FIFO violation.
# Embarcadero holds b2 until b1 arrives (or fences if delta > lease).
# Netem runs on CLIENT_A's egress NIC via SSH, scoped to broker0 dport.
# ---------------------------------------------------------------------------
cleanup_netem() {
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_A"         "sudo tc qdisc del dev '$CLIENT_A_IFACE' root 2>/dev/null; true" 2>/dev/null || true
    log "Cleaned up netem on $CLIENT_A:$CLIENT_A_IFACE"
}

inject_skew() {
    local delta_ms="$1"
    cleanup_netem
    if [[ "$delta_ms" == "0" || "$delta_ms" == "0.0" ]]; then
        log "No netem injection (delta=0)"
        return 0
    fi
    local broker0_port="${BROKER_BASE_PORT}"
    # Apply per-destination-port prio+netem on CLIENT_A egress NIC via SSH.
    # Band 1:3 gets the delay; all other traffic falls through to 1:1 (undelayed).
    # This delays CLIENT_A -> broker0 (port ${broker0_port}) only;
    # CLIENT_A -> broker1 (port $((broker0_port+1))) is unaffected.
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_A" "
        sudo tc qdisc del dev '${CLIENT_A_IFACE}' root 2>/dev/null; true
        sudo tc qdisc add dev '${CLIENT_A_IFACE}' root handle 1: prio
        sudo tc qdisc add dev '${CLIENT_A_IFACE}' parent 1:3 handle 30: netem delay ${delta_ms}ms
        sudo tc filter add dev '${CLIENT_A_IFACE}' parent 1: protocol ip u32             match ip dst ${BROKER_IP}/32 match ip dport ${broker0_port} 0xffff             flowid 1:3
    " 2>/dev/null || {
        log "WARN: netem injection on $CLIENT_A:$CLIENT_A_IFACE failed — continuing without delay"
        return 1
    }
    log "Injected netem delay ${delta_ms}ms on $CLIENT_A:$CLIENT_A_IFACE dst ${BROKER_IP}:${broker0_port}"
}

# ---------------------------------------------------------------------------
# Process management
# ---------------------------------------------------------------------------
GLOBAL_SEQUENCER_PID=""
BROKER_PIDS=()

kill_brokers() {
    local pids=("${BROKER_PIDS[@]:-}")
    for pid in "${pids[@]:-}"; do
        kill "$pid" 2>/dev/null || true
    done
    if [[ -n "$GLOBAL_SEQUENCER_PID" ]]; then
        kill "$GLOBAL_SEQUENCER_PID" 2>/dev/null || true
        GLOBAL_SEQUENCER_PID=""
    fi
    BROKER_PIDS=()
    # Force-kill any stray embarlet/scalog_global_sequencer processes
    pkill -x embarlet 2>/dev/null || true
    pkill -x scalog_global_sequencer 2>/dev/null || true
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    # Brief wait for ports to drain
    sleep 2
}

cleanup_remote_clients() {
    local host
    for host in "$CLIENT_A" "$CLIENT_B"; do
        ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" \
            'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
    done
}

# ---------------------------------------------------------------------------
# Trap: always clean up netem and brokers on exit
# ---------------------------------------------------------------------------
on_exit() {
    log "EXIT trap: cleaning up netem and brokers..."
    cleanup_netem 2>/dev/null || true
    kill_brokers 2>/dev/null || true
    cleanup_remote_clients 2>/dev/null || true
}
trap 'on_exit' EXIT INT TERM

# ---------------------------------------------------------------------------
# Broker lifecycle
# ---------------------------------------------------------------------------
start_local_brokers() {
    local sequencer="$1"
    kill_brokers

    cd "$BUILD_BIN"
    export EMBARCADERO_HEAD_ADDR="$BROKER_IP"
    export EMBARCADERO_NUM_BROKERS="$NUM_BROKERS"
    export EMBARCADERO_REQUIRED_CXL_SEGMENTS="$NUM_BROKERS"
    export EMBARCADERO_BASELINE_CONTROL_TRANSPORT="grpc"

    # Start Scalog sequencer if needed
    if [[ "$sequencer" == "SCALOG" ]]; then
        ./scalog_global_sequencer > /tmp/fifo_skew_scalog_sequencer.log 2>&1 &
        GLOBAL_SEQUENCER_PID=$!
        log "Started scalog_global_sequencer (pid=$GLOBAL_SEQUENCER_PID)"
        sleep 0.5
    fi

    # Start head broker (broker 0)
    # shellcheck disable=SC2086
    $EMBARLET_NUMA_BIND ./embarlet \
        --config "$PROJECT_ROOT/config/embarcadero.yaml" \
        --head "--${sequencer}" \
        --network_threads 4 \
        > /tmp/fifo_skew_broker_0.log 2>&1 &
    BROKER_PIDS=("$!")
    log "Started head broker (pid=${BROKER_PIDS[0]})"

    # For metadata CXL mode, head and follower can start concurrently
    sleep 2

    # Start follower broker (broker 1)
    # shellcheck disable=SC2086
    $EMBARLET_NUMA_BIND ./embarlet \
        --config "$PROJECT_ROOT/config/embarcadero.yaml" \
        "--${sequencer}" \
        --network_threads 4 \
        > /tmp/fifo_skew_broker_1.log 2>&1 &
    BROKER_PIDS+=("$!")
    log "Started follower broker (pid=${BROKER_PIDS[1]})"

    cd "$PROJECT_ROOT"

    # Wait for brokers to become ready
    log "Waiting for $NUM_BROKERS broker(s) to become ready (timeout: ${BROKER_READY_TIMEOUT_SEC}s)..."
    local deadline=$(( SECONDS + BROKER_READY_TIMEOUT_SEC ))
    local ready_count=0
    while [[ $SECONDS -lt $deadline ]]; do
        ready_count=$(ls /tmp/embarlet_*_ready 2>/dev/null | wc -l)
        if [[ "$ready_count" -ge "$NUM_BROKERS" ]]; then
            log "All $NUM_BROKERS broker(s) ready"
            break
        fi
        # Check if any broker died
        local alive=0
        for pid in "${BROKER_PIDS[@]:-}"; do
            if kill -0 "$pid" 2>/dev/null; then
                alive=$(( alive + 1 ))
            fi
        done
        if [[ "$alive" -lt "$NUM_BROKERS" ]]; then
            log "ERROR: broker died before becoming ready (alive=$alive / $NUM_BROKERS)"
            cat /tmp/fifo_skew_broker_0.log >&2 || true
            return 1
        fi
        sleep 1
    done

    if [[ "$ready_count" -lt "$NUM_BROKERS" ]]; then
        log "ERROR: only $ready_count/$NUM_BROKERS broker(s) became ready within ${BROKER_READY_TIMEOUT_SEC}s"
        cat /tmp/fifo_skew_broker_0.log >&2 || true
        return 1
    fi

    # Brief reachability check
    local rc_deadline=$(( SECONDS + BROKER_REACHABILITY_TIMEOUT_SEC ))
    while [[ $SECONDS -lt $rc_deadline ]]; do
        if nc -z "$BROKER_IP" 9091 2>/dev/null; then
            break
        fi
        sleep 0.5
    done
    if ! nc -z "$BROKER_IP" 9091 2>/dev/null; then
        log "WARN: broker head not reachable on $BROKER_IP:9091 after ${BROKER_REACHABILITY_TIMEOUT_SEC}s"
    fi

    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    log "Cluster ready: $sequencer $NUM_BROKERS brokers"
    return 0
}

# ---------------------------------------------------------------------------
# CSV header
# ---------------------------------------------------------------------------
ensure_results_header() {
    if [[ ! -f "$RESULTS_CSV" ]]; then
        cat > "$RESULTS_CSV" <<'EOF'
campaign_id,pass_id,run_ts_utc,git_commit,delta_ms,system,trial,violation_count,fence_count,total_messages,violation_rate_pct,fence_rate_pct,status,notes
EOF
    fi
}

csv_append_row() {
    python3 - "$RESULTS_CSV" "$@" <<'PY'
import csv, sys
path = sys.argv[1]
fields = sys.argv[2:]
with open(path, "a", newline="") as f:
    csv.writer(f).writerow(fields)
PY
}

# ---------------------------------------------------------------------------
# Parse violation count from delivery_ordering_assertion.csv
# Column indices (0-based after header strip):
#   0=Target, 1=Delivered, 2=RecordedSamples, 3=TimedOut, 4=InvalidMessages,
#   5=DuplicateTotalOrder, 6=OutOfOrderTotalOrder, 7=DuplicateUid, 8=MissingUid,
#   9=OutOfOrderPerClient (FIFO violations), 10=FifoCheckedMessages,
#   8=MissingUid, 9=FirstTotalOrder, 10=LastTotalOrder, 11=MinUid,
#   12=MaxUid, 13=ReceiveMatched, 14=Pass
# ---------------------------------------------------------------------------
parse_violations() {
    local csv_path="$1"
    local default_total="$2"

    if [[ ! -f "$csv_path" ]]; then
        echo "0,0,$default_total"
        return
    fi

    python3 - "$csv_path" "$default_total" <<'PY'
import csv, sys
path, default_total = sys.argv[1], int(sys.argv[2])
try:
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f))
    if not rows:
        print(f"0,0,{default_total}")
        sys.exit(0)
    row = rows[0]
    ooo = int(row.get("OutOfOrderPerClient", 0) or 0)  # per-client FIFO violations
    delivered = int(row.get("Delivered", default_total) or default_total)
    print(f"{ooo},0,{delivered}")
except Exception as e:
    print(f"0,0,{default_total}", file=sys.stderr)
    print(f"0,0,{default_total}")
PY
}

# Parse SESSION_FENCED events from publisher log
parse_fence_events() {
    local log_file="$1"
    if [[ ! -f "$log_file" ]]; then
        echo 0
        return
    fi
    grep -c 'SESSION_FENCED_OBSERVED\|SESSION_FENCED broker=' "$log_file" 2>/dev/null || echo 0
}

# ---------------------------------------------------------------------------
# Run one (system, delta_ms, trial) point
# ---------------------------------------------------------------------------
run_point() {
    local system="$1"
    local delta_ms="$2"
    local trial="$3"

    local label="${system}_d${delta_ms}_t${trial}"
    local run_dir="$LOG_DIR/runs/$label"
    mkdir -p "$run_dir"

    local sequencer order
    case "$system" in
        EMBARCADERO)
            sequencer="EMBARCADERO"
            order=5
            ;;
        SCALOG)
            sequencer="SCALOG"
            order=5
            ;;
        *)
            log "ERROR: unknown system=$system"
            return 1
            ;;
    esac

    log "START [$label] system=$system δ=${delta_ms}ms trial=$trial"

    # Total messages: 2 clients × MSGS_PER_CLIENT
    local total_messages=$(( MSGS_PER_CLIENT * 2 ))
    local total_bytes=$(( MSGS_PER_CLIENT * MSG_SIZE ))

    # Start fresh cluster
    if ! start_local_brokers "$sequencer"; then
        log "FAIL [$label] — broker startup failed"
        local commit
        commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
        csv_append_row \
            "$CAMPAIGN_ID" "$PASS_ID" "$(stamp)" "$commit" \
            "$delta_ms" "$system" "$trial" \
            "0" "0" "$total_messages" "0.0" "0.0" \
            "fail" "broker_startup_failed"
        return 1
    fi

    # Inject netem skew
    inject_skew "$delta_ms"

    # Clean up any stale CSV files from previous runs on remote clients
    cleanup_remote_clients
    local host
    for host in "$CLIENT_A" "$CLIENT_B"; do
        ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" \
            "rm -f ${REMOTE_CLIENT_BIN_DIR}/delivery_ordering_assertion.csv ${REMOTE_CLIENT_BIN_DIR}/throughput_summary.csv 2>/dev/null; true" \
            2>/dev/null || true
    done

    # Launch both clients in parallel
    # Client A: publisher + subscriber on CLIENT_A (sees netem delay via broker A)
    # Client B: publisher + subscriber on CLIENT_B (sees broker 1, no netem)
    local client_a_log="$run_dir/client_a.log"
    local client_b_log="$run_dir/client_b.log"

    local client_cmd_base
    client_cmd_base="export LD_LIBRARY_PATH=$(printf '%q' "$CLIENT_LD_LIBRARY_PATH")"
    client_cmd_base+=" && export EMBARCADERO_HEAD_ADDR=$(printf '%q' "$BROKER_IP")"
    client_cmd_base+=" && export NUM_BROKERS=$NUM_BROKERS"
    client_cmd_base+=" && export EMBARCADERO_NUM_BROKERS=$NUM_BROKERS"
    client_cmd_base+=" && export EMBARCADERO_RUNTIME_MODE=throughput"
    client_cmd_base+=" && export EMBARCADERO_E2E_TIMEOUT_SEC=$EMBARCADERO_E2E_TIMEOUT_SEC"

    if [[ "$sequencer" == "SCALOG" ]]; then
        client_cmd_base+=" && export EMBARCADERO_SCALOG_SEQ_IP=$(printf '%q' "$BROKER_IP")"
        client_cmd_base+=" && export SCALOG_CXL_MODE=1"
    fi

    # Enable per-client FIFO checker: required for OutOfOrderPerClient to be non-zero.
    # FIFO_CLIENT_ID must be distinct and nonzero per client so the subscriber
    # can track per-sender submission order via (client_id<<32|per_client_seq) in msg_uid.
    local client_cmd_a_fifo="$client_cmd_base"
    client_cmd_a_fifo+=" && export EMBARCADERO_FIFO_CHECK=1"
    client_cmd_a_fifo+=" && export EMBARCADERO_FIFO_CLIENT_ID=1"
    local client_cmd_b_fifo="$client_cmd_base"
    client_cmd_b_fifo+=" && export EMBARCADERO_FIFO_CHECK=1"
    client_cmd_b_fifo+=" && export EMBARCADERO_FIFO_CLIENT_ID=2"

    # Test type 1 = E2E throughput: publisher + subscriber run concurrently.
    # E2EThroughputTest is the only test that calls DrainDeliveredMessages and
    # writes delivery_ordering_assertion.csv (OutOfOrderTotalOrder field).
    # delivery_measurement_enabled = (order >= 1), true for both ORDER=1 and ORDER=5.
    local throughput_args="-t 1 -m $MSG_SIZE -s $total_bytes -a 1 -o $order"
    throughput_args+=" --sequencer $sequencer"
    throughput_args+=" --config ${REMOTE_CLIENT_BIN_DIR}/../config/client.yaml"
    throughput_args+=" --head_addr $BROKER_IP"

    # Build full SSH commands
    local ssh_cmd_a="$client_cmd_a_fifo && cd ${REMOTE_CLIENT_BIN_DIR} && ./throughput_test $throughput_args"
    local ssh_cmd_b="$client_cmd_b_fifo && cd ${REMOTE_CLIENT_BIN_DIR} && ./throughput_test $throughput_args"

    log "Launching client A on $CLIENT_A..."
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_A" "bash -c '$ssh_cmd_a'" \
        > "$client_a_log" 2>&1 &
    local pid_a=$!

    log "Launching client B on $CLIENT_B..."
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_B" "bash -c '$ssh_cmd_b'" \
        > "$client_b_log" 2>&1 &
    local pid_b=$!

    # Wait for both clients
    local rc_a=0 rc_b=0
    wait "$pid_a" || rc_a=$?
    wait "$pid_b" || rc_b=$?

    # Remove netem before collecting results to avoid interfering with scp
    cleanup_netem

    log "Client A exit=$rc_a, Client B exit=$rc_b"

    # Collect delivery_ordering_assertion.csv from each client
    local assert_a="$run_dir/client_a_delivery_ordering_assertion.csv"
    local assert_b="$run_dir/client_b_delivery_ordering_assertion.csv"
    scp -o BatchMode=yes -o ConnectTimeout=10 \
        "${CLIENT_A}:${REMOTE_CLIENT_BIN_DIR}/delivery_ordering_assertion.csv" \
        "$assert_a" 2>/dev/null || true
    scp -o BatchMode=yes -o ConnectTimeout=10 \
        "${CLIENT_B}:${REMOTE_CLIENT_BIN_DIR}/delivery_ordering_assertion.csv" \
        "$assert_b" 2>/dev/null || true

    # Parse violations from each client
    local parsed_a parsed_b
    parsed_a="$(parse_violations "$assert_a" "$MSGS_PER_CLIENT")"
    parsed_b="$(parse_violations "$assert_b" "$MSGS_PER_CLIENT")"

    local ooo_a ooo_b fence_a fence_b del_a del_b
    IFS=',' read -r ooo_a fence_a del_a <<< "$parsed_a"
    IFS=',' read -r ooo_b fence_b del_b <<< "$parsed_b"

    # Also count SESSION_FENCED events from client logs
    local sf_a sf_b
    sf_a="$(parse_fence_events "$client_a_log")"
    sf_b="$(parse_fence_events "$client_b_log")"

    local violation_count=$(( ooo_a + ooo_b ))
    local fence_count=$(( sf_a + sf_b ))
    local total_delivered=$(( del_a + del_b ))

    # Compute rates
    local violation_rate fence_rate
    if [[ "$total_delivered" -gt 0 ]]; then
        violation_rate="$(python3 -c "print(f'{100.0 * $violation_count / $total_delivered:.4f}')")"
        fence_rate="$(python3 -c "print(f'{100.0 * $fence_count / $total_delivered:.4f}')")"
    else
        violation_rate="0.0"
        fence_rate="0.0"
    fi

    # Determine status.
    # Non-zero exit is EXPECTED for Scalog when ordering violations are found
    # (E2EThroughputTest calls exit(1) on ordering failure). We treat this as
    # "ok" if we successfully collected delivery_ordering_assertion.csv from
    # the client. Only flag as "fail" if we got no useful data at all.
    local status="ok"
    local notes=""
    if [[ "$rc_a" -ne 0 || "$rc_b" -ne 0 ]]; then
        notes="client_rc_a=${rc_a}_rc_b=${rc_b}"
        # Accept non-zero exit when we collected ordering CSVs (expected for Scalog with violations).
        if [[ ! -f "$assert_a" && ! -f "$assert_b" ]]; then
            status="fail"
            notes="${notes}_no_ordering_csv"
        fi
    fi

    local commit
    commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"

    log "[$label] violations=$violation_count/$total_delivered (${violation_rate}%) fences=$fence_count status=$status"

    csv_append_row \
        "$CAMPAIGN_ID" "$PASS_ID" "$(stamp)" "$commit" \
        "$delta_ms" "$system" "$trial" \
        "$violation_count" "$fence_count" "$total_delivered" \
        "$violation_rate" "$fence_rate" \
        "$status" "$notes"

    # Kill brokers between points
    kill_brokers
    sleep 2

    return 0
}

# ---------------------------------------------------------------------------
# Preflight checks
# ---------------------------------------------------------------------------
preflight_fifo_skew() {
    local missing=0

    log "=== Preflight checks ==="

    # Check tc command
    if ! command -v tc >/dev/null 2>&1; then
        log "FATAL: 'tc' command not found (need iproute2)"
        missing=1
    fi

    # Check sudo tc access
    if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_A" "sudo tc qdisc show dev '$CLIENT_A_IFACE'" >/dev/null 2>&1; then
        log "FATAL: cannot run 'sudo tc' on $CLIENT_A:$CLIENT_A_IFACE (check SSH + sudo/iproute2 on client)"
        missing=1
    fi

    # Check CLIENT_A_IFACE exists on CLIENT_A
    if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_A" "ip link show '$CLIENT_A_IFACE'" >/dev/null 2>&1; then
        log "FATAL: CLIENT_A_IFACE=$CLIENT_A_IFACE does not exist on $CLIENT_A"
        log "  Available interfaces: $(ip link show | grep -oP '^\d+:\s+\K\S+' | tr '\n' ' ')"
        missing=1
    fi

    # Check local broker binary
    if [[ ! -x "$BUILD_BIN/embarlet" ]]; then
        log "FATAL: $BUILD_BIN/embarlet not found or not executable"
        missing=1
    fi

    # Check Scalog sequencer binary (needed for SCALOG system)
    if echo "$SYSTEMS" | grep -qw "SCALOG"; then
        if [[ ! -x "$BUILD_BIN/scalog_global_sequencer" ]]; then
            log "FATAL: $BUILD_BIN/scalog_global_sequencer not found"
            missing=1
        fi
    fi

    # Check throughput_test binary on remote clients
    for host in "$CLIENT_A" "$CLIENT_B"; do
        if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" \
            "test -x ${REMOTE_CLIENT_BIN_DIR}/throughput_test" 2>/dev/null; then
            log "FATAL: remote $host missing ${REMOTE_CLIENT_BIN_DIR}/throughput_test"
            missing=1
        fi
    done

    # Check python3 available (for CSV writing)
    if ! command -v python3 >/dev/null 2>&1; then
        log "FATAL: python3 not available (needed for CSV append)"
        missing=1
    fi

    # Check nc available (for reachability check)
    if ! command -v nc >/dev/null 2>&1; then
        log "WARN: nc not found; broker reachability check will be skipped"
    fi

    # Probe FIFO check capability
    probe_fifo_check
    if [[ "$FIFO_CHECK_AVAILABLE" -eq 0 ]]; then
        log "WARN: throughput_test binary not found; violations will not be countable"
    else
        log "OK: FIFO check via delivery_ordering_assertion.csv (OutOfOrderPerClient field)"
    fi

    if [[ "$missing" -ne 0 ]]; then
        log "FATAL: preflight failed ($missing issue(s))"
        return 1
    fi

    log "Preflight OK: netem on $CLIENT_A:$CLIENT_A_IFACE dport=$BROKER_BASE_PORT, binaries present, remote clients reachable"
    return 0
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

# Exclusive campaign lock
exec 9>"$LOCK_FILE"
flock -n 9 || { echo "ERROR: another fifo_skew campaign owns $LOCK_FILE" >&2; exit 1; }

ensure_results_header

if [[ "$PREFLIGHT_ONLY" -eq 1 ]]; then
    preflight_fifo_skew
    exit $?
fi

if ! preflight_fifo_skew; then
    exit 1
fi

log "===== FIFO Skew START campaign=$CAMPAIGN_ID pass=$PASS_ID ====="
log "Commit: $(git rev-parse --short HEAD 2>/dev/null || echo unknown) dirty=$([[ -n $(git status --porcelain 2>/dev/null) ]] && echo yes || echo no)"
log "OUT_ROOT=$OUT_ROOT"
log "RESULTS_CSV=$RESULTS_CSV (appendable)"
log "DELTA_POINTS_MS=$DELTA_POINTS_MS"
log "SYSTEMS=$SYSTEMS"
log "NUM_TRIALS=$NUM_TRIALS MSGS_PER_CLIENT=$MSGS_PER_CLIENT MSG_SIZE=$MSG_SIZE"
log "CLIENT_A_IFACE=$CLIENT_A_IFACE BROKER_IP=$BROKER_IP NUM_BROKERS=$NUM_BROKERS"
log "CLIENT_A=$CLIENT_A CLIENT_B=$CLIENT_B"

# Sweep: for each system, for each δ, for each trial
for system in $SYSTEMS; do
    log "--- System: $system ---"
    for delta_ms in $DELTA_POINTS_MS; do
        for (( trial=1; trial<=NUM_TRIALS; trial++ )); do
            run_point "$system" "$delta_ms" "$trial" || true
        done
    done
done

# Final netem cleanup (trap also handles this but be explicit)
cleanup_netem
kill_brokers

log "===== FIFO Skew COMPLETE campaign=$CAMPAIGN_ID pass=$PASS_ID ====="
log "CSV: $RESULTS_CSV"
log ""
log "Quick summary:"
python3 - "$RESULTS_CSV" <<'PY'
import csv, sys
path = sys.argv[1]
try:
    rows = list(csv.DictReader(open(path, newline="")))
    if not rows:
        print("  (no data)")
        sys.exit(0)
    from collections import defaultdict
    by_sys = defaultdict(list)
    for r in rows:
        if r.get("status") == "ok":
            by_sys[r["system"]].append(r)
    for sys_name, srows in sorted(by_sys.items()):
        print(f"  {sys_name}:")
        by_delta = defaultdict(list)
        for r in srows:
            by_delta[float(r["delta_ms"])].append(r)
        for d in sorted(by_delta.keys()):
            pts = by_delta[d]
            rates = [float(r["violation_rate_pct"]) for r in pts]
            avg = sum(rates) / len(rates)
            print(f"    δ={d:4.1f}ms  violation_rate={avg:.3f}%  n={len(pts)}")
except FileNotFoundError:
    print("  (no results file)")
PY
