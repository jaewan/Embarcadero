#!/bin/bash
# scripts/run_post_sweep_queue.sh
#
# Master evaluation queue: waits for a running overnight sweep to finish,
# then runs the remaining paper evaluations back-to-back:
#
#   1. E4 smoke   — failure-suite sanity (SMOKE=1 SUITE=E4a)
#   2. E4 full    — broker-kill / head-kill / baseline-kill suite (E4a+E4b+E4f)
#   3. E7         — YCSB A-F x {uniform, zipf} x {EMBARCADERO, CORFU, LAZYLOG}
#   4. E10        — sequencer scaling: epoch x threads + CORFU baseline, CPU probed
#   5. delivery   — delivery-ceiling scoping A/B (diag on/off)
#
# Fault tolerance: every stage runs under `timeout`, is followed by a full
# cluster cleanup (broker node + all clients), and NEVER aborts the queue —
# a crashed stage is logged and the queue moves on.
#
# Usage:
#   WAIT_PID=<sweep pid> nohup bash scripts/run_post_sweep_queue.sh > /tmp/post_sweep_queue.log 2>&1 &
#   (WAIT_PID optional — if unset or dead, the queue starts immediately once
#    the run_multiclient lock is free.)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

WAIT_PID="${WAIT_PID:-}"
CLIENT_HOSTS="${CLIENT_HOSTS:-c4 c3 c1}"
QUEUE_TAG="$(date -u +%Y%m%dT%H%M%SZ)_queue"
QUEUE_LOG_DIR="$PROJECT_ROOT/data/post_sweep_queue/$QUEUE_TAG"
mkdir -p "$QUEUE_LOG_DIR"
SUMMARY="$QUEUE_LOG_DIR/queue_summary.log"
touch "$SUMMARY"

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY"; }

# ---------------------------------------------------------------------------
# Full-cluster cleanup: broker node + every client. Never fatal.
# ---------------------------------------------------------------------------
cleanup_cluster() {
    log "cleanup: broker node + clients ($CLIENT_HOSTS)"
    # Broker-node processes (exact names only — never pkill -f)
    local name
    for name in embarlet throughput_test kv_ycsb_bench \
                corfu_global_sequencer lazylog_global_sequencer scalog_global_sequencer; do
        pkill -x "$name" 2>/dev/null || true
    done
    sleep 0.5
    pkill -9 -x embarlet 2>/dev/null || true
    # CXL shm + ready sentinels
    rm -f /dev/shm/CXL_SHARED_FILE* /tmp/embarlet_*_ready 2>/dev/null || true
    # Clients: stray publishers + stale timeseries CSVs
    local h
    for h in $CLIENT_HOSTS; do
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$h" \
            'pkill -x throughput_test 2>/dev/null; rm -f ~/Embarcadero/build/bin/throughput_timeseries_*.csv 2>/dev/null; true' \
            2>/dev/null || true
    done
    # Bounded wait for hugepage reservations of dying brokers to drain
    local deadline=$(( $(date +%s) + 20 )) rsvd
    while :; do
        rsvd="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
        [[ "$rsvd" -eq 0 || "$(date +%s)" -ge "$deadline" ]] && break
        sleep 0.5
    done
    [[ "${rsvd:-0}" -ne 0 ]] && log "cleanup WARNING: HugePages_Rsvd=$rsvd still held"
    return 0
}
trap cleanup_cluster EXIT

# ---------------------------------------------------------------------------
# Stage runner: timeout-bounded, cleanup before and after, never aborts queue
# ---------------------------------------------------------------------------
run_stage() {
    local name="$1" timeout_sec="$2"
    shift 2
    local stage_log="$QUEUE_LOG_DIR/${name}.log"
    log "===== STAGE START [$name] (timeout ${timeout_sec}s) ====="
    cleanup_cluster
    local t0 rc=0
    t0=$(date +%s)
    timeout --kill-after=60 "$timeout_sec" env "$@" >"$stage_log" 2>&1 || rc=$?
    local dt=$(( $(date +%s) - t0 ))
    if [[ "$rc" -eq 0 ]]; then
        log "===== STAGE PASS [$name] (${dt}s) ====="
    elif [[ "$rc" -eq 124 || "$rc" -eq 137 ]]; then
        log "===== STAGE TIMEOUT [$name] after ${dt}s — see $stage_log ====="
    else
        log "===== STAGE FAIL [$name] rc=$rc (${dt}s) — see $stage_log ====="
    fi
    cleanup_cluster
    return 0
}

# ---------------------------------------------------------------------------
# Wait for the in-flight sweep (if any), then for the lock
# ---------------------------------------------------------------------------
log "===== Post-sweep queue START — $QUEUE_TAG ====="
log "Commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
if [[ -n "$WAIT_PID" ]] && kill -0 "$WAIT_PID" 2>/dev/null; then
    log "Waiting for sweep PID $WAIT_PID to exit..."
    while kill -0 "$WAIT_PID" 2>/dev/null; do sleep 60; done
    log "Sweep PID $WAIT_PID exited."
fi
# Belt and braces: wait for the multiclient lock to be free (max 30 min)
lock_deadline=$(( $(date +%s) + 1800 ))
until flock -n /tmp/embarcadero_run_multiclient.lock true 2>/dev/null; do
    if [[ "$(date +%s)" -ge "$lock_deadline" ]]; then
        log "WARNING: run_multiclient lock still held after 30 min — proceeding anyway"
        break
    fi
    sleep 30
done
sleep 10

# ---------------------------------------------------------------------------
# The queue
# ---------------------------------------------------------------------------
run_stage e4_smoke 2400 \
    SMOKE=1 SUITE=E4a bash "$SCRIPT_DIR/run_failure_suite.sh"

run_stage e4_full 10800 \
    SUITE=all NUM_TRIALS=3 bash "$SCRIPT_DIR/run_failure_suite.sh"

run_stage e7_ycsb 10800 \
    bash "$SCRIPT_DIR/run_ycsb_eval.sh"

run_stage e10_ablation 14400 \
    NUM_TRIALS=4 WARMUP_TRIALS=1 bash "$SCRIPT_DIR/run_throughput_ablation.sh"

run_stage delivery_scoping 5400 \
    bash "$SCRIPT_DIR/run_delivery_scoping.sh"

log ""
log "===== Post-sweep queue COMPLETE — $QUEUE_TAG ====="
log "Stage logs: $QUEUE_LOG_DIR"
grep -E "STAGE (PASS|FAIL|TIMEOUT)" "$SUMMARY" || true
