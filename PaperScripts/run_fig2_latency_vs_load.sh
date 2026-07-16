#!/usr/bin/env bash
# PaperScripts/run_fig2_latency_vs_load.sh
#
# Fig 2: latency vs offered load under the paper shared-log contract.
#
# Primary panel: Embar O5 ACK2 RF=2 disk-durable (matches Fig1 / shared-log claim)
# Mechanism ablation (matched load): O0 ACK1 | O5 ACK1 RF0 | O5 ACK2 RF2
# Optional: matched-load baseline points only (SKIP_BASELINES=0)
#
# Results append to:
#   data/paper_eval/fig2/<CAMPAIGN_ID>/results.csv
#   data/paper_eval/fig2/<CAMPAIGN_ID>/mechanism_summary.csv
#
# Usage:
#   NUM_TRIALS=1 bash PaperScripts/run_fig2_latency_vs_load.sh
#   NUM_TRIALS=3 WARMUP_TRIALS=1 bash ...
#   ONLY_CELLS=fig2_embar_o5_ack2_rf2 bash ...
#   SKIP_MECHANISM=1 bash ...
#   INCLUDE_BASELINES=1 BASELINE_LOAD_MBPS="500 1000" bash ...
#   FIG2_PREFLIGHT_ONLY=1 bash ...   # validate binaries/media; start no cell
#
set -uo pipefail

PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$PAPER_DIR/.." && pwd)"
SCRIPTS_DIR="$PROJECT_ROOT/scripts"
cd "$PROJECT_ROOT"

CAMPAIGN_ID="${CAMPAIGN_ID:-fig2_latency_vs_load}"
PASS_ID="${PASS_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/data/paper_eval/fig2/$CAMPAIGN_ID}"
LOG_DIR="$OUT_ROOT/logs/$PASS_ID"
LATENCY_ROOT="$OUT_ROOT/latency"
RESULTS_CSV="${RESULTS_CSV:-$OUT_ROOT/results.csv}"
MECH_CSV="${MECH_CSV:-$OUT_ROOT/mechanism_summary.csv}"
SUMMARY_LOG="$OUT_ROOT/sweep_summary.log"
CONTRACT_MD="$OUT_ROOT/campaign_contract.md"
FIG_PDF="$OUT_ROOT/fig2_latency_vs_load.pdf"
FIG_PNG="$OUT_ROOT/fig2_latency_vs_load.png"
MECH_PDF="$OUT_ROOT/fig2_mechanism_ablation.pdf"
LOCK_FILE="${LOCK_FILE:-/tmp/embarcadero_paper_fig2.lock}"

mkdir -p "$LOG_DIR" "$LATENCY_ROOT"

# ---------------------------------------------------------------------------
# Fig2 publication knobs
# ---------------------------------------------------------------------------
NUM_TRIALS="${NUM_TRIALS:-1}"
WARMUP_TRIALS="${WARMUP_TRIALS:-0}"
TARGET_TRIALS="${TARGET_TRIALS:-0}"
TOTAL_BYTES="${TOTAL_BYTES:-$((4 * 1024 * 1024 * 1024))}"
MSG_SIZE="${MSG_SIZE:-1024}"
NUM_BROKERS="${NUM_BROKERS:-4}"
EPOCH_US_LATENCY="${EPOCH_US_LATENCY:-500}"
LOAD_POINTS_MBPS="${LOAD_POINTS_MBPS:-100 250 500 750 1000 1500 2000}"
# Matched load for Embar mechanism table (O0A1 / O5A1 / O5A2RF2).
MECHANISM_LOAD_MBPS="${MECHANISM_LOAD_MBPS:-500}"
# Optional 1–2 baseline points only (not a full baseline load sweep).
BASELINE_LOAD_MBPS="${BASELINE_LOAD_MBPS:-500 1000}"
PACING_MODE="${PACING_MODE:-open_loop}"
CLIENT_HOST="${CLIENT_HOST:-c4}"
BROKER_IP="${BROKER_IP:-10.10.10.10}"
WAIT_FOR_IDLE="${WAIT_FOR_IDLE:-1}"
RECHECK_DELAY_SEC="${RECHECK_DELAY_SEC:-300}"
# Primary = Embar RF2 ACK2. Baselines / RF0 companion / nolinger are opt-in.
INCLUDE_BASELINES="${INCLUDE_BASELINES:-0}"
SKIP_MECHANISM="${SKIP_MECHANISM:-0}"
SKIP_NOLINGER="${SKIP_NOLINGER:-1}"
SKIP_RF0_COMPANION="${SKIP_RF0_COMPANION:-1}"
SKIP_LAZYLOG="${SKIP_LAZYLOG:-1}"
ALLOW_DIRTY_ARTIFACT="${ALLOW_DIRTY_ARTIFACT:-1}"
export ALLOW_DIRTY_ARTIFACT

