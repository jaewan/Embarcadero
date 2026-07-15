#!/usr/bin/env bash
# Run the remaining single-remote E2 throughput cells as a reproducible,
# detached campaign.  It deliberately does not run latency/overhead cells and
# does not touch c1/c2/c3: c4 is the clean, NUMA-local 100G client used for
# the N=1 comparison.  N=2/N=3 remain separate campaigns until each remote
# worktree has explicit clean provenance.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$ROOT"

START_DELAY_SEC="${START_DELAY_SEC:-7200}"
RUN_TAG="${RUN_TAG:-$(date -u -d "+${START_DELAY_SEC} seconds" +%Y%m%dT%H%M%SZ)_remaining_throughput}"
CAMPAIGN_LOG="${CAMPAIGN_LOG:-/tmp/embarcadero_${RUN_TAG}.log}"
CAMPAIGN_LOCK="${CAMPAIGN_LOCK:-/tmp/embarcadero_remaining_throughput.lock}"
META_ROOT="${LAZYLOG_METADATA_ROOT:-$ROOT/.Replication/lazylog_metadata/$RUN_TAG}"
META_PIDS=()

exec 9>"$CAMPAIGN_LOCK"
if ! flock -n 9; then
    echo "ERROR: another remaining-throughput campaign owns $CAMPAIGN_LOCK" >&2
    exit 1
fi

cleanup_metadata() {
    local pid
    for pid in "${META_PIDS[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
}
trap cleanup_metadata EXIT INT TERM

echo "[$(date -u +%FT%TZ)] scheduled remaining-throughput campaign: tag=$RUN_TAG"
echo "[$(date -u +%FT%TZ)] sleeping ${START_DELAY_SEC}s before preflight"
sleep "$START_DELAY_SEC"

# Retained artifacts must be built from an exact source snapshot.  Do not
# paper over a collaborator's in-progress files with ALLOW_DIRTY_ARTIFACT.
if [[ -n "$(git status --porcelain)" ]]; then
    echo "ERROR: refusing campaign from dirty worktree; commit/stash unrelated work and reschedule." >&2
    exit 1
fi

cmake --build build -j"$(nproc)" --target embarlet throughput_test lazylog_metadata_replica

# c4 was explicitly selected: this avoids modifying c1's colleague worktree
# and avoids treating c3's old archive overlay as publication provenance.
CLIENT_NODES_CSV=c4 bash "$SCRIPT_DIR/cluster_setup.sh"

mkdir -p "$META_ROOT/a" "$META_ROOT/b"
setsid "$ROOT/build/bin/lazylog_metadata_replica" \
    --listen 0.0.0.0:50081 --sidecar "$META_ROOT/a/metadata.sidecar" \
    >"$META_ROOT/replica_a.log" 2>&1 < /dev/null &
META_PIDS+=("$!")
setsid "$ROOT/build/bin/lazylog_metadata_replica" \
    --listen 0.0.0.0:50082 --sidecar "$META_ROOT/b/metadata.sidecar" \
    >"$META_ROOT/replica_b.log" 2>&1 < /dev/null &
META_PIDS+=("$!")

ONLY_CELLS="e2_embar5_rf0_ack1_n1,e2_embar5_rf1_ack1_n1,e2_embar5_rf2_ack2_n1,e2_embar0_rf0_ack1_n1,e2_embar0_rf1_ack1_n1,e2_corfu_rf0_n1,e2_scalog_rf0_n1,e2_lazylog_rf0_n1,e2_corfu_rf2_ack2_n1,e2_scalog_rf2_ack2_n1,e2_lazylog_rf2_ack2_n1" \
RUN_TAG="$RUN_TAG" \
SKIP_BASELINES=0 \
INCLUDE_DURABLE_BASELINES_RF2=1 \
NUM_TRIALS="${NUM_TRIALS:-4}" \
WARMUP_TRIALS="${WARMUP_TRIALS:-1}" \
TOTAL_BYTES="${TOTAL_BYTES:-4294967296}" \
MSG_SIZE="${MSG_SIZE:-1024}" \
NUM_BROKERS="${NUM_BROKERS:-4}" \
THREADS_THROUGHPUT="${THREADS_THROUGHPUT:-6}" \
EPOCH_US_THROUGHPUT="${EPOCH_US_THROUGHPUT:-500}" \
INCLUDE_MEMORY_COPY_RF2=0 \
CLIENT_HOSTS_REMOTE=c4 \
CLIENT_HOSTS_E2=c4 \
BROKER_IP="${BROKER_IP:-10.10.10.10}" \
BROKER_IP_MULTI="${BROKER_IP_MULTI:-10.10.10.10}" \
EMBARCADERO_CXL_ZERO_MODE=metadata \
EMBARCADERO_CXL_MAP_POPULATE=0 \
EMBARCADERO_ACK_TIMEOUT_SEC=300 \
EMBARCADERO_REPLICA_DISK_DIRS="$ROOT/.Replication/disk0,$ROOT/.Replication/disk1" \
EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS="10.10.10.10:50081,10.10.10.10:50082" \
bash "$SCRIPT_DIR/run_overnight_eval.sh"

