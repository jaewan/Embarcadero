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
#   NUM_CLIENTS=4  → c4, c3, local, c2
#   NUM_CLIENTS=5  → c4, c3, local, c2, c1
#
# All configuration via environment variables:
#   NUM_CLIENTS, NUM_BROKERS, NUM_TRIALS, TRIAL_MAX_ATTEMPTS
#   TOTAL_MESSAGE_SIZE, MESSAGE_SIZE, THREADS_PER_BROKER
#   TEST_TYPE, ORDER, ACK, REPLICATION_FACTOR, SEQUENCER
#   EMBARCADERO_HEAD_ADDR (broker IP, default 10.10.10.143)
#   EMBARCADERO_ORDER0_FAST_PATH, EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES,
#   EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE, EMBARCADERO_BATCH_SIZE,
#   EMBARCADERO_CLIENT_PUB_BATCH_KB, EMBARCADERO_NETWORK_IO_THREADS,
#   EMBARCADERO_ORDER5_HOME_BROKERS, LOCAL_CLIENT_NUMA,
#   REMOTE_CORFU_SEQUENCER_HOST, REMOTE_CORFU_BUILD_BIN,
#   REMOTE_SCALOG_SEQUENCER_HOST, REMOTE_SCALOG_BUILD_BIN
#   EMBARCADERO_CORFU_SEQ_IP / EMBARCADERO_CORFU_SEQ_PORT for CORFU + SSH clients
#   REMOTE_LAZYLOG_SEQUENCER_HOST, REMOTE_LAZYLOG_BUILD_BIN
#   EMBARCADERO_LAZYLOG_SEQ_IP / EMBARCADERO_LAZYLOG_SEQ_PORT for LAZYLOG + SSH clients
#   EMBARCADERO_SCALOG_SEQ_IP / EMBARCADERO_SCALOG_SEQ_PORT for SCALOG
#   REMOTE_CLIENT_BIN_DIR to override the SSH client executable directory
#
# Example:
#   NUM_CLIENTS=3 NUM_BROKERS=4 MESSAGE_SIZE=8192 scripts/run_multiclient.sh

set -euo pipefail

RUN_LOCK_FILE="${RUN_LOCK_FILE:-/tmp/embarcadero_run_multiclient.lock}"
exec {RUN_LOCK_FD}>"$RUN_LOCK_FILE"
if ! flock -n "$RUN_LOCK_FD"; then
    echo "ERROR: another benchmark orchestrator is already running (lock: $RUN_LOCK_FILE)." >&2
    echo "       Clear the existing run or override RUN_LOCK_FILE if you intentionally need a separate harness lock." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Cluster topology — order determines activation sequence
# "local" means this broker machine (where brokers run); everything else is SSH
# ---------------------------------------------------------------------------
declare -a CLIENT_HOSTS=( "c4"  "c3"    "local" "c2"  "c1"  )
declare -a CLIENT_NUMAS=( "1"   "1"     ""      "1"   "1"   )
if [[ -n "${CLIENT_HOSTS_CSV:-}" ]]; then
    IFS=',' read -r -a CLIENT_HOSTS <<< "$CLIENT_HOSTS_CSV"
fi
if [[ -n "${CLIENT_NUMAS_CSV:-}" ]]; then
    IFS=',' read -r -a CLIENT_NUMAS <<< "$CLIENT_NUMAS_CSV"
fi
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

BROKER_READY_TIMEOUT_SEC=${BROKER_READY_TIMEOUT_SEC:-120}
if [[ "$SEQUENCER" == "CORFU" && -z "${BROKER_READY_TIMEOUT_SEC_OVERRIDE_APPLIED:-}" && "${BROKER_READY_TIMEOUT_SEC:-120}" == "120" ]]; then
    BROKER_READY_TIMEOUT_SEC=300
fi
BROKER_REACHABILITY_TIMEOUT_SEC=${BROKER_REACHABILITY_TIMEOUT_SEC:-20}
BROKER_REACHABILITY_POLL_SEC=${BROKER_REACHABILITY_POLL_SEC:-1}
BROKER_START_STAGGER_SEC=${BROKER_START_STAGGER_SEC:-1}
# Extra settle time for broker heartbeat/control-plane convergence after sockets listen.
BROKER_READY_PROPAGATION_SEC=${BROKER_READY_PROPAGATION_SEC:-4}
ORDER5_CLUSTER_READY_TIMEOUT_SEC=${ORDER5_CLUSTER_READY_TIMEOUT_SEC:-30}
# Extra lead time given to SSH connections and clock-sync settling
START_DELAY_SEC=${START_DELAY_SEC:-8}
# Enforce a safer minimum barrier delay when any client runs over SSH.
MIN_REMOTE_START_DELAY_SEC=${MIN_REMOTE_START_DELAY_SEC:-8}
QUIET=${QUIET:-0}

