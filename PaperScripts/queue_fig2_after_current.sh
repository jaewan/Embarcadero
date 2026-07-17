#!/usr/bin/env bash
# Wait for the current Fig2 campaign to release /tmp/embarcadero_paper_fig2.lock,
# then rebuild+sync the latency client and start a gap-fill pass.
#
# Guarantees: never overlaps another Fig2 holder of LOCK_FILE.
# Singleton: only one waiter via QUEUE_LOCK_FILE.
#
# Usage:
#   nohup bash PaperScripts/queue_fig2_after_current.sh \
#     >> data/paper_eval/fig2/fig2_append_latency/queue_followup.log 2>&1 &
#
set -uo pipefail

PAPER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$PAPER_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

LOCK_FILE="${LOCK_FILE:-/tmp/embarcadero_paper_fig2.lock}"
QUEUE_LOCK_FILE="${QUEUE_LOCK_FILE:-/tmp/embarcadero_paper_fig2_queue.lock}"
CLIENT_HOST="${CLIENT_HOST:-c4}"
CAMPAIGN_ID="${CAMPAIGN_ID:-fig2_append_latency}"
OUT_ROOT="${OUT_ROOT:-$PROJECT_ROOT/data/paper_eval/fig2/$CAMPAIGN_ID}"
QUEUE_LOG="${QUEUE_LOG:-$OUT_ROOT/queue_followup.log}"
mkdir -p "$OUT_ROOT"

# Avoid double-logging when launched with `>> QUEUE_LOG`.
log() {
  echo "[$(date -u +%Y-%m-%dT%H:%M:%SZ)] $*"
}

# Singleton waiter — exit quietly if another queue is already waiting/running.
exec 8>"$QUEUE_LOCK_FILE"
if ! flock -n 8; then
  log "another Fig2 queue waiter owns $QUEUE_LOCK_FILE; exit"
  exit 0
fi
echo $$ > /tmp/embarcadero_fig2_queue.pid

cluster_busy() {
  if pgrep -x embarlet >/dev/null || pgrep -x throughput_test >/dev/null; then
    return 0
  fi
  if pgrep -f '(^|/)run_fig2_latency_vs_load\.sh([[:space:]]|$)' >/dev/null; then
    return 0
  fi
  if pgrep -f '(^|/)run_latency_vs_load\.sh([[:space:]]|$)' >/dev/null; then
    return 0
  fi
  if ssh -o BatchMode=yes -o ConnectTimeout=5 "$CLIENT_HOST" \
      'pgrep -x throughput_test >/dev/null' 2>/dev/null; then
    return 0
  fi
  return 1
}

log "===== queued Fig2 follow-up waiter pid=$$ ====="
log "LOCK_FILE=$LOCK_FILE QUEUE_LOCK=$QUEUE_LOCK_FILE CLIENT_HOST=$CLIENT_HOST"
log "Waiting for exclusive flock on campaign lock (current must finish first)..."

# Block until campaign lock is free, then release so follow-up can flock -n.
flock -w "${FIG2_QUEUE_WAIT_SEC:-86400}" "$LOCK_FILE" -c 'true'
log "Campaign lock free. Waiting for cluster idle..."

idle_rounds=0
while true; do
  if cluster_busy; then
    idle_rounds=0
    log "cluster busy; sleep 30s"
    sleep 30
    continue
  fi
  idle_rounds=$((idle_rounds + 1))
  if [[ "$idle_rounds" -ge 2 ]]; then
    break
  fi
  sleep 5
done
log "Cluster idle. Rebuilding latency client + syncing to $CLIENT_HOST..."

if ! cmake -S "$PROJECT_ROOT" -B "$PROJECT_ROOT/build" -DCOLLECT_LATENCY_STATS=ON >/tmp/fig2_queue_cmake.log 2>&1; then
  log "FATAL: cmake configure failed (see /tmp/fig2_queue_cmake.log)"
  exit 1
fi
if ! cmake --build "$PROJECT_ROOT/build" --target throughput_test -j"$(nproc)" \
    >/tmp/fig2_queue_build.log 2>&1; then
  log "FATAL: throughput_test build failed (see /tmp/fig2_queue_build.log)"
  exit 1
fi

ssh -o BatchMode=yes "$CLIENT_HOST" 'mkdir -p ~/Embarcadero/build/bin' || true
if ! scp -o StrictHostKeyChecking=no \
    "$PROJECT_ROOT/build/bin/throughput_test" \
    "$CLIENT_HOST:~/Embarcadero/build/bin/throughput_test"; then
  log "FATAL: scp throughput_test to $CLIENT_HOST failed"
  exit 1
fi
log "Synced throughput_test md5=$(md5sum "$PROJECT_ROOT/build/bin/throughput_test" | awk '{print $1}')"

if cluster_busy; then
  log "FATAL: cluster busy again after sync; abort (re-queue manually)"
  exit 1
fi
# Probe that campaign lock is still free without holding it across the child.
if ! flock -n "$LOCK_FILE" -c 'true'; then
  log "FATAL: another Fig2 campaign grabbed $LOCK_FILE; abort"
  exit 1
fi

log "Launching gap-fill Fig2 (TARGET_TRIALS=1 skips already-ok cells)..."
TARGET_TRIALS="${TARGET_TRIALS:-1}" \
NUM_TRIALS="${NUM_TRIALS:-1}" \
CAMPAIGN_ID="$CAMPAIGN_ID" \
EMBARCADERO_LATENCY_ACK_PRIMARY=1 \
PASS_ID="${PASS_ID:-$(date -u +%Y%m%dT%H%M%SZ)_ack_primary_gapfill}" \
bash "$PAPER_DIR/run_fig2_latency_vs_load.sh"
rc=$?
log "===== queued Fig2 follow-up finished rc=$rc ====="
exit "$rc"
