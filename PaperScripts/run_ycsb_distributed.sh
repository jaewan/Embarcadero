#!/usr/bin/env bash
# PaperScripts/run_ycsb_distributed.sh
#
# Distributed YCSB driver for the preregistered matrix in
# docs/experiments/YCSB_DISTRIBUTED_KV_PLAN.md (Section 4/Section 6 item 3).
# Launches kv_ycsb_bench across 1-3 remote client hosts (c4, c3, c1) against
# a local (moscxl) broker cluster: each client gets a disjoint --key_offset
# range over one --shared_topic, exactly one client (the first) owns
# --manage_cluster, and all clients spin-wait a synchronized millisecond
# barrier before starting so the timed window overlaps across hosts.
#
# Per the plan doc's Section 6 item 3 decision: this is a NEW script under
# PaperScripts/ (paper-honest defaults, data/paper_eval/ output), not an
# extension of run_multiclient.sh (which only launches throughput_test) and
# not scripts/run_ycsb_eval.sh (local-only, RF0, host-wide pkill teardown —
# see the plan doc Section 2/Q6/Q8 audit). It reuses run_multiclient.sh's
# campaign lock file directly (never nest a second flock) and
# scripts/lib/broker_lifecycle.sh for all local cluster lifecycle, and
# mirrors run_multiclient.sh's NUMA pinning, millisecond barrier, and
# exact-PID-file remote teardown conventions rather than inventing new ones.
#
# Usage:
#   NUM_CLIENTS=1 SYSTEM=EMBARCADERO WORKLOAD=A bash PaperScripts/run_ycsb_distributed.sh
#   NUM_CLIENTS=2 SYSTEM=CORFU        WORKLOAD=F bash PaperScripts/run_ycsb_distributed.sh
#   NUM_CLIENTS=3 SYSTEM=SCALOG       WORKLOAD=A bash PaperScripts/run_ycsb_distributed.sh
#
# Key knobs (all overrideable via environment):
#   NUM_CLIENTS           1-3 remote clients, drawn in order from (c4 c3 c1)
#   SYSTEM                EMBARCADERO | CORFU | SCALOG (LazyLog excluded —
#                         plan doc Section 3: no faithful DRAM-only read path)
#   WORKLOAD              A | F (B only if explicitly requested)
#   RECORD_COUNT_TOTAL    total keyspace across all clients (plan doc: >=1e6
#                         for the real matrix; small for smokes)
#   OPERATION_COUNT_PER_CLIENT
#   REPLICATION_FACTOR / ACK   plan doc default: RF=2 ACK=2 (DRAM replica
#                         completion — label it that way, never "durable")
#   NUM_BROKERS           default 4
#   CLIENT_NODES_CSV      override the (c4,c3,c1) host order

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN_DIR="${BIN_DIR:-$PROJECT_ROOT/build/bin}"
source "$PROJECT_ROOT/scripts/lib/broker_lifecycle.sh"

export PROJECT_ROOT
broker_init_paths

# ---------------------------------------------------------------------------
# Campaign lock — the SAME file run_multiclient.sh uses, so this driver and
# any concurrent throughput_test campaign never both think the cluster is
# free. Do not wrap this in a second flock elsewhere (PaperScripts/README.md).
# ---------------------------------------------------------------------------
RUN_LOCK_FILE="${RUN_LOCK_FILE:-/tmp/embarcadero_run_multiclient.lock}"
exec {RUN_LOCK_FD}>"$RUN_LOCK_FILE"
if ! flock -n "$RUN_LOCK_FD"; then
    echo "ERROR: another benchmark orchestrator is already running (lock: $RUN_LOCK_FILE)." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Client topology — same hosts/NUMA convention as run_multiclient.sh.
# ---------------------------------------------------------------------------
declare -a ALL_CLIENT_HOSTS=(c4 c3 c1)
declare -a ALL_CLIENT_NUMAS=(1 1 0)
if [[ -n "${CLIENT_NODES_CSV:-}" ]]; then
    IFS=',' read -r -a ALL_CLIENT_HOSTS <<< "$CLIENT_NODES_CSV"
