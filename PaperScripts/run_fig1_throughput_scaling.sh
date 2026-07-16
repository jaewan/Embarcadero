#!/usr/bin/env bash
# PaperScripts/run_fig1_throughput_scaling.sh
#
# Fig 1: RF2/ACK2 append throughput vs N=1..4 clients (N=4 = 3 remote + local),
# Embar ORDER=5 + Corfu/Scalog/LazyLog, disk + memory-copy sinks.
#
# Results append to a stable campaign CSV so re-runs add trials:
#   data/paper_eval/fig1/<CAMPAIGN_ID>/results.csv
#
# Usage:
#   NUM_TRIALS=1 bash PaperScripts/run_fig1_throughput_scaling.sh
#   NUM_TRIALS=1 CAMPAIGN_ID=fig1_rf2_ack2_scaling bash PaperScripts/run_fig1_throughput_scaling.sh
#   WAIT_FOR_IDLE=0 bash ...   # skip idle wait (cluster already free)
#   TARGET_TRIALS=3 bash ...   # skip cells that already have ≥3 ok rows
#   ONLY_CELLS=fig1_embar_o5_disk_n1,fig1_embar_o5_mem_n1 bash ...
#
set -uo pipefail

PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$PAPER_DIR/.." && pwd)"
SCRIPTS_DIR="$PROJECT_ROOT/scripts"
cd "$PROJECT_ROOT"

# ---------------------------------------------------------------------------
# Campaign identity (stable → appendable CSV)
# ---------------------------------------------------------------------------
CAMPAIGN_ID="${CAMPAIGN_ID:-fig1_rf2_ack2_scaling}"
PASS_ID="${PASS_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/data/paper_eval/fig1/$CAMPAIGN_ID}"
LOG_DIR="$OUT_ROOT/logs/$PASS_ID"
MULTICLIENT_ROOT="$OUT_ROOT/multiclient"
RESULTS_CSV="${RESULTS_CSV:-$OUT_ROOT/results.csv}"
SUMMARY_LOG="$OUT_ROOT/sweep_summary.log"
FIG_PDF="$OUT_ROOT/fig1_throughput_scaling.pdf"
FIG_PNG="$OUT_ROOT/fig1_throughput_scaling.png"
LOCK_FILE="${LOCK_FILE:-/tmp/embarcadero_paper_fig1.lock}"
META_ROOT="$PROJECT_ROOT/.Replication/lazylog_metadata/${CAMPAIGN_ID}_${PASS_ID}"

mkdir -p "$LOG_DIR" "$MULTICLIENT_ROOT" "$META_ROOT/a" "$META_ROOT/b"

# ---------------------------------------------------------------------------
# Fig1 knobs (paper draft)
# ---------------------------------------------------------------------------
NUM_TRIALS="${NUM_TRIALS:-1}"
WARMUP_TRIALS="${WARMUP_TRIALS:-0}"
TARGET_TRIALS="${TARGET_TRIALS:-0}"   # 0 = always run; else skip cell with ≥N ok rows
TOTAL_BYTES="${TOTAL_BYTES:-$((10 * 1024 * 1024 * 1024))}"  # 10 GiB
MSG_SIZE="${MSG_SIZE:-4096}"
NUM_BROKERS="${NUM_BROKERS:-4}"
THREADS_THROUGHPUT="${THREADS_THROUGHPUT:-6}"
EPOCH_US_THROUGHPUT="${EPOCH_US_THROUGHPUT:-500}"
CLIENT_PUB_BATCH_KB="${CLIENT_PUB_BATCH_KB:-2048}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
WAIT_FOR_IDLE="${WAIT_FOR_IDLE:-1}"
RECHECK_DELAY_SEC="${RECHECK_DELAY_SEC:-300}"
SKIP_BASELINES="${SKIP_BASELINES:-0}"
# Scalog RF2 sinks matched (harness --replicate_to_disk + mem-copy + amortized sync).
# LazyLog ACK BW is metadata-bound (faithful AppendToAll); exclude from Fig1 sink panel
# by default. Set SKIP_LAZYLOG=0 to include. See PaperScripts/FIG1.md.
SKIP_SCALOG_LAZYLOG="${SKIP_SCALOG_LAZYLOG:-0}"
SKIP_LAZYLOG="${SKIP_LAZYLOG:-1}"
SKIP_DISK="${SKIP_DISK:-0}"
SKIP_MEM="${SKIP_MEM:-0}"
ALLOW_DIRTY_ARTIFACT="${ALLOW_DIRTY_ARTIFACT:-1}"
export ALLOW_DIRTY_ARTIFACT

