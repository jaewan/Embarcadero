#!/bin/bash
# Throughput-maximization study (2026-07-12). Context: all systems cluster at
# ~3.4 GB/s PER HOST (two c4 processes got 1.7 each in E4a) — a shared ceiling
# upstream of any system's ordering path. c4 (primary publisher) was found
# with UNTUNED kernel socket buffers (wmem_max 208 KB vs 256 MB fleet-wide),
# now fixed. This study isolates the ceiling factor by factor on c4, then
# takes the best config to N=2/N=3.
#
# Stage A — single client (c4), post-buffer-tuning:
#   A1 epoch500 baseline (compare vs banked 3.47)         — buffer effect alone
#   A2 epoch100 1GiB pool6   (exact E10 replica)          — reproduce 6.1?
#   A3 epoch100 4GiB pool6                                — burst vs sustained
#   A4 epoch100 4GiB pool32                               — pool contention
#   A5-A7 threads {6,8,12} at epoch100 4GiB pool64        — send parallelism
#   A8 two processes on c4 (epoch100 pool64)              — host cap test
# Stage B — best-of-A config:
#   B1 N=2 c4+c3   B2 N=3 c4,c3,c1 HOME_BROKERS=4 (full striping)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"
RUN_TAG="$(date -u +%Y%m%dT%H%M%SZ)_tpstudy"
OUT="$PROJECT_ROOT/data/throughput_study/$RUN_TAG"
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
    local d=$(( $(date +%s) + 20 )) r
    while :; do r="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo)"; [[ "$r" -eq 0 || "$(date +%s)" -ge "$d" ]] && break; sleep 0.5; done
}
trap cleanup_all EXIT

# cell <label> <nc> <hosts> <numas> <total> <epoch> <pool(empty=yaml)> <threads> [extra env...]
cell() {
    local label="$1" nc="$2" hosts="$3" numas="$4" total="$5" epoch="$6" pool="$7" threads="$8"
    shift 8
    log "START [$label] epoch=$epoch pool=${pool:-yaml6} threads=$threads total=$total nc=$nc"
    cleanup_all
    local -a poolenv=()
    [[ -n "$pool" ]] && poolenv=( EMBARCADERO_NETWORK_IO_THREADS="$pool" )
    local rc=0
    env "${poolenv[@]}" "$@" \
        EMBAR_ORDER5_EPOCH_US="$epoch" THREADS_PER_BROKER="$threads" \
        NUM_CLIENTS="$nc" CLIENT_HOSTS_CSV="$hosts" CLIENT_NUMAS_CSV="$numas" \
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
    cleanup_all
}

GiB=$((1024*1024*1024))
log "===== THROUGHPUT STUDY $RUN_TAG (post c4 buffer tuning) ====="
log "Stage A: single-client c4 factor isolation"
cell A1_ep500_4g_yaml_t6   1 c4 1 $((4*GiB)) 500 ""  6
cell A2_ep100_1g_yaml_t6   1 c4 1 $((1*GiB)) 100 ""  6
cell A3_ep100_4g_yaml_t6   1 c4 1 $((4*GiB)) 100 ""  6
cell A4_ep100_4g_p32_t6    1 c4 1 $((4*GiB)) 100 32  6
cell A5_ep100_4g_p64_t6    1 c4 1 $((4*GiB)) 100 64  6
cell A6_ep100_4g_p64_t8    1 c4 1 $((4*GiB)) 100 64  8
cell A7_ep100_4g_p64_t12   1 c4 1 $((4*GiB)) 100 64  12
cell A8_ep100_8g_p64_2proc 2 c4,c4 1,1 $((8*GiB)) 100 64 6

log "Stage B: scale-out at epoch=100 pool64"
cell B1_ep100_n2           2 c4,c3 1,1 $((8*GiB)) 100 64 6
cell B2_ep100_n3_home4     3 c4,c3,c1 1,1,0 $((12*GiB)) 100 64 6 EMBARCADERO_ORDER5_HOME_BROKERS=4

log "===== THROUGHPUT STUDY COMPLETE ====="
log "Results: $OUT"