fi

NUM_CLIENTS="${NUM_CLIENTS:-1}"
if [[ "$NUM_CLIENTS" -lt 1 || "$NUM_CLIENTS" -gt "${#ALL_CLIENT_HOSTS[@]}" ]]; then
    echo "ERROR: NUM_CLIENTS=$NUM_CLIENTS out of range (have ${#ALL_CLIENT_HOSTS[@]} client hosts: ${ALL_CLIENT_HOSTS[*]})" >&2
    exit 1
fi

SYSTEM="${SYSTEM:-EMBARCADERO}"
case "$SYSTEM" in
    EMBARCADERO) SEQUENCER=EMBARCADERO; ORDER=5 ;;
    CORFU)       SEQUENCER=CORFU; ORDER=2 ;;
    SCALOG)      SEQUENCER=SCALOG; ORDER=1 ;;
    *) echo "ERROR: SYSTEM=$SYSTEM unsupported (EMBARCADERO|CORFU|SCALOG — LazyLog is excluded per the plan doc, Section 3)" >&2; exit 1 ;;
esac

WORKLOAD="${WORKLOAD:-A}"
RECORD_COUNT_TOTAL="${RECORD_COUNT_TOTAL:-6000}"
OPERATION_COUNT_PER_CLIENT="${OPERATION_COUNT_PER_CLIENT:-4000}"
VALUE_SIZE="${VALUE_SIZE:-100}"
ZIPF_THETA="${ZIPF_THETA:-0.99}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-2}"
ACK="${ACK:-2}"
NUM_BROKERS="${NUM_BROKERS:-4}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-120}"
START_DELAY_SEC="${START_DELAY_SEC:-5}"
MIN_REMOTE_START_DELAY_SEC="${MIN_REMOTE_START_DELAY_SEC:-5}"
REMOTE_PROJECT_ROOT="${REMOTE_PROJECT_ROOT:-$HOME/Embarcadero}"
CLIENT_LD_LIBRARY_PATH="${CLIENT_LD_LIBRARY_PATH:-/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib}"

if [[ "$RECORD_COUNT_TOTAL" -lt "$NUM_CLIENTS" ]]; then
    echo "ERROR: RECORD_COUNT_TOTAL=$RECORD_COUNT_TOTAL smaller than NUM_CLIENTS=$NUM_CLIENTS" >&2
    exit 1
fi
RECORD_COUNT_PER_CLIENT=$(( RECORD_COUNT_TOTAL / NUM_CLIENTS ))

RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/build/results/ycsb_distributed_${SYSTEM}_${WORKLOAD}_n${NUM_CLIENTS}_${RUN_TAG}}"
mkdir -p "$OUT_ROOT"

GIT_COMMIT="$(git -C "$PROJECT_ROOT" rev-parse HEAD)"
GIT_DIRTY="$(git -C "$PROJECT_ROOT" status --porcelain | wc -l)"
if [[ "$GIT_DIRTY" -ne 0 && "${ALLOW_DIRTY_ARTIFACT:-0}" != "1" ]]; then
    echo "ERROR: refusing to run from a dirty tree ($GIT_DIRTY changed files)." >&2
    echo "       Set ALLOW_DIRTY_ARTIFACT=1 to override for a non-publication smoke." >&2
    exit 1
fi

echo "=== YCSB distributed run ==="
echo "  system=$SYSTEM sequencer=$SEQUENCER order=$ORDER workload=$WORKLOAD"
echo "  clients=$NUM_CLIENTS hosts=${ALL_CLIENT_HOSTS[*]:0:$NUM_CLIENTS}"
echo "  record_count_total=$RECORD_COUNT_TOTAL (per_client=$RECORD_COUNT_PER_CLIENT) operation_count_per_client=$OPERATION_COUNT_PER_CLIENT"
echo "  rf=$REPLICATION_FACTOR ack=$ACK (DRAM replica completion — never label RF>=2/ACK=2 'durable' without the disk-durable sink)"
echo "  num_brokers=$NUM_BROKERS commit=$GIT_COMMIT dirty=$GIT_DIRTY"
echo "  out_root=$OUT_ROOT"

