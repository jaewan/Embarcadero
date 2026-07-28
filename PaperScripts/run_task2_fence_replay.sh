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
#     + digest correct for both deliberately delayed sessions [from kv_ycsb_bench --fifo_valid]
#   - old-session committed HWM, reopen epoch, replayed suffix, fenced-suffix-not-committed
#     [scraped from broker/client logs: SESSION_FENCED / SESSION_OPEN_ACK / resubmit markers]
#
#   TRIALS=3 bash PaperScripts/run_task2_fence_replay.sh
set -euo pipefail
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
CAMPAIGN_ID="${CAMPAIGN_ID:-${SYS}_lease${LEASE_MS}_gap${GAP_DELAY_MS}_$(date -u +%Y%m%dT%H%M%SZ)}"
OUT="${OUT_ROOT:-$ROOT/data/paper_eval/task2_fence_replay/$CAMPAIGN_ID}"
[[ "$GAP_DELAY_MS" -gt "$LEASE_MS" ]] || {
  echo "ERROR: GAP_DELAY_MS must exceed LEASE_MS to exercise fencing" >&2; exit 2;
}
[[ ! -e "$OUT" ]] || {
  echo "ERROR: refusing existing OUT_ROOT=$OUT" >&2; exit 2;
}
mkdir -p "$OUT"
echo "commit=$(git rev-parse HEAD) dirty=[$(git status --porcelain --untracked-files=no|head -1)]" > "$OUT/provenance.txt"
echo "embarlet_sha256=$(sha256sum build/bin/embarlet|awk '{print $1}')" >> "$OUT/provenance.txt"
echo "client_sha256=$(sha256sum build/bin/kv_ycsb_bench|awk '{print $1}')" >> "$OUT/provenance.txt"
echo "campaign_id=$CAMPAIGN_ID lease_ms=$LEASE_MS gap_delay_ms=$GAP_DELAY_MS gap_batch_seq=$GAP_BATCH_SEQ trials=$TRIALS sessions=$SESSIONS" >> "$OUT/provenance.txt"

pkill -KILL -x embarlet 2>/dev/null || true
for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null

echo "system,trial,session,valid,applied,published,session_reorders,key_reorders,final_mismatch,failed_checks,fenced,reopened,status" > "$OUT/results.csv"
campaign_fail=0

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
    setsid bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$DRV" 2>&1 &
  run_pid=$!
  run_rc=0
  deadline=$((SECONDS + 300))
  while kill -0 "$run_pid" 2>/dev/null; do
    if (( SECONDS >= deadline )); then
      echo "ERROR: trial $t exceeded 300 s" | tee -a "$DRV" >&2
      kill -TERM -- "-$run_pid" 2>/dev/null || true
      sleep 2
      kill -KILL -- "-$run_pid" 2>/dev/null || true
      run_rc=124
      break
    fi
    sleep 1
  done
  if [[ "$run_rc" -eq 0 ]]; then
    wait "$run_pid" || run_rc=$?
  else
    wait "$run_pid" 2>/dev/null || true
  fi
  # fence-boundary evidence
  fenced=$({ grep -rhoE "SESSION_FENCED|SessionFenced|delivered SessionFenced" "$cell"/*.log "$cell"/*broker*/*.log 2>/dev/null || true; } | wc -l)
  reopened=$({ grep -rhoE "SESSION_OPEN_ACK.*assigned_session_epoch=[2-9]|SESSION_OPEN_ACK.*has_committed_prefix=1" "$cell"/*.log "$cell"/*broker*/*.log 2>/dev/null || true; } | wc -l)
  status="done"; any=0; rows=0; trial_fail=0
  for f in "$cell"/${SYS}_*pipe*trial1_s*/summary.csv; do
    [ -e "$f" ] || continue; any=1
    session=$(basename "$(dirname "$f")" | sed -n 's/.*_s\([0-9][0-9]*\)$/\1/p')
    row=$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{printf "%s,%s,%s,%s,%s,%s,%s",$h["valid"],$h["applied_entries"],$h["published_entries"],$h["session_reorders"],$h["key_reorders"],$h["final_mismatch_keys"],$h["failed_checks"]}' "$f")
    rows=$((rows + 1))
    valid="${row%%,*}"
    [[ "$valid" == "1" ]] || trial_fail=1
    echo "$SYS,$t,${session:-unknown},$row,$fenced,$reopened,$status" | tee -a "$OUT/results.csv"
  done
  if [[ "$any" = "0" ]]; then
    echo "$SYS,$t,none,NOSUMMARY,,,,,,$fenced,$reopened,stall_or_broken" | tee -a "$OUT/results.csv"
    trial_fail=1
  fi
  [[ "$run_rc" -eq 0 && "$rows" -eq "$SESSIONS" && "$fenced" -gt 0 && "$reopened" -gt 0 ]] || trial_fail=1
  if [[ "$trial_fail" -ne 0 ]]; then
    campaign_fail=$((campaign_fail + 1))
    echo "ERROR: trial $t failed gates: rc=$run_rc rows=$rows/$SESSIONS fenced=$fenced reopened=$reopened" | tee -a "$DRV" >&2
  fi
  echo "   fence markers: SESSION_FENCED=$fenced reopen(epoch>=2 or committed_prefix)=$reopened"
  ( { grep -rhoE "committed_msg_hwm=[0-9]+|committed_batch_seq=[0-9]+|assigned_session_epoch=[0-9]+ committed_hwm=[0-9]+|resubmit[a-z ]*[0-9]+|replay[a-z ]*[0-9]+" "$cell"/*.log "$cell"/*broker*/*.log 2>/dev/null || true; } | sort | uniq -c | sed 's/^/   marker /' | head -10 ) || true
  pkill -KILL -x embarlet 2>/dev/null || true
  for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null
  sleep 2
done
python3 - "$OUT" "$CAMPAIGN_ID" "$campaign_fail" <<'PY'
import csv, hashlib, json, pathlib, sys
root, campaign, failures = pathlib.Path(sys.argv[1]), sys.argv[2], int(sys.argv[3])
rows = list(csv.DictReader((root / "results.csv").open()))
manifest = {
    "campaign_id": campaign,
    "failed_trials": failures,
    "rows": len(rows),
    "valid_rows": sum(r.get("valid") == "1" for r in rows),
    "results_sha256": hashlib.sha256((root / "results.csv").read_bytes()).hexdigest(),
}
(root / "campaign_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY
echo "=== Task 2 fence-replay results ==="; cat "$OUT/results.csv"
if [[ "$campaign_fail" -ne 0 ]]; then
  echo "ERROR: $campaign_fail/$TRIALS trials failed; artifact retained at $OUT" >&2
  exit 1
fi
echo "PASS: $OUT"
