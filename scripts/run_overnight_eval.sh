#!/bin/bash
# scripts/run_overnight_eval.sh
#
# Overnight evaluation sweep — E2 (throughput), E3 (SLO latency), E9 (RF sensitivity),
# E8 (overheads). Brokers run on moscxl with CXL NUMA node 2; publishers run on remote
# SSH client machines (c1, c2, c3 — all passwordless-sudo).
#
# Architecture:
#   - Brokers:   moscxl  (CXL NUMA node 2 for data, NIC on node 1)
#   - Clients:   c1 / c2 / c3 (remote SSH, passwordless sudo)
#   - All cells delegated to run_multiclient.sh, which handles:
#       • shm creation + cleanup (EMBARCADERO_CXL_SHM_NAME)
#       • numactl binding (--membind=1,2 when node 2 is CPU-less CXL)
#       • hugepage checks
#       • barrier-start synchronization with SSH clients
#       • post-trial shm_unlink + /dev/shm cleanup
#
# Prerequisites (run once on moscxl before starting):
#   bash scripts/cluster_setup.sh   # syncs bins + config to c1/c2/c3, sets hugepages
#
# Usage:
#   bash scripts/run_overnight_eval.sh              # full overnight (~6-8 h)
#   SMOKE=1 bash scripts/run_overnight_eval.sh      # ~10-min sanity pass
#   NUM_TRIALS=5 bash scripts/run_overnight_eval.sh # tighter CIs
#   SKIP_BASELINES=1 bash scripts/run_overnight_eval.sh
#
# Key env overrides:
#   SMOKE=1               fast sanity: 1 trial, 512 MiB, 2 load pts, 1 client
#   NUM_TRIALS=N          trials per cell (default 4; warmup=1 discarded → 3 measured)
#   WARMUP_TRIALS=N       warm-up trials to discard from stats (default 1)
#   TOTAL_BYTES=N         bytes per publisher per trial (default 4 GiB)
#   MSG_SIZE=N            message size in bytes (default 1024)
#   NUM_CLIENTS=N         remote publishers: 1|2|3 (default 1 for single-remote; 3 for E2)
#   BROKER_IP=X.X.X.X    moscxl dataplane IP (default 10.10.10.10 — 10G fabric, reachable from c1/c2/c3)
#   SKIP_BASELINES=1      skip CORFU/SCALOG/LAZYLOG cells
#   LOAD_POINTS_MBPS="…"  space-separated MB/s load points for E3
#
# Constraints honoured:
#   - Never touches ~/Embarcadero
#   - Never pkill -f (broker_lifecycle.sh tracks and kills only started PIDs)
#   - CXL NUMA node 2 used via EMBARLET_NUMA_BIND (set by run_multiclient.sh automatically)
#   - /dev/shm cleaned after every trial (shm_unlink in cleanup trap)
#   - Remote client nodes cleaned of stray throughput_test processes before each trial

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# ---------------------------------------------------------------------------
# Cluster topology
# ---------------------------------------------------------------------------
# moscxl is always the broker node. Clients are activated in order by NUM_CLIENTS.
# run_multiclient.sh's CLIENT_HOSTS roster: c4 c3 local c2 c1
# We override with c1/c2/c3 only (no local publisher, avoids broker CPU contention):
CLIENT_HOSTS_REMOTE="${CLIENT_HOSTS_REMOTE:-c1}"   # single remote client (E3/E9/smoke)
CLIENT_HOSTS_E2="${CLIENT_HOSTS_E2:-c1,c2,c3}"     # three remote clients for E2 scaling

# Broker IP for single-client runs (c1 only — uses 10G 10.10.10.x fabric)
BROKER_IP="${BROKER_IP:-10.10.10.10}"

# Broker IP for multi-client runs (c1+c2+c3) — must be reachable from ALL clients.
# c2 is only on 192.168.60.x, not on 10.10.10.x. 192.168.60.8 is reachable by all three.
BROKER_IP_MULTI="${BROKER_IP_MULTI:-192.168.60.8}"

# ---------------------------------------------------------------------------
# Mode / config
# ---------------------------------------------------------------------------
if [[ "${SMOKE:-0}" == "1" ]]; then
    NUM_TRIALS="${NUM_TRIALS:-1}"
    WARMUP_TRIALS="${WARMUP_TRIALS:-0}"
    TOTAL_BYTES="${TOTAL_BYTES:-$((512 * 1024 * 1024))}"   # 512 MiB
    LOAD_POINTS_MBPS="${LOAD_POINTS_MBPS:-100 500}"
    NUM_CLIENTS_E3="${NUM_CLIENTS_E3:-1}"
    NUM_CLIENTS_E2="${NUM_CLIENTS_E2:-1}"
    echo "[overnight] SMOKE mode: ${NUM_TRIALS} trial(s), 512 MiB, 2 load pts, 1 client"
