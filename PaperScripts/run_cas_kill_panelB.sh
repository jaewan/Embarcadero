#!/usr/bin/env bash
# Panel B (Q3 end-to-end): CAS metadata-service correctness + availability through
# a broker failure. Runs the CAS workload at RF=2 (payload replicated so it
# survives a dead broker) with ACK=1 (ordering completion — the failover-stable
# contract; ACK=2's durable frontier cannot advance past a dead replica and pins
# the client's pool). A follower broker (data port 1214+id) is kill -9'd
# KILL_DELAY s into the RUN phase. Metric: cas_rejections (order violations
# through the failure; prefix-safe -> 0) + applied completeness.
#
#   SYS=EMBARCADERO KILL=1 bash PaperScripts/run_cas_kill_panelB.sh   # kill mid-run
#   SYS=EMBARCADERO KILL=0 bash PaperScripts/run_cas_kill_panelB.sh   # control
#
# Co-located topology: broker i on 127.0.0.1:(1214+i); we kill the one on
# KILL_PORT (default 1217 = broker 3, a follower).
set -uo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

SYS="${SYS:-EMBARCADERO}"
KILL="${KILL:-1}"
KILL_PORT="${KILL_PORT:-1217}"
KILL_DELAY="${KILL_DELAY:-6}"
OPS="${OPS:-12000000}"
RF="${RF:-2}"
ACK="${ACK:-1}"
KEYS="${KEYS:-5000}"
TAG="${TAG:-cas_kill_${SYS}_k${KILL}}"
OUT="$PROJECT_ROOT/data/paper_eval/cas/$TAG"; rm -rf "$OUT"; mkdir -p "$OUT"
DRV="$OUT/driver.log"

pkill -KILL -x embarlet 2>/dev/null || true
for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null

echo ">>> $SYS CAS RF=$RF ACK=$ACK mem-sink OPS=$OPS KILL=$KILL (port $KILL_PORT @ +${KILL_DELAY}s)"
SMR_FIFO_SEQUENCERS="$SYS" SMR_FIFO_MODES="pipe" SMR_FIFO_CAS=1 SMR_FIFO_NUM_TRIALS=1 \
  SMR_FIFO_RF="$RF" SMR_FIFO_ACK="$ACK" \
  SMR_FIFO_RECORD_COUNT="$KEYS" SMR_FIFO_OPERATION_COUNT="$OPS" SMR_FIFO_WARMUP_OPS=5000 \
  EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy EMBARCADERO_CHAIN_REPLICATION_INMEM=1 EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1 \
  EMBARCADERO_CORFU_SEQ_IP=10.10.10.10 \
  BENCH_TIMEOUT_SEC=400 OUT_ROOT="$OUT" \
  setsid nohup bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$DRV" 2>&1 &
disown

clog=""
for i in $(seq 1 180); do
  clog=$(ls "$OUT"/${SYS}_pipe_trial1_s0.log 2>/dev/null | head -1)
  [ -n "$clog" ] && grep -q "Run: .* ops" "$clog" 2>/dev/null && { echo "  RUN phase started (t+${i}s)"; break; }
  sleep 1
done

if [ "$KILL" = "1" ] && [ -n "$clog" ]; then
  sleep "$KILL_DELAY"
  bpid=$(ss -tlnp 2>/dev/null | grep -E ":${KILL_PORT}\b" | grep -oE 'pid=[0-9]+' | head -1 | cut -d= -f2)
  [ -n "$bpid" ] && { echo "  KILL broker port $KILL_PORT (pid=$bpid) @ $(date -u +%H:%M:%SZ)"; kill -9 "$bpid" 2>/dev/null; } || echo "  WARN no listener on $KILL_PORT"
fi

for i in $(seq 1 400); do pgrep -f run_smr_fifo_eval >/dev/null 2>&1 || break; sleep 1; done

echo "=== RESULT ($SYS KILL=$KILL) ==="
f=$(ls "$OUT"/${SYS}_*pipe*_s0/summary.csv 2>/dev/null | head -1)
[ -n "$f" ] && awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{printf "  valid=%s cas_rejections=%s cas_success=%s writes=%s applied=%s failed=%s\n",$h["valid"],$h["cas_rejections"],$h["cas_success"],$h["writes"],$h["applied_entries"],$h["failed_checks"]}' "$f" || echo "  NO SUMMARY"
pkill -KILL -x embarlet 2>/dev/null || true
for f in /dev/shm/CXL_*; do [ -e "$f" ] && [ "$(stat -c '%U' "$f")" = "domin" ] && rm -f "$f"; done 2>/dev/null
