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
    # 4 GiB: at the ~6 GB/s epoch=100 operating point a 1 GiB send lasts
    # <300 ms — under the 500 ms overlap-window floor, so overlap extraction
    # produced nothing (run 20260711T181503Z). 4 GiB keeps windows >600 ms.
    TOTAL_BYTES="${TOTAL_BYTES:-$((4 * 1024 * 1024 * 1024))}"
    echo "[ablation] FULL sweep: epoch_us={$EPOCH_US_LIST} threads={$THREADS_LIST} ${NUM_TRIALS} trials/cell"
fi

NUM_BROKERS="${NUM_BROKERS:-4}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
# c4 is the NUMA-local x16 client (c1 is x8-downgraded and its NIC is on NUMA 0)
CLIENT_HOST="${CLIENT_HOST:-c4}"
case "$CLIENT_HOST" in
    c1) CLIENT_NUMA="${CLIENT_NUMA:-0}" ;;
    *)  CLIENT_NUMA="${CLIENT_NUMA:-1}" ;;
esac
MSG_SIZE="${MSG_SIZE:-1024}"
# Sequencer CPU is the point of E10 — probe by default.
PROBE_CPU="${PROBE_CPU:-1}"
# Broker ReqReceive pool must cover the largest THREADS_LIST value: each
# handler thread binds to one publish connection for its lifetime, and the
# yaml default (6) starves threads>=8 cells into SessionOpen timeouts
# (e10_epoch100_t8 wedged then timed out, run 20260711T181503Z). Env wins
# over yaml (configuration.h get() checks env first).
export EMBARCADERO_NETWORK_IO_THREADS="${EMBARCADERO_NETWORK_IO_THREADS:-32}"
# Wall-clock bound per cell so one wedge can't stall the sweep.
CELL_TIMEOUT_SEC="${CELL_TIMEOUT_SEC:-1500}"
# CORFU comparison cells (honest sequencer baseline): sweep THREADS_LIST at
# the same load with the batched CXL-Corfu sequencer, probing its CPU.
RUN_CORFU_BASELINE="${RUN_CORFU_BASELINE:-1}"

REMOTE_EMBAR_ROOT="${REMOTE_EMBAR_ROOT:-$HOME/Embarcadero}"
export CLIENT_LD_LIBRARY_PATH="$REMOTE_EMBAR_ROOT/lib"

RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)_ablation}"
OUT_BASE="$PROJECT_ROOT/data/throughput_ablation/$RUN_TAG"
LOG_DIR="$OUT_BASE/logs"
mkdir -p "$LOG_DIR"

SUMMARY_CSV="$OUT_BASE/ablation_summary.csv"
echo "sequencer,epoch_us,threads_per_broker,trial,overlap_gbps,overlap_window_ms,cell_label" > "$SUMMARY_CSV"

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

cleanup_local() {
    # run_multiclient.sh normally cleans up via its EXIT trap, but a timeout
    # SIGKILL skips the trap — reap brokers/sequencers here so a crashed cell
    # cannot poison the next one (stale CXL shm / held hugepage reservations).
    pkill -x embarlet 2>/dev/null || true
    pkill -x corfu_global_sequencer 2>/dev/null || true
    pkill -x lazylog_global_sequencer 2>/dev/null || true
    pkill -x scalog_global_sequencer 2>/dev/null || true
    sleep 0.3
    pkill -9 -x embarlet 2>/dev/null || true
    rm -f /dev/shm/CXL_SHARED_FILE* /tmp/embarlet_*_ready 2>/dev/null || true
}

cleanup_shm() {
    local shm_name="${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}"
    rm -f "/dev/shm${shm_name}" 2>/dev/null || true
    ssh -o BatchMode=yes "$CLIENT_HOST" \
        "rm -f /dev/shm${shm_name} 2>/dev/null; true" 2>/dev/null || true
}

