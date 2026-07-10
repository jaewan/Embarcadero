#!/bin/bash
# scripts/run_throughput_ablation.sh
#
# E10 sequencer scalability ablation sweep — single c1 client, 4 brokers, ORDER=5.
# Sweeps EMBAR_ORDER5_EPOCH_US × THREADS_PER_BROKER to find the optimal single-client
# throughput ceiling on c1 (PCIe 3.0 x8, ~7.88 GB/s CXL write ceiling).
#
# Design:
#   epoch_us  ∈ {100, 200, 300, 500}  (broker EpochDriverThread period)
#   threads   ∈ {3, 6, 8, 12}         (publisher threads per broker)
#   trials: 2 per cell (fast; for paper-quality use NUM_TRIALS=4 WARMUP_TRIALS=1)
#   load: 1 GiB per trial (fast convergence; large enough to avoid start/stop noise)
#   metric: OVERLAP GB/s reported by run_multiclient.sh overlap_summary.csv
#
# Theory (ceiling = epochs/sec × batch_size):
#   epoch_us=100  → 10000 epochs/sec × 2 MB = 20 GB/s  (CXL ceiling: 21 GB/s)
#   epoch_us=200  →  5000 epochs/sec × 2 MB = 10 GB/s  (NIC ceiling: 12.5 GB/s)
#   epoch_us=300  →  3333 epochs/sec × 2 MB =  6.7 GB/s
#   epoch_us=500  →  2000 epochs/sec × 2 MB =  4.0 GB/s  (baseline)
#
# CPU overhead note: shorter epochs increase commit-path CPU usage on the broker.
# At 100 µs the head broker may become CPU-bound before hitting the NIC ceiling.
# Measure actual CPU (pidstat) alongside throughput for each cell.
#
# Usage:
#   bash scripts/run_throughput_ablation.sh              # full sweep
#   SMOKE=1 bash scripts/run_throughput_ablation.sh      # epoch_us={200,500} × threads={3,6}, 1 trial
#   NUM_TRIALS=4 WARMUP_TRIALS=1 bash scripts/run_throughput_ablation.sh   # paper-quality
#
# Key env overrides:
#   EPOCH_US_LIST="100 200 300 500"   space-separated list of epoch_us values to sweep
#   THREADS_LIST="3 6 8 12"           space-separated list of threads/broker values
#   NUM_TRIALS=N                      trials per cell (default 2)
#   WARMUP_TRIALS=N                   warm-up trials to discard (default 0 for fast run)
#   TOTAL_BYTES=N                     bytes per trial (default 1 GiB)
#   BROKER_IP=X.X.X.X                 moscxl dataplane IP (default 10.10.10.10)
#   CLIENT_HOST=hostname              client SSH host (default c1)
#   NUM_BROKERS=N                     brokers (default 4)
#   PROBE_CPU=1                       run pidstat on embarlet alongside each cell (adds ~20s)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# ---------------------------------------------------------------------------
# Parameters
# ---------------------------------------------------------------------------
if [[ "${SMOKE:-0}" == "1" ]]; then
    EPOCH_US_LIST="${EPOCH_US_LIST:-200 500}"
    THREADS_LIST="${THREADS_LIST:-3 6}"
    NUM_TRIALS="${NUM_TRIALS:-1}"
    WARMUP_TRIALS="${WARMUP_TRIALS:-0}"
    TOTAL_BYTES="${TOTAL_BYTES:-$((512 * 1024 * 1024))}"   # 512 MiB for smoke
    echo "[ablation] SMOKE mode: epoch_us={$EPOCH_US_LIST} threads={$THREADS_LIST} 1 trial 512 MiB"
else
    EPOCH_US_LIST="${EPOCH_US_LIST:-100 200 300 500}"
    THREADS_LIST="${THREADS_LIST:-3 6 8 12}"
    NUM_TRIALS="${NUM_TRIALS:-2}"
    WARMUP_TRIALS="${WARMUP_TRIALS:-0}"
    TOTAL_BYTES="${TOTAL_BYTES:-$((1024 * 1024 * 1024))}"  # 1 GiB per cell
    echo "[ablation] FULL sweep: epoch_us={$EPOCH_US_LIST} threads={$THREADS_LIST} ${NUM_TRIALS} trials/cell"
fi