export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
export EMBARCADERO_CXL_MAP_POPULATE="${EMBARCADERO_CXL_MAP_POPULATE:-0}"
# 256 GiB CXL region: 64 GiB default only fits 3×8 GiB segments after metadata
# (preflight: required=4 available=3) and aborts all 4-broker cells.
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-274877906944}"
export BROKER_REACHABILITY_TIMEOUT_SEC="${BROKER_REACHABILITY_TIMEOUT_SEC:-60}"
export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-300}"
# Dual NVMe: primary on root NVMe, replica on /mnt/nvme0.
export EMBARCADERO_REPLICA_DISK_DIRS="${EMBARCADERO_REPLICA_DISK_DIRS:-$PROJECT_ROOT/.Replication/disk0,/mnt/nvme0/replication/disk1}"

CLIENT_LIB="${CLIENT_LD_LIBRARY_PATH:-/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib}"
export CLIENT_LD_LIBRARY_PATH="$CLIENT_LIB"

# N → hosts (N=4 is 3 remote + local)
HOSTS_N1="${HOSTS_N1:-c4}"
HOSTS_N2="${HOSTS_N2:-c4,c3}"
HOSTS_N3="${HOSTS_N3:-c4,c3,c1}"
HOSTS_N4="${HOSTS_N4:-c4,c3,c1,local}"
N_VALUES="${N_VALUES:-1 2 3 4}"

# Cross-host RF=2: replica A on broker (moscxl) — distinct failure domains.
# Replica B runs on c4 (mos182) but c4 only exposes port 22.
# SSH local-port-forward tunnels broker:50082 → c4:127.0.0.1:50082 so broker
# RPCs reach c4's replica without requiring direct port access.
# Endpoints seen by embarlet brokers: 127.0.0.1:50081 (local A), 127.0.0.1:50082 (tunnel→B).
LAZYLOG_METADATA_HOST_A="${LAZYLOG_METADATA_HOST_A:-local}"  # local = broker itself
LAZYLOG_METADATA_HOST_B="${LAZYLOG_METADATA_HOST_B:-c4}"
LAZYLOG_METADATA_PORT_A="${LAZYLOG_METADATA_PORT_A:-50081}"
LAZYLOG_METADATA_PORT_B="${LAZYLOG_METADATA_PORT_B:-50082}"
# Both endpoints use 127.0.0.1: A is local; B reaches c4 via SSH tunnel on the same port.
LAZYLOG_METADATA_IP_A="${LAZYLOG_METADATA_IP_A:-127.0.0.1}"
LAZYLOG_METADATA_IP_B="${LAZYLOG_METADATA_IP_B:-127.0.0.1}"
LAZYLOG_RF2_METADATA_ENDPOINTS="${LAZYLOG_RF2_METADATA_ENDPOINTS:-${LAZYLOG_METADATA_IP_A}:${LAZYLOG_METADATA_PORT_A},${LAZYLOG_METADATA_IP_B}:${LAZYLOG_METADATA_PORT_B}}"

# Exclusive campaign lock (does NOT wrap run_multiclient's flock).
exec 9>"$LOCK_FILE"
flock -n 9 || { echo "ERROR: another Fig1 campaign owns $LOCK_FILE" >&2; exit 1; }

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY_LOG"; }

is_cluster_busy() {
    local host
    if pgrep -x embarlet >/dev/null || pgrep -x throughput_test >/dev/null ||
       pgrep -f '[r]un_overnight_eval\.sh' >/dev/null ||
       pgrep -f '[r]un_multiclient\.sh' >/dev/null ||
       pgrep -f '[r]un_e2_throughput_matrix\.sh' >/dev/null; then
        return 0
    fi
    for host in c4 c3 c1; do
        if ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" \
            'pgrep -x throughput_test >/dev/null' 2>/dev/null; then
            return 0
        fi
    done
    return 1
}