# Performance knobs (set to empty string to disable)
EMBARCADERO_ORDER0_FAST_PATH=${EMBARCADERO_ORDER0_FAST_PATH:-1}
EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES=${EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES:-524288}
EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE=${EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE:-1}
EMBARCADERO_BATCH_SIZE=${EMBARCADERO_BATCH_SIZE:-524288}
EMBARCADERO_CLIENT_PUB_BATCH_KB=${EMBARCADERO_CLIENT_PUB_BATCH_KB:-512}
EMBARCADERO_NETWORK_IO_THREADS=${EMBARCADERO_NETWORK_IO_THREADS:-4}
EMBARCADERO_ORDER5_HOME_BROKERS=${EMBARCADERO_ORDER5_HOME_BROKERS:-}
EMBARCADERO_CORFU_SEQ_IP=${EMBARCADERO_CORFU_SEQ_IP:-}
EMBARCADERO_CORFU_SEQ_PORT=${EMBARCADERO_CORFU_SEQ_PORT:-}
EMBARCADERO_LAZYLOG_SEQ_IP=${EMBARCADERO_LAZYLOG_SEQ_IP:-}
EMBARCADERO_LAZYLOG_SEQ_PORT=${EMBARCADERO_LAZYLOG_SEQ_PORT:-}
CLIENT_LD_LIBRARY_PATH=${CLIENT_LD_LIBRARY_PATH:-${LD_LIBRARY_PATH:-}}
REMOTE_CORFU_SEQUENCER_HOST=${REMOTE_CORFU_SEQUENCER_HOST:-}
REMOTE_CORFU_BUILD_BIN=${REMOTE_CORFU_BUILD_BIN:-}
REMOTE_LAZYLOG_SEQUENCER_HOST=${REMOTE_LAZYLOG_SEQUENCER_HOST:-}
REMOTE_LAZYLOG_BUILD_BIN=${REMOTE_LAZYLOG_BUILD_BIN:-}
EMBARCADERO_SCALOG_SEQ_IP=${EMBARCADERO_SCALOG_SEQ_IP:-}
EMBARCADERO_SCALOG_SEQ_PORT=${EMBARCADERO_SCALOG_SEQ_PORT:-}
REMOTE_SCALOG_SEQUENCER_HOST=${REMOTE_SCALOG_SEQUENCER_HOST:-}
REMOTE_SCALOG_BUILD_BIN=${REMOTE_SCALOG_BUILD_BIN:-}
# ---------------------------------------------------------------------------
# Derived paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_BIN="$PROJECT_ROOT/build/bin"
REMOTE_CLIENT_BIN_DIR=${REMOTE_CLIENT_BIN_DIR:-$BUILD_BIN}
BROKER_CONFIG="${BROKER_CONFIG:-config/embarcadero.yaml}"
CLIENT_CONFIG="${CLIENT_CONFIG:-config/client.yaml}"
BROKER_CONFIG_ABS="$PROJECT_ROOT/$BROKER_CONFIG"
CLIENT_CONFIG_ABS="$PROJECT_ROOT/$CLIENT_CONFIG"
LOG_DIR="$PROJECT_ROOT/multiclient_logs"

default_broker_ip() {
    if [[ -n "${EMBARCADERO_HEAD_ADDR:-}" ]]; then
        printf '%s\n' "$EMBARCADERO_HEAD_ADDR"
        return 0
    fi
    local host_ip
    host_ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
    if [[ -n "$host_ip" ]]; then
        printf '%s\n' "$host_ip"
    else
        printf '%s\n' "127.0.0.1"
    fi
}

BROKER_IP="$(default_broker_ip)"
export EMBARCADERO_CXL_SHM_NAME="${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"

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
if [[ "$SEQUENCER" == "LAZYLOG" ]] && [[ "$ORDER" != "2" ]]; then
    echo "ERROR: LAZYLOG baseline requires ORDER=2 (got ORDER=$ORDER)" >&2
    exit 1
fi

corfu_seq_ip_is_loopback() {
    case "${EMBARCADERO_CORFU_SEQ_IP:-}" in
        ""|127.0.0.1|localhost)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

corfu_uses_remote_clients() {
    local i host
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        if [[ "$host" != "local" ]]; then
            return 0
        fi
    done
    return 1
}

has_remote_clients() {
    local i host
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        if [[ "$host" != "local" ]]; then
            return 0
        fi
    done
    return 1
}

all_active_clients_are_remote() {
    local i host
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        if [[ "$host" == "local" ]]; then
            return 1
        fi
    done
    return 0
}

if [[ -z "${EMBARCADERO_ORDER5_HOME_BROKERS:-}" ]] &&
   [[ "$SEQUENCER" == "EMBARCADERO" ]] &&
   [[ "$ORDER" == "5" ]] &&
   [[ "$TEST_TYPE" == "5" ]] &&
   [[ "$THREADS_PER_BROKER" -ge 4 ]] &&
   [[ "$NUM_CLIENTS" -ge 3 ]] &&
   all_active_clients_are_remote; then
    # Fully striped remote ORDER=5 publish-only runs can create large head-broker hold/expiry
    # waves during disconnect tail drain. Bias each client to a small broker home set to keep
    # cross-broker sequence skew bounded without changing ORDER=5 semantics.
    EMBARCADERO_ORDER5_HOME_BROKERS=2
fi

