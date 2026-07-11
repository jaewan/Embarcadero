#!/bin/bash
# In-flight window study: verify the 100 MB client flow-control window is the
# throughput limiter and find the true system ceiling. Single client (c4),
# window in {100 (control), 256, 512, 1024, 2048} MB at epoch {500, 100}.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"
RUN_TAG="$(date -u +%Y%m%dT%H%M%SZ)_inflight"
OUT="$PROJECT_ROOT/data/throughput_study/$RUN_TAG"
mkdir -p "$OUT/logs"
S="$OUT/summary.log"
log() { local m="[$(date -u +%FT%TZ)] $*"; echo "$m"; echo "$m" >> "$S"; }
cleanup_all() {
    pkill -x embarlet 2>/dev/null || true; sleep 0.5; pkill -9 -x embarlet 2>/dev/null || true
    rm -f /dev/shm/CXL_SHARED_FILE* /tmp/embarlet_*_ready 2>/dev/null || true
    ssh -o BatchMode=yes c4 'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
    local d=$(( $(date +%s) + 20 )) r
    while :; do r="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo)"; [[ "$r" -eq 0 || "$(date +%s)" -ge "$d" ]] && break; sleep 0.5; done
}
trap cleanup_all EXIT

cell() {
    local label="$1" window_mb="$2" epoch="$3" total="$4"
    log "START [$label] window=${window_mb}MB epoch=$epoch"
    cleanup_all
    local rc=0
    env EMBARCADERO_MAX_INFLIGHT_MB="$window_mb" \
        EMBAR_ORDER5_EPOCH_US="$epoch" THREADS_PER_BROKER=6 \
        EMBARCADERO_NETWORK_IO_THREADS=64 \
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
    { grep -h "OVERLAP" "$OUT/logs/${label}.log" | tail -2;
      grep -A5 "Grand Summary" "$OUT/logs/${label}.log" | grep Trial | tail -2; } \
      | sed 's/^ *//' | while read -r l; do log "  [$label] $l"; done
    grep -c "Throttle stall" "$OUT/logs/${label}.log" 2>/dev/null | while read -r c; do
        [[ "$c" != "0" ]] && log "  [$label] throttle-relax events: $c"
    done
    cleanup_all
}

GiB=$((1024*1024*1024))
log "===== IN-FLIGHT WINDOW STUDY $RUN_TAG ====="
cell W100_ep500  100  500 $((4*GiB))    # control: must reproduce 3.33
cell W256_ep500  256  500 $((4*GiB))
cell W512_ep500  512  500 $((8*GiB))
cell W1024_ep500 1024 500 $((8*GiB))
cell W2048_ep500 2048 500 $((8*GiB))
cell W1024_ep100 1024 100 $((8*GiB))
cell W2048_ep100 2048 100 $((8*GiB))
log "===== WINDOW STUDY COMPLETE ====="
log "Results: $OUT"
