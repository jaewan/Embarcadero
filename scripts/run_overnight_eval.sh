#!/bin/bash
# scripts/run_overnight_eval.sh
#
# Overnight evaluation sweep — runs E2, E3, E8 (overheads), E9 (RF sensitivity),
# and a single-node throughput matrix for all sequencer modes and baselines.
# Does NOT require remote SSH clients (single-node moscxl only) so it can run
# unattended without cluster policy issues.
#
# Scope (what fits overnight on a single node, all with warm-up discard):
#   A. Throughput matrix: Embarcadero ORDER=0/5, CORFU, LAZYLOG, SCALOG
#      × RF ∈ {0,1} × {1,2,4} brokers (E2 single-node, E9 partial)
#   B. Latency-vs-load curves (E3 SLO headline):
#      Embarcadero ORDER=5 with linger, ORDER=0; CORFU; at RF=0,1
#   C. RF sensitivity: ORDER=5 RF ∈ {0,1,2} (E9)
#   D. Overhead probe: sequencer CPU at idle/peak (E8 partial)
#
# Variables you can override:
#   NUM_TRIALS        number of trials per cell (default 3; use 1 for smoke)
#   WARMUP_TRIALS     warm trials discarded by the aggregator (default 1)
#   TOTAL_BYTES       bytes per trial (default 4 GiB; use 512 MiB for smoke)
#   MSG_SIZE          message size in bytes (default 1024)
#   LOAD_POINTS_MBPS  space-separated load points for latency curves (default as below)
#   SKIP_BASELINES    set to 1 to skip CORFU/SCALOG/LAZYLOG throughput (faster)
#   SMOKE             set to 1 for a fast sanity run (1 trial, 512 MiB, 2 load pts)
#
# Usage:
#   bash scripts/run_overnight_eval.sh              # full overnight
#   SMOKE=1 bash scripts/run_overnight_eval.sh      # ~5-minute sanity pass
#   NUM_TRIALS=5 bash scripts/run_overnight_eval.sh # tighter confidence intervals
#
# Constraints honoured:
#   - Never touches ~/Embarcadero
#   - Never pkill -f  (only kills PIDs tracked by broker_lifecycle.sh)
#   - WARMUP_TRIALS=1 by default (matches aggregate_*.py default)
#   - Results land under data/overnight_eval/<RUN_TAG>/

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# ---------------------------------------------------------------------------
# Smoke / full mode
# ---------------------------------------------------------------------------
if [[ "${SMOKE:-0}" == "1" ]]; then
    NUM_TRIALS="${NUM_TRIALS:-1}"
    TOTAL_BYTES="${TOTAL_BYTES:-$((512 * 1024 * 1024))}"   # 512 MiB
    LOAD_POINTS_MBPS="${LOAD_POINTS_MBPS:-100 500}"
    echo "[overnight] SMOKE mode: 1 trial, 512 MiB, 2 load points"
else
    NUM_TRIALS="${NUM_TRIALS:-3}"
    TOTAL_BYTES="${TOTAL_BYTES:-$((4 * 1024 * 1024 * 1024))}"   # 4 GiB
    LOAD_POINTS_MBPS="${LOAD_POINTS_MBPS:-100 250 500 750 1000 1500 2000}"
    echo "[overnight] FULL mode: ${NUM_TRIALS} trials, 4 GiB, $(echo "$LOAD_POINTS_MBPS" | wc -w) load points"
fi

WARMUP_TRIALS="${WARMUP_TRIALS:-1}"
MSG_SIZE="${MSG_SIZE:-1024}"
NUM_BROKERS_LIST="${NUM_BROKERS_LIST:-4}"   # single-node default; extend to "1 2 4" for E9 broker-count sweep
SKIP_BASELINES="${SKIP_BASELINES:-0}"

RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)_overnight}"
OUT_BASE="$PROJECT_ROOT/data/overnight_eval/$RUN_TAG"
LOG_DIR="$OUT_BASE/logs"
mkdir -p "$LOG_DIR"

SUMMARY_LOG="$OUT_BASE/sweep_summary.log"
touch "$SUMMARY_LOG"

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }

log() {
    local msg="[$(stamp)] $*"
    echo "$msg"
    echo "$msg" >> "$SUMMARY_LOG"
}

fail_cell() {
    local cell="$1"
    log "FAIL [$cell] — see $LOG_DIR/${cell}.log"
}

pass_cell() {
    local cell="$1"
    log "PASS [$cell]"
}

# ---------------------------------------------------------------------------
# Helper: run one throughput cell
# ---------------------------------------------------------------------------
run_throughput_cell() {
    local label="$1"; shift
    local cell_log="$LOG_DIR/${label}.log"
    log "START throughput [$label]"
    if env "$@" \
        NUM_TRIALS="$NUM_TRIALS" \
        WARMUP_TRIALS="$WARMUP_TRIALS" \
        TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
        MESSAGE_SIZE="$MSG_SIZE" \
        SCENARIO=local \
        BENCHMARK_TAG="$RUN_TAG" \
        OUT_BASE="$OUT_BASE/throughput" \
        bash scripts/run_e2e_throughput_benchmark.sh >"$cell_log" 2>&1; then
        pass_cell "$label"
    else
        fail_cell "$label"
    fi
}