else
    NUM_TRIALS="${NUM_TRIALS:-4}"           # 4 total → 3 measured after warmup discard
    WARMUP_TRIALS="${WARMUP_TRIALS:-1}"
    TOTAL_BYTES="${TOTAL_BYTES:-$((4 * 1024 * 1024 * 1024))}"   # 4 GiB per client
    LOAD_POINTS_MBPS="${LOAD_POINTS_MBPS:-100 250 500 750 1000 1500 2000}"
    NUM_CLIENTS_E3="${NUM_CLIENTS_E3:-1}"   # single remote publisher for latency
    NUM_CLIENTS_E2="${NUM_CLIENTS_E2:-3}"   # 3 remote publishers for throughput scaling
    echo "[overnight] FULL mode: ${NUM_TRIALS} trials (${WARMUP_TRIALS} warm-up discarded)"
fi

MSG_SIZE="${MSG_SIZE:-1024}"
NUM_BROKERS="${NUM_BROKERS:-4}"
SKIP_BASELINES="${SKIP_BASELINES:-0}"

# Shared libs that may be missing or wrong-version on client nodes.
# cluster_setup.sh collects these into $PROJECT_ROOT/lib/; we point clients there.
# run_multiclient.sh honours CLIENT_LD_LIBRARY_PATH and passes it to remote throughput_test.
REMOTE_EMBAR_ROOT="${REMOTE_EMBAR_ROOT:-$HOME/Embarcadero}"
export CLIENT_LD_LIBRARY_PATH="$REMOTE_EMBAR_ROOT/lib"

RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)_overnight}"
OUT_BASE="$PROJECT_ROOT/data/overnight_eval/$RUN_TAG"
LOG_DIR="$OUT_BASE/logs"
mkdir -p "$LOG_DIR"

SUMMARY_LOG="$OUT_BASE/sweep_summary.log"
touch "$SUMMARY_LOG"

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY_LOG"; }

pass_cell() { log "PASS [$1]"; }
fail_cell() { log "FAIL [$1] — see $LOG_DIR/${1}.log"; }

# ---------------------------------------------------------------------------
# Pre-flight: verify cluster reachability and binary presence on all nodes
# ---------------------------------------------------------------------------
preflight_check() {
    log "Pre-flight: checking cluster connectivity and binary sync"
    local all_ok=1

    for host in c1 c2 c3; do
        if ssh -o ConnectTimeout=5 -o BatchMode=yes "$host" \
                "test -x ~/Embarcadero/build/bin/throughput_test" 2>/dev/null; then
            log "  $host: throughput_test OK"
        else
            log "  $host: throughput_test MISSING — run cluster_setup.sh first"
            all_ok=0
        fi
    done

    if [[ "$all_ok" -eq 0 ]]; then
        log "Pre-flight FAILED. Running cluster_setup.sh to sync bins..."
        bash "$SCRIPT_DIR/cluster_setup.sh" || {
            log "cluster_setup.sh failed — aborting"
            exit 1
        }
        log "Sync done, re-checking..."
        for host in c1 c2 c3; do
            ssh -o BatchMode=yes "$host" \
                "test -x ~/Embarcadero/build/bin/throughput_test" || {
                log "FATAL: $host still missing binary after sync"
                exit 1
            }
        done
    fi

    log "Pre-flight: all client nodes OK"
}

# ---------------------------------------------------------------------------
# Cleanup helper: remote clients + local shm
# ---------------------------------------------------------------------------
cleanup_remote_stray_procs() {
    local hosts="${1:-c1 c2 c3}"
    for host in $hosts; do
        # Kill any stale throughput_test left from a prior aborted run.
        # Intentionally conservative: only kill by exact binary name, never pkill -f.
        ssh -o BatchMode=yes "$host" \
            'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
    done
}

cleanup_shm_all() {
    # Clean /dev/shm on moscxl and all clients
    local shm_name="${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}"
    rm -f "/dev/shm${shm_name}" 2>/dev/null || true
    for host in c1 c2 c3; do
        ssh -o BatchMode=yes "$host" \
            "rm -f /dev/shm${shm_name} 2>/dev/null; true" 2>/dev/null || true
    done
}