# ---------------------------------------------------------------------------
# Preflight: refuse a live cluster/client host-wide (matches
# run_kv_baseline_compare.sh's guard, adapted for kv_ycsb_bench too, which
# broker_lifecycle.sh's name-based allowlist does not cover).
# ---------------------------------------------------------------------------
for name in embarlet kv_ycsb_bench corfu_global_sequencer scalog_global_sequencer lazylog_global_sequencer; do
    while IFS= read -r pid; do
        [[ -n "$pid" ]] || continue
        state="$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null || true)"
        if [[ -n "$state" && "$state" != "Z" ]]; then
            echo "ERROR: live $name pid=$pid exists locally; refusing host-wide cleanup" >&2
            exit 1
        fi
    done < <(pgrep -x "$name" 2>/dev/null || true)
done
for (( i = 0; i < NUM_CLIENTS; i++ )); do
    host="${ALL_CLIENT_HOSTS[$i]}"
    if ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" "pgrep -x kv_ycsb_bench >/dev/null 2>&1"; then
        echo "ERROR: $host already has a live kv_ycsb_bench process; refusing to launch another" >&2
        exit 1
    fi
done

# ---------------------------------------------------------------------------
# Local cluster lifecycle
# ---------------------------------------------------------------------------
DRIVER_DONE=0
DRIVER_OWNS_CLUSTER=0
DRIVER_CXL_SHM_NAME=""
declare -a CLIENT_REMOTE_PID_HOSTS=()
declare -a CLIENT_REMOTE_PID_FILES=()

