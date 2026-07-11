#!/usr/bin/env bash
# E7 YCSB-style evaluation across EMBARCADERO, CORFU, and LAZYLOG.
#
# Environment overrides:
#   SMOKE=1          — quick sanity pass: 1 trial, 10k ops, workloads A+C only
#   RUNTAG           — subdirectory name under data/ycsb_eval/ (default: timestamp)
#   TRIALS           — number of trials per cell (default: 3; SMOKE overrides to 1)
#   RECORD_COUNT     — number of pre-loaded records (default: 100000)
#   OP_COUNT         — operations per trial (default: 100000)
#   VALUE_SIZE       — bytes per value (default: 100)
#   REPO_ROOT        — repo root (default: parent of scripts/)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$SCRIPT_DIR/.." && pwd)}"
# CMake target is kv_ycsb_bench; binary lands at build/bin/kv_ycsb_bench
KV_BENCH="$REPO_ROOT/build/bin/kv_ycsb_bench"

# Per-trial wall-clock bound. kv_ycsb_bench manages its own broker cluster;
# a hung trial would otherwise wedge the whole sweep.
TRIAL_TIMEOUT_SEC="${TRIAL_TIMEOUT_SEC:-1200}"

# kv_ycsb_bench spawns embarlet + sequencer processes itself. If a trial
# crashes or times out they leak and poison the next cell (stale CXL shm →
# SIGBUS-class startup failures). Clean between trials, never fatal.
cleanup_kv_cluster() {
    pkill -x kv_ycsb_bench 2>/dev/null || true
    pkill -x embarlet 2>/dev/null || true
    pkill -x corfu_global_sequencer 2>/dev/null || true
    pkill -x lazylog_global_sequencer 2>/dev/null || true
    pkill -x scalog_global_sequencer 2>/dev/null || true
    sleep 0.3
    pkill -9 -x embarlet 2>/dev/null || true
    rm -f /dev/shm/CXL_SHARED_FILE* /tmp/embarlet_*_ready 2>/dev/null || true
    # Bounded wait for hugepage reservations of dying brokers to drain
    local deadline=$(( $(date +%s) + 15 )) rsvd
    while :; do
        rsvd="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
        [[ "$rsvd" -eq 0 || "$(date +%s)" -ge "$deadline" ]] && break
        sleep 0.5
    done
}
trap cleanup_kv_cluster EXIT

# ---- Validate binary ----
if [[ ! -x "$KV_BENCH" ]]; then
    echo "ERROR: kv_ycsb_bench binary not found at $KV_BENCH" >&2
    echo "       Build with: cmake --build $REPO_ROOT/build --target kv_ycsb_bench -j" >&2
    exit 1
fi

# ---- Mode configuration ----
SMOKE="${SMOKE:-0}"
if [[ "$SMOKE" == "1" ]]; then
    TRIALS=1
    RECORD_COUNT="${RECORD_COUNT:-10000}"
    OP_COUNT="${OP_COUNT:-10000}"
    WORKLOADS=(A C)
else
    TRIALS="${TRIALS:-3}"
    RECORD_COUNT="${RECORD_COUNT:-100000}"
    OP_COUNT="${OP_COUNT:-100000}"
    WORKLOADS=(A B C D E F)
fi

# ---- Key distributions ----
KEY_DISTS=(uniform zipf)

# ---- Systems: name, sequencer flag, order ----
# Each entry: "LABEL:--sequencer X --order N"
declare -a SYSTEMS=(
    "EMBARCADERO:--sequencer EMBARCADERO --order 5"
    "CORFU:--sequencer CORFU --order 2"
    "LAZYLOG:--sequencer LAZYLOG --order 2"
)

# ---- Output paths ----
RUNTAG="${RUNTAG:-$(date -u +%Y%m%dT%H%M%SZ)}"
DATA_DIR="$REPO_ROOT/data/ycsb_eval/$RUNTAG"
LOG_DIR="$REPO_ROOT/logs/ycsb_eval/$RUNTAG"
mkdir -p "$DATA_DIR" "$LOG_DIR"

echo "========================================"
echo "E7 YCSB Evaluation"
echo "  SMOKE=$SMOKE  TRIALS=$TRIALS"
echo "  RECORD_COUNT=$RECORD_COUNT  OP_COUNT=$OP_COUNT"
echo "  workloads: ${WORKLOADS[*]}"
echo "  key_dists: ${KEY_DISTS[*]}"
echo "  output:    $DATA_DIR"
echo "========================================"