# ---------------------------------------------------------------------------
# Core cell runner — all cells go through run_multiclient.sh
# run_multiclient handles: shm, numactl, hugepages, cleanup, barrier start
#
# run_multi_cell <label> <num_clients> <client_hosts_csv> [extra_env...]
# ---------------------------------------------------------------------------
run_multi_cell() {
    local label="$1"
    local nclients="$2"
    local client_csv="$3"
    shift 3
    local cell_log="$LOG_DIR/${label}.log"
    log "START [$label] clients=$nclients hosts=$client_csv"

    # Clean up stray processes from any previous run before starting
    cleanup_remote_stray_procs "$(echo "$client_csv" | tr ',' ' ')"
    cleanup_shm_all

    if env "$@" \
        NUM_CLIENTS="$nclients" \
        CLIENT_HOSTS_CSV="$client_csv" \
        CLIENT_NUMAS_CSV="$(printf '1,%.0s' $(seq 1 "$nclients") | sed 's/,$//')" \
        NUM_BROKERS="$NUM_BROKERS" \
        NUM_TRIALS="$NUM_TRIALS" \
        WARMUP_TRIALS="$WARMUP_TRIALS" \
        TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
        MESSAGE_SIZE="$MSG_SIZE" \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        OUT_BASE="$OUT_BASE/multiclient" \
        BENCHMARK_TAG="$RUN_TAG/$label" \
        bash "$SCRIPT_DIR/run_multiclient.sh" >"$cell_log" 2>&1; then
        pass_cell "$label"
    else
        fail_cell "$label"
        # Always clean up even on failure
        cleanup_remote_stray_procs "$(echo "$client_csv" | tr ',' ' ')"
        cleanup_shm_all
    fi
}

# ---------------------------------------------------------------------------
# Latency cell runner — delegates to run_latency_vs_load.sh with a single
# remote publisher (SCENARIO=remote, REMOTE_CLIENT_HOST=<host>)
# ---------------------------------------------------------------------------
run_latency_cell() {
    local label="$1"
    shift
    local cell_log="$LOG_DIR/${label}.log"
    log "START latency [$label]"

    # Use first remote client for latency runs
    local client_host
    client_host="$(echo "$CLIENT_HOSTS_REMOTE" | cut -d',' -f1)"

    cleanup_remote_stray_procs "$client_host"
    cleanup_shm_all

    if env "$@" \
        NUM_TRIALS="$NUM_TRIALS" \
        WARMUP_TRIALS="$WARMUP_TRIALS" \
        TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
        MSG_SIZE="$MSG_SIZE" \
        LOAD_POINTS_MBPS="$LOAD_POINTS_MBPS" \
        NUM_BROKERS="$NUM_BROKERS" \
        SCENARIO=remote \
        REMOTE_CLIENT_HOST="$client_host" \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        PACING_MODE=steady \
        BENCHMARK_TAG="$RUN_TAG" \
        OUT_BASE="$OUT_BASE/latency" \
        bash "$SCRIPT_DIR/run_latency_vs_load.sh" >"$cell_log" 2>&1; then
        pass_cell "$label"
    else
        fail_cell "$label"
        cleanup_remote_stray_procs "$client_host"
        cleanup_shm_all
    fi
}

# ---------------------------------------------------------------------------
log "===== Overnight eval sweep START — $RUN_TAG ====="
log "Commit: $(git rev-parse HEAD)"
log "Broker IP: $BROKER_IP  Clients (E3): $CLIENT_HOSTS_REMOTE  Clients (E2): $CLIENT_HOSTS_E2"
log "Output: $OUT_BASE"
log "Trials: $NUM_TRIALS (warmup=$WARMUP_TRIALS discarded, $(( NUM_TRIALS - WARMUP_TRIALS )) measured)"
log "Total bytes/client: $TOTAL_BYTES  MSG_SIZE: $MSG_SIZE  NUM_BROKERS: $NUM_BROKERS"
log "Skip baselines: $SKIP_BASELINES"

preflight_check

# ===========================================================================
# PART A — E2: THROUGHPUT MATRIX  (Embarcadero + baselines × RF × clients)
# ===========================================================================
log "===== PART A: E2 throughput matrix ====="

# Single-client throughput (N=1, all-remote — the clean comparison cell)
for rf in 0 1; do
    run_multi_cell "e2_embar5_rf${rf}_n1" 1 "$CLIENT_HOSTS_REMOTE" \
        SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=$rf \
        TEST_TYPE=5 EMBARCADERO_RUNTIME_MODE=latency