rf2_disk_env() {
    echo "EMBARCADERO_REPLICA_DISK_DIRS=$EMBARCADERO_REPLICA_DISK_DIRS"
    echo "EMBARCADERO_CHAIN_REPLICATION_SINK=disk-durable"
    echo "EMBARCADERO_CHAIN_REPLICATION_INMEM=0"
    echo "EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=0"
}

rf2_memcopy_env() {
    echo "EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy"
    echo "EMBARCADERO_CHAIN_REPLICATION_INMEM=1"
    echo "EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1"
}

hosts_for_n() {
    case "$1" in
        1) echo "$HOSTS_N1" ;;
        2) echo "$HOSTS_N2" ;;
        3) echo "$HOSTS_N3" ;;
        4) echo "$HOSTS_N4" ;;
        *) echo "ERROR: unsupported N=$1" >&2; return 1 ;;
    esac
}

numas_for_hosts() {
    local csv="$1" out="" h
    for h in $(echo "$csv" | tr ',' ' '); do
        case "$h" in
            c1|local) out="${out:+$out,}0" ;;
            *)        out="${out:+$out,}1" ;;
        esac
    done
    echo "${out:-1}"
}

should_run_cell() {
    local label="$1"
    if [[ -z "${ONLY_CELLS:-}" ]]; then
        return 0
    fi
    local IFS=',' c
    for c in $ONLY_CELLS; do
        [[ "$c" == "$label" ]] && return 0
    done
    log "SKIP [$label] (not in ONLY_CELLS)"
    return 1
}

# Count existing ok rows for a cell in the appendable CSV (python for quoted CSV).
ok_trial_count() {
    local cell="$1"
    python3 - "$RESULTS_CSV" "$cell" <<'PY'
import csv, sys
path, cell = sys.argv[1], sys.argv[2]
n = 0
try:
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("cell") == cell and row.get("status") == "ok":
                n += 1
except FileNotFoundError:
    pass
print(n)
PY
}

ensure_results_header() {
    if [[ ! -f "$RESULTS_CSV" ]]; then
        cat >"$RESULTS_CSV" <<'EOF'
campaign_id,pass_id,run_ts_utc,git_commit,cell,system,order,sink,n_clients,client_hosts,trial_in_pass,global_trial_seq,status,overlap_gbps,bandwidth_sum_gbps,send_done_sum_gbps,overlap_window_ms,msg_size,total_bytes,num_brokers,rf,ack,threads,batch_kb,epoch_us,multiclient_log_dir,notes
EOF
    fi
}

next_global_trial_seq() {
    local cell="$1"
    python3 - "$RESULTS_CSV" "$cell" <<'PY'
import csv, sys
path, cell = sys.argv[1], sys.argv[2]
n = 0
try:
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("cell") == cell:
                n += 1
except FileNotFoundError:
    pass
print(n + 1)
PY
}

csv_append_row() {
    # Append one properly quoted CSV row (handles commas in client_hosts).
    python3 - "$RESULTS_CSV" "$@" <<'PY'
import csv, sys
path = sys.argv[1]
fields = sys.argv[2:]
# Keep empty strings; do not convert.
with open(path, "a", newline="") as f:
    csv.writer(f).writerow(fields)
PY
}

cleanup_remote_stray_procs() {
    local hosts="${1:-c4 c3 c1}"
    local host
    for host in $hosts; do
        [[ "$host" == "local" ]] && continue
        ssh -o BatchMode=yes "$host" \
            'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
    done
}

cleanup_shm_all() {
    local shm_name="${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}"
    rm -f "/dev/shm${shm_name}" 2>/dev/null || true
    local host
    for host in c4 c3 c1; do
        ssh -o BatchMode=yes "$host" \
            "rm -f /dev/shm${shm_name} 2>/dev/null; true" 2>/dev/null || true
    done
}