# Keep the campaign's durable-media contract separate from the per-cell
# environment.  RF0 cells intentionally unset EMBARCADERO_REPLICA_DISK_DIRS
# so they cannot accidentally inherit an RF2 disk sink.  Preflight and the
# later RF2 cells must nevertheless retain the original configured value.
REPLICA_DISK_DIRS_CONFIG="${EMBARCADERO_REPLICA_DISK_DIRS:-$PROJECT_ROOT/.Replication/disk0,/mnt/nvme0/replication/disk1}"
export EMBARCADERO_REPLICA_DISK_DIRS="$REPLICA_DISK_DIRS_CONFIG"

FIG2_FAST_CXL="${FIG2_FAST_CXL:-0}"
if [[ "$FIG2_FAST_CXL" == "1" ]]; then
  export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
  export EMBARCADERO_CXL_MAP_POPULATE="${EMBARCADERO_CXL_MAP_POPULATE:-0}"
else
  export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-full}"
  export EMBARCADERO_CXL_MAP_POPULATE="${EMBARCADERO_CXL_MAP_POPULATE:-0}"
fi
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-274877906944}"
export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}"
export BROKER_REACHABILITY_TIMEOUT_SEC="${BROKER_REACHABILITY_TIMEOUT_SEC:-60}"
export EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-300}"
export EMBAR_ORDER5_EPOCH_US="${EMBAR_ORDER5_EPOCH_US:-$EPOCH_US_LATENCY}"

CLIENT_LIB="${CLIENT_LD_LIBRARY_PATH:-/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib}"
export CLIENT_LD_LIBRARY_PATH="$CLIENT_LIB"

exec 9>"$LOCK_FILE"
flock -n 9 || { echo "ERROR: another Fig2 campaign owns $LOCK_FILE" >&2; exit 1; }

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY_LOG"; }

# ---------------------------------------------------------------------------
# Env helpers: RF0 vs RF2 disk-durable
# ---------------------------------------------------------------------------
clear_rf2_ambient() {
  unset EMBARCADERO_CHAIN_REPLICATION_SINK \
        EMBARCADERO_CHAIN_REPLICATION_INMEM \
        EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY \
        EMBARCADERO_CHAIN_REPLICATION_INMEM_BYTES_PER_SOURCE \
        EMBARCADERO_CHAIN_SYNC_BYTES \
        EMBARCADERO_CHAIN_SYNC_INTERVAL_MS \
        EMBARCADERO_REPLICA_DISK_ROOT \
        EMBARCADERO_REPLICA_DISK_WEIGHTS \
        EMBARCADERO_ACK2_OFFERED_RATE \
        EMBARCADERO_ACK2_RETENTION \
        EMBARCADERO_QUEUE_POOL_MAX_BYTES \
        EMBARCADERO_CORFU_REPLICA_ENDPOINTS \
        2>/dev/null || true
  unset EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS REQUIRE_FAITHFUL_LAZYLOG
  export REQUIRE_FAITHFUL_LAZYLOG=0
}

apply_rf0_env() {
  clear_rf2_ambient
  unset EMBARCADERO_REPLICA_DISK_DIRS 2>/dev/null || true
}

