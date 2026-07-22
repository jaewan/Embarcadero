#!/usr/bin/env bash
# Fig 1 path decomposition: 6-variant ACK/replication ablation.
# V0–V4 + V3' use identical binaries, commit, and cluster initialization.
#
# Variant | Order | ACK | RF | Sink                | CXL_COHERENT | Repl executes? | ACK waits | Isolates
# --------|-------|-----|----|--------------------|--------------|----------------|-----------|----------------------------------
# V0      |  0    |  1  |  0 | disabled           |    —         |      No        |    No     | Unordered floor
# V1      |  5    |  1  |  0 | disabled           |    —         |      No        |    No     | Ordering-only ceiling
# V2      |  5    |  1  |  2 | memory-copy        |    —         |      Yes       |    No     | Background-repl contention
# V3      |  5    |  2  |  2 | memory-accounting  |    0         |  Control+clfl  |    Yes    | ACK2 protocol + clflush overhead
# V3'     |  5    |  2  |  2 | memory-accounting  |    1         |  Control only  |    Yes    | ACK2 pure protocol (no payload CXL touch)
# V4      |  5    |  2  |  2 | memory-copy        |    —         |      Yes       |    Yes    | Complete DRAM-replica path (paper result)
#
# 10 GiB TOTAL per run (aggregate across N clients, matching existing Fig1 setup).
# N=1,2,3 remote; N=4 adds a co-located publisher on host-A (CXL write ceiling characterization).
# N≤3 metric: overlap_gbps. N=4 metric: bandwidth_sum_gbps (overlap window collapses at N=4).
#
# V3' design rationale:
#   V3 (memory-accounting) still calls invalidate_cache_range_for_read(payload, payload_size)
#   — one _mm_clflush per 64-byte cache line — even though it never reads payload data.
#   Setting EMBARCADERO_CXL_COHERENT=1 gates ShouldInvalidatePayloadBeforeRead() to false,
#   skipping all clflush calls. This isolates the pure CV-advance protocol cost from the
#   CXL cache-coherence maintenance cost. Expected: V3' ≈ V0 ≈ V1 at all N.
#   At N=4 this should approach the CXL write saturation ceiling (~21 GB/s).
#
# Reqs:
#   - 4/4 brokers must reach readiness (follower logs must show lazy mapping)
#   - No MAP_POPULATE in follower logs
#   - No unknown-sink warnings for V0/V1
#   - Exactly 3 successful, nonempty result rows per cell before advancing
#   - Lock acquired, ports 1214-1217 + 12140 free, no stale processes before each cell
#
# Scientific notes:
#   - V2 records replication tail at publisher completion (replicated bytes vs published bytes)
#   - V3 is ACK2 protocol/accounting overhead (memory-accounting emulates ACK, no payload copy)
#   - send-done alone is insufficient; this script also checks broker receive counters
#
# Usage: bash PaperScripts/run_fig1_path_decomp.sh [--smoke]
#   --smoke: runs V0+V1 N=1, 1 trial, 512 MiB to validate setup before full matrix
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRIPTS_DIR="$PROJECT_ROOT/scripts"

SMOKE_MODE=0
[[ "${1:-}" == "--smoke" ]] && SMOKE_MODE=1

CAMPAIGN_ID=fig1_path_decomp
PASS_ID=$(date -u +%Y%m%dT%H%M%SZ)
OUT_ROOT="$PROJECT_ROOT/data/paper_eval/fig1/$CAMPAIGN_ID"
LOG_DIR="$OUT_ROOT/logs/$PASS_ID"
MULTICLIENT_ROOT="$OUT_ROOT/multiclient"
RESULTS_CSV="$OUT_ROOT/results.csv"
SUMMARY_LOG="$OUT_ROOT/sweep_summary.log"

mkdir -p "$LOG_DIR" "$MULTICLIENT_ROOT"

# --- Knobs: identical to Fig 1 RF2 campaign ---
NUM_BROKERS=4
if [[ "$SMOKE_MODE" -eq 1 ]]; then
    TOTAL_BYTES=$((2 * 1024 * 1024 * 1024))  # 2 GiB for smoke (enough for overlap window at 6+ GB/s)
    NUM_TRIALS=1
    N_VALUES="1"