# Parse one multiclient cell log + overlap CSV into appended result rows.
append_cell_results() {
    local label="$1"
    local system="$2"
    local order="$3"
    local sink="$4"
    local nclients="$5"
    local hosts_csv="$6"
    local cell_rc="$7"
    local mc_log_dir="$8"
    local cell_log="$9"

    local commit
    commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    local overlap_csv="$mc_log_dir/overlap_summary.csv"
    local trial=1
    local appended=0

    # Prefer per-trial overlap rows when present.
    if [[ -f "$overlap_csv" ]]; then
        while IFS=',' read -r t ov_gbps ov_ms ov_clients; do
            [[ "$t" == "trial" ]] && continue
            [[ -z "$t" ]] && continue
            local status="ok"
            local notes=""
            # Mark fail if cell harness failed even if a partial overlap row exists.
            if [[ "$cell_rc" -ne 0 ]]; then
                status="fail"
                notes="cell_rc=$cell_rc"
            fi
            local bw_sum="" sd_sum=""
            # Sum Bandwidth / Send-done across client logs for this trial (MB/s → GB/s).
            local trial_glob="$mc_log_dir/trial${t}_*.log"
            # shellcheck disable=SC2086
            if compgen -G "$trial_glob" >/dev/null; then
                bw_sum="$(
                    # shellcheck disable=SC2086
                    grep -hE '\] Bandwidth:.*Send-done:' $trial_glob 2>/dev/null \
                      | grep -oiP 'Bandwidth:\s*\K[0-9]+(\.[0-9]+)?' \
                      | awk '{s+=$1} END{if(NR) printf "%.6f", s/1000.0}'
                )"
                sd_sum="$(
                    # shellcheck disable=SC2086
                    grep -hE '\] Bandwidth:.*Send-done:' $trial_glob 2>/dev/null \
                      | grep -oiP 'Send-done:\s*\K[0-9]+(\.[0-9]+)?' \
                      | awk '{s+=$1} END{if(NR) printf "%.6f", s/1000.0}'
                )"
            fi
            local gseq
            gseq="$(next_global_trial_seq "$label")"
            csv_append_row \
                "$CAMPAIGN_ID" "$PASS_ID" "$(stamp)" "$commit" \
                "$label" "$system" "$order" "$sink" "$nclients" "$hosts_csv" \
                "$t" "$gseq" "$status" \
                "${ov_gbps:-}" "${bw_sum:-}" "${sd_sum:-}" "${ov_ms:-}" \
                "$MSG_SIZE" "$TOTAL_BYTES" "$NUM_BROKERS" 2 2 \
                "$THREADS_THROUGHPUT" "$CLIENT_PUB_BATCH_KB" "$EPOCH_US_THROUGHPUT" \
                "$mc_log_dir" "$notes"
            appended=1
        done <"$overlap_csv"
    fi

    if [[ "$appended" -eq 0 ]]; then
        local status="fail" notes="no_overlap_rows"
        [[ "$cell_rc" -eq 0 ]] && notes="missing_overlap_csv"
        local gseq
        gseq="$(next_global_trial_seq "$label")"
        csv_append_row \
            "$CAMPAIGN_ID" "$PASS_ID" "$(stamp)" "$commit" \
            "$label" "$system" "$order" "$sink" "$nclients" "$hosts_csv" \
            1 "$gseq" "$status" \
            "" "" "" "" \
            "$MSG_SIZE" "$TOTAL_BYTES" "$NUM_BROKERS" 2 2 \
            "$THREADS_THROUGHPUT" "$CLIENT_PUB_BATCH_KB" "$EPOCH_US_THROUGHPUT" \
            "$mc_log_dir" "$notes"
    fi

    # Refresh figure after every cell so writing can start early.
    if [[ -x "$(command -v python3)" ]]; then
        python3 "$PAPER_DIR/plot_fig1_throughput_scaling.py" \
            --csv "$RESULTS_CSV" --pdf "$FIG_PDF" --png "$FIG_PNG" \
            >>"$LOG_DIR/plot.log" 2>&1 || log "WARN: plot refresh failed (see $LOG_DIR/plot.log)"
    fi
}