apply_rf2_disk_env() {
  clear_rf2_ambient
  export EMBARCADERO_CHAIN_REPLICATION_SINK=disk-durable
  export EMBARCADERO_CHAIN_REPLICATION_INMEM=0
  export EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=0
  unset EMBARCADERO_CHAIN_REPLICATION_INMEM_BYTES_PER_SOURCE
  export EMBARCADERO_REPLICA_DISK_DIRS="$REPLICA_DISK_DIRS_CONFIG"
  # Match Fig1 Embar amortization.
  export EMBARCADERO_CHAIN_SYNC_BYTES="${EMBARCADERO_CHAIN_SYNC_BYTES:-268435456}"
  export EMBARCADERO_CHAIN_SYNC_INTERVAL_MS="${EMBARCADERO_CHAIN_SYNC_INTERVAL_MS:-250}"
}

# ---------------------------------------------------------------------------
# Preflight
# ---------------------------------------------------------------------------
binary_has_latency_stats() {
  local bin="$1"
  # Do not use `strings | grep -q` here: under pipefail, grep exits as soon
  # as it finds the marker, strings receives SIGPIPE, and a valid instrumented
  # binary is falsely rejected.  grep -a scans the executable directly and
  # returns the intended binary result without a pipeline.
  grep -aFq 'publish_to_deliver_latency' "$bin" 2>/dev/null
}

preflight_fig2() {
  local missing=0 bin
  for bin in \
      "$PROJECT_ROOT/build/bin/embarlet" \
      "$PROJECT_ROOT/build/bin/throughput_test" \
      "$PROJECT_ROOT/build/bin/corfu_global_sequencer" \
      "$PROJECT_ROOT/build/bin/scalog_global_sequencer" \
      "$PROJECT_ROOT/build/bin/lazylog_global_sequencer"; do
    if [[ ! -x "$bin" ]]; then
      log "FATAL: missing executable $bin"
      missing=1
    fi
  done
  [[ "$missing" -eq 0 ]] || { echo "ERROR: Fig2 preflight failed (missing binaries)" >&2; exit 1; }

  if ! binary_has_latency_stats "$PROJECT_ROOT/build/bin/throughput_test"; then
    log "FATAL: local throughput_test lacks COLLECT_LATENCY_STATS"
    echo "ERROR: cmake -S . -B build -DCOLLECT_LATENCY_STATS=ON && cmake --build build -j" >&2
    exit 1
  fi

  if [[ "$CLIENT_HOST" != "local" ]]; then
    if ! ssh -o BatchMode=yes -o ConnectTimeout=10 "$CLIENT_HOST" \
        "test -x ~/Embarcadero/build/bin/throughput_test" 2>/dev/null; then
      log "FATAL: remote $CLIENT_HOST missing throughput_test"
      exit 1
    fi
    if ! ssh -o BatchMode=yes "$CLIENT_HOST" \
        'grep -aFq publish_to_deliver_latency ~/Embarcadero/build/bin/throughput_test 2>/dev/null' \
        2>/dev/null; then
      log "FATAL: remote $CLIENT_HOST throughput_test not COLLECT_LATENCY_STATS"
      exit 1
    fi
  fi

  case "$PACING_MODE" in
    open_loop|steady) ;;
    *) echo "ERROR: PACING_MODE='$PACING_MODE'" >&2; exit 1 ;;
  esac

  # Dual NVMe for primary RF2 series
  IFS=',' read -r -a _rdirs <<< "$REPLICA_DISK_DIRS_CONFIG"
  local d
  for d in "${_rdirs[@]}"; do
    mkdir -p "$d" || { log "FATAL: cannot mkdir $d"; exit 1; }
    [[ -w "$d" ]] || { log "FATAL: unwritable $d"; exit 1; }
  done

  log "Preflight OK (latency stats, pacing=$PACING_MODE, replica_dirs=$REPLICA_DISK_DIRS_CONFIG)"
}