# Live CPU probe: the old post-hoc pidstat ran after run_multiclient returned,
# when the brokers were already dead — it silently measured nothing. This
# variant runs CONCURRENTLY with the cell: waits for the target processes to
# appear, then samples embarlet (head = Sequencer5 thread) and, for CORFU,
# corfu_global_sequencer, for up to 60 x 1 s.
start_cpu_probe() {
    local cpu_log="$1"
    (
        local waited=0 head_pid seq_pid
        while [[ $waited -lt 90 ]]; do
            head_pid="$(pgrep -x embarlet 2>/dev/null | head -1 || true)"
            [[ -n "$head_pid" ]] && break
            sleep 1; waited=$((waited + 1))
        done
        [[ -z "${head_pid:-}" ]] && exit 0
        seq_pid="$(pgrep -x corfu_global_sequencer 2>/dev/null | head -1 || true)"
        {
            echo "# embarlet(head)=$head_pid corfu_seq=${seq_pid:-none}"
            pidstat -t -p "$head_pid${seq_pid:+,$seq_pid}" 1 60 2>&1
        } > "$cpu_log"
    ) &
    echo $!
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
    local sequencer="$1"    # EMBARCADERO | CORFU
    local epoch_us="$2"     # "-" for CORFU (no epoch knob)
    local threads="$3"
    local label
    if [[ "$sequencer" == "EMBARCADERO" ]]; then
        label="e10_epoch${epoch_us}_t${threads}"
    else
        label="e10_corfu_t${threads}"
    fi
    local cell_log="$LOG_DIR/${label}.log"

    log "START cell [$label]: sequencer=$sequencer epoch_us=$epoch_us threads/broker=$threads"
    cleanup_remote || true
    cleanup_local || true
    cleanup_shm || true

    local cell_out="$OUT_BASE/cells/${label}"
    mkdir -p "$cell_out"

    # Sequencer-specific env
    local -a seq_env=()
    if [[ "$sequencer" == "EMBARCADERO" ]]; then
        seq_env=( SEQUENCER=EMBARCADERO ORDER=5 EMBAR_ORDER5_EPOCH_US="$epoch_us" )
    else
        seq_env=( SEQUENCER=CORFU ORDER=2 EMBARCADERO_CORFU_SEQ_IP="$BROKER_IP" )
    fi

    # Live sequencer/broker CPU probe, concurrent with the cell
    local probe_pid=""
    if [[ "$PROBE_CPU" == "1" ]] && command -v pidstat &>/dev/null; then
        probe_pid="$(start_cpu_probe "$LOG_DIR/${label}_cpu.log")"
    fi

    local rc=0
    {
        env "${seq_env[@]}" \
        THREADS_PER_BROKER="$threads" \
        NUM_CLIENTS=1 \
        CLIENT_HOSTS_CSV="$CLIENT_HOST" \
        CLIENT_NUMAS_CSV="$CLIENT_NUMA" \
        NUM_BROKERS="$NUM_BROKERS" \
        NUM_TRIALS="$NUM_TRIALS" \
        WARMUP_TRIALS="$WARMUP_TRIALS" \
        TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
        MESSAGE_SIZE="$MSG_SIZE" \
        ACK=1 \
        REPLICATION_FACTOR=0 \
        TEST_TYPE=5 \
        EMBARCADERO_RUNTIME_MODE=throughput \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        CLIENT_LD_LIBRARY_PATH="$CLIENT_LD_LIBRARY_PATH" \
        OUT_BASE="$cell_out" \
        BENCHMARK_TAG="$RUN_TAG/$label" \
            timeout --kill-after=30 "$CELL_TIMEOUT_SEC" \
            bash "$SCRIPT_DIR/run_multiclient.sh"
    } >"$cell_log" 2>&1 || rc=$?

    if [[ -n "$probe_pid" ]]; then
        kill "$probe_pid" 2>/dev/null || true
        wait "$probe_pid" 2>/dev/null || true
    fi

    if [[ "$rc" -eq 0 ]]; then
        log "PASS [$label]"
    elif [[ "$rc" -eq 124 || "$rc" -eq 137 ]]; then
        log "TIMEOUT [$label] after ${CELL_TIMEOUT_SEC}s — see $cell_log"
    else
        log "FAIL [$label] — see $cell_log"
    fi

    # Extract overlap GB/s rows from the cell's overlap_summary.csv and append to master CSV
    local overlap_csv="$cell_out/multiclient/overlap_summary.csv"
    if [[ ! -f "$overlap_csv" ]]; then
        # run_multiclient.sh writes overlap_summary.csv into its LOG_DIR
        overlap_csv="$PROJECT_ROOT/multiclient_logs/overlap_summary.csv"
    fi
    if [[ -f "$overlap_csv" ]]; then
        # Skip header row; emit: sequencer, epoch_us, threads, trial, overlap_gbps, window_ms, label
        awk -F',' -v sq="$sequencer" -v eu="$epoch_us" -v th="$threads" -v lbl="$label" \
            'NR>1 && NF>=3 { printf "%s,%s,%s,%s,%s,%s,%s\n", sq, eu, th, $1, $2, $3, lbl }' \
            "$overlap_csv" >> "$SUMMARY_CSV" || true
    fi

    cleanup_remote || true
    cleanup_local || true
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
if [[ "$RUN_CORFU_BASELINE" == "1" ]]; then
    for threads in $THREADS_LIST; do
        total_cells=$(( total_cells + 1 ))
    done
fi

cell_num=0
for epoch_us in $EPOCH_US_LIST; do
    for threads in $THREADS_LIST; do
        cell_num=$(( cell_num + 1 ))
        log "--- Cell $cell_num / $total_cells: EMBARCADERO epoch_us=$epoch_us threads=$threads ---"
        run_ablation_cell EMBARCADERO "$epoch_us" "$threads"
        # Brief inter-cell pause for port drain + CXL region cleanup
        sleep 3
    done
done

# Honest sequencer baseline: batched CXL-Corfu at the same load points,
# with sequencer-process CPU probed live (E10 v2 per improvement_plan IV).
if [[ "$RUN_CORFU_BASELINE" == "1" ]]; then
    for threads in $THREADS_LIST; do
        cell_num=$(( cell_num + 1 ))
        log "--- Cell $cell_num / $total_cells: CORFU threads=$threads ---"
        run_ablation_cell CORFU "-" "$threads"
        sleep 3
    done
fi

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
    echo "Column order: sequencer, epoch_us, threads_per_broker, trial, overlap_gbps, overlap_window_ms, cell_label"
    echo ""
    # Quick matrix view: for each (sequencer, epoch_us, threads), print max overlap_gbps
    echo "--- Peak OVERLAP GB/s per cell (max trial) ---"
    awk -F',' '
        NR==1 { next }
        {
            key = $1 " epoch=" $2 " threads=" $3
            val = $5 + 0
            if (val > best[key]) best[key] = val
        }
        END {
            for (k in best) printf "  %-40s → %.3f GB/s\n", k, best[k]
        }
    ' "$SUMMARY_CSV" | sort
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