run_fig1_cell() {
    local label="$1"
    local nclients="$2"
    local hosts_csv="$3"
    local system="$4"
    local order="$5"
    local sequencer="$6"
    local sink="$7"
    shift 7

    should_run_cell "$label" || return 0

    if [[ "$TARGET_TRIALS" -gt 0 ]]; then
        local have
        have="$(ok_trial_count "$label")"
        if [[ "$have" -ge "$TARGET_TRIALS" ]]; then
            log "SKIP [$label] (already $have ok trials ≥ TARGET_TRIALS=$TARGET_TRIALS)"
            return 0
        fi
    fi

    local numas_csv cell_log mc_tag mc_log_dir
    numas_csv="$(numas_for_hosts "$hosts_csv")"
    cell_log="$LOG_DIR/${label}.log"
    mc_tag="${CAMPAIGN_ID}/${PASS_ID}/${label}"
    mc_log_dir="$MULTICLIENT_ROOT/logs/$mc_tag"

    log "START [$label] system=$system order=$order sink=$sink n=$nclients hosts=$hosts_csv"

    cleanup_remote_stray_procs "$(echo "$hosts_csv" | tr ',' ' ')" || true
    cleanup_shm_all || true

    local rc=0
    {
        env "$@" \
            NUM_CLIENTS="$nclients" \
            CLIENT_HOSTS_CSV="$hosts_csv" \
            CLIENT_NUMAS_CSV="$numas_csv" \
            NUM_BROKERS="$NUM_BROKERS" \
            NUM_TRIALS="$NUM_TRIALS" \
            WARMUP_TRIALS="$WARMUP_TRIALS" \
            TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
            MESSAGE_SIZE="$MSG_SIZE" \
            SEQUENCER="$sequencer" \
            ORDER="$order" \
            ACK=2 \
            REPLICATION_FACTOR=2 \
            TEST_TYPE=5 \
            EMBARCADERO_RUNTIME_MODE=throughput \
            EMBARCADERO_CLIENT_PUB_BATCH_KB="$CLIENT_PUB_BATCH_KB" \
            THREADS_PER_BROKER="$THREADS_THROUGHPUT" \
            EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
            OUT_BASE="$MULTICLIENT_ROOT" \
            BENCHMARK_TAG="$mc_tag" \
            CLIENT_LD_LIBRARY_PATH="$CLIENT_LD_LIBRARY_PATH" \
            ALLOW_DIRTY_ARTIFACT="$ALLOW_DIRTY_ARTIFACT" \
            SKIP_CLUSTER_SETUP="${SKIP_CLUSTER_SETUP:-1}" \
            bash "$SCRIPTS_DIR/run_multiclient.sh"
    } >"$cell_log" 2>&1 || rc=$?

    if [[ "$rc" -eq 0 ]]; then
        log "PASS [$label]"
    else
        log "FAIL [$label] — see $cell_log"
    fi

    append_cell_results "$label" "$system" "$order" "$sink" \
        "$nclients" "$hosts_csv" "$rc" "$mc_log_dir" "$cell_log"

    cleanup_remote_stray_procs "$(echo "$hosts_csv" | tr ',' ' ')" || true
    cleanup_shm_all || true
    return 0
}

# ---------------------------------------------------------------------------
# Metadata replicas for LazyLog RF2 (cross-host: broker + c4)
# ---------------------------------------------------------------------------
metadata_pids=()
cleanup_metadata() {
    local pid
    for pid in "${metadata_pids[@]:-}"; do
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    done
    # Kill remote replica B on c4 and close its SSH tunnel
    if [[ "$LAZYLOG_METADATA_HOST_B" != "local" ]]; then
        ssh -o BatchMode=yes "$LAZYLOG_METADATA_HOST_B" \
            "pkill -f 'lazylog_metadata_replica.*${LAZYLOG_METADATA_PORT_B}' 2>/dev/null; true" 2>/dev/null || true
        # Close any lingering tunnel for this port
        pkill -f "ssh.*-L.*${LAZYLOG_METADATA_PORT_B}:127.0.0.1:${LAZYLOG_METADATA_PORT_B}.*${LAZYLOG_METADATA_HOST_B}" 2>/dev/null || true
    fi
}
trap cleanup_metadata EXIT INT TERM