NUM_BROKERS="${NUM_BROKERS:-4}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
CLIENT_HOST="${CLIENT_HOST:-c1}"
MSG_SIZE="${MSG_SIZE:-1024}"
PROBE_CPU="${PROBE_CPU:-0}"

REMOTE_EMBAR_ROOT="${REMOTE_EMBAR_ROOT:-$HOME/Embarcadero}"
export CLIENT_LD_LIBRARY_PATH="$REMOTE_EMBAR_ROOT/lib"

RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)_ablation}"
OUT_BASE="$PROJECT_ROOT/data/throughput_ablation/$RUN_TAG"
LOG_DIR="$OUT_BASE/logs"
mkdir -p "$LOG_DIR"

SUMMARY_CSV="$OUT_BASE/ablation_summary.csv"
echo "epoch_us,threads_per_broker,trial,overlap_gbps,overlap_window_ms,cell_label" > "$SUMMARY_CSV"

SUMMARY_LOG="$OUT_BASE/sweep_summary.log"
touch "$SUMMARY_LOG"

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY_LOG"; }

# ---------------------------------------------------------------------------
# Cleanup helpers (same pattern as run_overnight_eval.sh)
# ---------------------------------------------------------------------------
cleanup_remote() {
    ssh -o BatchMode=yes "$CLIENT_HOST" \
        'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
}

