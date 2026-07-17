#!/usr/bin/env bash
# PaperScripts/run_fig3_failure.sh
#
# Fig3: failure under the prefix-safe contract (paper fig:failure_throughput).
# Runs two panels via scripts/run_failures.sh, then plots failure_combined.pdf.
#
#   (a) arrival_order — holding disabled (ORDER=4 sensitivity)
#   (b) prefix_safe   — ORDER=5 prefix-safe hold (contract claim)
#
# Usage:
#   bash PaperScripts/run_fig3_failure.sh
#   FIG3_PREFLIGHT_ONLY=1 bash PaperScripts/run_fig3_failure.sh
#   SKIP_NOHOLD=1 bash ...          # only contract panel
#
set -uo pipefail

PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$PAPER_DIR/.." && pwd)"
SCRIPTS_DIR="$PROJECT_ROOT/scripts"
cd "$PROJECT_ROOT"

CAMPAIGN_ID="${CAMPAIGN_ID:-fig3_failure}"
PASS_ID="${PASS_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/data/paper_eval/fig3/$CAMPAIGN_ID}"
PASS_DIR="$OUT_ROOT/runs/$PASS_ID"
LOG_DIR="$OUT_ROOT/logs/$PASS_ID"
FIG_PDF="${FIG_PDF:-$OUT_ROOT/failure_combined.pdf}"
LOCK_FILE="${LOCK_FILE:-/tmp/embarcadero_paper_fig3.lock}"
FIG2_LOCK="${FIG2_LOCK:-/tmp/embarcadero_paper_fig2.lock}"

mkdir -p "$PASS_DIR" "$LOG_DIR"

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$OUT_ROOT/sweep_summary.log"; }

# Mutual exclusion: never overlap Fig2 or another Fig3.
exec 9>"$LOCK_FILE"
flock -n 9 || { echo "ERROR: another Fig3 owns $LOCK_FILE" >&2; exit 1; }
if ! flock -n "$FIG2_LOCK" -c 'true' 2>/dev/null; then
  echo "ERROR: Fig2 lock $FIG2_LOCK is held — wait for Fig2 to finish" >&2
  exit 1
fi
if pgrep -x embarlet >/dev/null || pgrep -x throughput_test >/dev/null; then
  echo "ERROR: cluster busy (embarlet/throughput_test live)" >&2
  exit 1
fi

# Shared Fig3 knobs (passed through to run_failures.sh).
export SCENARIO="${SCENARIO:-remote}"
export REMOTE_CLIENT_HOST="${REMOTE_CLIENT_HOST:-c4}"
export EMBARCADERO_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-10.10.10.10}"
export NUM_BROKERS="${NUM_BROKERS:-4}"
export NUM_TRIALS="${NUM_TRIALS:-1}"
export TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-21474836480}"
export MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
export FAILURE_AFTER_MS="${FAILURE_AFTER_MS:-1800}"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-77309411328}"
export EMBARCADERO_CXL_MAP_POPULATE="${EMBARCADERO_CXL_MAP_POPULATE:-0}"
export BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-900}"
export CLIENT_TIMEOUT="${CLIENT_TIMEOUT:-600}"
export CLIENT_LD_LIBRARY_PATH="${CLIENT_LD_LIBRARY_PATH:-/home/domin/Embarcadero/third_party/glog-0.6/lib:/home/domin/Embarcadero/third_party/yaml-cpp-0.8/lib}"

log "===== Fig3 START campaign=$CAMPAIGN_ID pass=$PASS_ID ====="
log "OUT_ROOT=$OUT_ROOT PASS_DIR=$PASS_DIR"
log "SCENARIO=$SCENARIO HEAD=$EMBARCADERO_HEAD_ADDR AFTER_MS=$FAILURE_AFTER_MS"

if [[ "${FIG3_PREFLIGHT_ONLY:-0}" == "1" ]]; then
  test -x "$PROJECT_ROOT/build/bin/embarlet" || { echo "missing embarlet" >&2; exit 1; }
  test -x "$PROJECT_ROOT/build/bin/throughput_test" || { echo "missing throughput_test" >&2; exit 1; }
  if [[ "$SCENARIO" == "remote" ]]; then
    ssh -o BatchMode=yes -o ConnectTimeout=5 "$REMOTE_CLIENT_HOST" \
      'test -x ~/Embarcadero/build/bin/throughput_test' \
      || { echo "remote throughput_test missing on $REMOTE_CLIENT_HOST" >&2; exit 1; }
  fi
  log "Preflight OK"
  exit 0