start_lazylog_metadata() {
    if [[ "$SKIP_BASELINES" == "1" ]]; then
        return 0
    fi

    local replica_bin="$PROJECT_ROOT/build/bin/lazylog_metadata_replica"
    if [[ ! -x "$replica_bin" ]]; then
        log "FATAL: missing $replica_bin"
        exit 1
    fi

    # --- Replica A: local (broker/moscxl) ---
    mkdir -p "$META_ROOT/a"
    setsid "$replica_bin" \
        --listen "0.0.0.0:${LAZYLOG_METADATA_PORT_A}" \
        --sidecar "$META_ROOT/a/metadata.sidecar" \
        >"$META_ROOT/replica_a.log" 2>&1 < /dev/null 9>&- &
    metadata_pids+=("$!")

    # --- Replica B: remote host (c4/mos182) — distinct failure domain ---
    if [[ "$LAZYLOG_METADATA_HOST_B" == "local" ]]; then
        # Fallback single-host mode (not publication-grade for cross-host durability)
        log "WARN: replica B is local — same-host RF=2, not cross-host durable"
        mkdir -p "$META_ROOT/b"
        setsid "$replica_bin" \
            --listen "0.0.0.0:${LAZYLOG_METADATA_PORT_B}" \
            --sidecar "$META_ROOT/b/metadata.sidecar" \
            >"$META_ROOT/replica_b.log" 2>&1 < /dev/null 9>&- &
        metadata_pids+=("$!")
    else
        local remote_sidecar_dir="/home/domin/Embarcadero/.Replication/lazylog_meta_b/${CAMPAIGN_ID}_${PASS_ID}"
        local remote_sidecar="${remote_sidecar_dir}/metadata.sidecar"
        local remote_bin="/home/domin/Embarcadero/build/bin/lazylog_metadata_replica"
        local remote_log="/tmp/lazylog_meta_b_${CAMPAIGN_ID}_${PASS_ID}.log"

        # Verify remote binary exists and is executable.
        # NOTE: do NOT copy from broker — glibc version mismatch will prevent it
        # from running on c4. Build natively: ssh c4 "cd ~/Embarcadero/build && \
        #   cmake --build . --target lazylog_metadata_replica -j4"
        ssh -o BatchMode=yes "$LAZYLOG_METADATA_HOST_B" \
            "[[ -x '$remote_bin' ]]" 2>/dev/null || {
            log "FATAL: $remote_bin not found on $LAZYLOG_METADATA_HOST_B"
            log "  Build natively (do NOT scp from broker — glibc mismatch):"
            log "  ssh $LAZYLOG_METADATA_HOST_B 'cd ~/Embarcadero/build && cmake --build . --target lazylog_metadata_replica -j4'"
            exit 1
        }

        # Kill any stale replica B on the remote host
        ssh -o BatchMode=yes "$LAZYLOG_METADATA_HOST_B" \
            "pkill -f 'lazylog_metadata_replica.*${LAZYLOG_METADATA_PORT_B}' 2>/dev/null; true" 2>/dev/null || true

        # Start replica B on c4, listening on loopback only (port not exposed externally)
        ssh -o BatchMode=yes "$LAZYLOG_METADATA_HOST_B" \
            "mkdir -p '$remote_sidecar_dir' && setsid '$remote_bin' \
             --listen '127.0.0.1:${LAZYLOG_METADATA_PORT_B}' \
             --sidecar '$remote_sidecar' \
             >'$remote_log' 2>&1 </dev/null &" 2>/dev/null || {
            log "FATAL: failed to start replica B on $LAZYLOG_METADATA_HOST_B"
            exit 1
        }

        # Wait for replica B to bind its port on c4 before opening the tunnel.
        # ExitOnForwardFailure connects to the remote side immediately; if the
        # replica hasn't bound yet the tunnel fails. Poll via SSH until ready.
        local b_ready_deadline=$(( SECONDS + 15 ))
        local b_ready=0
        while [[ $SECONDS -lt $b_ready_deadline ]]; do
            if ssh -o BatchMode=yes "$LAZYLOG_METADATA_HOST_B" \
                "nc -z 127.0.0.1 ${LAZYLOG_METADATA_PORT_B}" 2>/dev/null; then
                b_ready=1; break
            fi
            sleep 0.5
        done
        if [[ $b_ready -eq 0 ]]; then
            log "FATAL: replica B on $LAZYLOG_METADATA_HOST_B:$LAZYLOG_METADATA_PORT_B did not start within 15s"
            exit 1
        fi

        # SSH local-port-forward: broker:PORT_B → c4:127.0.0.1:PORT_B
        # Replica is confirmed listening; tunnel will connect immediately.
        ssh -o BatchMode=yes -o ExitOnForwardFailure=yes \
            -L "${LAZYLOG_METADATA_PORT_B}:127.0.0.1:${LAZYLOG_METADATA_PORT_B}" \
            -N -f "$LAZYLOG_METADATA_HOST_B" 2>/dev/null || {
            log "FATAL: SSH tunnel to $LAZYLOG_METADATA_HOST_B:$LAZYLOG_METADATA_PORT_B failed"
            exit 1
        }
        # Track the tunnel PID for cleanup
        local tunnel_pid
        tunnel_pid=$(pgrep -n -f "ssh.*-L.*${LAZYLOG_METADATA_PORT_B}:127.0.0.1:${LAZYLOG_METADATA_PORT_B}.*${LAZYLOG_METADATA_HOST_B}" 2>/dev/null || true)
        [[ -n "$tunnel_pid" ]] && metadata_pids+=("$tunnel_pid")

        log "Replica B started on $LAZYLOG_METADATA_HOST_B via SSH tunnel (:${LAZYLOG_METADATA_PORT_B})"
    fi

    # Wait for both replicas to be ready via TCP port check (readiness, not just liveness)
    local deadline=$(( SECONDS + 20 ))
    local all_ready=0
    while [[ $SECONDS -lt $deadline ]]; do
        local a_ok=0 b_ok=0
        nc -z "${LAZYLOG_METADATA_IP_A}" "${LAZYLOG_METADATA_PORT_A}" 2>/dev/null && a_ok=1
        nc -z "${LAZYLOG_METADATA_IP_B}" "${LAZYLOG_METADATA_PORT_B}" 2>/dev/null && b_ok=1
        if [[ $a_ok -eq 1 && $b_ok -eq 1 ]]; then
            all_ready=1
            break
        fi
        sleep 0.5
    done

    if [[ $all_ready -eq 0 ]]; then
        log "FATAL: metadata replicas did not become ready within 20s"
        log "  A ($LAZYLOG_METADATA_IP_A:$LAZYLOG_METADATA_PORT_A): $(nc -z ${LAZYLOG_METADATA_IP_A} ${LAZYLOG_METADATA_PORT_A} 2>/dev/null && echo OK || echo FAIL)"
        log "  B ($LAZYLOG_METADATA_IP_B:$LAZYLOG_METADATA_PORT_B): $(nc -z ${LAZYLOG_METADATA_IP_B} ${LAZYLOG_METADATA_PORT_B} 2>/dev/null && echo OK || echo FAIL)"
        exit 1
    fi

    log "LazyLog metadata replicas up — A(local):${LAZYLOG_METADATA_PORT_A} B(${LAZYLOG_METADATA_HOST_B}):${LAZYLOG_METADATA_PORT_B}"
    log "Endpoints: $LAZYLOG_RF2_METADATA_ENDPOINTS"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
ensure_results_header

log "===== Fig1 START campaign=$CAMPAIGN_ID pass=$PASS_ID ====="
log "Commit: $(git rev-parse --short HEAD) dirty=$([[ -n $(git status --porcelain) ]] && echo yes || echo no)"
log "OUT_ROOT=$OUT_ROOT"
log "RESULTS_CSV=$RESULTS_CSV (appendable)"
log "NUM_TRIALS=$NUM_TRIALS TOTAL_BYTES=$TOTAL_BYTES MSG_SIZE=$MSG_SIZE THREADS=$THREADS_THROUGHPUT BATCH_KB=$CLIENT_PUB_BATCH_KB"
log "Replica dirs: $EMBARCADERO_REPLICA_DISK_DIRS"
log "SKIP_BASELINES=$SKIP_BASELINES SKIP_SCALOG_LAZYLOG=$SKIP_SCALOG_LAZYLOG CXL_SIZE=${EMBARCADERO_CXL_SIZE:-}"

if [[ "$WAIT_FOR_IDLE" == "1" ]]; then
    while is_cluster_busy; do
        log "cluster busy; retrying in ${RECHECK_DELAY_SEC}s"
        sleep "$RECHECK_DELAY_SEC"
    done
    log "cluster idle — beginning Fig1 pass"
fi

# Preflight replica dirs
IFS=',' read -r -a _rdirs <<< "$EMBARCADERO_REPLICA_DISK_DIRS"
for d in "${_rdirs[@]}"; do
    mkdir -p "$d" || { log "FATAL: cannot mkdir $d"; exit 1; }
    [[ -w "$d" ]] || { log "FATAL: unwritable $d"; exit 1; }
done

start_lazylog_metadata

# Embar first (figure story), then baselines. Within each: disk then mem, N=1..4.
run_system_sink() {
    local system="$1" order="$2" sequencer="$3" sink="$4"
    local n hosts label extra_env=()

    if [[ "$sink" == "disk" ]]; then
        [[ "$SKIP_DISK" == "1" ]] && return 0
        # shellcheck disable=SC2207
        extra_env=( $(rf2_disk_env) )
    else
        [[ "$SKIP_MEM" == "1" ]] && return 0
        # shellcheck disable=SC2207
        extra_env=( $(rf2_memcopy_env) )
    fi

    for n in $N_VALUES; do
        hosts="$(hosts_for_n "$n")" || continue
        label="fig1_${system}_o${order}_${sink}_n${n}"

        case "$sequencer" in
            EMBARCADERO)
                run_fig1_cell "$label" "$n" "$hosts" "$system" "$order" "$sequencer" "$sink" \
                    EMBAR_ORDER5_EPOCH_US="$EPOCH_US_THROUGHPUT" \
                    ${extra_env[@]+"${extra_env[@]}"}
                ;;
            CORFU)
                run_fig1_cell "$label" "$n" "$hosts" "$system" "$order" "$sequencer" "$sink" \
                    EMBARCADERO_CORFU_SEQ_IP="$BROKER_IP" \
                    ${extra_env[@]+"${extra_env[@]}"}
                ;;
            SCALOG)
                run_fig1_cell "$label" "$n" "$hosts" "$system" "$order" "$sequencer" "$sink" \
                    SKIP_REMOTE_SCALOG_SEQUENCER=1 \
                    EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP" \
                    ${extra_env[@]+"${extra_env[@]}"}
                ;;
            LAZYLOG)
                # LazyLog faithful ACK is metadata-fdatasync-bound regardless of data sink.
                # Running LazyLog with a mem sink produces results indistinguishable from disk
                # (both dominated by metadata fdatasync) but labelled "mem" — misleading.
                # Guard: skip LazyLog mem cells silently. Use SKIP_MEM=0 for Embar/Corfu/Scalog;
                # LazyLog disk is the only scientifically meaningful variant.
                if [[ "$sink" != "disk" ]]; then
                    log "SKIP [$label] (LazyLog mem sink — metadata fdatasync dominates; use FIG2_LAZYLOG.md)"
                    return 0
                fi
                run_fig1_cell "$label" "$n" "$hosts" "$system" "$order" "$sequencer" "$sink" \
                    SKIP_REMOTE_LAZYLOG_SEQUENCER=1 \
                    EMBARCADERO_LAZYLOG_SEQ_IP="$BROKER_IP" \
                    REQUIRE_FAITHFUL_LAZYLOG=1 \
                    EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS="$LAZYLOG_RF2_METADATA_ENDPOINTS" \
                    ${extra_env[@]+"${extra_env[@]}"}
                ;;
        esac
    done
}

