#!/usr/bin/env bash
# PaperScripts/run_e2_throughput_matrix.sh
#
# Paper E2 N=1 throughput matrix (Embar O5/O0 + baselines). Forked from
# scripts/run_baseline_throughput_when_clean.sh; waits for an idle cluster,
# then runs PaperScripts/run_overnight_eval.sh with paper TP defaults.
#
# Requires: join-watchdog fix (9c6aea99+), client binary deployed to c4 with
# compatible libs (CLIENT_LD_LIBRARY_PATH below).
set -euo pipefail

PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$PAPER_DIR/.." && pwd)"
cd "$ROOT"

INITIAL_DELAY_SEC="${INITIAL_DELAY_SEC:-0}"
RECHECK_DELAY_SEC="${RECHECK_DELAY_SEC:-600}"
RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)_paper_e2_tp}"
OUT_BASE="${OUT_BASE:-$ROOT/data/paper_eval/$RUN_TAG}"
LOG_FILE="${LOG_FILE:-/tmp/paper_e2_${RUN_TAG}.log}"
META_ROOT="$ROOT/.Replication/lazylog_metadata/$RUN_TAG"
LOCK_FILE="${LOCK_FILE:-/tmp/embarcadero_paper_e2_throughput.lock}"

exec 9>"$LOCK_FILE"
flock -n 9 || { echo "ERROR: another paper E2 campaign owns $LOCK_FILE" >&2; exit 1; }

is_cluster_busy() {
    local host
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

echo "[$(date -u +%FT%TZ)] paper E2 campaign tag=$RUN_TAG out=$OUT_BASE"
if [[ "$INITIAL_DELAY_SEC" -gt 0 ]]; then
    echo "[$(date -u +%FT%TZ)] waiting ${INITIAL_DELAY_SEC}s before first cleanliness check"
    sleep "$INITIAL_DELAY_SEC"
fi
while is_cluster_busy; do
    echo "[$(date -u +%FT%TZ)] cluster busy; retrying in ${RECHECK_DELAY_SEC}s"
    sleep "$RECHECK_DELAY_SEC"
done

mkdir -p "$META_ROOT/a" "$META_ROOT/b"
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
CLIENT_LIB="${CLIENT_LD_LIBRARY_PATH:-/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib}"

# Paper TP defaults (also set inside run_overnight_eval.sh; explicit here for clarity).
env \
    RUN_TAG="$RUN_TAG" OUT_BASE="$OUT_BASE" \
    SKIP_CLUSTER_SETUP="${SKIP_CLUSTER_SETUP:-1}" ALLOW_DIRTY_ARTIFACT=1 \
    ONLY_CELLS="$ONLY_CELLS" SKIP_BASELINES=0 INCLUDE_DURABLE_BASELINES_RF2=1 \
    NUM_TRIALS="${NUM_TRIALS:-4}" WARMUP_TRIALS="${WARMUP_TRIALS:-1}" \
    TOTAL_BYTES="${TOTAL_BYTES:-4294967296}" MSG_SIZE="${MSG_SIZE:-1024}" \
    NUM_BROKERS=4 THREADS_THROUGHPUT="${THREADS_THROUGHPUT:-6}" \
    EPOCH_US_THROUGHPUT="${EPOCH_US_THROUGHPUT:-500}" \
    CLIENT_PUB_BATCH_KB="${CLIENT_PUB_BATCH_KB:-2048}" \
    INCLUDE_MEMORY_COPY_RF2=0 CLIENT_HOSTS_REMOTE=c4 CLIENT_HOSTS_E2=c4 \
    BROKER_IP=10.10.10.10 BROKER_IP_MULTI=10.10.10.10 \
    EMBARCADERO_CXL_SIZE=274877906944 \
    EMBARCADERO_CXL_ZERO_MODE=metadata EMBARCADERO_CXL_MAP_POPULATE=0 \
    EMBARCADERO_ACK_TIMEOUT_SEC=300 \
    EMBARCADERO_REPLICA_DISK_DIRS="$ROOT/.Replication/disk0,$ROOT/.Replication/disk1" \
    LAZYLOG_RF2_METADATA_ENDPOINTS='10.10.10.10:50081,10.10.10.10:50082' \
    CLIENT_LD_LIBRARY_PATH="$CLIENT_LIB" \
    bash "$PAPER_DIR/run_overnight_eval.sh" | tee -a "$LOG_FILE"
status=${PIPESTATUS[0]}
exit "$status"