fi

# Rebuild client so FAILURE_AFTER_MS wall-clock kill is present.
if [[ "${FIG3_SKIP_BUILD:-0}" != "1" ]]; then
  log "Building throughput_test (FAILURE_AFTER_MS support)..."
  cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build" -DCOLLECT_LATENCY_STATS=ON \
    >/tmp/fig3_cmake.log 2>&1 || { log "FATAL: cmake failed"; exit 1; }
  cmake --build "$PROJECT_ROOT/build" --target throughput_test -j"$(nproc)" \
    >/tmp/fig3_build.log 2>&1 || { log "FATAL: build failed"; exit 1; }
  if [[ "$SCENARIO" == "remote" ]]; then
    ssh -o BatchMode=yes "$REMOTE_CLIENT_HOST" 'mkdir -p ~/Embarcadero/build/bin' || true
    scp -o StrictHostKeyChecking=no \
      "$PROJECT_ROOT/build/bin/throughput_test" \
      "$REMOTE_CLIENT_HOST:~/Embarcadero/build/bin/throughput_test" \
      || { log "FATAL: scp throughput_test failed"; exit 1; }
  fi
fi

run_panel() {
  local mode="$1"
  local tag="$2"
  local panel_dir="$PASS_DIR/$tag"
  mkdir -p "$panel_dir"
  log "START panel=$mode → $panel_dir"
  local rc=0
  HOLD_MODE="$mode" OUT_TAG="" FAILURE_DATA_DIR="$panel_dir" \
    bash "$SCRIPTS_DIR/run_failures.sh" \
    >"$LOG_DIR/${tag}.log" 2>&1 || rc=$?
  if [[ "$rc" -eq 0 && -f "$panel_dir/real_time_acked_throughput.csv" ]]; then
    log "PASS panel=$mode"
  else
    log "FAIL panel=$mode rc=$rc (see $LOG_DIR/${tag}.log)"
  fi
  return "$rc"
}

suite_rc=0
if [[ "${SKIP_NOHOLD:-0}" != "1" ]]; then
  run_panel arrival_order arrival_order || suite_rc=$?
fi
run_panel prefix_safe prefix_safe || suite_rc=$?

HOLD_DIR="$PASS_DIR/prefix_safe"
NOHOLD_DIR="$PASS_DIR/arrival_order"
if [[ -f "$HOLD_DIR/real_time_acked_throughput.csv" ]]; then
  plot_args=(--hold-dir "$HOLD_DIR" --out "$FIG_PDF")
  if [[ -f "$NOHOLD_DIR/real_time_acked_throughput.csv" ]]; then
    plot_args+=(--nohold-dir "$NOHOLD_DIR")
  else
    log "WARN: nohold panel missing; plotting hold-only requires both dirs — skip combined"
    plot_args=()
  fi
  if [[ ${#plot_args[@]} -gt 0 ]]; then
    python3 "$SCRIPTS_DIR/publication/plot_failure_combined.py" "${plot_args[@]}" \
      >>"$LOG_DIR/plot.log" 2>&1 || log "WARN: combined plot failed"
    # Also copy into Paper/Figures when plot succeeded.
    if [[ -f "$FIG_PDF" ]]; then
      cp -f "$FIG_PDF" "$PROJECT_ROOT/Paper/Figures/failure_combined.pdf" 2>/dev/null || true
      cp -f "${FIG_PDF%.pdf}.png" "$PROJECT_ROOT/Paper/Figures/failure_combined.png" 2>/dev/null || true
    fi
  fi
fi

cat > "$OUT_ROOT/campaign_contract.md" <<EOF
# Fig3 campaign contract

- pass_id: $PASS_ID
- panels: arrival_order (a), prefix_safe (b)
- kill: FAILURE_AFTER_MS=$FAILURE_AFTER_MS
- bytes: $TOTAL_MESSAGE_SIZE
- scenario: $SCENARIO client=$REMOTE_CLIENT_HOST
- CXL: zero=$EMBARCADERO_CXL_ZERO_MODE size=$EMBARCADERO_CXL_SIZE
- artifacts: $PASS_DIR
- figure: $FIG_PDF
EOF

log "===== Fig3 COMPLETE rc=$suite_rc ====="
log "CSV/traces: $PASS_DIR"
log "Fig: $FIG_PDF"
exit "$suite_rc"