write_campaign_contract() {
  cat >"$CONTRACT_MD" <<EOF
# Fig2 campaign contract

- Campaign: \`$CAMPAIGN_ID\`
- Pass: \`$PASS_ID\`
- Commit: \`$(git rev-parse HEAD 2>/dev/null || echo unknown)\`
- Dirty: $([[ -n $(git status --porcelain 2>/dev/null) ]] && echo yes || echo no)

## Primary panel (shared-log SLO)
- Embar **ORDER=5 ACK=2 RF=2 disk-durable** latency vs offered load
- Pacing: \`$PACING_MODE\` (open_loop→burst; do not mix with steady in one plot)
- Load points (MB/s): $LOAD_POINTS_MBPS
- Publisher: 1× \`$CLIENT_HOST\` → \`$BROKER_IP\`
- Metric: publish→deliver p50/p99 (µs)

## Mechanism ablation (matched load = ${MECHANISM_LOAD_MBPS} MB/s)
- \`fig2_mech_embar_o0_ack1_rf0\` — unordered floor (pub ACK; no ordered deliver)
- \`fig2_mech_embar_o5_ack1_rf0\` — + ordering
- \`fig2_mech_embar_o5_ack2_rf2\` — + durable RF2 (same claim as primary)
- Table metric: **append→ack** p50/p99 for all three rows

## Optional
- Baselines at \`$BASELINE_LOAD_MBPS\` only if \`INCLUDE_BASELINES=1\`
- RF0 companion full sweep if \`SKIP_RF0_COMPANION=0\`
- Nolinger companion if \`SKIP_NOLINGER=0\`

## Knobs
- Msg / bytes: ${MSG_SIZE} B / ${TOTAL_BYTES} B
- Epoch µs: $EMBAR_ORDER5_EPOCH_US
- CXL: size=$EMBARCADERO_CXL_SIZE zero=$EMBARCADERO_CXL_ZERO_MODE populate=$EMBARCADERO_CXL_MAP_POPULATE
- Replica dirs: $REPLICA_DISK_DIRS_CONFIG
- Requires \`-DCOLLECT_LATENCY_STATS=ON\`
EOF
}

is_cluster_busy() {
    local host
    if pgrep -x embarlet >/dev/null || pgrep -x throughput_test >/dev/null; then
        return 0
    fi
    if pgrep -f '[r]un_overnight_eval\.sh' >/dev/null ||
       pgrep -f '[r]un_multiclient\.sh' >/dev/null ||
       pgrep -f '[r]un_fig1_throughput' >/dev/null ||
       pgrep -f '[r]un_latency_vs_load\.sh' >/dev/null; then
        return 0
    fi
    if pgrep -af '[r]un_fig2_latency' 2>/dev/null | grep -v "$$" >/dev/null; then
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

ok_trial_count() {
    local cell="$1" target="$2"
    python3 - "$RESULTS_CSV" "$cell" "$target" <<'PY'
import csv, sys
path, cell, target = sys.argv[1], sys.argv[2], sys.argv[3]
n = 0
try:
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if (row.get("cell") == cell and row.get("status") == "ok" and
                    str(row.get("target_mbps", "")).strip() == str(target)):
                n += 1
except FileNotFoundError:
    pass
print(n)
PY
}

ensure_results_header() {
    local expected="campaign_id,pass_id,run_ts_utc,git_commit,cell,panel,system,order,linger,n_clients,client_host,target_mbps,trial_in_pass,global_trial_seq,status,p50_us,p95_us,p99_us,achieved_offered_mbps,achieved_e2e_goodput_mbps,pub_ack_p50_us,pub_ack_p99_us,msg_size,total_bytes,num_brokers,rf,ack,pacing_mode,cxl_zero_mode,epoch_us,artifact_dir,notes"
    if [[ ! -f "$RESULTS_CSV" ]]; then
        echo "$expected" >"$RESULTS_CSV"
        return 0
    fi
    local have
    have="$(head -1 "$RESULTS_CSV")"
    if [[ "$have" != "$expected" ]]; then
        local bak="${RESULTS_CSV}.bak_schema_$(date -u +%Y%m%dT%H%M%SZ)"
        log "WARN: results.csv schema mismatch — moving to $bak"
        mv "$RESULTS_CSV" "$bak"
        echo "$expected" >"$RESULTS_CSV"
    fi
}

cleanup_remote_stray_procs() {
    local host="${1:-$CLIENT_HOST}"
    [[ "$host" == "local" ]] && return 0
    ssh -o BatchMode=yes "$host" \
        'pkill -x throughput_test 2>/dev/null; true' 2>/dev/null || true
}

cleanup_shm_all() {
    pkill -x embarlet 2>/dev/null || true
    pkill -x corfu_global_sequencer 2>/dev/null || true
    pkill -x scalog_global_sequencer 2>/dev/null || true
    pkill -x lazylog_global_sequencer 2>/dev/null || true
    sleep 1
}

refresh_plot() {
    if [[ -f "$PAPER_DIR/plot_fig2_latency_vs_load.py" ]]; then
        python3 "$PAPER_DIR/plot_fig2_latency_vs_load.py" \
            --csv "$RESULTS_CSV" \
            --pdf "$FIG_PDF" \
            --png "$FIG_PNG" \
            --mech-csv "$MECH_CSV" \
            --mech-pdf "$MECH_PDF" \
            >>"$LOG_DIR/plot.log" 2>&1 || log "WARN: plot refresh failed (see $LOG_DIR/plot.log)"
    fi
}

append_point_results() {
    local label="$1" panel="$2" system="$3" order="$4" linger="$5"
    local rf="$6" ack="$7" target="$8" cell_rc="$9" run_dir="${10}"

    local commit
    commit="$(git rev-parse HEAD 2>/dev/null || echo unknown)"

    python3 - "$RESULTS_CSV" "$CAMPAIGN_ID" "$PASS_ID" "$(stamp)" "$commit" \
        "$label" "$panel" "$system" "$order" "$linger" "$CLIENT_HOST" "$target" \
        "$cell_rc" "$run_dir" "$MSG_SIZE" "$TOTAL_BYTES" "$NUM_BROKERS" \
        "$rf" "$ack" "$PACING_MODE" "$EMBARCADERO_CXL_ZERO_MODE" "$EMBAR_ORDER5_EPOCH_US" <<'PY'
import csv, sys
from pathlib import Path

(results_csv, campaign_id, pass_id, run_ts, commit, label, panel, system, order, linger,
 client_host, target, cell_rc, run_dir, msg_size, total_bytes, num_brokers,
 rf, ack, pacing_mode, cxl_zero_mode, epoch_us) = sys.argv[1:]
cell_rc = int(cell_rc)
run_path = Path(run_dir)

def next_seq(cell, tgt):
    n = 0
    p = Path(results_csv)
    if not p.exists():
        return 1
    with p.open(newline="") as f:
        for row in csv.DictReader(f):
            if row.get("cell") == cell and str(row.get("target_mbps", "")).strip() == str(tgt):
                n += 1
    return n + 1

def append_row(fields):
    with open(results_csv, "a", newline="") as f:
        csv.writer(f).writerow(fields)

trial_csvs = list(run_path.rglob("trial_results.csv")) if run_path.exists() else []
trial_csvs.sort(key=lambda p: (len(p.parts), str(p)))
appended = 0
for trial_csv in trial_csvs:
    with trial_csv.open(newline="") as f:
        for row in csv.DictReader(f):
            tgt = str(row.get("target_mbps", "")).strip()
            if tgt and tgt != str(target):
                continue
            status = "ok"
            notes = ""
            if cell_rc != 0:
                status = "fail"
                notes = f"cell_rc={cell_rc}"
            p50 = (row.get("publish_to_deliver_p50_us") or "").strip()
            p99 = (row.get("publish_to_deliver_p99_us") or "").strip()
            pub50 = (row.get("pub_ack_p50_us") or "").strip()
            pub99 = (row.get("pub_ack_p99_us") or "").strip()
            # Mechanism / ORDER=0: allow missing deliver if pub ACK present.
            if panel == "mechanism" or str(order) == "0":
                if not pub50 or not pub99:
                    status = "fail"
                    notes = (notes + ";" if notes else "") + "missing_pub_ack_percentiles"
                if not p50:
                    notes = (notes + ";" if notes else "") + "no_deliver_metric"
            else:
                if not p50 or not p99:
                    status = "fail"
                    notes = (notes + ";" if notes else "") + "missing_latency_percentiles"
            try:
                ach = float(row.get("achieved_offered_load_mbps") or "nan")
                tgt_f = float(target)
                if ach == ach and tgt_f > 0 and ach < 0.5 * tgt_f:
                    notes = (notes + ";" if notes else "") + "saturated_offered_lt_50pct_target"
            except ValueError:
                pass
            gseq = next_seq(label, target)
            append_row([
                campaign_id, pass_id, run_ts, commit,
                label, panel, system, order, linger, "1", client_host,
                target, row.get("trial") or "1", str(gseq), status,
                p50, row.get("publish_to_deliver_p95_us") or "",
                p99,
                row.get("achieved_offered_load_mbps") or "",
                row.get("achieved_e2e_goodput_mbps") or "",
                pub50, pub99,
                msg_size, total_bytes, num_brokers, rf, ack,
                pacing_mode, cxl_zero_mode, epoch_us,
                row.get("artifact_dir") or str(run_path), notes,
            ])
            appended += 1
    if appended:
        break

if appended == 0:
    notes = "missing_trial_results"
    if cell_rc != 0:
        notes = f"cell_rc={cell_rc};missing_trial_results"
    gseq = next_seq(label, target)
    append_row([
        campaign_id, pass_id, run_ts, commit,
        label, panel, system, order, linger, "1", client_host,
        target, "1", str(gseq), "fail",
        "", "", "", "", "", "", "",
        msg_size, total_bytes, num_brokers, rf, ack,
        pacing_mode, cxl_zero_mode, epoch_us,
        str(run_path), notes,
    ])
print(f"appended={appended}")
PY
    refresh_plot
}

run_fig2_point() {
    local label="$1" panel="$2" system="$3" order="$4" linger="$5"
    local sequencer="$6" rf="$7" ack="$8" target="$9"
    shift 9

    should_run_cell "$label" || return 0

    if [[ "$TARGET_TRIALS" -gt 0 ]]; then
        local have
        have="$(ok_trial_count "$label" "$target")"
        if [[ "$have" -ge "$TARGET_TRIALS" ]]; then
            log "SKIP [$label @ ${target}MB/s] (already $have ok ≥ TARGET_TRIALS)"
            return 0
        fi
    fi

    local cell_log="$LOG_DIR/${label}_l${target}.log"
    local run_id="l${target}"
    local bench_tag="$PASS_ID"
    if [[ -e "$LATENCY_ROOT/$bench_tag/$label/run_$run_id" ]]; then
        run_id="l${target}_$(date -u +%H%M%S)"
    fi

    log "START [$label] panel=$panel target=${target} MB/s order=$order ack=$ack rf=$rf linger=$linger"

    if [[ "$rf" -ge 2 ]]; then
        apply_rf2_disk_env
    else
        apply_rf0_env
    fi

    cleanup_remote_stray_procs "$CLIENT_HOST" || true
    cleanup_shm_all || true

    local runtime_mode="latency"
    [[ "$linger" == "off" ]] && runtime_mode="throughput"

    local cell_rc=0
    {
        env "$@" \
            NUM_TRIALS="$NUM_TRIALS" \
            WARMUP_TRIALS="$WARMUP_TRIALS" \
            TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
            MSG_SIZE="$MSG_SIZE" \
            LOAD_POINTS_MBPS="$target" \
            NUM_BROKERS="$NUM_BROKERS" \
            SCENARIO=remote \
            REMOTE_CLIENT_HOST="$CLIENT_HOST" \
            EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
            BROKER_LISTEN_ADDR="$BROKER_IP" \
            PACING_MODE="$PACING_MODE" \
            SEQUENCER="$sequencer" \
            ORDER="$order" \
            ACK_LEVEL="$ack" \
            REPLICATION_FACTOR="$rf" \
            SYSTEM_LABEL="$label" \
            BENCHMARK_TAG="$bench_tag" \
            RUN_ID="$run_id" \
            OUT_BASE="$LATENCY_ROOT" \
            EMBARCADERO_RUNTIME_MODE="$runtime_mode" \
            EMBAR_ORDER5_EPOCH_US="$EMBAR_ORDER5_EPOCH_US" \
            EMBARCADERO_CXL_SIZE="$EMBARCADERO_CXL_SIZE" \
            EMBARCADERO_CXL_ZERO_MODE="$EMBARCADERO_CXL_ZERO_MODE" \
            EMBARCADERO_CXL_MAP_POPULATE="$EMBARCADERO_CXL_MAP_POPULATE" \
            EMBAR_USE_HUGETLB="$EMBAR_USE_HUGETLB" \
            SKIP_CLUSTER_SETUP="${SKIP_CLUSTER_SETUP:-1}" \
            CLIENT_LD_LIBRARY_PATH="$CLIENT_LD_LIBRARY_PATH" \
            bash "$SCRIPTS_DIR/run_latency_vs_load.sh"
    } >"$cell_log" 2>&1 || cell_rc=$?

    local actual_run_dir="$LATENCY_ROOT/$bench_tag/$label/run_$run_id"
    if [[ ! -d "$actual_run_dir" ]]; then
        actual_run_dir="$(find "$LATENCY_ROOT" -type d -path "*/$label/run_$run_id" 2>/dev/null | head -1 || true)"
        [[ -z "$actual_run_dir" ]] && actual_run_dir="$LATENCY_ROOT/$bench_tag/$label/run_$run_id"
    fi

    append_point_results "$label" "$panel" "$system" "$order" "$linger" \
        "$rf" "$ack" "$target" "$cell_rc" "$actual_run_dir"

    if [[ "$cell_rc" -eq 0 ]]; then
        log "PASS [$label @ ${target}MB/s]"
    else
        log "FAIL [$label @ ${target}MB/s] — see $cell_log"
    fi

    cleanup_remote_stray_procs "$CLIENT_HOST" || true
    cleanup_shm_all || true
    return 0
}

run_series_loads() {
    local label="$1" panel="$2" system="$3" order="$4" linger="$5"
    local sequencer="$6" rf="$7" ack="$8"
    shift 8
    local loads="${SERIES_LOADS:-$LOAD_POINTS_MBPS}"
    local target
    for target in $loads; do
        run_fig2_point "$label" "$panel" "$system" "$order" "$linger" \
            "$sequencer" "$rf" "$ack" "$target" "$@"
    done
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
apply_rf0_env
ensure_results_header
write_campaign_contract

log "===== Fig2 START campaign=$CAMPAIGN_ID pass=$PASS_ID ====="
log "Commit: $(git rev-parse --short HEAD) dirty=$([[ -n $(git status --porcelain) ]] && echo yes || echo no)"
log "OUT_ROOT=$OUT_ROOT RESULTS_CSV=$RESULTS_CSV"
log "PRIMARY: Embar O5 ACK2 RF2 disk-durable | loads=$LOAD_POINTS_MBPS | pacing=$PACING_MODE"
log "MECHANISM_LOAD_MBPS=$MECHANISM_LOAD_MBPS SKIP_MECHANISM=$SKIP_MECHANISM"
log "INCLUDE_BASELINES=$INCLUDE_BASELINES BASELINE_LOADS=$BASELINE_LOAD_MBPS"
log "SKIP_RF0_COMPANION=$SKIP_RF0_COMPANION SKIP_NOLINGER=$SKIP_NOLINGER SKIP_LAZYLOG=$SKIP_LAZYLOG"

preflight_fig2

# A non-mutating validation mode is useful before reserving the cluster for a
# publication pass.  It intentionally runs after the complete preflight,
# including local/remote instrumentation and durable-media checks, but before
# any wait loop, cleanup, broker, sequencer, or client action.
if [[ "${FIG2_PREFLIGHT_ONLY:-0}" == "1" ]]; then
    log "===== Fig2 PREFLIGHT COMPLETE (FIG2_PREFLIGHT_ONLY=1) ====="
    exit 0
fi

if [[ "$WAIT_FOR_IDLE" == "1" ]]; then
    while is_cluster_busy; do
        log "cluster busy; retrying in ${RECHECK_DELAY_SEC}s"
        sleep "$RECHECK_DELAY_SEC"
    done
    log "cluster idle — beginning Fig2 pass"
fi

# --- Primary: shared-log SLO curve ---
SERIES_LOADS="$LOAD_POINTS_MBPS" \
  run_series_loads fig2_embar_o5_ack2_rf2 primary embar 5 on EMBARCADERO 2 2

# Optional RF0 companion (ordering-only floor across loads)
if [[ "$SKIP_RF0_COMPANION" != "1" ]]; then
    SERIES_LOADS="$LOAD_POINTS_MBPS" \
      run_series_loads fig2_embar_o5_ack1_rf0 companion embar 5 on EMBARCADERO 0 1
fi

if [[ "$SKIP_NOLINGER" != "1" ]]; then
    SERIES_LOADS="$LOAD_POINTS_MBPS" \
      run_series_loads fig2_embar_o5_ack2_rf2_nolinger companion embar 5 off EMBARCADERO 2 2
fi

# --- Mechanism ablation at one matched load ---
if [[ "$SKIP_MECHANISM" != "1" ]]; then
    log "===== Mechanism ablation @ ${MECHANISM_LOAD_MBPS} MB/s ====="
    SERIES_LOADS="$MECHANISM_LOAD_MBPS" \
      run_series_loads fig2_mech_embar_o0_ack1_rf0 mechanism embar 0 on EMBARCADERO 0 1
    SERIES_LOADS="$MECHANISM_LOAD_MBPS" \
      run_series_loads fig2_mech_embar_o5_ack1_rf0 mechanism embar 5 on EMBARCADERO 0 1
    SERIES_LOADS="$MECHANISM_LOAD_MBPS" \
      run_series_loads fig2_mech_embar_o5_ack2_rf2 mechanism embar 5 on EMBARCADERO 2 2
fi

# --- Optional matched-load baselines (not a full sweep) ---
if [[ "$INCLUDE_BASELINES" == "1" ]]; then
    log "===== Matched-load baselines @ $BASELINE_LOAD_MBPS ====="
    SERIES_LOADS="$BASELINE_LOAD_MBPS" \
      run_series_loads fig2_corfu_o2_ack1_rf0 baseline corfu 2 na CORFU 0 1 \
        EMBARCADERO_CORFU_SEQ_IP="$BROKER_IP"
    SERIES_LOADS="$BASELINE_LOAD_MBPS" \
      run_series_loads fig2_scalog_o1_ack1_rf0 baseline scalog 1 na SCALOG 0 1 \
        SKIP_REMOTE_SCALOG_SEQUENCER=1 \
        EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP"
    if [[ "$SKIP_LAZYLOG" != "1" ]]; then
        SERIES_LOADS="$BASELINE_LOAD_MBPS" \
          run_series_loads fig2_lazylog_o2_ack1_rf0 baseline lazylog 2 na LAZYLOG 0 1 \
            SKIP_REMOTE_LAZYLOG_SEQUENCER=1 \
            EMBARCADERO_LAZYLOG_SEQ_IP="$BROKER_IP" \
            BROKER_LISTEN_ADDR="$BROKER_IP" \
            REQUIRE_FAITHFUL_LAZYLOG=0
    fi
fi

cleanup_shm_all
cleanup_remote_stray_procs "$CLIENT_HOST"
refresh_plot

log "===== Fig2 COMPLETE campaign=$CAMPAIGN_ID pass=$PASS_ID ====="
log "CSV: $RESULTS_CSV"
log "Mechanism summary: $MECH_CSV"
log "Contract: $CONTRACT_MD"
log "Fig: $FIG_PDF | Mech: $MECH_PDF"
log "Publication tip: NUM_TRIALS=3 WARMUP_TRIALS=1"