done

for rf in 0 1; do
    run_multi_cell "e2_embar0_rf${rf}_n1" 1 "$CLIENT_HOSTS_REMOTE" \
        SEQUENCER=EMBARCADERO ORDER=0 ACK=1 REPLICATION_FACTOR=$rf \
        TEST_TYPE=5
done

if [[ "$SKIP_BASELINES" != "1" ]]; then
    run_multi_cell "e2_corfu_rf0_n1" 1 "$CLIENT_HOSTS_REMOTE" \
        SEQUENCER=CORFU ORDER=2 ACK=1 REPLICATION_FACTOR=0 \
        TEST_TYPE=5 EMBARCADERO_CORFU_SEQ_IP="$BROKER_IP"

    run_multi_cell "e2_scalog_rf0_n1" 1 "$CLIENT_HOSTS_REMOTE" \
        SEQUENCER=SCALOG ORDER=1 ACK=1 REPLICATION_FACTOR=0 \
        TEST_TYPE=5 SKIP_REMOTE_SCALOG_SEQUENCER=1 \
        EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP"

    run_multi_cell "e2_lazylog_rf0_n1" 1 "$CLIENT_HOSTS_REMOTE" \
        SEQUENCER=LAZYLOG ORDER=2 ACK=1 REPLICATION_FACTOR=0 \
        TEST_TYPE=5 SKIP_REMOTE_LAZYLOG_SEQUENCER=1
fi

# Multi-client throughput scaling N=1,2,3 (all-remote — E2 scaling figure)
if [[ "${SMOKE:-0}" != "1" ]]; then
    for nc in 2 3; do
        # Use first $nc hosts from CLIENT_HOSTS_E2
        local_csv="$(echo "$CLIENT_HOSTS_E2" | tr ',' '\n' | head -"$nc" | tr '\n' ',' | sed 's/,$//')"
        # N>=2 runs must use BROKER_IP_MULTI (192.168.60.8) — c2 is not on 10.10.10.x
        run_multi_cell "e2_embar5_rf0_n${nc}" "$nc" "$local_csv" \
            SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=0 \
            TEST_TYPE=5 EMBARCADERO_RUNTIME_MODE=latency \
            EMBARCADERO_HEAD_ADDR="$BROKER_IP_MULTI"

        if [[ "$SKIP_BASELINES" != "1" ]]; then
            run_multi_cell "e2_corfu_rf0_n${nc}" "$nc" "$local_csv" \
                SEQUENCER=CORFU ORDER=2 ACK=1 REPLICATION_FACTOR=0 \
                TEST_TYPE=5 EMBARCADERO_CORFU_SEQ_IP="$BROKER_IP_MULTI" \
                EMBARCADERO_HEAD_ADDR="$BROKER_IP_MULTI"

            run_multi_cell "e2_lazylog_rf0_n${nc}" "$nc" "$local_csv" \
                SEQUENCER=LAZYLOG ORDER=2 ACK=1 REPLICATION_FACTOR=0 \
                TEST_TYPE=5 SKIP_REMOTE_LAZYLOG_SEQUENCER=1 \
                EMBARCADERO_LAZYLOG_SEQ_IP="$BROKER_IP_MULTI" \
                EMBARCADERO_HEAD_ADDR="$BROKER_IP_MULTI"
        fi
    done
fi

# ===========================================================================
# PART B — E3: LATENCY-VS-LOAD SLO CURVES (single remote publisher)
# ===========================================================================
log "===== PART B: E3 latency-vs-load (SLO curves) ====="

# Primary headline: ORDER=5 with linger enabled
run_latency_cell "e3_embar5_linger_rf0" \
    SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
    EMBARCADERO_RUNTIME_MODE=latency

# ORDER=5 without linger — shows batch-fill cost (for comparison panel)
run_latency_cell "e3_embar5_nolinger_rf0" \
    SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=1 REPLICATION_FACTOR=0

# NOTE: ORDER=0 (unordered) does not produce delivery_latency_stats.csv
# because it uses Consume() not ConsumeOrdered(). Skip latency measurement for ORDER=0.
# (ORDER=0 throughput is in Part A; it's the baseline for comparing ordering overhead.)

