#!/usr/bin/env bash
# PaperScripts/run_fig3_session_isolation.sh
#
# Paper experiment: per-session isolation during broker failure.
#
# 4 independent ORDER=5 sessions, each pinned to one broker.
# Broker FAILED_BROKER is killed at FAILURE_AFTER_MS ms.
# Expected:
#   Sessions on surviving brokers → throughput flat, zero ACK stall.
#   Session on failed broker     → stalls, SESSION_FENCED, suffix replay.
#
# This empirically proves the paper's key claim: Embarcadero provides
# session-granularity failure isolation without a global seal or cut.
#
# Usage:
#   bash PaperScripts/run_fig3_session_isolation.sh
#   FAILED_BROKER=2 bash PaperScripts/run_fig3_session_isolation.sh
#   NUM_TRIALS=3 bash PaperScripts/run_fig3_session_isolation.sh
#
set -uo pipefail

PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$PAPER_DIR/.." && pwd)"
SCRIPTS_DIR="$PROJECT_ROOT/scripts"
cd "$PROJECT_ROOT"

# ---------------------------------------------------------------------------
# Campaign identity
# ---------------------------------------------------------------------------
CAMPAIGN_ID="${CAMPAIGN_ID:-fig3_session_isolation}"
PASS_ID="${PASS_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/data/paper_eval/fig3/$CAMPAIGN_ID}"
FIG_PDF="${FIG_PDF:-$PROJECT_ROOT/Paper/Figures/session_isolation.pdf}"
LOG="${OUT_ROOT}/sweep_summary.log"

mkdir -p "$OUT_ROOT"

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local m="[$(stamp)] $*"; echo "$m"; echo "$m" >> "$LOG"; }

# ---------------------------------------------------------------------------
# Knobs (override via env)
# ---------------------------------------------------------------------------
export NUM_BROKERS="${NUM_BROKERS:-4}"
export FAILED_BROKER="${FAILED_BROKER:-1}"       # broker index to kill
export NUM_TRIALS="${NUM_TRIALS:-3}"
export SESSION_BYTES="${SESSION_BYTES:-10737418240}"  # 10 GiB per session
export MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
export FAILURE_AFTER_MS="${FAILURE_AFTER_MS:-15000}"  # 15 s — fire well into run
export SESSION_LEASE_MS="${SESSION_LEASE_MS:-180000}"
export IDLE_FORCE_EXPIRE_MS="${IDLE_FORCE_EXPIRE_MS:-180000}"
export ORDER="${ORDER:-5}"
export ACK="${ACK:-1}"
export REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}"
export SCENARIO="${SCENARIO:-remote}"
# One host per session (|‑separated); default: c4|c3|c1|local
export SESSION_HOSTS_PIPE="${SESSION_HOSTS_PIPE:-c4|c3|c1|local}"
export CLIENT_HEAD_ADDR="${CLIENT_HEAD_ADDR:-${EMBARCADERO_HEAD_ADDR:-10.10.10.10}}"
export EMBARCADERO_HEAD_ADDR="$CLIENT_HEAD_ADDR"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
export EMBARCADERO_CXL_SIZE="${EMBARCADERO_CXL_SIZE:-77309411328}"
export BROKER_READY_TIMEOUT_SEC="${BROKER_READY_TIMEOUT_SEC:-900}"
export CLIENT_TIMEOUT="${CLIENT_TIMEOUT:-600}"
export ISOLATION_DATA_DIR="$OUT_ROOT"
export EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS="${EMBARCADERO_FAILURE_MEASURE_INTERVAL_MS:-100}"

# Mutual exclusion with Fig2/Fig3 campaigns
LOCK_FILE="${LOCK_FILE:-/tmp/embarcadero_paper_fig3_isolation.lock}"
exec 9>"$LOCK_FILE"
flock -n 9 || { echo "ERROR: another isolation campaign owns $LOCK_FILE" >&2; exit 1; }
if pgrep -x embarlet >/dev/null || pgrep -x throughput_test >/dev/null; then
  echo "ERROR: cluster busy" >&2; exit 1
fi

log "===== Session Isolation START campaign=$CAMPAIGN_ID pass=$PASS_ID ====="
log "FAILED_BROKER=$FAILED_BROKER  FAILURE_AFTER_MS=$FAILURE_AFTER_MS"
log "SESSION_BYTES=$SESSION_BYTES  NUM_TRIALS=$NUM_TRIALS  SCENARIO=$SCENARIO"
log "SESSION_HOSTS_PIPE=$SESSION_HOSTS_PIPE"

# ---------------------------------------------------------------------------
# Optionally rebuild throughput_test with FAILURE_AFTER_MS support
# ---------------------------------------------------------------------------
if [[ "${FIG3_SKIP_BUILD:-0}" != "1" ]]; then
  log "Building throughput_test..."
  cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build" -DCOLLECT_LATENCY_STATS=ON \
    >/tmp/fig3_iso_cmake.log 2>&1 || { log "FATAL: cmake failed"; exit 1; }
  cmake --build "$PROJECT_ROOT/build" --target throughput_test -j"$(nproc)" \
    >/tmp/fig3_iso_build.log 2>&1 || { log "FATAL: build failed"; exit 1; }
  log "Build OK"

  # Sync binary to all session hosts
  IFS='|' read -r -a _hosts <<< "$SESSION_HOSTS_PIPE"
  for h in "${_hosts[@]}"; do
    [[ "$h" == "local" ]] && continue
    log "Syncing throughput_test to $h..."
    scp -o StrictHostKeyChecking=no \
      "$PROJECT_ROOT/build/bin/throughput_test" \
      "${h}:/home/domin/Embarcadero/build/bin/throughput_test" \
      || log "WARNING: scp to $h failed"
  done
fi

# ---------------------------------------------------------------------------
# Run the experiment
# ---------------------------------------------------------------------------
bash "$SCRIPTS_DIR/run_session_isolation.sh"
rc=$?
if [[ "$rc" -ne 0 ]]; then
  log "WARN: run_session_isolation.sh exited $rc"
fi

# ---------------------------------------------------------------------------
# Plot: best trial (use the first with a combined.csv)
# ---------------------------------------------------------------------------
best_trial=""
for t in $(seq 1 "$NUM_TRIALS"); do
  if [[ -f "$OUT_ROOT/trial_${t}/combined.csv" ]]; then
    best_trial="$t"
    break
  fi
done

if [[ -n "$best_trial" ]]; then
  log "Plotting trial $best_trial → $FIG_PDF"
  python3 "$PROJECT_ROOT/scripts/publication/plot_session_isolation.py" \
    --data-dir "$OUT_ROOT/trial_${best_trial}" \
    --failed-broker "$FAILED_BROKER" \
    --kill-ms "$FAILURE_AFTER_MS" \
    --lease-ms "$SESSION_LEASE_MS" \
    --out "$FIG_PDF" \
    || log "WARN: plotting failed"
else
  log "WARN: no combined.csv found; skipping plot"
fi

log "===== Session Isolation COMPLETE ====="
log "Data: $OUT_ROOT"
log "Fig:  $FIG_PDF"
