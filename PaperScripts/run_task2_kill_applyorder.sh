#!/usr/bin/env bash
# Task 2 (Q2xQ3): prefix-safe FIFO through broker failure, audited END-TO-END by the
# apply-order checker. One logical stream (kv_ycsb_bench --fifo_valid) striped across
# 4 brokers at RF=2 (payload survives a dead broker), ACK=1 (ordering completion, the
# failover-stable contract). A SECOND independent session runs concurrently (isolation).
# A follower broker (data port 1214+id) is kill -9'd KILL_DELAY s into the RUN phase.
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
KILL_DELAY="${KILL_DELAY:-8}"
TRIALS="${TRIALS:-3}"
OPS="${OPS:-2000000}"
KEYS="${KEYS:-5000}"
RF="${RF:-2}"; ACK="${ACK:-1}"
SESSIONS="${SESSIONS:-2}"
OUT="$ROOT/data/paper_eval/task2_kill_applyorder/${SYS}_k${KILL}_p${KILL_PORT}"; rm -rf "$OUT"; mkdir -p "$OUT"
echo "commit=$(git rev-parse HEAD) dirty=[$(git status --porcelain --untracked-files=no|head -1)]" > "$OUT/provenance.txt"
echo "embarlet_sha256=$(sha256sum build/bin/embarlet|awk '{print $1}')" >> "$OUT/provenance.txt"

pkill -KILL -x embarlet 2>/dev/null || true
for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null

echo "system,trial,kill,valid,applied,published,session_reorders,key_reorders,final_mismatch,failed_checks,status" > "$OUT/results.csv"

for t in $(seq 1 "$TRIALS"); do
  cell="$OUT/trial${t}"; mkdir -p "$cell"; DRV="$cell/driver.log"
  echo ">>> $SYS trial $t/$TRIALS RF=$RF ACK=$ACK sessions=$SESSIONS KILL=$KILL (port $KILL_PORT @ +${KILL_DELAY}s) $(date -u +%H:%M:%SZ)"
  SMR_FIFO_SEQUENCERS="$SYS" SMR_FIFO_MODES="pipe" SMR_FIFO_NUM_TRIALS=1 \
    SMR_FIFO_SESSIONS="$SESSIONS" SMR_FIFO_RF="$RF" SMR_FIFO_ACK="$ACK" \
    SMR_FIFO_RECORD_COUNT="$KEYS" SMR_FIFO_OPERATION_COUNT="$OPS" SMR_FIFO_WARMUP_OPS=5000 \
    EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy EMBARCADERO_CHAIN_REPLICATION_INMEM=1 EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1 \
    EMBARCADERO_CORFU_SEQ_IP=10.10.10.10 \
    EMBARCADERO_SESSION_LEASE_MS=8000 EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=8000 \
    EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1 \
    BENCH_TIMEOUT_SEC=300 OUT_ROOT="$cell" \
    setsid nohup bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$DRV" 2>&1 &
  disown
  # wait for RUN phase
  clog=""; for i in $(seq 1 180); do
    clog=$(ls "$cell"/${SYS}_*pipe*trial1_s0.log 2>/dev/null | head -1)
    [ -n "$clog" ] && grep -qiE "Run: .* ops|RUN phase|steady" "$clog" 2>/dev/null && { echo "   RUN started t+${i}s"; break; }
    pgrep -f run_smr_fifo_eval >/dev/null 2>&1 || { echo "   driver exited early"; break; }
    sleep 1
  done
  if [ "$KILL" = "1" ] && [ -n "$clog" ]; then
    sleep "$KILL_DELAY"
    kb=$(( KILL_PORT - 1214 ))   # data port base 1214 -> broker index
    # The head (broker 0) is the only process launched with --head; target it
    # unambiguously (lsof on the shared port can return a client/other pid).
    bpid=""
    if [ "$kb" = "0" ]; then bpid=$(pgrep -f 'embarlet .*--head' | head -1); fi
    # Robust port->pid (ss -p often hides pid without privilege): try lsof, fuser,
    # then the broker's own log (embarlet logs "embarlet_<PID>_ready"), then ss.
    [ -z "$bpid" ] && bpid=$(lsof -ti "tcp:${KILL_PORT}" -sTCP:LISTEN 2>/dev/null | head -1)
    [ -z "$bpid" ] && bpid=$(fuser -n tcp "$KILL_PORT" 2>/dev/null | tr -s ' ' '\n' | grep -E '^[0-9]+$' | head -1)
    [ -z "$bpid" ] && bpid=$(grep -oE 'embarlet_[0-9]+_ready' "$ROOT/build/bin/broker_${kb}.log" 2>/dev/null | grep -oE '[0-9]+' | head -1)
    [ -z "$bpid" ] && bpid=$(ss -tlnp 2>/dev/null | grep -E ":${KILL_PORT}\b" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
    if [ -n "$bpid" ] && kill -0 "$bpid" 2>/dev/null; then
      echo "   KILL follower broker $kb port $KILL_PORT pid=$bpid @ $(date -u +%H:%M:%SZ)"; kill -9 "$bpid" 2>/dev/null
      sleep 1; echo "   port $KILL_PORT after kill: $(ss -ltn 2>/dev/null | grep -cE ":${KILL_PORT}\b") listeners (expect 0)"
    else
      echo "   WARN could not resolve pid for broker $kb port $KILL_PORT (bpid='$bpid')"
    fi
  fi
  # wait for completion (bounded)
  for i in $(seq 1 320); do pgrep -f run_smr_fifo_eval >/dev/null 2>&1 || break; sleep 1; done
  # parse per-session summaries
  status="done"; any=0
  for f in "$cell"/${SYS}_*pipe*trial1_s*/summary.csv; do
    [ -e "$f" ] || continue; any=1
    row=$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{printf "%s,%s,%s,%s,%s,%s,%s",$h["valid"],$h["applied_entries"],$h["published_entries"],$h["session_reorders"],$h["key_reorders"],$h["final_mismatch_keys"],$h["failed_checks"]}' "$f")
    echo "$SYS,$t,$KILL,$row,$status" | tee -a "$OUT/results.csv"
  done
  [ "$any" = "0" ] && echo "$SYS,$t,$KILL,NOSUMMARY,,,,,,,stall_or_broken" | tee -a "$OUT/results.csv"
  # scrape recovery markers
  grep -hoiE "SESSION_FENCED|committed_msg_hwm=[0-9]+|committed_batch_seq=[0-9]+|SESSION_OPEN|resubmit|replay" "$cell"/*broker*/*.log "$cell"/*.log 2>/dev/null | sort | uniq -c | sed 's/^/   marker: /' | head -12
  pkill -KILL -x embarlet 2>/dev/null || true
  for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null
  sleep 2
done
echo "=== Task 2 results ==="; cat "$OUT/results.csv"