if [[ "$SKIP_BASELINES" != "1" ]]; then
    run_latency_cell "e3_corfu_rf0" \
        SEQUENCER=CORFU ORDER=2 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
        EMBARCADERO_CORFU_SEQ_IP="$BROKER_IP"

    run_latency_cell "e3_scalog_rf0" \
        SEQUENCER=SCALOG ORDER=1 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
        SKIP_REMOTE_SCALOG_SEQUENCER=1 \
        EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP"

    run_latency_cell "e3_lazylog_rf0" \
        SEQUENCER=LAZYLOG ORDER=2 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
        SKIP_REMOTE_LAZYLOG_SEQUENCER=1 \
        EMBARCADERO_LAZYLOG_SEQ_IP="$BROKER_IP" \
        BROKER_LISTEN_ADDR="$BROKER_IP"
fi

# ===========================================================================
# PART C — E9: RF SENSITIVITY (ORDER=5, RF={0,1,2})
# ===========================================================================
log "===== PART C: E9 RF sensitivity ====="

for rf in 0 1 2; do
    run_multi_cell "e9_embar5_rf${rf}" "$NUM_CLIENTS_E3" "$CLIENT_HOSTS_REMOTE" \
        SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=$rf \
        TEST_TYPE=5 EMBARCADERO_RUNTIME_MODE=latency
done

# Latency vs RF
for rf in 0 1; do
    run_latency_cell "e9_latency_embar5_rf${rf}" \
        SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=1 REPLICATION_FACTOR=$rf \
        EMBARCADERO_RUNTIME_MODE=latency
done

# ===========================================================================
# PART D — E8: OVERHEAD PROBE (broker CPU during sustained load)
# ===========================================================================
log "===== PART D: E8 overhead probe ====="

OVERHEAD_LOG="$LOG_DIR/e8_overhead_probe.log"
{
    echo "=== E8 overhead probe: $(stamp) ==="
    echo "Commit: $(git rev-parse HEAD)"

    if ! command -v pidstat &>/dev/null; then
        echo "WARNING: pidstat not available (install sysstat). Skipping CPU probe."
    else
        # Run one throughput trial in the background, sample broker CPU for 20 s
        cleanup_remote_stray_procs c1
        cleanup_shm_all

        NUM_CLIENTS=1 \
        CLIENT_HOSTS_CSV=c1 \
        CLIENT_NUMAS_CSV=1 \
        NUM_BROKERS="$NUM_BROKERS" \
        NUM_TRIALS=1 \
        TOTAL_MESSAGE_SIZE=$((2 * 1024 * 1024 * 1024)) \
        MESSAGE_SIZE="$MSG_SIZE" \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=0 \
        TEST_TYPE=5 \
        OUT_BASE="$OUT_BASE/overhead" \
        BENCHMARK_TAG="${RUN_TAG}/e8" \
        bash "$SCRIPT_DIR/run_multiclient.sh" &
        BENCH_PID=$!

        sleep 6
        EMBARLET_PID=$(pgrep -x embarlet 2>/dev/null | head -1 || true)
        if [[ -n "$EMBARLET_PID" ]]; then
            echo "=== pidstat -t -p $EMBARLET_PID 1 20 ==="
            pidstat -t -p "$EMBARLET_PID" 1 20 || true
        else
            echo "WARNING: embarlet PID not found for pidstat sampling"
        fi
        wait "$BENCH_PID" || true
    fi
    echo "=== E8 probe complete: $(stamp) ==="
} >"$OVERHEAD_LOG" 2>&1
log "E8 overhead probe done"

# ===========================================================================
# FINAL CLEANUP + SUMMARY
# ===========================================================================
cleanup_shm_all
cleanup_remote_stray_procs "c1 c2 c3"

log ""
log "===== Overnight eval sweep COMPLETE — $RUN_TAG ====="
log "Results: $OUT_BASE"
log ""

# Aggregate and print summary tables
{
    echo "=== THROUGHPUT SUMMARY ==="
    find "$OUT_BASE/multiclient" -name "trial_results.csv" 2>/dev/null | sort | while read -r f; do
        dir=$(dirname "$f")
        label=$(basename "$(dirname "$dir")")
        echo "--- $label ---"
        cat "$f"
    done

    echo ""
    echo "=== LATENCY SUMMARY ==="
    find "$OUT_BASE/latency" -name "latency_summary.csv" 2>/dev/null | sort | while read -r f; do
        echo "--- $f ---"
        cat "$f"
    done
} | tee -a "$SUMMARY_LOG"

log "Full log: $SUMMARY_LOG"
echo ""
echo "Next steps:"
echo "  python3 scripts/aggregate_e2e_throughput.py $OUT_BASE/multiclient"
echo "  python3 scripts/aggregate_latency_vs_load.py $OUT_BASE/latency"