# ---- Helper: write_ratio and scan_len per YCSB workload ----
# YCSB workload definitions:
#   A: 50% reads,  50% writes         (read/write mix)
#   B: 95% reads,   5% writes         (read-heavy)
#   C: 100% reads,  0% writes         (read-only)
#   D: 95% reads,   5% writes, latest key distribution
#   E: 95% scans,   5% writes         (short scans)
#   F: 50% reads,  50% read-modify-write
workload_flags() {
    local wl="$1"
    case "$wl" in
        A) echo "--workload A --write_ratio 0.5" ;;
        B) echo "--workload B --write_ratio 0.05" ;;
        C) echo "--workload C --write_ratio 0.0" ;;
        D) echo "--workload D --write_ratio 0.05" ;;
        E) echo "--workload E --write_ratio 0.05" ;;
        F) echo "--workload F --write_ratio 0.5" ;;
        *) echo "--write_ratio 0.5" ;;
    esac
}

# In smoke mode CORFU/LAZYLOG skip workloads D and E (scan/latest semantics
# may not be supported by those systems' implementations).
should_skip() {
    local system="$1" wl="$2"
    if [[ "$SMOKE" == "1" ]]; then
        return 1  # smoke only runs A+C, no skipping needed
    fi
    if [[ "$system" != "EMBARCADERO" && ("$wl" == "D" || "$wl" == "E") ]]; then
        return 0  # skip
    fi
    return 1  # don't skip
}

# ---- Main sweep ----
total_cells=0
failed_cells=0

for system_entry in "${SYSTEMS[@]}"; do
    SYSTEM_LABEL="${system_entry%%:*}"
    SYSTEM_FLAGS="${system_entry#*:}"

    for KEY_DIST in "${KEY_DISTS[@]}"; do
        for WL in "${WORKLOADS[@]}"; do

            if should_skip "$SYSTEM_LABEL" "$WL"; then
                echo "SKIP  $SYSTEM_LABEL workload=$WL dist=$KEY_DIST (not applicable)"
                continue
            fi

            cell_dir="$DATA_DIR/${SYSTEM_LABEL}_${WL}_${KEY_DIST}"
            mkdir -p "$cell_dir"

            WL_FLAGS="$(workload_flags "$WL")"

            for ((trial=1; trial<=TRIALS; trial++)); do
                total_cells=$((total_cells + 1))
                log_file="$LOG_DIR/${SYSTEM_LABEL}_${WL}_${KEY_DIST}_trial${trial}.log"
                run_id="${SYSTEM_LABEL}_${WL}_${KEY_DIST}_t${trial}"

                echo "---- $SYSTEM_LABEL workload=$WL dist=$KEY_DIST trial=$trial/$TRIALS ----"

                # kv_bench manages its own broker cluster (manage_cluster=true internally).
                # Bound each trial and clean up leaked processes on failure so one
                # crash cannot poison the rest of the sweep.
                cleanup_kv_cluster
                # shellcheck disable=SC2086
                if timeout --kill-after=30 "$TRIAL_TIMEOUT_SEC" "$KV_BENCH" \
                        $SYSTEM_FLAGS \
                        $WL_FLAGS \
                        --key_dist "$KEY_DIST" \
                        --record_count "$RECORD_COUNT" \
                        --operation_count "$OP_COUNT" \
                        --rf 0 \
                        --output_dir "$cell_dir" \
                        --run_id "$run_id" \
                        >"$log_file" 2>&1; then
                    echo "  OK  -> $cell_dir/$run_id/summary.csv"
                else
                    rc=$?
                    if [[ "$rc" -eq 124 || "$rc" -eq 137 ]]; then
                        echo "  TIMEOUT after ${TRIAL_TIMEOUT_SEC}s — see $log_file"
                    else
                        echo "  FAIL (exit $rc) — see $log_file"
                    fi
                    failed_cells=$((failed_cells + 1))
                    cleanup_kv_cluster
                fi
            done
        done
    done
done

echo ""
echo "========================================"
echo "YCSB eval complete."
echo "  total cells run : $total_cells"
echo "  failed cells    : $failed_cells"
echo "  results dir     : $DATA_DIR"
echo "  logs dir        : $LOG_DIR"
echo "========================================"

if [[ "$failed_cells" -gt 0 ]]; then
    echo "WARNING: $failed_cells cell(s) failed. Check logs in $LOG_DIR." >&2
    exit 1
fi
exit 0
