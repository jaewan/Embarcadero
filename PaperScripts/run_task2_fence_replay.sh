#!/usr/bin/env bash
# Task 2 (fence path): explicitly exercise the prefix-safe SESSION_FENCED -> committed-HWM ->
# reopen(new epoch) -> replay(unacked suffix) recovery, audited end-to-end by the apply-order
# checker. Unlike the broker-kill variant (which RF=2 recovers via replica WITHOUT fencing), here
# we inject a predecessor delay LONGER than the session lease so the held gap is unrepaired past
# the lease and the broker MUST fence the session; the client then reopens under a new epoch and
# replays its retained unacked suffix.
#
# Audits (prompt Task-2 step 5):
#   - apply-order inversions == 0, no missing committed ops (applied==published), final-state
#     + digest correct, control session keeps committing  [from kv_ycsb_bench --fifo_valid]
#   - old-session committed HWM, reopen epoch, replayed suffix, fenced-suffix-not-committed
#     [scraped from broker/client logs: SESSION_FENCED / SESSION_OPEN_ACK / resubmit markers]
#
#   TRIALS=3 bash PaperScripts/run_task2_fence_replay.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"; cd "$ROOT"

SYS="${SYS:-EMBARCADERO}"
TRIALS="${TRIALS:-3}"
LEASE_MS="${LEASE_MS:-6000}"          # broker fences a held gap after this
GAP_DELAY_MS="${GAP_DELAY_MS:-10000}"  # > LEASE_MS so the gap is unrepaired past the lease -> fence
GAP_BATCH_SEQ="${GAP_BATCH_SEQ:-300}"  # which batch_seq to delay (after warmup)
OPS="${OPS:-1500000}"
KEYS="${KEYS:-5000}"
RF="${RF:-2}"; ACK="${ACK:-1}"
SESSIONS="${SESSIONS:-2}"
OUT="$ROOT/data/paper_eval/task2_fence_replay/${SYS}_lease${LEASE_MS}_gap${GAP_DELAY_MS}"; rm -rf "$OUT"; mkdir -p "$OUT"
echo "commit=$(git rev-parse HEAD) dirty=[$(git status --porcelain --untracked-files=no|head -1)]" > "$OUT/provenance.txt"
echo "embarlet_sha256=$(sha256sum build/bin/embarlet|awk '{print $1}')" >> "$OUT/provenance.txt"

pkill -KILL -x embarlet 2>/dev/null || true
for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null

echo "system,trial,valid,applied,published,session_reorders,key_reorders,final_mismatch,failed_checks,fenced,reopened,status" > "$OUT/results.csv"

for t in $(seq 1 "$TRIALS"); do
  cell="$OUT/trial${t}"; mkdir -p "$cell"; DRV="$cell/driver.log"
  echo ">>> $SYS fence-replay trial $t/$TRIALS RF=$RF ACK=$ACK sessions=$SESSIONS lease=${LEASE_MS}ms gap=${GAP_DELAY_MS}ms@seq${GAP_BATCH_SEQ} $(date -u +%H:%M:%SZ)"
  SMR_FIFO_SEQUENCERS="$SYS" SMR_FIFO_MODES="pipe" SMR_FIFO_NUM_TRIALS=1 \
    SMR_FIFO_SESSIONS="$SESSIONS" SMR_FIFO_RF="$RF" SMR_FIFO_ACK="$ACK" \
    SMR_FIFO_RECORD_COUNT="$KEYS" SMR_FIFO_OPERATION_COUNT="$OPS" SMR_FIFO_WARMUP_OPS=5000 \
    EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy EMBARCADERO_CHAIN_REPLICATION_INMEM=1 EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1 \
    EMBARCADERO_CORFU_SEQ_IP=10.10.10.10 \
    EMBARCADERO_SESSION_LEASE_MS="$LEASE_MS" EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS="$LEASE_MS" \
    EMBARCADERO_ORDER5_GAP_DELAY_MS="$GAP_DELAY_MS" EMBARCADERO_ORDER5_GAP_BATCH_SEQ="$GAP_BATCH_SEQ" \
    EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1 \
    BENCH_TIMEOUT_SEC=240 OUT_ROOT="$cell" \
    setsid nohup bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$DRV" 2>&1 &
  disown
  for i in $(seq 1 300); do pgrep -f run_smr_fifo_eval >/dev/null 2>&1 || break; sleep 1; done
  # fence-boundary evidence
  fenced=$(grep -rhoE "SESSION_FENCED|SessionFenced|delivered SessionFenced" "$cell"/*.log "$cell"/*broker*/*.log 2>/dev/null | wc -l)
  reopened=$(grep -rhoE "SESSION_OPEN_ACK.*assigned_session_epoch=[2-9]|SESSION_OPEN_ACK.*has_committed_prefix=1" "$cell"/*.log "$cell"/*broker*/*.log 2>/dev/null | wc -l)
  status="done"; any=0
  for f in "$cell"/${SYS}_*pipe*trial1_s*/summary.csv; do
    [ -e "$f" ] || continue; any=1
    row=$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{printf "%s,%s,%s,%s,%s,%s,%s",$h["valid"],$h["applied_entries"],$h["published_entries"],$h["session_reorders"],$h["key_reorders"],$h["final_mismatch_keys"],$h["failed_checks"]}' "$f")
    echo "$SYS,$t,$row,$fenced,$reopened,$status" | tee -a "$OUT/results.csv"
  done
  [ "$any" = "0" ] && echo "$SYS,$t,NOSUMMARY,,,,,,$fenced,$reopened,stall_or_broken" | tee -a "$OUT/results.csv"
  echo "   fence markers: SESSION_FENCED=$fenced reopen(epoch>=2 or committed_prefix)=$reopened"
  grep -rhoE "committed_msg_hwm=[0-9]+|committed_batch_seq=[0-9]+|assigned_session_epoch=[0-9]+ committed_hwm=[0-9]+|resubmit[a-z ]*[0-9]+|replay[a-z ]*[0-9]+" "$cell"/*.log "$cell"/*broker*/*.log 2>/dev/null | sort | uniq -c | sed 's/^/   marker /' | head -10
  pkill -KILL -x embarlet 2>/dev/null || true
  for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null
  sleep 2
done
echo "=== Task 2 fence-replay results ==="; cat "$OUT/results.csv"