unlink_kvbase_shm() {
    [[ -n "$DRIVER_CXL_SHM_NAME" ]] || return 0
    case "$DRIVER_CXL_SHM_NAME" in
        /CXL_KVBASE_"${UID}"_*) rm -f "/dev/shm/${DRIVER_CXL_SHM_NAME#/}" 2>/dev/null || true ;;
        *) echo "ERROR: refusing to unlink unexpected SHM name: $DRIVER_CXL_SHM_NAME" >&2 ;;
    esac
}
remote_teardown() {
    # Exact-PID-file kill only — never a broad remote pkill (run_multiclient.sh
    # convention: remote hosts are shared by independent experimenters).
    for (( i = 0; i < ${#CLIENT_REMOTE_PID_FILES[@]}; i++ )); do
        local h="${CLIENT_REMOTE_PID_HOSTS[$i]}" pf="${CLIENT_REMOTE_PID_FILES[$i]}"
        ssh -o BatchMode=yes "$h" \
            "if test -r '$pf'; then read -r p < '$pf'; kill -TERM \"\$p\" 2>/dev/null || true; sleep 0.2; kill -KILL \"\$p\" 2>/dev/null || true; rm -f '$pf'; fi" \
            2>/dev/null || true
    done
}
cleanup() {
    [[ "$DRIVER_DONE" == 1 ]] && return 0
    remote_teardown
    [[ "$DRIVER_OWNS_CLUSTER" == 1 ]] && broker_local_cleanup
    unlink_kvbase_shm
}
trap cleanup EXIT

DRIVER_OWNS_CLUSTER=1
broker_local_cleanup
DRIVER_CXL_SHM_NAME="/CXL_KVBASE_${UID}_ycsbdist_$$_$(date +%s)_${RANDOM}"
export EMBARCADERO_CXL_SHM_NAME="$DRIVER_CXL_SHM_NAME"
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-137438953472}"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-0}"
export EMBARCADERO_HEAD_ADDR="$BROKER_IP"
export EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"
export NUM_BROKERS EMBARCADERO_NUM_BROKERS="$NUM_BROKERS"
export EMBARCADERO_CORFU_SEQ_IP="${EMBARCADERO_CORFU_SEQ_IP:-$BROKER_IP}"
export EMBARCADERO_SCALOG_SEQ_IP="${EMBARCADERO_SCALOG_SEQ_IP:-$BROKER_IP}"
unlink_kvbase_shm
sleep 0.5

if command -v numactl >/dev/null 2>&1 && numactl -H 2>/dev/null | grep -qE '^node 1 cpus:'; then
    if numactl -H 2>/dev/null | grep -qE '^node 2 cpus:'; then
        EMBARLET_NUMA_ARR=(numactl --cpunodebind=1 --membind=1,2)
    else
        EMBARLET_NUMA_ARR=(numactl --cpunodebind=1 --membind=1)
    fi
else
    EMBARLET_NUMA_ARR=()
fi

if [[ "$SEQUENCER" == "CORFU" ]]; then
    "$BIN_DIR/corfu_global_sequencer" >>"$OUT_ROOT/corfu_sequencer.log" 2>&1 &
    sleep 0.3
elif [[ "$SEQUENCER" == "SCALOG" ]]; then
    "$BIN_DIR/scalog_global_sequencer" >>"$OUT_ROOT/scalog_sequencer.log" 2>&1 &
    for _ in $(seq 1 100); do
        ss -H -ltn "sport = :${EMBARCADERO_SCALOG_SEQ_PORT:-50051}" 2>/dev/null | grep -q . && break
        sleep 0.1
    done
fi

echo "Starting $NUM_BROKERS-broker $SYSTEM cluster..."
broker_pids=()
env EMBARCADERO_RUNTIME_MODE=throughput SCALOG_CXL_MODE=1 \
    "$BIN_DIR/embarlet" --config "$BROKER_CONFIG_ABS" --head --"$SEQUENCER" \
    >"$OUT_ROOT/broker_0.log" 2>&1 &
broker_pids+=("$!")
for (( i = 1; i < NUM_BROKERS; i++ )); do
    env EMBARCADERO_RUNTIME_MODE=throughput SCALOG_CXL_MODE=1 \
        "$BIN_DIR/embarlet" --config "$BROKER_CONFIG_ABS" --"$SEQUENCER" \
        >"$OUT_ROOT/broker_${i}.log" 2>&1 &
    broker_pids+=("$!")
done
if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS" "${broker_pids[@]}"; then
    echo "ERROR: brokers not ready" >&2
    exit 1
fi
rm -f /tmp/embarlet_*_ready 2>/dev/null || true
sleep 4

# ---------------------------------------------------------------------------
# Synchronized millisecond barrier (run_multiclient.sh convention: uutils
# date on c3 does not support %3N, so compute via %s%N/1e6, not %s%3N).
# ---------------------------------------------------------------------------
effective_start_delay_sec="$START_DELAY_SEC"
if [[ "$NUM_CLIENTS" -gt 0 && "$effective_start_delay_sec" -lt "$MIN_REMOTE_START_DELAY_SEC" ]]; then
    effective_start_delay_sec="$MIN_REMOTE_START_DELAY_SEC"
fi
START_TIME_MS=$(( $(date +%s%3N) + effective_start_delay_sec * 1000 ))
echo "Barrier start time: ${START_TIME_MS} ms (T+${effective_start_delay_sec}s)"

client_pids=()
for (( i = 0; i < NUM_CLIENTS; i++ )); do
    host="${ALL_CLIENT_HOSTS[$i]}"
    numa="${ALL_CLIENT_NUMAS[$i]}"
    log_file="$OUT_ROOT/client_${i}_${host}.log"
    remote_pid_file="/tmp/embarcadero_ycsb_dist_${RUN_TAG}_${host}_$$_pid"
    CLIENT_REMOTE_PID_HOSTS+=("$host")
    CLIENT_REMOTE_PID_FILES+=("$remote_pid_file")

    manage_cluster=0
    (( i == 0 )) && manage_cluster=1
    shared_topic_flag=""
    (( NUM_CLIENTS > 1 )) && shared_topic_flag="--shared_topic"
    key_offset=$(( i * RECORD_COUNT_PER_CLIENT ))

    EXEC_CMD="$(cat <<ENDINNERSCRIPT
set -e
export EMBARCADERO_HEAD_ADDR=$BROKER_IP
export EMBARCADERO_CORFU_SEQ_IP=$BROKER_IP
export EMBARCADERO_SCALOG_SEQ_IP=$BROKER_IP
if [ -n "$CLIENT_LD_LIBRARY_PATH" ]; then export LD_LIBRARY_PATH=$CLIENT_LD_LIBRARY_PATH; fi
cd $REMOTE_PROJECT_ROOT/build/bin
while [ \$(( \$(date +%s%N) / 1000000 )) -lt $START_TIME_MS ]; do sleep 0.0005; done
__bar_now_ms=\$(( \$(date +%s%N) / 1000000 ))
if [ \$(( __bar_now_ms - $START_TIME_MS )) -gt 2000 ]; then
  echo "WARNING: BARRIER MISSED by \$(( __bar_now_ms - $START_TIME_MS )) ms — host clock skewed vs broker; concurrency of this trial is suspect" >&2
fi
echo \$\$ > $remote_pid_file
exec numactl --cpunodebind=$numa --membind=$numa ./kv_ycsb_bench \\
  --sequencer=$SEQUENCER --order=$ORDER --ack=$ACK --rf=$REPLICATION_FACTOR \\
  --workload=$WORKLOAD --zipf_theta=$ZIPF_THETA \\
  --record_count=$RECORD_COUNT_PER_CLIENT --operation_count=$OPERATION_COUNT_PER_CLIENT \\
  --value_size=$VALUE_SIZE --batch_size=1 --warmup_ops=0 \\
  --key_offset=$key_offset --manage_cluster=$manage_cluster $shared_topic_flag \\
  --broker_ip=$BROKER_IP --latency \\
  --run_id=${RUN_TAG}_c${i} --output_dir=$REMOTE_PROJECT_ROOT/build/results/ycsb_distributed_out
ENDINNERSCRIPT
)"

    echo "Launching client[$i] host=$host numa=$numa key_offset=$key_offset manage_cluster=$manage_cluster"
    ssh -o BatchMode=yes "$host" "mkdir -p $REMOTE_PROJECT_ROOT/build/results/ycsb_distributed_out; $EXEC_CMD" \
        >"$log_file" 2>&1 &
    client_pids+=("$!")
done

echo "Waiting for ${#client_pids[@]} client(s)..."
all_ok=1
for pid in "${client_pids[@]}"; do
    wait "$pid" || all_ok=0
done

cleanup
DRIVER_DONE=1

echo ""
echo "=== Per-client summary ==="
fail=0
for (( i = 0; i < NUM_CLIENTS; i++ )); do
    host="${ALL_CLIENT_HOSTS[$i]}"
    log_file="$OUT_ROOT/client_${i}_${host}.log"
    ops_line="$(grep -oP 'kv_bench_main\.cc:\d+\] \KOps:.*' "$log_file" 2>/dev/null | tail -1 || true)"
    valid_line="$(grep -oP 'valid=(YES|NO)' "$log_file" 2>/dev/null | tail -1 || true)"
    echo "client[$i] host=$host: ${ops_line:-<no Ops line found>}  ${valid_line:-<no valid= found>}"
    if [[ "$valid_line" != "valid=YES" ]]; then
        echo "  FAIL: client[$i] ($host) did not report valid=YES" >&2
        fail=1
    fi
done
if [[ "$all_ok" -ne 1 ]]; then
    echo "FAIL: at least one client process exited non-zero" >&2
    fail=1
fi

if [[ "$fail" -eq 0 ]]; then
    echo "PASS: $SYSTEM workload=$WORKLOAD n=$NUM_CLIENTS completed and drained cleanly"
else
    echo "Full logs kept under $OUT_ROOT"
fi
exit "$fail"