# ---------------------------------------------------------------------------
# Helper: run one latency-vs-load cell
# ---------------------------------------------------------------------------
run_latency_cell() {
    local label="$1"; shift
    local cell_log="$LOG_DIR/${label}.log"
    log "START latency [$label]"
    if env "$@" \
        NUM_TRIALS="$NUM_TRIALS" \
        WARMUP_TRIALS="$WARMUP_TRIALS" \
        TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
        MSG_SIZE="$MSG_SIZE" \
        LOAD_POINTS_MBPS="$LOAD_POINTS_MBPS" \
        SCENARIO=local \
        PACING_MODE=steady \
        BENCHMARK_TAG="$RUN_TAG" \
        OUT_BASE="$OUT_BASE/latency" \
        bash scripts/run_latency_vs_load.sh >"$cell_log" 2>&1; then
        pass_cell "$label"
    else
        fail_cell "$label"
    fi
}

# ---------------------------------------------------------------------------
log "===== Overnight eval sweep START — $RUN_TAG ====="
log "Project root: $PROJECT_ROOT"
log "Commit: $(git rev-parse HEAD)"
log "Output: $OUT_BASE"
log "Trials: $NUM_TRIALS (warmup=$WARMUP_TRIALS discarded)"
log "Total bytes: $TOTAL_BYTES  MSG_SIZE: $MSG_SIZE"
log "Brokers (list): $NUM_BROKERS_LIST"
log "Skip baselines: $SKIP_BASELINES"
log ""

# ===========================================================================
# PART A — THROUGHPUT MATRIX
# ===========================================================================
log "===== PART A: Throughput matrix ====="

for nb in $NUM_BROKERS_LIST; do

    # ---- Embarcadero ORDER=5 (primary, with linger) ----
    for rf in 0 1; do
        label="embar_order5_rf${rf}_nb${nb}"
        run_throughput_cell "$label" \
            SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=$rf \
            NUM_BROKERS=$nb \
            EMBARCADERO_RUNTIME_MODE=latency   # enables linger (300 µs)
    done

    # ---- Embarcadero ORDER=0 ----
    for rf in 0 1; do
        label="embar_order0_rf${rf}_nb${nb}"
        run_throughput_cell "$label" \
            SEQUENCER=EMBARCADERO ORDER=0 ACK=1 REPLICATION_FACTOR=$rf \
            NUM_BROKERS=$nb
    done

    if [[ "$SKIP_BASELINES" != "1" ]]; then

        # ---- CORFU (ORDER=2, RF=0; RF=1 requires corfu replication client) ----
        label="corfu_order2_rf0_nb${nb}"
        run_throughput_cell "$label" \
            SEQUENCER=CORFU ORDER=2 ACK=1 REPLICATION_FACTOR=0 \
            NUM_BROKERS=$nb

        # ---- SCALOG (ORDER=1 per-broker, RF=0) ----
        # NOTE: Scalog RF=1 has a known anomaly (RF=1 slower than RF=2); run RF=0 only
        # until that is diagnosed. SKIP_REMOTE_SCALOG_SEQUENCER=1 because scalog global
        # sequencer is co-located on the same host (local scenario).
        label="scalog_order1_rf0_nb${nb}"
        run_throughput_cell "$label" \
            SEQUENCER=SCALOG ORDER=1 ACK=1 REPLICATION_FACTOR=0 \
            NUM_BROKERS=$nb \
            SKIP_REMOTE_SCALOG_SEQUENCER=1

        # ---- LAZYLOG (ORDER=2, RF=0) ----
        label="lazylog_order2_rf0_nb${nb}"
        run_throughput_cell "$label" \
            SEQUENCER=LAZYLOG ORDER=2 ACK=1 REPLICATION_FACTOR=0 \
            NUM_BROKERS=$nb \
            SKIP_REMOTE_LAZYLOG_SEQUENCER=1

    fi

done

# ===========================================================================
# PART B — LATENCY-VS-LOAD CURVES (E3 SLO headline)
# ===========================================================================
log "===== PART B: Latency-vs-load (E3 SLO curves) ====="

# Embarcadero ORDER=5 with linger — THE headline curve
run_latency_cell "e3_embar_order5_linger_rf0" \
    SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
    NUM_BROKERS=4 \
    EMBARCADERO_RUNTIME_MODE=latency

# Embarcadero ORDER=5 WITHOUT linger — to show batch-fill cost (comparison)
run_latency_cell "e3_embar_order5_nolinger_rf0" \
    SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
    NUM_BROKERS=4

# Embarcadero ORDER=0 — per-record ordering baseline
run_latency_cell "e3_embar_order0_rf0" \
    SEQUENCER=EMBARCADERO ORDER=0 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
    NUM_BROKERS=4

