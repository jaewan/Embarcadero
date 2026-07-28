#!/usr/bin/env bash
# Task 2 (Q2xQ3): prefix-safe FIFO through broker failure, audited END-TO-END by the
# apply-order checker. One logical stream (kv_ycsb_bench --fifo_valid) striped across
# 4 brokers at RF=2 (payload survives a dead broker), ACK=1 (ordering completion
# within the active sequencer epoch). A SECOND independent session runs concurrently.
# A selected broker (data port 1214+id) is kill -9'd KILL_DELAY s into the RUN phase.
# We then audit the checker's summary: apply-order inversions (session_reorders==0),
# completeness (applied==published), final-state (final_mismatch_keys==0), and each
# session's progress. If ORDER=5 commit-recovery stalls after the kill (a documented
# broker-death limitation), applied<published will show it — reported honestly, not hidden.
#
#   TRIALS=3 bash PaperScripts/run_task2_kill_applyorder.sh
#   KILL=0 TRIALS=1 bash PaperScripts/run_task2_kill_applyorder.sh   # no-kill control
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"

SYS="${SYS:-EMBARCADERO}"
KILL="${KILL:-1}"
KILL_PORT="${KILL_PORT:-1216}"     # broker 2 (a follower; head=1214)
KILL_DELAY="${KILL_DELAY:-15}"
TRIALS="${TRIALS:-3}"
OPS="${OPS:-30000000}"
KEYS="${KEYS:-5000}"
RF="${RF:-2}"; ACK="${ACK:-1}"
SESSIONS="${SESSIONS:-2}"
BENCH_TIMEOUT_SEC="${BENCH_TIMEOUT_SEC:-300}"
if [[ "$SESSIONS" -ne 2 ]]; then
  echo "ERROR: this isolation campaign requires exactly two sessions" >&2
  exit 2
fi
CAMPAIGN_ID="${CAMPAIGN_ID:-${SYS}_k${KILL}_p${KILL_PORT}}"
OUT="$ROOT/data/paper_eval/task2_kill_applyorder/$CAMPAIGN_ID"; rm -rf "$OUT"; mkdir -p "$OUT"
echo "git_commit=$(git rev-parse HEAD)" > "$OUT/provenance.txt"
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "git_dirty=1" >> "$OUT/provenance.txt"
else
  echo "git_dirty=0" >> "$OUT/provenance.txt"
fi
echo "embarlet_sha256=$(sha256sum build/bin/embarlet|awk '{print $1}')" >> "$OUT/provenance.txt"
echo "kv_ycsb_bench_sha256=$(sha256sum build/bin/kv_ycsb_bench|awk '{print $1}')" >> "$OUT/provenance.txt"
git status --porcelain=v1 > "$OUT/worktree_status.txt"
git diff --binary > "$OUT/worktree.patch"

pkill -KILL -x embarlet 2>/dev/null || true
for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null

echo "system,trial,session,kill,kill_target,kill_verified,valid,applied,published,session_reorders,key_reorders,final_mismatch,failed_checks,status" > "$OUT/results.csv"

