#!/bin/bash
# Discriminate why epoch=100 gave 6.0-6.2 GB/s in the E10 ablation (pool=6,
# threads={3,6}, 1 GiB) but 3.4-3.7 GB/s in the makeup pass (pool=32,
# threads=4, 4 GiB). One factor flipped per cell, exact E10 replica first.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"
RUN_TAG="$(date -u +%Y%m%dT%H%M%SZ)_epoch100_disc"
OUT="$PROJECT_ROOT/data/makeup_pass/$RUN_TAG"
mkdir -p "$OUT/logs"
S="$OUT/summary.log"
log() { local m="[$(date -u +%FT%TZ)] $*"; echo "$m"; echo "$m" >> "$S"; }
cleanup_all() {
    pkill -x embarlet 2>/dev/null || true; sleep 0.5; pkill -9 -x embarlet 2>/dev/null || true
    rm -f /dev/shm/CXL_SHARED_FILE* /tmp/embarlet_*_ready 2>/dev/null || true
    ssh -o BatchMode=yes c4 'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
    local d=$(( $(date +%s) + 15 )) r
    while :; do r="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo)"; [[ "$r" -eq 0 || "$(date +%s)" -ge "$d" ]] && break; sleep 0.5; done
}
trap cleanup_all EXIT

# cell <label> <pool(empty=yaml6)> <threads> <total_bytes>
cell() {
    local label="$1" pool="$2" threads="$3" total="$4"
    log "START [$label] pool=${pool:-yaml6} threads=$threads total=$total"
    cleanup_all
    local -a poolenv=()
    [[ -n "$pool" ]] && poolenv=( EMBARCADERO_NETWORK_IO_THREADS="$pool" )
    local rc=0
    env "${poolenv[@]}" \
        EMBAR_ORDER5_EPOCH_US=100 THREADS_PER_BROKER="$threads" \
        NUM_CLIENTS=1 CLIENT_HOSTS_CSV=c4 CLIENT_NUMAS_CSV=1 \
        NUM_BROKERS=4 NUM_TRIALS=3 WARMUP_TRIALS=1 \
        TOTAL_MESSAGE_SIZE="$total" MESSAGE_SIZE=1024 \
        SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=0 TEST_TYPE=5 \
        EMBARCADERO_RUNTIME_MODE=throughput \
        EMBARCADERO_CXL_ZERO_MODE=metadata EMBARCADERO_CXL_MAP_POPULATE=0 \
        EMBARCADERO_HEAD_ADDR=10.10.10.10 \
        CLIENT_LD_LIBRARY_PATH="$HOME/Embarcadero/lib" \
        timeout --kill-after=30 1500 bash "$SCRIPT_DIR/run_multiclient.sh" \
        > "$OUT/logs/${label}.log" 2>&1 || rc=$?
    log "END [$label] rc=$rc"
    grep -A5 "Grand Summary" "$OUT/logs/${label}.log" | grep Trial | sed 's/^ */  /' | while read -r l; do log "  [$label] $l"; done
    cleanup_all
}

GiB=$((1024*1024*1024))
log "===== epoch100 discriminator $RUN_TAG ====="
cell replica_e10_pool6_t6_1g  ""   6 $((1*GiB))   # exact E10 replica → expect ~6.1
cell flip_pool32_t6_1g        32   6 $((1*GiB))   # pool 6→32 only
cell flip_4gib_pool6_t6       ""   6 $((4*GiB))   # load 1→4 GiB only
log "===== discriminator COMPLETE ====="