cleanup_shm() {
    local shm_name="${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}"
    rm -f "/dev/shm${shm_name}" 2>/dev/null || true
    ssh -o BatchMode=yes "$CLIENT_HOST" \
        "rm -f /dev/shm${shm_name} 2>/dev/null; true" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# Pre-flight
# ---------------------------------------------------------------------------
log "===== Throughput ablation sweep START — $RUN_TAG ====="
log "Commit: $(git rev-parse HEAD 2>/dev/null || echo unknown)"
log "Broker IP: $BROKER_IP  Client: $CLIENT_HOST  Brokers: $NUM_BROKERS"
log "Load: $TOTAL_BYTES bytes/trial  MSG_SIZE: $MSG_SIZE B"
log "epoch_us sweep: $EPOCH_US_LIST"
log "threads sweep: $THREADS_LIST"
log "Trials per cell: $NUM_TRIALS (warmup=$WARMUP_TRIALS discarded)"
log "Output: $OUT_BASE"

if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$CLIENT_HOST" \
        "test -x ~/Embarcadero/build/bin/throughput_test" 2>/dev/null; then
    log "ERROR: $CLIENT_HOST missing throughput_test — run cluster_setup.sh first"
    exit 1
fi

# ---------------------------------------------------------------------------
# Cell runner
# ---------------------------------------------------------------------------
run_ablation_cell() {
    local epoch_us="$1"
    local threads="$2"
    local label="e10_epoch${epoch_us}_t${threads}"
    local cell_log="$LOG_DIR/${label}.log"

    log "START cell [$label]: epoch_us=$epoch_us threads/broker=$threads"
    cleanup_remote || true
    cleanup_shm || true

    local cell_out="$OUT_BASE/cells/${label}"
    mkdir -p "$cell_out"

    local rc=0
    {
        EMBAR_ORDER5_EPOCH_US="$epoch_us" \
        THREADS_PER_BROKER="$threads" \
        NUM_CLIENTS=1 \
        CLIENT_HOSTS_CSV="$CLIENT_HOST" \
        CLIENT_NUMAS_CSV=1 \
        NUM_BROKERS="$NUM_BROKERS" \
        NUM_TRIALS="$NUM_TRIALS" \
        WARMUP_TRIALS="$WARMUP_TRIALS" \
        TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
        MESSAGE_SIZE="$MSG_SIZE" \
        SEQUENCER=EMBARCADERO \
        ORDER=5 \
        ACK=1 \
        REPLICATION_FACTOR=0 \
        TEST_TYPE=5 \
        EMBARCADERO_RUNTIME_MODE=throughput \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        CLIENT_LD_LIBRARY_PATH="$CLIENT_LD_LIBRARY_PATH" \
        OUT_BASE="$cell_out" \
        BENCHMARK_TAG="$RUN_TAG/$label" \
            bash "$SCRIPT_DIR/run_multiclient.sh"
    } >"$cell_log" 2>&1 || rc=$?

    if [[ "$rc" -eq 0 ]]; then
        log "PASS [$label]"
    else
        log "FAIL [$label] — see $cell_log"
    fi

    # Optional: sample CPU usage during the run (requires pidstat / sysstat)
    if [[ "${PROBE_CPU:-0}" == "1" ]] && command -v pidstat &>/dev/null; then
        local cpu_log="$LOG_DIR/${label}_cpu.log"
        local embar_pid
        embar_pid="$(pgrep -x embarlet 2>/dev/null | head -1 || true)"
        if [[ -n "$embar_pid" ]]; then
            pidstat -t -p "$embar_pid" 1 10 >"$cpu_log" 2>&1 || true
        fi
    fi

    # Extract overlap GB/s rows from the cell's overlap_summary.csv and append to master CSV
    local overlap_csv="$cell_out/multiclient/overlap_summary.csv"
    if [[ -f "$overlap_csv" ]]; then
        # Skip header row; emit: epoch_us, threads, trial, overlap_gbps, window_ms, label
        awk -F',' -v eu="$epoch_us" -v th="$threads" -v lbl="$label" \
            'NR>1 && NF>=3 { printf "%s,%s,%s,%s,%s,%s\n", eu, th, $1, $2, $3, lbl }' \
            "$overlap_csv" >> "$SUMMARY_CSV" || true
    fi

    cleanup_remote || true
    cleanup_shm || true
    return 0   # never propagate failure — sweep must continue
}

# ---------------------------------------------------------------------------
# Sweep: iterate epoch_us × threads
# ---------------------------------------------------------------------------
total_cells=0
for epoch_us in $EPOCH_US_LIST; do
    for threads in $THREADS_LIST; do
        total_cells=$(( total_cells + 1 ))
    done
done

cell_num=0
for epoch_us in $EPOCH_US_LIST; do
    for threads in $THREADS_LIST; do
        cell_num=$(( cell_num + 1 ))
        log "--- Cell $cell_num / $total_cells: epoch_us=$epoch_us threads=$threads ---"
        run_ablation_cell "$epoch_us" "$threads"
        # Brief inter-cell pause for port drain + CXL region cleanup
        sleep 3
    done
done

# ---------------------------------------------------------------------------
# Summary table
# ---------------------------------------------------------------------------
log ""
log "===== Ablation sweep COMPLETE — $RUN_TAG ====="
log "Results: $OUT_BASE"
log ""
log "Summary CSV: $SUMMARY_CSV"
echo ""
echo "================================================================"
echo "  Ablation Summary: epoch_us × threads → OVERLAP GB/s"
echo "  (rows = mean over measured trials; warmup discarded)"
echo "================================================================"
if [[ -s "$SUMMARY_CSV" ]]; then
    # Print header + all data rows
    cat "$SUMMARY_CSV"
    echo ""
    echo "Column order: epoch_us, threads_per_broker, trial, overlap_gbps, overlap_window_ms, cell_label"
    echo ""
    # Quick matrix view: for each (epoch_us, threads), print max overlap_gbps
    echo "--- Peak OVERLAP GB/s per cell (max trial) ---"
    awk -F',' '
        NR==1 { next }
        {
            key = $1 "," $2
            val = $4 + 0
            if (val > best[key]) best[key] = val
        }
        END {
            for (k in best) printf "  epoch_us=%-5s threads=%-3s → %.3f GB/s\n", \
                substr(k,1,index(k,",")-1), substr(k,index(k,",")+1), best[k]
        }
    ' "$SUMMARY_CSV" | sort -t= -k2 -n -k4 -n
fi
echo "================================================================"
echo ""
echo "Next steps:"
echo "  python3 scripts/aggregate_e2e_throughput.py $OUT_BASE/cells/"
echo "  python3 scripts/plot_scaling_results.py $SUMMARY_CSV"
echo ""
echo "Theory reference (2 MB batch):"
echo "  epoch_us=100  → ceiling 20.0 GB/s   (likely CPU-bound before NIC limit)"
echo "  epoch_us=200  → ceiling 10.0 GB/s   (c1 NIC: 12.5 GB/s 100G)"
echo "  epoch_us=300  → ceiling  6.7 GB/s"
echo "  epoch_us=500  → ceiling  4.0 GB/s   (baseline / original kEpochUs)"