for t in $(seq 1 "$TRIALS"); do
  cell="$OUT/trial${t}"; mkdir -p "$cell"; DRV="$cell/driver.log"; EVENT="$cell/failure_event.log"
  mkdir -p "$cell/timeseries"
  kb=$(( KILL_PORT - 1214 ))   # data port base 1214 -> broker index
  if [[ -z "${SESSION_ALLOWLISTS:-}" ]]; then
    survivor_allowlist=""
    for broker_id in 0 1 2 3; do
      [[ "$broker_id" -eq "$kb" ]] && continue
      [[ -n "$survivor_allowlist" ]] && survivor_allowlist+=","
      survivor_allowlist+="$broker_id"
    done
    # Session 0 is the affected, fully striped session. Session 1 remains
    # striped across all three surviving servers and is the isolation control.
    SESSION_ALLOWLISTS="ALL|$survivor_allowlist"
  fi
  echo ">>> $SYS trial $t/$TRIALS RF=$RF ACK=$ACK sessions=$SESSIONS KILL=$KILL (port $KILL_PORT @ +${KILL_DELAY}s) $(date -u +%H:%M:%SZ)"
  echo "    per-session broker allowlists: $SESSION_ALLOWLISTS"
  SMR_FIFO_SEQUENCERS="$SYS" SMR_FIFO_MODES="pipe" SMR_FIFO_NUM_TRIALS=1 \
    SMR_FIFO_SESSIONS="$SESSIONS" SMR_FIFO_RF="$RF" SMR_FIFO_ACK="$ACK" \
    SMR_FIFO_SESSION_ALLOWLISTS="$SESSION_ALLOWLISTS" \
    SMR_FIFO_TIMESERIES_DIR="$cell/timeseries" SMR_FIFO_TIMESERIES_INTERVAL_MS=100 \
    SMR_FIFO_RECORD_COUNT="$KEYS" SMR_FIFO_OPERATION_COUNT="$OPS" SMR_FIFO_WARMUP_OPS=5000 \
    EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy EMBARCADERO_CHAIN_REPLICATION_INMEM=1 EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1 \
    EMBARCADERO_CORFU_SEQ_IP=10.10.10.10 \
    EMBARCADERO_SESSION_LEASE_MS=8000 EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=8000 \
    EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1 \
    KV_BENCH_CLIENT_RUNTIME_MODE=throughput \
    BENCH_TIMEOUT_SEC="$BENCH_TIMEOUT_SEC" OUT_ROOT="$cell" \
    setsid nohup bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$DRV" 2>&1 &
  disown
  # wait for RUN phase
  clog=""; for i in $(seq 1 180); do
    clog=$(ls "$cell"/${SYS}_*pipe*trial1_s0.log 2>/dev/null | head -1)
    [ -n "$clog" ] && grep -qiE "Run: .* ops|RUN phase|steady" "$clog" 2>/dev/null && { echo "   RUN started t+${i}s"; break; }
    pgrep -f run_smr_fifo_eval >/dev/null 2>&1 || { echo "   driver exited early"; break; }
    sleep 1
  done
  kill_verified=0
  if [ "$KILL" = "1" ] && [ -n "$clog" ]; then
    sleep "$KILL_DELAY"
    # The head (broker 0) is the only process launched with --head; target it
    # unambiguously (lsof on the shared port can return a client/other pid).
    bpid=""
    if [ "$kb" = "0" ]; then bpid=$(pgrep -f '(^|/)embarlet .*--head([[:space:]]|$)' | head -1); fi
    # Robust port->pid (ss -p often hides pid without privilege): try lsof, fuser,
    # then the broker's own log (embarlet logs "embarlet_<PID>_ready"), then ss.
    [ -z "$bpid" ] && bpid=$(lsof -ti "tcp:${KILL_PORT}" -sTCP:LISTEN 2>/dev/null | head -1)
    [ -z "$bpid" ] && bpid=$(fuser -n tcp "$KILL_PORT" 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' | head -1)
    [ -z "$bpid" ] && bpid=$(grep -oE 'embarlet_[0-9]+_ready' "$ROOT/build/bin/broker_${kb}.log" 2>/dev/null | grep -oE '[0-9]+' | head -1)
    [ -z "$bpid" ] && bpid=$(ss -tlnp 2>/dev/null | grep -E ":${KILL_PORT}\b" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
    {
      echo "utc_before=$(date -u +%FT%TZ)"
      echo "epoch_ms_before=$(date +%s%3N)"
      echo "target_broker=$kb target_port=$KILL_PORT resolved_pid=${bpid:-none}"
      echo "listeners_before=$(ss -ltnp 2>/dev/null | grep -E \":${KILL_PORT}\\b\" || true)"
      if [ -n "$bpid" ] && [ -r "/proc/$bpid/cmdline" ]; then
        printf "cmdline_before="; tr '\0' ' ' < "/proc/$bpid/cmdline"; echo
      fi
    } >> "$EVENT"
    if [ -n "$bpid" ] && kill -0 "$bpid" 2>/dev/null &&
       [ -r "/proc/$bpid/cmdline" ] &&
       tr '\0' ' ' < "/proc/$bpid/cmdline" | grep -qE '(^|/)embarlet([[:space:]]|$)'; then
      echo "   KILL broker $kb port $KILL_PORT pid=$bpid @ $(date -u +%H:%M:%SZ)" | tee -a "$EVENT"
      kill -9 "$bpid" 2>/dev/null
      listeners_after=""
      for _ in $(seq 1 10); do
        sleep 1
        listeners_after=$(ss -ltnp 2>/dev/null | grep -E ":${KILL_PORT}\b" || true)
        ! kill -0 "$bpid" 2>/dev/null && [ -z "$listeners_after" ] && break
      done
      echo "utc_after=$(date -u +%FT%TZ)" >> "$EVENT"
      echo "epoch_ms_after=$(date +%s%3N)" >> "$EVENT"
      echo "pid_alive_after=$(kill -0 "$bpid" 2>/dev/null && echo 1 || echo 0)" >> "$EVENT"
      echo "listeners_after=${listeners_after:-none}" >> "$EVENT"
      if ! kill -0 "$bpid" 2>/dev/null && [ -z "$listeners_after" ]; then
        kill_verified=1
        echo "kill_verified=1" >> "$EVENT"
      else
        echo "   WARN target process or listener remains after kill" | tee -a "$EVENT"
      fi
    else
      echo "   ERROR could not validate embarlet PID for broker $kb port $KILL_PORT (bpid='$bpid')" | tee -a "$EVENT"
    fi
  fi
  # wait for completion (bounded)
  for i in $(seq 1 320); do pgrep -f run_smr_fifo_eval >/dev/null 2>&1 || break; sleep 1; done
  # parse per-session summaries
  status="done"; any=0
  for f in "$cell"/${SYS}_*pipe*trial1_s*/summary.csv; do
    [ -e "$f" ] || continue; any=1
    session=$(basename "$(dirname "$f")" | sed -n 's/.*_s\([0-9][0-9]*\)$/\1/p')
    row=$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{printf "%s,%s,%s,%s,%s,%s,%s",$h["valid"],$h["applied_entries"],$h["published_entries"],$h["session_reorders"],$h["key_reorders"],$h["final_mismatch_keys"],$h["failed_checks"]}' "$f")
    [ "$KILL" = "0" ] || [ "$kill_verified" = "1" ] || status="invalid_no_verified_kill"
    echo "$SYS,$t,${session:-unknown},$KILL,broker${kb:-none}:port${KILL_PORT},$kill_verified,$row,$status" | tee -a "$OUT/results.csv"
  done
  [ "$any" = "0" ] && echo "$SYS,$t,none,$KILL,broker${kb:-none}:port${KILL_PORT},$kill_verified,NOSUMMARY,,,,,,,stall_or_broken" | tee -a "$OUT/results.csv"
  mkdir -p "$cell/broker_logs"
  cp -a "$ROOT"/build/bin/broker_*.log "$cell/broker_logs/" 2>/dev/null || true
  # scrape recovery markers
  grep -hoiE "SESSION_FENCED|committed_msg_hwm=[0-9]+|committed_batch_seq=[0-9]+|SESSION_OPEN|resubmit|replay" "$cell"/*broker*/*.log "$cell"/*.log 2>/dev/null | sort | uniq -c | sed 's/^/   marker: /' | head -12
  pkill -KILL -x embarlet 2>/dev/null || true
  for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null
  sleep 2
done
echo "=== Task 2 results ==="; cat "$OUT/results.csv"