if [[ "$SEQUENCER" == "CORFU" ]] && corfu_uses_remote_clients && corfu_seq_ip_is_loopback; then
    echo "ERROR: CORFU with SSH clients requires EMBARCADERO_CORFU_SEQ_IP to be a routable non-loopback address." >&2
    if [[ -n "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        echo "       Sequencer host: $REMOTE_CORFU_SEQUENCER_HOST" >&2
        echo "       Hint: ssh $REMOTE_CORFU_SEQUENCER_HOST 'hostname -I'" >&2
    else
        echo "       Hint: set EMBARCADERO_CORFU_SEQ_IP to the sequencer host dataplane IP." >&2
        echo "       Local interfaces: $(hostname -I 2>/dev/null || true)" >&2
    fi
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

embar_default_numa_membind() {
  if command -v numactl >/dev/null 2>&1 && numactl -H 2>/dev/null | grep -qE '^node 2 cpus:'; then
    echo "1,2"
  else
    echo "1"
  fi
}

# Inherited LazyLog sequencer IP breaks all-local broker clusters unless a remote sequencer is in use.
if [[ "${EMBARCADERO_KEEP_LAZYLOG_SEQ_ENV:-0}" != "1" ]] && [[ -z "${REMOTE_LAZYLOG_SEQUENCER_HOST:-}" ]]; then
  unset EMBARCADERO_LAZYLOG_SEQ_IP EMBARCADERO_LAZYLOG_SEQ_PORT
fi
if [[ "$SEQUENCER" == "LAZYLOG" && -z "${REMOTE_LAZYLOG_SEQUENCER_HOST:-}" ]]; then
  EMBARCADERO_LAZYLOG_SEQ_IP="${EMBARCADERO_LAZYLOG_SEQ_IP:-$BROKER_IP}"
fi

_default_mb="$(embar_default_numa_membind)"
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=${_default_mb}}"
unset _default_mb
[[ "$SEQUENCER" == "CORFU" ]] && EMBARLET_NUMA_BIND=""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log() { [ "$QUIET" != "1" ] && echo "$*"; }

START_BROKERS_FAILURE_REASON=""

print_local_resource_snapshot() {
    local mem_available_kb huge_free page_kb huge_free_mb shm_avail_kb
    mem_available_kb="$(awk '/MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    huge_free="$(awk '/HugePages_Free:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    page_kb="$(awk '/Hugepagesize:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    shm_avail_kb="$(df -Pk /dev/shm 2>/dev/null | awk 'NR==2 {print $4}' || echo 0)"
    huge_free_mb=$(( huge_free * page_kb / 1024 ))
    log "Resource snapshot: MemAvailable=$(( mem_available_kb / 1024 ))MB HugePagesFree=${huge_free} (${huge_free_mb}MB) /dev/shm_avail=$(( shm_avail_kb / 1024 ))MB"
}

preflight_local_broker_resources() {
    local mem_available_kb huge_free page_kb huge_free_mb
    mem_available_kb="$(awk '/MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    huge_free="$(awk '/HugePages_Free:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    page_kb="$(awk '/Hugepagesize:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    huge_free_mb=$(( huge_free * page_kb / 1024 ))
    print_local_resource_snapshot
    if [[ "$mem_available_kb" -lt $(( 8 * 1024 * 1024 )) ]]; then
        echo "WARNING: low MemAvailable before broker start: $(( mem_available_kb / 1024 ))MB" >&2
    fi
    if [[ "${EMBAR_USE_HUGETLB:-1}" == "1" && "$huge_free_mb" -lt 8192 ]]; then
        echo "WARNING: low free hugepages before broker start: ${huge_free_mb}MB" >&2
    fi
    return 0
}

classify_broker_startup_failure() {
    local -a pids=( "$@" )
    local idx pid log_file
    for idx in "${!pids[@]}"; do
        pid="${pids[$idx]}"
        log_file="/tmp/broker_${idx}.log"
        if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
            if grep -Eqi 'Killed|Out of memory|oom-kill|oom_reaper|std::bad_alloc' "$log_file" 2>/dev/null; then
                printf '%s\n' "infra_broker_killed"
                return
            fi
            printf '%s\n' "infra_broker_exited"
            return
        fi
    done
    printf '%s\n' "infra_broker_startup_timeout"
}

infer_broker_cpu_numa() {
    if [[ "$EMBARLET_NUMA_BIND" =~ --cpunodebind=([0-9]+) ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
        return
    fi
    printf '%s\n' "1"
}

resolve_local_client_numa() {
    if [[ -n "${LOCAL_CLIENT_NUMA:-}" ]]; then
        printf '%s\n' "$LOCAL_CLIENT_NUMA"
        return
    fi
    # Publication throughput uses the broker-node local client on NUMA 0 to match
    # the requested study topology and avoid cross-NUMA loopback penalties.
    printf '%s\n' "0"
}

resolve_client_numa() {
    local host="$1"
    local default_numa="$2"
    if [[ "$host" == "local" ]]; then
        resolve_local_client_numa
        return
    fi
    printf '%s\n' "$default_numa"
}

verify_client_binary() {
    local host="$1"
    local client_bin="$BUILD_BIN"
    if [[ "$host" != "local" ]]; then
        client_bin="$REMOTE_CLIENT_BIN_DIR"
    fi
    local remote_cmd="
set -e
cd '$client_bin'
if [ ! -x ./throughput_test ]; then
    echo 'missing throughput_test in $client_bin'
    exit 10
fi
if command -v ldd >/dev/null 2>&1; then
    LD_LIBRARY_PATH=\"$CLIENT_LD_LIBRARY_PATH\" ldd ./throughput_test 2>/dev/null | grep -E 'not found|GLIBC_|GLIBCXX_' && exit 11 || true
fi
"
    if [[ "$host" == "local" ]]; then
        bash -lc "$remote_cmd"
    else
        ssh "$host" "$remote_cmd"
    fi
}

preflight_clients() {
    local failed=0
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        local host="${CLIENT_HOSTS[$i]}"
        if ! verify_client_binary "$host"; then
            echo "ERROR: client preflight failed on host '$host'." >&2
            echo "       Ensure /home/domin/Embarcadero/build/bin/throughput_test exists and is runnable on that host." >&2
            failed=1
        fi
    done
    return "$failed"
}

probe_tcp_from_host() {
    local host="$1"
    local ip="$2"
    local port="$3"
    local probe_cmd="timeout 1 bash -lc '</dev/tcp/$ip/$port' >/dev/null 2>&1"
    if [[ "$host" == "local" ]]; then
        eval "$probe_cmd"
    else
        ssh "$host" "$probe_cmd" >/dev/null 2>&1
    fi
}

wait_for_broker_reachability() {
    local deadline=$(( $(date +%s) + BROKER_REACHABILITY_TIMEOUT_SEC ))
    local i j host port

    while (( $(date +%s) < deadline )); do
        local all_ok=1
        local missing=""

        for (( i=0; i<NUM_CLIENTS; i++ )); do
            host="${CLIENT_HOSTS[$i]}"
            for (( j=0; j<NUM_BROKERS; j++ )); do
                port=$((BROKER_DATA_PORT_BASE + j))
                if ! probe_tcp_from_host "$host" "$BROKER_IP" "$port"; then
                    all_ok=0
                    missing+=" ${host}:${BROKER_IP}:${port}"
                fi
            done
            if [[ -n "${BROKER_HEARTBEAT_PORT:-}" ]]; then
                if ! probe_tcp_from_host "$host" "$BROKER_IP" "$BROKER_HEARTBEAT_PORT"; then
                    all_ok=0
                    missing+=" ${host}:${BROKER_IP}:${BROKER_HEARTBEAT_PORT}"
                fi
            fi
        done

        if [[ "$all_ok" -eq 1 ]]; then
            return 0
        fi

        log "Waiting for client-side broker reachability:${missing}"
        sleep "$BROKER_REACHABILITY_POLL_SEC"
    done

    return 1
}

wait_for_order5_cluster_convergence() {
    if [[ "$SEQUENCER" != "EMBARCADERO" || "$ORDER" != "5" ]]; then
        return 0
    fi

    local timeout_sec="${ORDER5_CLUSTER_READY_TIMEOUT_SEC}"
    local deadline=$(( $(date +%s) + timeout_sec ))
    local broker_id expected_scanners scanner_lines
    expected_scanners="$NUM_BROKERS"

    log "Waiting for ORDER=5 head convergence (${expected_scanners} scanner(s), timeout: ${timeout_sec}s)..."
    while (( $(date +%s) < deadline )); do
        if [[ ! -f /tmp/broker_0.log ]]; then
            sleep 0.2
            continue
        fi

        if ! kill -0 "${HEAD_BROKER_PID:-0}" 2>/dev/null; then
            echo "ERROR: head broker exited while waiting for ORDER=5 convergence" >&2
            START_BROKERS_FAILURE_REASON="infra_broker_exited"
            return 1
        fi

        scanner_lines="$(grep -Ec 'Started (initial )?BrokerScannerWorker5 for broker|Started BrokerScannerWorker5 for broker' /tmp/broker_0.log 2>/dev/null || true)"
        if [[ "$scanner_lines" -ge "$expected_scanners" ]]; then
            for (( broker_id=0; broker_id<NUM_BROKERS; broker_id++ )); do
                if ! grep -Eq "Started (initial )?BrokerScannerWorker5 for broker ${broker_id}|Started BrokerScannerWorker5 for broker ${broker_id}" /tmp/broker_0.log 2>/dev/null; then
                    scanner_lines=0
                    break
                fi
            done
            if [[ "$scanner_lines" -ne 0 ]]; then
                log "ORDER=5 head convergence complete: scanner workers active for brokers 0..$((NUM_BROKERS - 1))."
                return 0
            fi
        fi

        sleep 0.2
    done

    echo "ERROR: ORDER=5 head convergence did not complete within ${timeout_sec}s" >&2
    tail -n 120 /tmp/broker_0.log >&2 || true
    START_BROKERS_FAILURE_REASON="infra_order5_cluster_convergence_timeout"
    return 1
}

precreate_embarcadero_order5_topic() {
    if [[ "$SEQUENCER" != "EMBARCADERO" || "$ORDER" != "5" ]]; then
        return 0
    fi

    local precreate_log="/tmp/embarcadero_order5_precreate.log"
    log "Precreating ORDER=5 topic metadata and scanner state..."
    (
        cd "$BUILD_BIN"
        EMBARCADERO_RUNTIME_MODE=throughput \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        EMBARCADERO_CXL_SHM_NAME="$EMBARCADERO_CXL_SHM_NAME" \
        EMBARCADERO_CXL_ZERO_MODE="$EMBARCADERO_CXL_ZERO_MODE" \
        EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR" \
        EMBARCADERO_ORDER0_FAST_PATH="$EMBARCADERO_ORDER0_FAST_PATH" \
        EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES="$EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES" \
        EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE="$EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE" \
        EMBARCADERO_BATCH_SIZE="$EMBARCADERO_BATCH_SIZE" \
        EMBARCADERO_CLIENT_PUB_BATCH_KB="$EMBARCADERO_CLIENT_PUB_BATCH_KB" \
        EMBARCADERO_NETWORK_IO_THREADS="$EMBARCADERO_NETWORK_IO_THREADS" \
        EMBARCADERO_ORDER5_HOME_BROKERS="$EMBARCADERO_ORDER5_HOME_BROKERS" \
        EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-}" \
        LD_LIBRARY_PATH="$CLIENT_LD_LIBRARY_PATH" \
        EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}" \
        numactl --cpunodebind="$(resolve_local_client_numa)" --membind="$(resolve_local_client_numa)" \
            ./throughput_test \
            --config "$CLIENT_CONFIG_ABS" \
            -n "$THREADS_PER_BROKER" \
            -m "$MESSAGE_SIZE" \
            -s 0 \
            -t "$TEST_TYPE" \
            -o "$ORDER" \
            -a 0 \
            -r "$REPLICATION_FACTOR" \
            --sequencer "$SEQUENCER" \
            -l 0
    ) >"$precreate_log" 2>&1
    local status=$?
    if [[ "$status" -ne 0 ]]; then
        echo "ERROR: ORDER=5 topic precreate failed" >&2
        cat "$precreate_log" >&2 || true
        return 1
    fi
    return 0
}

compute_overlap_throughput_gbps() {
    local trial="$1"
    shift
    local -a files=("$@")
    local tmp_bounds
    tmp_bounds="$(mktemp)"
    local file

    for file in "${files[@]}"; do
        [[ -s "$file" ]] || continue
        awk -F',' '
            NR==1 {
                for (i = 1; i <= NF; ++i) if ($i == "Total_GBps") col = i
                next
            }
            col > 0 {
                ts = $1 + 0
                g = $col + 0
                if (g > 0.000001) {
                    if (!seen) { first = ts; seen = 1 }
                    last = ts
                }
            }
            END {
                if (seen) printf "%s,%s,%s\n", FILENAME, first, last
            }
        ' "$file" >> "$tmp_bounds"
    done

    if [[ ! -s "$tmp_bounds" ]]; then
        rm -f "$tmp_bounds"
        return 1
    fi

    # Phase-align each client's active window to its first positive sample so that
    # host clock skew / SSH launch jitter does not eliminate overlap coverage.
    local overlap_start overlap_end
    overlap_start=0
    overlap_end="$(awk -F',' '
        NR==1 { m = ($3 - $2) }
        {
            d = ($3 - $2)
            if (d < m) m = d
        }
        END { printf "%.0f", m }
    ' "$tmp_bounds")"
    if [[ -z "$overlap_end" ]]; then
        rm -f "$tmp_bounds"
        return 1
    fi
    if [[ "$overlap_end" -le "$overlap_start" ]]; then
        overlap_end=$((overlap_start + 1))
    fi

    local overlap_window_ms=$((overlap_end - overlap_start))
    local sum_mean_gbps="0"
    local client_count=0
    local ts_file first_ts mean
    while IFS=',' read -r ts_file first_ts _; do
        [[ -f "$ts_file" ]] || continue
        mean="$(awk -F',' -v b="$first_ts" -v s="$overlap_start" -v e="$overlap_end" '
            NR==1 {
                for (i = 1; i <= NF; ++i) if ($i == "Total_GBps") col = i
                next
            }
            col > 0 {
                ts = ($1 + 0) - b
                g = $col + 0
                if (ts >= s && ts <= e) { sum += g; n += 1 }
            }
            END { if (n > 0) printf "%.9f", sum / n }
        ' "$ts_file")"
        if [[ -n "$mean" ]]; then
            sum_mean_gbps="$(awk -v a="$sum_mean_gbps" -v b="$mean" 'BEGIN {printf "%.9f", a + b}')"
            client_count=$((client_count + 1))
        fi
    done < "$tmp_bounds"
    rm -f "$tmp_bounds"

    if [[ "$client_count" -le 0 ]]; then
        return 1
    fi

    printf "%s,%s,%s,%s\n" "$trial" "$sum_mean_gbps" "$overlap_window_ms" "$client_count"
}

shm_cleanup() {
    shm_unlink "${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
    rm -f "/dev/shm${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
}

wait_for_broker_ports_free() {
    local timeout_sec="${BROKER_PORT_DRAIN_TIMEOUT_SEC:-20}"
    local deadline=$(( $(date +%s) + timeout_sec ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        local listeners=""
        if [ -n "${BROKER_HEARTBEAT_PORT:-}" ]; then
            listeners="$(ss -H -ltn "( sport = :${BROKER_DATA_PORT_BASE} or sport = :${BROKER_HEARTBEAT_PORT} )" 2>/dev/null || true)"
        else
            listeners="$(ss -H -ltn "sport = :${BROKER_DATA_PORT_BASE}" 2>/dev/null || true)"
        fi
        if [ -z "$listeners" ]; then
            return 0
        fi
        sleep 0.2
    done
    echo "WARNING: broker port drain timed out after ${timeout_sec}s; continuing with restart" >&2
    return 0
}

cleanup() {
    log "Cleaning up..."
    broker_local_cleanup
    if [[ "$SEQUENCER" == "CORFU" && -n "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        broker_remote_corfu_stop || true
  elif [[ "$SEQUENCER" == "LAZYLOG" && -n "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
        broker_remote_lazylog_stop || true
    fi
    if [[ "$SEQUENCER" == "SCALOG" && -n "$REMOTE_SCALOG_SEQUENCER_HOST" ]]; then
        broker_remote_scalog_stop || true
    fi
    shm_cleanup
    wait_for_broker_ports_free
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
    START_BROKERS_FAILURE_REASON=""
    log "Resetting previous broker state..."
    broker_local_cleanup
    shm_cleanup
    wait_for_broker_ports_free
    preflight_local_broker_resources

    if [[ "$SEQUENCER" == "CORFU" && -n "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        export REMOTE_CORFU_BUILD_BIN="${REMOTE_CORFU_BUILD_BIN:-$BUILD_BIN}"
        log "Starting remote Corfu sequencer on $REMOTE_CORFU_SEQUENCER_HOST..."
        if ! broker_remote_corfu_start; then
            echo "ERROR: failed to start remote Corfu sequencer on $REMOTE_CORFU_SEQUENCER_HOST" >&2
            return 1
        fi
        sleep 1
    elif [[ "$SEQUENCER" == "LAZYLOG" && -n "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
        export REMOTE_LAZYLOG_BUILD_BIN="${REMOTE_LAZYLOG_BUILD_BIN:-$BUILD_BIN}"
        log "Starting remote LazyLog sequencer on $REMOTE_LAZYLOG_SEQUENCER_HOST..."
        if ! broker_remote_lazylog_start; then
            echo "ERROR: failed to start remote LazyLog sequencer on $REMOTE_LAZYLOG_SEQUENCER_HOST" >&2
            return 1
        fi
        sleep 1
    elif [[ "$SEQUENCER" == "SCALOG" && -n "$REMOTE_SCALOG_SEQUENCER_HOST" ]]; then
        export REMOTE_SCALOG_BUILD_BIN="${REMOTE_SCALOG_BUILD_BIN:-$BUILD_BIN}"
        log "Starting remote Scalog sequencer on $REMOTE_SCALOG_SEQUENCER_HOST..."
        if ! broker_remote_scalog_start; then
            echo "ERROR: failed to start remote Scalog sequencer on $REMOTE_SCALOG_SEQUENCER_HOST" >&2
            return 1
        fi
        sleep 1
    fi

    cd "$BUILD_BIN"

    export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}"
    export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-full}"
    export EMBARCADERO_RUNTIME_MODE="throughput"
    export EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"
    export EMBARCADERO_HEAD_ADDR="$BROKER_IP"
    export EMBARCADERO_NUM_BROKERS="$NUM_BROKERS"
    export EMBARCADERO_ORDER0_FAST_PATH
    export EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES
    if [[ "$SEQUENCER" == "LAZYLOG" ]]; then
        export LAZYLOG_CXL_MODE=1
    fi
    if [[ "$SEQUENCER" == "LAZYLOG" && -n "${EMBARCADERO_LAZYLOG_SEQ_IP:-}" ]]; then
        export EMBARCADERO_LAZYLOG_SEQ_IP
    fi
    if [[ "$SEQUENCER" == "LAZYLOG" && -n "${EMBARCADERO_LAZYLOG_SEQ_PORT:-}" ]]; then
        export EMBARCADERO_LAZYLOG_SEQ_PORT
    fi
    if [[ "$SEQUENCER" == "SCALOG" ]]; then
        export SCALOG_CXL_MODE="${SCALOG_CXL_MODE:-1}"
    fi

    log "Starting $NUM_BROKERS broker(s) with NUMA bind: '${EMBARLET_NUMA_BIND}'"
    local -a launched_broker_pids=()
    if [[ "$SEQUENCER" == "CORFU" && -z "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        ./corfu_global_sequencer > /tmp/corfu_sequencer.log 2>&1 &
    elif [[ "$SEQUENCER" == "LAZYLOG" && -z "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
        ./lazylog_global_sequencer > /tmp/lazylog_sequencer.log 2>&1 &
    elif [[ "$SEQUENCER" == "SCALOG" ]]; then
        ./scalog_global_sequencer > /tmp/scalog_sequencer.log 2>&1 &
    fi

    # Start the head broker first. Cold CXL initialization and full-region zeroing are the
    # heaviest part of startup; launching followers concurrently increases memory pressure and
    # can OOM-kill broker 0 before the cluster even forms.
    # shellcheck disable=SC2086
    $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" --head "--${SEQUENCER}" \
        > /tmp/broker_0.log 2>&1 &
    launched_broker_pids+=("$!")
    HEAD_BROKER_PID="${launched_broker_pids[0]}"
    if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" 1 "${launched_broker_pids[0]}"; then
        echo "ERROR: head broker did not become ready within ${BROKER_READY_TIMEOUT_SEC}s" >&2
        cat /tmp/broker_0.log >&2
        START_BROKERS_FAILURE_REASON="$(classify_broker_startup_failure "${launched_broker_pids[@]}")"
        return 1
    fi
    if [[ "$BROKER_START_STAGGER_SEC" -gt 0 ]]; then
        sleep "$BROKER_START_STAGGER_SEC"
    fi
    for (( i=1; i<NUM_BROKERS; i++ )); do
        # shellcheck disable=SC2086
        $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" "--${SEQUENCER}" \
            > /tmp/broker_"$i".log 2>&1 &
        launched_broker_pids+=("$!")
        if [[ "$BROKER_START_STAGGER_SEC" -gt 0 ]]; then
            sleep "$BROKER_START_STAGGER_SEC"
        fi
    done

    log "Waiting for $NUM_BROKERS broker(s) to become ready (timeout: ${BROKER_READY_TIMEOUT_SEC}s)..."
    if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS" "${launched_broker_pids[@]}"; then
        echo "ERROR: Brokers did not become ready within ${BROKER_READY_TIMEOUT_SEC}s" >&2
        cat /tmp/broker_0.log >&2
        START_BROKERS_FAILURE_REASON="$(classify_broker_startup_failure "${launched_broker_pids[@]}")"
        log "Broker startup failure classification: ${START_BROKERS_FAILURE_REASON}"
        return 1
    fi
    # Clear ready sentinels so the next trial's wait starts from a clean state
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    if ! wait_for_broker_reachability; then
        echo "ERROR: Brokers are not reachable from client host(s) within ${BROKER_REACHABILITY_TIMEOUT_SEC}s" >&2
        START_BROKERS_FAILURE_REASON="infra_broker_unreachable"
        return 1
    fi
    if [[ "$BROKER_READY_PROPAGATION_SEC" -gt 0 ]]; then
        log "Waiting ${BROKER_READY_PROPAGATION_SEC}s for cluster state propagation..."
        sleep "$BROKER_READY_PROPAGATION_SEC"
    fi
    if ! precreate_embarcadero_order5_topic; then
        START_BROKERS_FAILURE_REASON="infra_order5_topic_precreate_failed"
        return 1
    fi
    if ! wait_for_order5_cluster_convergence; then
        return 1
    fi
    if [[ "$SEQUENCER" == "SCALOG" && -n "$REMOTE_SCALOG_SEQUENCER_HOST" ]]; then
        local precreate_settle_sec="${SCALOG_PRECREATE_SETTLE_SEC:-15}"
        if [[ "$precreate_settle_sec" -gt 0 ]]; then
            log "Waiting ${precreate_settle_sec}s for Scalog precreate settle..."
            sleep "$precreate_settle_sec"
        fi
        local precreate_attempt precreate_max_attempts precreate_retry_sleep_sec
        precreate_max_attempts="${SCALOG_PRECREATE_ATTEMPTS:-3}"
        precreate_retry_sleep_sec="${SCALOG_PRECREATE_RETRY_SLEEP_SEC:-3}"
        for (( precreate_attempt=1; precreate_attempt<=precreate_max_attempts; precreate_attempt++ )); do
            log "Precreating Scalog topic metadata (attempt ${precreate_attempt}/${precreate_max_attempts})..."
            (
                cd "$BUILD_BIN"
                EMBARCADERO_RUNTIME_MODE=throughput \
                EMBARCADERO_SCALOG_SEQ_IP="${EMBARCADERO_SCALOG_SEQ_IP:-}" \
                EMBARCADERO_SCALOG_SEQ_PORT="${EMBARCADERO_SCALOG_SEQ_PORT:-}" \
                SCALOG_CXL_MODE="${SCALOG_CXL_MODE:-1}" \
                ./throughput_test \
                    --config "$CLIENT_CONFIG_ABS" \
                    --head_addr "$BROKER_IP" \
                    -n "$NUM_BROKERS" \
                    -m "$MESSAGE_SIZE" \
                    -s 0 \
                    -t 5 \
                    -o "$ORDER" \
                    -a "$ACK" \
                    -r "$REPLICATION_FACTOR" \
                    --sequencer "$SEQUENCER" \
                    -l 0 \
                    >/tmp/scalog_topic_precreate.log 2>&1
            ) && break

            if [[ "$precreate_attempt" -lt "$precreate_max_attempts" ]]; then
                log "Scalog topic precreation attempt ${precreate_attempt} failed; retrying after ${precreate_retry_sleep_sec}s..."
                sleep "$precreate_retry_sleep_sec"
                continue
            fi

            echo "ERROR: Scalog topic precreation failed" >&2
            cat /tmp/scalog_topic_precreate.log >&2 || true
            return 1
        done
        log "Waiting for remote Scalog sequencer readiness..."
        if ! broker_wait_for_remote_scalog_ready "${SCALOG_READY_TIMEOUT_SEC:-30}" "$NUM_BROKERS" "$REPLICATION_FACTOR"; then
            echo "ERROR: remote Scalog sequencer did not reach full readiness" >&2
            return 1
        fi
        if [[ "$REPLICATION_FACTOR" -gt 1 ]]; then
            log "Waiting for local Scalog replication pollers to initialize..."
            if ! broker_wait_for_local_scalog_replication_ready "${SCALOG_REPLICATION_READY_TIMEOUT_SEC:-30}" "$NUM_BROKERS" "$REPLICATION_FACTOR"; then
                echo "ERROR: local Scalog replication did not reach full readiness" >&2
                return 1
            fi
        fi
    fi
    log "All $NUM_BROKERS brokers ready and reachable."
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
printf "  %-32s %s\n" "LOCAL_CLIENT_NUMA:"             "$(resolve_local_client_numa)"
if [[ "$SEQUENCER" == "CORFU" ]]; then
    printf "  %-32s %s\n" "CORFU_SEQ_IP:"               "${EMBARCADERO_CORFU_SEQ_IP:-"(unset)"}"
    printf "  %-32s %s\n" "CORFU_SEQ_PORT:"             "${EMBARCADERO_CORFU_SEQ_PORT:-"(default)"}"
elif [[ "$SEQUENCER" == "LAZYLOG" ]]; then
    printf "  %-32s %s\n" "LAZYLOG_SEQ_IP:"             "${EMBARCADERO_LAZYLOG_SEQ_IP:-"(unset)"}"
    printf "  %-32s %s\n" "LAZYLOG_SEQ_PORT:"           "${EMBARCADERO_LAZYLOG_SEQ_PORT:-"(default)"}"
elif [[ "$SEQUENCER" == "SCALOG" ]]; then
    printf "  %-32s %s\n" "SCALOG_SEQ_IP:"              "${EMBARCADERO_SCALOG_SEQ_IP:-"(unset)"}"
    printf "  %-32s %s\n" "SCALOG_SEQ_PORT:"            "${EMBARCADERO_SCALOG_SEQ_PORT:-"(default)"}"
    printf "  %-32s %s\n" "SCALOG_CXL_MODE:"            "${SCALOG_CXL_MODE:-1}"
fi
echo "================================================================"

mkdir -p "$LOG_DIR"
overall_status=0
ATTEMPT_SUMMARY_CSV="$LOG_DIR/attempt_summary.csv"
echo "trial,attempt,result,reason" > "$ATTEMPT_SUMMARY_CSV"
OVERLAP_SUMMARY_CSV="$LOG_DIR/overlap_summary.csv"
echo "trial,overlap_total_gbps,overlap_window_ms,timeseries_clients" > "$OVERLAP_SUMMARY_CSV"

if ! preflight_clients; then
    exit 1
fi

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
            echo "$trial,$attempt,failed,${START_BROKERS_FAILURE_REASON:-infra_broker_startup}" >> "$ATTEMPT_SUMMARY_CSV"
            for b in $(seq 0 $((NUM_BROKERS - 1))); do
                cp -f "/tmp/broker_${b}.log" "$LOG_DIR/trial${trial}_attempt${attempt}_broker${b}.log" 2>/dev/null || true
            done
            cleanup
            continue
        fi

        # ------------------------------------------------------------------
        # Compute synchronized barrier timestamp (NTP-synced wall clock)
        # All clients spin-wait until this exact millisecond.
        # ------------------------------------------------------------------
        effective_start_delay_sec="$START_DELAY_SEC"
        if has_remote_clients && [[ "$effective_start_delay_sec" -lt "$MIN_REMOTE_START_DELAY_SEC" ]]; then
            log "  START_DELAY_SEC=${START_DELAY_SEC}s too small for SSH clients; using ${MIN_REMOTE_START_DELAY_SEC}s"
            effective_start_delay_sec="$MIN_REMOTE_START_DELAY_SEC"
        fi
        START_TIME_MS=$(( $(date +%s%3N) + effective_start_delay_sec * 1000 ))
        log "  Barrier start time: ${START_TIME_MS} ms  (T+${effective_start_delay_sec}s)"

        # ------------------------------------------------------------------
        # Launch all clients in parallel
        # ------------------------------------------------------------------
        declare -a CLIENT_PIDS=()
        declare -a CLIENT_LOGS=()
        declare -a CLIENT_TS_LOCAL_FILES=()
        declare -a CLIENT_TAGS=()

        for (( i=0; i<NUM_CLIENTS; i++ )); do
            host="${CLIENT_HOSTS[$i]}"
            numa="$(resolve_client_numa "$host" "${CLIENT_NUMAS[$i]}")"
            host_occurrences=0
            for (( j=0; j<NUM_CLIENTS; j++ )); do
                if [[ "${CLIENT_HOSTS[$j]}" == "$host" ]]; then
                    host_occurrences=$((host_occurrences + 1))
                fi
            done
            client_tag="$host"
            if [[ "$host_occurrences" -gt 1 ]]; then
                client_tag="${host}${i}"
            fi
            log_file="$LOG_DIR/trial${trial}_${client_tag}.log"
            remote_build_bin="$BUILD_BIN"
            if [[ "$host" != "local" ]]; then
                remote_build_bin="$REMOTE_CLIENT_BIN_DIR"
            fi
            ts_file="$remote_build_bin/throughput_timeseries_trial${trial}_${client_tag}.csv"
            CLIENT_TAGS+=( "$client_tag" )
            CLIENT_LOGS+=( "$log_file" )
            CLIENT_TS_LOCAL_FILES+=( "$LOG_DIR/trial${trial}_${client_tag}_timeseries.csv" )

            # Build the command that will execute on the remote (or local) shell.
            # We use export statements so every env var is properly set regardless
            # of the remote shell's inherited environment.
            EXEC_CMD="$(cat <<ENDINNERSCRIPT
set -e
export EMBARCADERO_HEAD_ADDR=$BROKER_IP
export NUM_BROKERS=$NUM_BROKERS
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
if [ -n "${EMBARCADERO_ACK_TIMEOUT_SEC:-}" ]; then export EMBARCADERO_ACK_TIMEOUT_SEC=${EMBARCADERO_ACK_TIMEOUT_SEC:-}; fi
export EMBARCADERO_THROUGHPUT_TIMESERIES_FILE=$ts_file
export EMBARCADERO_THROUGHPUT_TIMESERIES_ORIGIN_MS=$START_TIME_MS
rm -f $ts_file
if [ "$SEQUENCER" = "CORFU" ] && [ -n "${EMBARCADERO_CORFU_SEQ_IP:-}" ]; then export EMBARCADERO_CORFU_SEQ_IP=${EMBARCADERO_CORFU_SEQ_IP:-}; fi
if [ "$SEQUENCER" = "CORFU" ] && [ -n "${EMBARCADERO_CORFU_SEQ_PORT:-}" ]; then export EMBARCADERO_CORFU_SEQ_PORT=${EMBARCADERO_CORFU_SEQ_PORT:-}; fi
if [ "$SEQUENCER" = "LAZYLOG" ] && [ -n "${EMBARCADERO_LAZYLOG_SEQ_IP:-}" ]; then export EMBARCADERO_LAZYLOG_SEQ_IP=${EMBARCADERO_LAZYLOG_SEQ_IP:-}; fi
if [ "$SEQUENCER" = "LAZYLOG" ] && [ -n "${EMBARCADERO_LAZYLOG_SEQ_PORT:-}" ]; then export EMBARCADERO_LAZYLOG_SEQ_PORT=${EMBARCADERO_LAZYLOG_SEQ_PORT:-}; fi
if [ "$SEQUENCER" = "SCALOG" ] && [ -n "${EMBARCADERO_SCALOG_SEQ_IP:-}" ]; then export EMBARCADERO_SCALOG_SEQ_IP=${EMBARCADERO_SCALOG_SEQ_IP:-}; fi
if [ "$SEQUENCER" = "SCALOG" ] && [ -n "${EMBARCADERO_SCALOG_SEQ_PORT:-}" ]; then export EMBARCADERO_SCALOG_SEQ_PORT=${EMBARCADERO_SCALOG_SEQ_PORT:-}; fi
if [ "$SEQUENCER" = "SCALOG" ]; then export SCALOG_CXL_MODE=${SCALOG_CXL_MODE:-1}; fi
if [ -n "$CLIENT_LD_LIBRARY_PATH" ]; then export LD_LIBRARY_PATH=$CLIENT_LD_LIBRARY_PATH; fi
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
cd $remote_build_bin
# Spin-wait until the synchronized barrier millisecond (requires NTP-synced clocks)
while [ \$(date +%s%3N) -lt $START_TIME_MS ]; do sleep 0.0005; done
numactl --cpunodebind=$numa --membind=$numa ./throughput_test --config $CLIENT_CONFIG_ABS -n $THREADS_PER_BROKER -m $MESSAGE_SIZE -s $LOAD_PER_CLIENT -t $TEST_TYPE -o $ORDER -a $ACK -r $REPLICATION_FACTOR --sequencer $SEQUENCER --head_addr $BROKER_IP -l 0
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

        # Collect per-client throughput timeseries from client hosts.
        for (( i=0; i<NUM_CLIENTS; i++ )); do
            host="${CLIENT_HOSTS[$i]}"
            client_tag="${CLIENT_TAGS[$i]}"
            local_ts="$LOG_DIR/trial${trial}_${client_tag}_timeseries.csv"
            remote_ts="$BUILD_BIN/throughput_timeseries_trial${trial}_${client_tag}.csv"
            if [[ "$host" != "local" ]]; then
                remote_ts="$REMOTE_CLIENT_BIN_DIR/throughput_timeseries_trial${trial}_${client_tag}.csv"
            fi
            if [[ "$host" == "local" ]]; then
                cp "$remote_ts" "$local_ts" 2>/dev/null || true
            else
                scp -o StrictHostKeyChecking=no "$host:$remote_ts" "$local_ts" >/dev/null 2>&1 || true
            fi
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
            overlap_row="$(compute_overlap_throughput_gbps "$trial" "${CLIENT_TS_LOCAL_FILES[@]}" || true)"
            if [[ -n "$overlap_row" ]]; then
                echo "$overlap_row" >> "$OVERLAP_SUMMARY_CSV"
            fi
            trial_success=1
            echo "$trial,$attempt,success,ok" >> "$ATTEMPT_SUMMARY_CSV"
            break
        fi

        echo "WARNING: trial $trial attempt $attempt incomplete — retrying..." >&2
        echo "$trial,$attempt,failed,incomplete_or_missing_bandwidth" >> "$ATTEMPT_SUMMARY_CSV"
        for b in $(seq 0 $((NUM_BROKERS - 1))); do
            cp -f "/tmp/broker_${b}.log" "$LOG_DIR/trial${trial}_attempt${attempt}_broker${b}.log" 2>/dev/null || true
        done
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
        client_tag="${CLIENT_TAGS[$i]:-$host}"
        log_file="$LOG_DIR/trial${trial}_${client_tag}.log"
        bw_line=$(grep -i "bandwidth:" "$log_file" 2>/dev/null | tail -1 || echo "")
        # glog prefix: "I0326 HH:MM:SS PID file:line] Bandwidth: VALUE UNIT"
        # extract the number that follows "Bandwidth:" regardless of prefix fields
        bw_val=$(echo "$bw_line" | grep -oiP 'bandwidth:\s*\K[0-9]+(\.[0-9]+)?' || true)
        bw_unit=$(echo "$bw_line" | awk '{for(i=1;i<NF;i++) if($i=="Bandwidth:") {print $(i+2); exit}}' || true)
        printf "  %-8s → %s %s\n" "$client_tag" "${bw_val:-N/A}" "${bw_unit:-}"
        if [[ "$bw_val" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            total_bw_mbs=$(awk "BEGIN {printf \"%.2f\", $total_bw_mbs + $bw_val}")
        fi
    done
    total_gbs=$(awk "BEGIN {printf \"%.3f\", $total_bw_mbs / 1024}")
    echo "  ────────────────────────────────────────"
    printf "  %-8s → %s MB/s  (%s GB/s)\n" "TOTAL" "$total_bw_mbs" "$total_gbs"
    overlap_line="$(awk -F',' -v t="$trial" 'NR>1 && $1==t {print $0; exit}' "$OVERLAP_SUMMARY_CSV" || true)"
    if [[ -n "$overlap_line" ]]; then
        overlap_gbps="$(echo "$overlap_line" | awk -F',' '{print $2}')"
        overlap_window_ms="$(echo "$overlap_line" | awk -F',' '{print $3}')"
        overlap_clients="$(echo "$overlap_line" | awk -F',' '{print $4}')"
        printf "  %-8s → %s GB/s  (window=%sms clients=%s)\n" "OVERLAP" "$overlap_gbps" "$overlap_window_ms" "$overlap_clients"
    fi
    echo "  (naive sum of per-client averages; see OSDI/SOSP note below)"

    if [[ "$trial_success" -ne 1 ]]; then
        echo "ERROR: Trial $trial failed after $TRIAL_MAX_ATTEMPTS attempts." >&2
        if ! awk -F',' -v t="$trial" '$1==t && $3=="success"{found=1} END{exit !found}' "$ATTEMPT_SUMMARY_CSV"; then
            echo "$trial,$TRIAL_MAX_ATTEMPTS,failed,max_attempts_exhausted" >> "$ATTEMPT_SUMMARY_CSV"
        fi
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
        client_tag="$host"
        host_occurrences=0
        for (( j=0; j<NUM_CLIENTS; j++ )); do
            if [[ "${CLIENT_HOSTS[$j]}" == "$host" ]]; then
                host_occurrences=$((host_occurrences + 1))
            fi
        done
        if [[ "$host_occurrences" -gt 1 ]]; then
            client_tag="${host}${i}"
        fi
        log_file="$LOG_DIR/trial${trial}_${client_tag}.log"
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
