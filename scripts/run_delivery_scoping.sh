#!/bin/bash
# scripts/run_delivery_scoping.sh
#
# Delivery-ceiling scoping experiment (paper scoping decision input).
#
# Context (docs/experiments/delivery_ceiling_profiling.md, 2026-07-09):
# the ORDER=5 delivery ceiling (~280 MiB/s windowed goodput at 8 GiB) is
# subscriber-side — four ReceiveWorkerThreads pegged at ~100% CPU while
# brokers idle at 7-9% with deep Send-Qs. The receive-path copy-reduction
# pass did NOT move the number, so the per-message cost sits elsewhere.
# Untested hypothesis: the measurement harness itself (per-message dedup /
# UID tracking / latency recording, enabled via EMBARCADERO_DEDUPE_LATENCY
# and EMBARCADERO_SUBSCRIBER_DIAG) is a significant share of the pegged
# receive-worker CPU.
#
# This script A/Bs the exact profiled workload with diagnostics ON vs OFF.
#   - If OFF >> ON: the ceiling is substantially instrument overhead; the
#     paper reports the OFF number and scopes the diag cost as methodology.
#   - If OFF ≈ ON: the ceiling is real; the paper scopes the delivery claim
#     and cites the profiling analysis.
#
# Usage:
#   bash scripts/run_delivery_scoping.sh            # full (8 GiB, 2 trials/cell)
#   SMOKE=1 bash scripts/run_delivery_scoping.sh    # 1 GiB, 1 trial/cell
#
# Each cell is bounded by CELL_TIMEOUT_SEC and fully cleaned up; a failed
# cell never aborts the sweep.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

if [[ "${SMOKE:-0}" == "1" ]]; then
    TOTAL_BYTES=$(( 1024 * 1024 * 1024 ))
    NUM_TRIALS="${NUM_TRIALS:-1}"
else
    TOTAL_BYTES="${TOTAL_BYTES:-$(( 8 * 1024 * 1024 * 1024 ))}"
    NUM_TRIALS="${NUM_TRIALS:-2}"
fi
TARGET_MBPS="${TARGET_MBPS:-1000}"
CELL_TIMEOUT_SEC="${CELL_TIMEOUT_SEC:-1800}"
NUM_BROKERS="${NUM_BROKERS:-4}"

RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)_delivery_scoping}"
OUT_BASE="$PROJECT_ROOT/data/delivery_scoping/$RUN_TAG"
LOG_DIR="$OUT_BASE/logs"
mkdir -p "$LOG_DIR"
SUMMARY_LOG="$OUT_BASE/sweep_summary.log"
touch "$SUMMARY_LOG"

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY_LOG"; }

cleanup_local() {
    pkill -x throughput_test 2>/dev/null || true
    pkill -x embarlet 2>/dev/null || true
    pkill -x corfu_global_sequencer 2>/dev/null || true
    pkill -x lazylog_global_sequencer 2>/dev/null || true
    pkill -x scalog_global_sequencer 2>/dev/null || true
    sleep 0.3
    pkill -9 -x embarlet 2>/dev/null || true
    rm -f /dev/shm/CXL_SHARED_FILE* /tmp/embarlet_*_ready 2>/dev/null || true
    local deadline=$(( $(date +%s) + 15 )) rsvd
    while :; do
        rsvd="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
        [[ "$rsvd" -eq 0 || "$(date +%s)" -ge "$deadline" ]] && break
        sleep 0.5
    done
}
trap cleanup_local EXIT

run_scoping_cell() {
    local label="$1" dedupe="$2" subdiag="$3"
    local cell_log="$LOG_DIR/${label}.log"
    log "START [$label] dedupe_latency=$dedupe subscriber_diag=$subdiag target=${TARGET_MBPS}MB/s bytes=$TOTAL_BYTES"

    cleanup_local

    local rc=0
    {
        env \
        DATA_DIR="$OUT_BASE/$label" \
        RUN_ID="$label" \
        TARGET_MBPS="$TARGET_MBPS" \
        THREADS_PER_BROKER=1 \
        ORDERS=5 \
        MODES=steady \
        SEQUENCER=EMBARCADERO \
        ACK_LEVEL=1 \
        REPLICATION_FACTOR=0 \
        MSG_SIZE=1024 \
        TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
        NUM_TRIALS="$NUM_TRIALS" \
        NUM_BROKERS="$NUM_BROKERS" \
        SCENARIO=local \
        EMBARCADERO_DEDUPE_LATENCY="$dedupe" \
        EMBARCADERO_SUBSCRIBER_DIAG="$subdiag" \
        EMBARCADERO_DELIVERY_DRAIN_BATCH=4096 \
            timeout --kill-after=30 "$CELL_TIMEOUT_SEC" \
            bash "$SCRIPT_DIR/run_latency.sh"
    } > "$cell_log" 2>&1 || rc=$?

    if [[ "$rc" -eq 0 ]]; then
        log "PASS [$label]"
    elif [[ "$rc" -eq 124 || "$rc" -eq 137 ]]; then
        log "TIMEOUT [$label] after ${CELL_TIMEOUT_SEC}s — see $cell_log"
    else
        log "FAIL [$label] (rc=$rc) — see $cell_log"
    fi

    # Surface the windowed delivery goodput for quick reading
    local f
    while IFS= read -r f; do
        log "  [$label] $(basename "$(dirname "$f")"): $(tail -1 "$f")"
    done < <(find "$OUT_BASE/$label" -name delivery_steady_throughput.csv 2>/dev/null)

    cleanup_local
    return 0
}

log "===== Delivery-ceiling scoping START — $RUN_TAG ====="
log "Commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"

run_scoping_cell "diag_on"  1 1   # reproduces the profiled baseline (~280 MiB/s)
run_scoping_cell "diag_off" 0 0   # instrument-overhead-free delivery goodput

log ""
log "===== Delivery scoping COMPLETE — $RUN_TAG ====="
log "Results: $OUT_BASE"
log "Compare windowed PayloadMiBPerSec between diag_on and diag_off:"
log "  OFF >> ON  → ceiling is largely instrument overhead (report OFF number)"
log "  OFF ≈ ON   → ceiling is real; scope the delivery claim per profiling doc"
