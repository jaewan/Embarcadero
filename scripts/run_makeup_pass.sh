#!/bin/bash
# Makeup/headline pass phase 1+2: epoch=100 throughput set, then E4 failure suite.
# Continue-on-failure per cell; full cleanup between cells.
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

RUN_TAG="$(date -u +%Y%m%dT%H%M%SZ)_makeup"
OUT="$PROJECT_ROOT/data/makeup_pass/$RUN_TAG"
mkdir -p "$OUT/logs"
S="$OUT/summary.log"
log() { local m="[$(date -u +%FT%TZ)] $*"; echo "$m"; echo "$m" >> "$S"; }

cleanup_all() {
    local n
    for n in embarlet throughput_test corfu_global_sequencer lazylog_global_sequencer scalog_global_sequencer; do
        pkill -x "$n" 2>/dev/null || true
    done
    sleep 0.5; pkill -9 -x embarlet 2>/dev/null || true
    rm -f /dev/shm/CXL_SHARED_FILE* /tmp/embarlet_*_ready 2>/dev/null || true
    local h
    for h in c4 c3 c1; do
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$h" 'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
    done
    local deadline=$(( $(date +%s) + 20 )) rsvd
    while :; do
        rsvd="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo || echo 0)"
        [[ "$rsvd" -eq 0 || "$(date +%s)" -ge "$deadline" ]] && break
        sleep 0.5
    done
}
trap cleanup_all EXIT

# tp_cell <label> <nclients> <hosts_csv> <numas_csv> <total_bytes> <rf> [extra env...]
tp_cell() {
    local label="$1" nc="$2" hosts="$3" numas="$4" total="$5" rf="$6"
    shift 6
    log "START [$label]"
    cleanup_all
    local rc=0
    env "$@" \
        EMBAR_ORDER5_EPOCH_US=100 \
        EMBARCADERO_NETWORK_IO_THREADS=32 \
        NUM_CLIENTS="$nc" CLIENT_HOSTS_CSV="$hosts" CLIENT_NUMAS_CSV="$numas" \
        NUM_BROKERS=4 NUM_TRIALS=5 WARMUP_TRIALS=1 \
        TOTAL_MESSAGE_SIZE="$total" MESSAGE_SIZE=1024 \
        SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR="$rf" TEST_TYPE=5 \
        EMBARCADERO_CXL_ZERO_MODE=metadata EMBARCADERO_CXL_MAP_POPULATE=0 \
        EMBARCADERO_HEAD_ADDR=10.10.10.10 \
        CLIENT_LD_LIBRARY_PATH="$HOME/Embarcadero/lib" \
        timeout --kill-after=30 2400 bash "$SCRIPT_DIR/run_multiclient.sh" \
        > "$OUT/logs/${label}.log" 2>&1 || rc=$?
    if [[ "$rc" -eq 0 ]]; then log "PASS [$label]";
    elif [[ "$rc" -eq 124 || "$rc" -eq 137 ]]; then log "TIMEOUT [$label]";
    else log "FAIL [$label] rc=$rc"; fi
    grep -h "OVERLAP" "$OUT/logs/${label}.log" | tail -4 | while read -r l; do log "  [$label] $l"; done
    cleanup_all
    return 0
}

log "===== MAKEUP PASS $RUN_TAG — phase 1: epoch=100 throughput ====="
GiB=$((1024*1024*1024))
tp_cell ep100_n1_rf0_latmode 1 c4 1 $((4*GiB)) 0 EMBARCADERO_RUNTIME_MODE=latency
tp_cell ep100_n1_rf0_tpmode  1 c4 1 $((4*GiB)) 0 EMBARCADERO_RUNTIME_MODE=throughput
tp_cell ep100_n1_rf1         1 c4 1 $((4*GiB)) 1 EMBARCADERO_RUNTIME_MODE=latency
tp_cell ep100_n1_rf2         1 c4 1 $((4*GiB)) 2 EMBARCADERO_RUNTIME_MODE=latency
tp_cell ep100_n2_rf0         2 c4,c3 1,1 $((8*GiB)) 0 EMBARCADERO_RUNTIME_MODE=latency
tp_cell ep100_n3_rf0         3 c4,c3,c1 1,1,0 $((12*GiB)) 0 EMBARCADERO_RUNTIME_MODE=latency

log "===== phase 2: E4 failure suite ====="
cleanup_all
rc=0
env CLIENT_LD_LIBRARY_PATH="$HOME/Embarcadero/lib" \
    EMBARCADERO_NETWORK_IO_THREADS=32 \
    SUITE=all NUM_TRIALS=3 \
    timeout --kill-after=60 7200 bash "$SCRIPT_DIR/run_failure_suite.sh" \
    > "$OUT/logs/e4_full.log" 2>&1 || rc=$?
if [[ "$rc" -eq 0 ]]; then log "PASS [e4_full]"; else log "FAIL/TIMEOUT [e4_full] rc=$rc"; fi
cleanup_all

log "===== MAKEUP PASS phases 1-2 COMPLETE ====="
log "Results: $OUT"