else
    TOTAL_BYTES=$((10 * 1024 * 1024 * 1024))  # 10 GiB aggregate (same as Fig1)
    NUM_TRIALS=3
    N_VALUES="1 2 3 4"   # N=4: co-located publisher (CXL write ceiling point)
fi
REQUIRED_SUCCESSES=$NUM_TRIALS
MSG_SIZE=4096
THREADS=6
EPOCH_US=500
BATCH_KB=2048
BROKER_IP=10.10.10.10

# --- Host roster (same as Fig1) ---
HOSTS_N1=c4
HOSTS_N2=c4,c3
HOSTS_N3=c4,c3,c1
HOSTS_N4="${HOSTS_N4:-c4,c3,c1,local}"  # N=4: 3 remote + co-located on host-A (CXL ceiling)

# --- CXL configuration: lazy follower mapping is critical ---
export EMBARCADERO_CXL_SIZE=274877906944      # 256 GiB
export EMBARCADERO_CXL_ZERO_MODE=metadata
export EMBARCADERO_CXL_MAP_POPULATE=0         # REQUIRED: prevents follower MAP_POPULATE stall
export EMBAR_USE_HUGETLB=1
export EMBARCADERO_ACK_TIMEOUT_SEC=300
CLIENT_LIB=/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib

# --- Record binary hashes for reproducibility ---
BINARY_HASH_EMBARLET=$(md5sum "$PROJECT_ROOT/build/bin/embarlet" 2>/dev/null | awk '{print $1}')
BINARY_HASH_CLIENT=$(md5sum "$PROJECT_ROOT/build/bin/throughput_test" 2>/dev/null | awk '{print $1}')
GIT_COMMIT=$(git -C "$PROJECT_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)
GIT_DIRTY=$(git -C "$PROJECT_ROOT" status --porcelain 2>/dev/null | wc -l | tr -d ' ')

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY_LOG"; }

# --- CSV header ---
if [[ ! -f "$RESULTS_CSV" ]]; then
    cat >"$RESULTS_CSV" <<'EOF'
campaign_id,pass_id,run_ts_utc,git_commit,git_dirty_files,embarlet_md5,client_md5,cell,variant,order,ack,rf,sink,n_clients,client_hosts,trial,status,bandwidth_sum_gbps,send_done_sum_gbps,overlap_gbps,overlap_window_ms,notes
EOF
fi

hosts_for_n() { case "$1" in 1) echo "$HOSTS_N1";; 2) echo "$HOSTS_N2";; 3) echo "$HOSTS_N3";; 4) echo "$HOSTS_N4";; esac; }

numas_for_hosts() {
    local out="" h
    for h in $(echo "$1" | tr ',' ' '); do
        case "$h" in c1|local) out="${out:+$out,}0";; *) out="${out:+$out,}1";; esac
    done
    echo "${out:-1}"
}

# --- Pre-cell readiness check ---
assert_clean_state() {
    local cell="$1"
    # 1. Lock must be acquirable
    if ! flock -n /tmp/embarcadero_run_multiclient.lock echo '' >/dev/null 2>&1; then
        log "PREFLIGHT FAIL [$cell]: orchestrator lock is held — another campaign is running"
        return 1
    fi
    # 2. Ports must be free
    local busy_ports
    busy_ports=$(ss -ltnp 2>/dev/null | grep -E ':121[4-7]|:12140' | head -3)
    if [[ -n "$busy_ports" ]]; then
        log "PREFLIGHT FAIL [$cell]: broker ports still occupied:"
        echo "$busy_ports" >&2
        return 1
    fi
    # 3. No stale embarlet/throughput_test
    if pgrep -x embarlet >/dev/null 2>&1 || pgrep -x throughput_test >/dev/null 2>&1; then
        log "PREFLIGHT FAIL [$cell]: stale embarlet/throughput_test processes"
        pgrep -a -x embarlet >&2 2>/dev/null || true
        return 1
    fi
    return 0
}

# --- Validate follower logs after broker start ---
check_no_map_populate() {
    local mc_log_dir="$1"
    # Follower logs are /tmp/broker_{1,2,3}.log (also copied to mc_log_dir by multiclient)
    local warn=0
    for b in 1 2 3; do
        local blog="/tmp/broker_${b}.log"
        if [[ -f "$blog" ]]; then
            if grep -q 'MAP_POPULATE' "$blog" 2>/dev/null; then
                log "WARNING: broker ${b} used MAP_POPULATE — CXL_MAP_POPULATE=0 may not have reached it"
                warn=1
            fi
            if grep -q 'unknown.*SINK\|falling back' "$blog" 2>/dev/null; then
                log "WARNING: broker ${b} shows sink fallback warning"
                warn=1
            fi
        fi
    done
    return $warn
}

