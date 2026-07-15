#!/usr/bin/env bash
# Delayed developer throughput campaign.  Never kills another experiment: it
# waits until the broker and remote clients are idle, then launches the E2 N=1
# matrix with the real 256 GiB CXL capacity declared explicitly.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

INITIAL_DELAY_SEC="${INITIAL_DELAY_SEC:-1800}"
RECHECK_DELAY_SEC="${RECHECK_DELAY_SEC:-600}"
RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)_baseline_throughput_c4}"
OUT_BASE="${OUT_BASE:-$ROOT/data/overnight_eval/$RUN_TAG}"
LOG_FILE="${LOG_FILE:-/tmp/overnight_${RUN_TAG}.log}"
META_ROOT="$ROOT/.Replication/lazylog_metadata/$RUN_TAG"
LOCK_FILE="${LOCK_FILE:-/tmp/embarcadero_delayed_baseline_throughput.lock}"

exec 9>"$LOCK_FILE"
flock -n 9 || { echo "ERROR: another delayed baseline campaign owns $LOCK_FILE" >&2; exit 1; }

is_cluster_busy() {
    local host
    # Exact executable names avoid matching this launcher itself.  The SMR
    # driver is included because it can create local brokers underneath it.
    if pgrep -x embarlet >/dev/null || pgrep -x throughput_test >/dev/null ||
       pgrep -f '[r]un_smr_fifo_eval\.sh' >/dev/null ||
       pgrep -f '[r]un_overnight_eval\.sh' >/dev/null ||
       pgrep -f '[r]un_multiclient\.sh' >/dev/null; then
        return 0
    fi
    for host in c4 c3 c1; do
        if ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" \
            'pgrep -x throughput_test >/dev/null' 2>/dev/null; then
            return 0
        fi
    done
    return 1
}

metadata_pids=()
cleanup_metadata() {
    local pid
    for pid in "${metadata_pids[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup_metadata EXIT INT TERM

echo "[$(date -u +%FT%TZ)] delayed baseline campaign tag=$RUN_TAG"
echo "[$(date -u +%FT%TZ)] waiting ${INITIAL_DELAY_SEC}s before first cleanliness check"
sleep "$INITIAL_DELAY_SEC"
while is_cluster_busy; do
    echo "[$(date -u +%FT%TZ)] cluster busy; retrying in ${RECHECK_DELAY_SEC}s"
    sleep "$RECHECK_DELAY_SEC"
done

mkdir -p "$META_ROOT/a" "$META_ROOT/b"
# Do not let the detached sidecars inherit the campaign lock (fd 9).  More
# importantly, retain this shell as their supervisor below: using `exec` for
# the sweep would bypass cleanup_metadata and leave both ports and the lock
# owned after an interrupted campaign.
setsid "$ROOT/build/bin/lazylog_metadata_replica" \
    --listen 0.0.0.0:50081 --sidecar "$META_ROOT/a/metadata.sidecar" \
    >"$META_ROOT/replica_a.log" 2>&1 < /dev/null 9>&- &
metadata_pids+=("$!")
setsid "$ROOT/build/bin/lazylog_metadata_replica" \
    --listen 0.0.0.0:50082 --sidecar "$META_ROOT/b/metadata.sidecar" \
    >"$META_ROOT/replica_b.log" 2>&1 < /dev/null 9>&- &
metadata_pids+=("$!")
sleep 1
for pid in "${metadata_pids[@]}"; do
    kill -0 "$pid" 2>/dev/null || { echo "ERROR: metadata sidecar failed to start" >&2; exit 1; }
done

ONLY_CELLS='e2_embar5_rf0_ack1_n1,e2_embar5_rf1_ack1_n1,e2_embar5_rf2_ack2_n1,e2_embar0_rf0_ack1_n1,e2_embar0_rf1_ack1_n1,e2_corfu_rf0_n1,e2_scalog_rf0_n1,e2_lazylog_rf0_n1,e2_corfu_rf2_ack2_n1,e2_scalog_rf2_ack2_n1,e2_lazylog_rf2_ack2_n1'
CLIENT_LIB='/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib'

env \
    RUN_TAG="$RUN_TAG" OUT_BASE="$OUT_BASE" \
    SKIP_CLUSTER_SETUP=1 ALLOW_DIRTY_ARTIFACT=1 \
    ONLY_CELLS="$ONLY_CELLS" SKIP_BASELINES=0 INCLUDE_DURABLE_BASELINES_RF2=1 \
    NUM_TRIALS=4 WARMUP_TRIALS=1 TOTAL_BYTES=4294967296 MSG_SIZE=1024 \
    NUM_BROKERS=4 THREADS_THROUGHPUT=6 EPOCH_US_THROUGHPUT=500 \
    INCLUDE_MEMORY_COPY_RF2=0 CLIENT_HOSTS_REMOTE=c4 CLIENT_HOSTS_E2=c4 \
    BROKER_IP=10.10.10.10 BROKER_IP_MULTI=10.10.10.10 \
    EMBARCADERO_CXL_SIZE=274877906944 \
    EMBARCADERO_CXL_ZERO_MODE=metadata EMBARCADERO_CXL_MAP_POPULATE=0 \
    EMBARCADERO_ACK_TIMEOUT_SEC=300 \
    EMBARCADERO_REPLICA_DISK_DIRS="$ROOT/.Replication/disk0,$ROOT/.Replication/disk1" \
    EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS='10.10.10.10:50081,10.10.10.10:50082' \
    CLIENT_LD_LIBRARY_PATH="$CLIENT_LIB" \
    bash "$SCRIPT_DIR/run_overnight_eval.sh"