# Priority order for early figure: Embar disk → Embar mem → baselines.
run_system_sink embar 5 EMBARCADERO disk
run_system_sink embar 5 EMBARCADERO mem

if [[ "$SKIP_BASELINES" != "1" ]]; then
    run_system_sink corfu 2 CORFU disk
    run_system_sink corfu 2 CORFU mem
    if [[ "$SKIP_SCALOG_LAZYLOG" == "1" ]]; then
        log "SKIP Scalog/LazyLog (SKIP_SCALOG_LAZYLOG=1)"
    else
        run_system_sink scalog 1 SCALOG disk
        run_system_sink scalog 1 SCALOG mem
        if [[ "$SKIP_LAZYLOG" == "1" ]]; then
            log "SKIP LazyLog (SKIP_LAZYLOG=1 — metadata-bound; not a fair data-sink A/B)"
        else
            run_system_sink lazylog 2 LAZYLOG disk
            run_system_sink lazylog 2 LAZYLOG mem
        fi
    fi
fi

cleanup_shm_all
cleanup_remote_stray_procs "c4 c3 c1"

log "===== Fig1 COMPLETE campaign=$CAMPAIGN_ID pass=$PASS_ID ====="
log "CSV: $RESULTS_CSV"
log "Fig: $FIG_PDF / $FIG_PNG"
log "Re-run with same CAMPAIGN_ID to append more trials."