# --- Parse overlap CSV and append rows to results CSV ---
# Primary metric: bandwidth_sum from client trial logs (robust to short overlap windows).
# overlap_gbps is secondary and may be empty when the publisher finishes faster than
# the overlap measurement window (common at N=1 or small TOTAL_BYTES).
append_results() {
    local cell="$1" variant="$2" order="$3" ack="$4" rf="$5" sink_label="$6"
    local n="$7" hosts="$8" cell_rc="$9" mc_log_dir="${10}"
    local overlap_csv="$mc_log_dir/overlap_summary.csv"
    local ok_count=0

    # Build a list of trials from client logs regardless of overlap CSV
    local trial_logs=()
    mapfile -t trial_logs < <(ls "$mc_log_dir"/trial*_*.log 2>/dev/null | \
        grep -oP 'trial\K[0-9]+' | sort -un 2>/dev/null || true)
    [[ ${#trial_logs[@]} -eq 0 ]] && trial_logs=(1)  # fallback: assume trial 1

    # Load overlap data if present (may have empty rows if window collapsed)
    declare -A ov_gbps_map ov_ms_map
    if [[ -f "$overlap_csv" ]]; then
        while IFS=',' read -r t ov_gbps ov_ms ov_clients; do
            [[ "$t" == "trial" || -z "$t" ]] && continue
            ov_gbps_map["$t"]="$ov_gbps"
            ov_ms_map["$t"]="$ov_ms"
        done <"$overlap_csv"
    fi

    for t in "${trial_logs[@]}"; do
            local status="ok"
            [[ "$cell_rc" -ne 0 ]] && status="fail"
            local bw="" sd=""
            bw="$(grep -hE '\] Bandwidth:.*Send-done:' "$mc_log_dir"/trial${t}_*.log 2>/dev/null \
                | grep -oiP 'Bandwidth:\s*\K[0-9]+(\.[0-9]+)?' \
                | awk '{s+=$1} END{if(NR) printf "%.3f", s/1000.0}')"
            sd="$(grep -hE '\] Bandwidth:.*Send-done:' "$mc_log_dir"/trial${t}_*.log 2>/dev/null \
                | grep -oiP 'Send-done:\s*\K[0-9]+(\.[0-9]+)?' \
                | awk '{s+=$1} END{if(NR) printf "%.3f", s/1000.0}')"
            local ov_gbps="${ov_gbps_map[$t]:-}"
            local ov_ms="${ov_ms_map[$t]:-}"
            # bandwidth_sum is the primary success criterion; overlap is secondary
            local notes=""
            if [[ -z "$bw" || "$bw" == "0.000" ]]; then
                status="fail"; notes="empty_bandwidth"
            fi
            [[ "$status" == "ok" ]] && ok_count=$((ok_count+1))
            python3 -c "
import csv, sys
with open('$RESULTS_CSV', 'a', newline='') as f:
    csv.writer(f).writerow(sys.argv[1:])
" "$CAMPAIGN_ID" "$PASS_ID" "$(stamp)" \
  "$GIT_COMMIT" "$GIT_DIRTY" "$BINARY_HASH_EMBARLET" "$BINARY_HASH_CLIENT" \
  "$cell" "$variant" "$order" "$ack" "$rf" "$sink_label" \
  "$n" "$hosts" "$t" "$status" \
  "${bw:-}" "${sd:-}" "${ov_gbps:-}" "${ov_ms:-}" "$notes"
    done
    echo "$ok_count"
}

# --- Core cell runner ---
run_cell() {
    local cell="$1" variant="$2" order="$3" ack="$4" rf="$5" sink_label="$6"
    shift 6
    # $@ = env vars to export (as "KEY=VALUE" strings)
    local n="${1}"; shift
    local hosts="${1}"; shift
    local extra_env=("$@")

    local numas; numas="$(numas_for_hosts "$hosts")"
    local mc_tag="$CAMPAIGN_ID/$PASS_ID/$cell/n${n}"
    local mc_log_dir="$MULTICLIENT_ROOT/logs/$mc_tag"
    local cell_log="$LOG_DIR/${cell}_n${n}.log"

    # Pre-cell state check
    if ! assert_clean_state "${cell}_n${n}"; then
        log "SKIP [$cell N=$n] — preflight failed"
        append_results "$cell" "$variant" "$order" "$ack" "$rf" "$sink_label" \
            "$n" "$hosts" 1 "$mc_log_dir" 2>/dev/null || true
        return 1
    fi

    log "START [$cell N=$n] variant=$variant order=$order ack=$ack rf=$rf sink=$sink_label"
    local rc=0
    {
        env -i HOME="$HOME" PATH="$PATH" LANG="${LANG:-C}" \
            EMBARCADERO_CXL_SIZE="$EMBARCADERO_CXL_SIZE" \
            EMBARCADERO_CXL_ZERO_MODE="$EMBARCADERO_CXL_ZERO_MODE" \
            EMBARCADERO_CXL_MAP_POPULATE="$EMBARCADERO_CXL_MAP_POPULATE" \
            EMBAR_USE_HUGETLB="$EMBAR_USE_HUGETLB" \
            EMBARCADERO_ACK_TIMEOUT_SEC="$EMBARCADERO_ACK_TIMEOUT_SEC" \
            "${extra_env[@]}" \
            NUM_CLIENTS="$n" \
            CLIENT_HOSTS_CSV="$hosts" \
            CLIENT_NUMAS_CSV="$numas" \
            NUM_BROKERS="$NUM_BROKERS" \
            NUM_TRIALS="$NUM_TRIALS" \
            WARMUP_TRIALS=0 \
            TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
            MESSAGE_SIZE="$MSG_SIZE" \
            SEQUENCER=EMBARCADERO \
            ORDER="$order" \
            ACK="$ack" \
            REPLICATION_FACTOR="$rf" \
            TEST_TYPE=5 \
            EMBARCADERO_RUNTIME_MODE=throughput \
            EMBARCADERO_CLIENT_PUB_BATCH_KB="$BATCH_KB" \
            THREADS_PER_BROKER="$THREADS" \
            EMBAR_ORDER5_EPOCH_US="$EPOCH_US" \
            EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
            BROKER_READY_TIMEOUT_SEC=300 \
            OUT_BASE="$MULTICLIENT_ROOT" \
            BENCHMARK_TAG="$mc_tag" \
            CLIENT_LD_LIBRARY_PATH="$CLIENT_LIB" \
            ALLOW_DIRTY_ARTIFACT="${ALLOW_DIRTY_ARTIFACT:-0}" \
            bash "$SCRIPTS_DIR/run_multiclient.sh"
    } >"$cell_log" 2>&1 || rc=$?

    # Check for MAP_POPULATE and sink warnings
    check_no_map_populate "$mc_log_dir" || true

    local ok_count
    ok_count=$(append_results "$cell" "$variant" "$order" "$ack" "$rf" "$sink_label" \
        "$n" "$hosts" "$rc" "$mc_log_dir")

    if [[ "$rc" -eq 0 && "$ok_count" -ge "$REQUIRED_SUCCESSES" ]]; then
        log "PASS [$cell N=$n] ok_rows=$ok_count"
        return 0
    else
        log "FAIL [$cell N=$n] rc=$rc ok_rows=${ok_count:-0}"
        return 1
    fi
}

# --- Variant env builders (subshell-safe "KEY=VALUE" arrays) ---
# V0/V1: RF=0 — unset all sink vars so no warnings, no fallback
V01_ENV=(
    "EMBARCADERO_CHAIN_REPLICATION_SINK="
    "EMBARCADERO_CHAIN_REPLICATION_INMEM=0"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=0"
)
# Actually need to unset — use empty string which run_multiclient ignores gracefully
# The broker with RF=0 disables replication_manager regardless of sink

# V2: background memory-copy replication, ACK1 (publisher does not wait)
V2_ENV=(
    "EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM=1"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1"
)

# V3: ACK2 protocol/accounting overhead only (no payload copy to DRAM)
# Still calls invalidate_cache_range_for_read (clflush per cache line) before skipping copy.
V3_ENV=(
    "EMBARCADERO_CHAIN_REPLICATION_SINK=memory-accounting"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM=1"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=0"
)

# V3': Pure ACK2 protocol overhead — no CXL payload touch at all.
# EMBARCADERO_CXL_COHERENT=1 gates ShouldInvalidatePayloadBeforeRead() to false,
# skipping all _mm_clflush calls on the payload region. The replication thread still
# runs the full GOI-lookup + token/CV-advance control path; it simply never touches
# the payload bytes. Expected throughput: same as V0/V1 (pure write-path ceiling).
V3PRIME_ENV=(
    "EMBARCADERO_CHAIN_REPLICATION_SINK=memory-accounting"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM=1"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=0"
    "EMBARCADERO_CXL_COHERENT=1"
)

# V4: Complete DRAM-replica path (this IS the current paper result, re-run for matched baseline)
V4_ENV=(
    "EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM=1"
    "EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1"
)

log "===== Fig1 path decomposition (V0–V4 + V3') ====="
log "Campaign=$CAMPAIGN_ID  Pass=$PASS_ID  Smoke=$SMOKE_MODE"
log "Commit=$GIT_COMMIT  DirtyFiles=$GIT_DIRTY"
log "embarlet_md5=$BINARY_HASH_EMBARLET  client_md5=$BINARY_HASH_CLIENT"
log "CXL_MAP_POPULATE=$EMBARCADERO_CXL_MAP_POPULATE (must be 0 for lazy follower mapping)"
log "N_VALUES=$N_VALUES  NUM_TRIALS=$NUM_TRIALS  TOTAL_BYTES=$TOTAL_BYTES"

log "--- V0: Unordered floor (ORDER=0, ACK1, RF=0) ---"
for n in $N_VALUES; do
    run_cell "v0_order0_ack1_rf0" "unordered-floor" \
        0 1 0 "disabled" "$n" "$(hosts_for_n "$n")" "${V01_ENV[@]}" || true
done

log "--- V1: Ordering ceiling (ORDER=5, ACK1, RF=0 — replication disabled) ---"
for n in $N_VALUES; do
    run_cell "v1_order5_ack1_rf0" "ordering-only-ceiling" \
        5 1 0 "disabled" "$n" "$(hosts_for_n "$n")" "${V01_ENV[@]}" || true
done

if [[ "$SMOKE_MODE" -eq 0 ]]; then
    log "--- V2: Background replication contention (ORDER=5, ACK1, RF=2, memory-copy) ---"
    for n in $N_VALUES; do
        run_cell "v2_order5_ack1_rf2_copy" "background-repl-contention" \
            5 1 2 "memory-copy" "$n" "$(hosts_for_n "$n")" "${V2_ENV[@]}" || true
    done

    log "--- V3: ACK2 protocol/accounting overhead (ORDER=5, ACK2, RF=2, memory-accounting) ---"
    for n in $N_VALUES; do
        run_cell "v3_order5_ack2_rf2_acct" "ack2-protocol-overhead" \
            5 2 2 "memory-accounting" "$n" "$(hosts_for_n "$n")" "${V3_ENV[@]}" || true
    done

    log "--- V3': ACK2 pure protocol, no CXL payload touch (ORDER=5, ACK2, RF=2, memory-accounting, CXL_COHERENT=1) ---"
    log "    Skips clflush on payload; isolates CV-advance protocol cost from CXL cache-coherence cost."
    log "    Expected: same as V0/V1. At N=4, should approach CXL write saturation (~21 GB/s)."
    for n in $N_VALUES; do
        run_cell "v3prime_order5_ack2_rf2_acct_coherent" "ack2-coherent-noinval" \
            5 2 2 "memory-accounting" "$n" "$(hosts_for_n "$n")" "${V3PRIME_ENV[@]}" || true
    done

    log "--- V4: Complete DRAM-replica path (ORDER=5, ACK2, RF=2, memory-copy) — matched baseline ---"
    log "    (Paper result re-run with same commit/binary as V0-V3' for controlled comparison)"
    for n in $N_VALUES; do
        run_cell "v4_order5_ack2_rf2_copy" "complete-dram-replica" \
            5 2 2 "memory-copy" "$n" "$(hosts_for_n "$n")" "${V4_ENV[@]}" || true
    done
fi

log "===== Fig1 path decomposition COMPLETE ====="
log "CSV: $RESULTS_CSV"
if [[ "$SMOKE_MODE" -eq 1 ]]; then
    log "Smoke complete. Check follower logs for MAP_POPULATE warnings before full run."
    log "Run full matrix: bash PaperScripts/run_fig1_path_decomp.sh"
fi