if [[ "$SKIP_BASELINES" != "1" ]]; then
    # CORFU latency-vs-load (E3 baseline overlay)
    run_latency_cell "e3_corfu_order2_rf0" \
        SEQUENCER=CORFU ORDER=2 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
        NUM_BROKERS=4

    # SCALOG latency-vs-load
    run_latency_cell "e3_scalog_order1_rf0" \
        SEQUENCER=SCALOG ORDER=1 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
        NUM_BROKERS=4 \
        SKIP_REMOTE_SCALOG_SEQUENCER=1

    # LAZYLOG latency-vs-load
    run_latency_cell "e3_lazylog_order2_rf0" \
        SEQUENCER=LAZYLOG ORDER=2 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
        NUM_BROKERS=4 \
        SKIP_REMOTE_LAZYLOG_SEQUENCER=1
fi

# ===========================================================================
# PART C — RF SENSITIVITY (E9 partial)
# ===========================================================================
log "===== PART C: RF sensitivity (E9) ====="

# Throughput at RF=0,1,2 — ORDER=5 only (ORDER=0 in Part A)
for rf in 0 1 2; do
    label="e9_embar_order5_rf${rf}_nb4"
    run_throughput_cell "$label" \
        SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=$rf \
        NUM_BROKERS=4 \
        EMBARCADERO_RUNTIME_MODE=latency
done

# Latency at RF=0,1 with linger (RF=2 latency is high; include if time allows)
for rf in 0 1; do
    label="e9_latency_embar_order5_rf${rf}"
    run_latency_cell "$label" \
        SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=1 REPLICATION_FACTOR=$rf \
        NUM_BROKERS=4 \
        EMBARCADERO_RUNTIME_MODE=latency
done

# ===========================================================================
# PART D — OVERHEAD PROBE (E8 partial)
# ===========================================================================
log "===== PART D: Overhead probe (E8 partial) ====="

# Run a single 60-second ORDER=5 idle-then-peak load and capture broker pidstat.
# This is a best-effort wrapper: if pidstat is unavailable it logs and continues.
OVERHEAD_LOG="$LOG_DIR/e8_overhead_probe.log"
log "START overhead probe (pidstat on broker)"

{
    echo "=== Overhead probe: $(stamp) ==="
    echo "Commit: $(git rev-parse HEAD)"

    if ! command -v pidstat &>/dev/null; then
        echo "WARNING: pidstat not available; skipping CPU overhead probe."
        echo "Install sysstat to enable E8 measurements."
        exit 0
    fi

    # Start embarlet in background (ORDER=5, 4 brokers) with a brief load run
    EMBARLET_PID=""
    export EMBARCADERO_ORDER5_EXPORT_OVERRUN_FATAL=1
    # Use the throughput test to generate a 30-second sustained load window
    # while pidstat samples the broker process.
    # We launch embarlet via the harness so teardown is clean.
    echo "Launching ORDER=5 embarlet for CPU sampling..."
    SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=0 \
        NUM_BROKERS=4 NUM_TRIALS=1 \
        TOTAL_MESSAGE_SIZE=$((2 * 1024 * 1024 * 1024)) \
        SCENARIO=local \
        BENCHMARK_TAG="${RUN_TAG}_e8" \
        OUT_BASE="$OUT_BASE/overhead" \
        bash scripts/run_e2e_throughput_benchmark.sh &
    BENCH_PID=$!

    # Give the broker 5 seconds to start, then sample for 20 seconds
    sleep 5
    EMBARLET_PID=$(pgrep -x embarlet 2>/dev/null | head -1 || true)
    if [[ -n "$EMBARLET_PID" ]]; then
        echo "=== pidstat -t -p $EMBARLET_PID 1 20 ==="
        pidstat -t -p "$EMBARLET_PID" 1 20 || true
    else
        echo "WARNING: embarlet PID not found; cannot sample CPU."
    fi

    wait "$BENCH_PID" || true
    echo "=== Overhead probe complete: $(stamp) ==="
} >"$OVERHEAD_LOG" 2>&1
log "Overhead probe done — see $OVERHEAD_LOG"

# ===========================================================================
# SUMMARY
# ===========================================================================
log ""
log "===== Overnight eval sweep COMPLETE — $RUN_TAG ====="
log "Results under: $OUT_BASE"
log ""
log "Quick result summary:"
echo ""

# Print any aggregate CSV files that were written
find "$OUT_BASE/throughput" "$OUT_BASE/latency" \
    -name "throughput_summary.csv" -o -name "latency_summary.csv" \
    2>/dev/null | sort | while read -r f; do
    echo "--- $f ---"
    cat "$f"
    echo ""
done | tee -a "$SUMMARY_LOG"

log "Full sweep log: $SUMMARY_LOG"
echo ""
echo "To view latency curves:"
echo "  python3 scripts/plot/plot_latency_vs_load.py $OUT_BASE/latency"
echo "To compare throughput:"
echo "  python3 scripts/aggregate_e2e_throughput.py $OUT_BASE/throughput"
