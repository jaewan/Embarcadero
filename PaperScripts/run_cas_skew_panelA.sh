#!/usr/bin/env bash
# Panel A (Q3 end-to-end): CAS metadata-service correctness vs injected
# inter-broker skew. Skew is injected with `tc netem` (route 1) on the loopback
# path to ONE broker's data port, so a client's round-robin-striped batches to
# that broker arrive late — exactly the WBO skew hazard. Metric: cas_rejections
# (application-visible failed etcd/ZK compare-and-set ops) vs skew.
#
# Expected: order-preserving logs (Embarcadero server-side hold; Corfu
# token-before-write) stay at 0 across the skew range; Scalog (write-before-
# order) commits past the gap so rejections climb steeply with skew.
#
# Co-located topology (this testbed): broker i listens on 127.0.0.1:(1214+i);
# control on 12140. Requires passwordless `sudo tc`.
#
#   DELAYS="0 1 2 5 10" SYSTEMS="EMBARCADERO SCALOG CORFU" \
#     bash PaperScripts/run_cas_skew_panelA.sh
#
# Output: data/paper_eval/cas/<TAG>/panelA.csv
#   system,delay_ms,valid,cas_rejections,cas_success,session_reorders
set -uo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

DELAYS="${DELAYS:-0 1 2 5 10}"
SYSTEMS="${SYSTEMS:-EMBARCADERO SCALOG CORFU}"
SKEW_PORT="${SKEW_PORT:-1217}"        # broker 3 data port (1214 + broker_id)
OPS="${OPS:-100000}"; KEYS="${KEYS:-5000}"
TAG="${TAG:-cas_skew_panelA}"
OUT="$PROJECT_ROOT/data/paper_eval/cas/$TAG"; mkdir -p "$OUT"
RESULT="$OUT/panelA.csv"
echo "system,delay_ms,valid,cas_rejections,cas_success,session_reorders" > "$RESULT"

clear_netem() { sudo -n tc qdisc del dev lo root 2>/dev/null || true; }
set_netem() {  # $1 = delay ms; 0 => clean baseline (no qdisc)
  clear_netem
  [ "$1" = "0" ] && return 0
  sudo -n tc qdisc add dev lo root handle 1: prio >/dev/null 2>&1
  sudo -n tc qdisc add dev lo parent 1:3 handle 30: netem delay "${1}ms" >/dev/null 2>&1
  sudo -n tc filter add dev lo parent 1: protocol ip u32 match ip dport "$SKEW_PORT" 0xffff flowid 1:3 >/dev/null 2>&1
}
# SAFETY: always clear netem on any exit so a lingering delay cannot corrupt runs.
trap 'clear_netem' EXIT INT TERM
sudo -n modprobe sch_netem 2>/dev/null || true

for d in $DELAYS; do
  set_netem "$d"
  echo ">>> delay=${d}ms port $SKEW_PORT (netem=$(sudo -n tc qdisc show dev lo | grep -c netem)) ($(date -u +%H:%M:%SZ))"
  for sys in $SYSTEMS; do
    pt="$OUT/${sys}_d${d}"; rm -rf "$pt"; mkdir -p "$pt"
    SMR_FIFO_SEQUENCERS="$sys" SMR_FIFO_MODES="pipe" SMR_FIFO_CAS=1 SMR_FIFO_NUM_TRIALS=1 \
      SMR_FIFO_RECORD_COUNT="$KEYS" SMR_FIFO_OPERATION_COUNT="$OPS" SMR_FIFO_WARMUP_OPS=5000 \
      BENCH_TIMEOUT_SEC=200 OUT_ROOT="$pt" \
      EMBARCADERO_CORFU_SEQ_IP=10.10.10.10 \
      bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$pt/driver.log" 2>&1 || true
    f=$(ls "$pt"/${sys}_*pipe*_s0/summary.csv 2>/dev/null | head -1)
    if [ -n "$f" ]; then
      row=$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{print $h["valid"]","$h["cas_rejections"]","$h["cas_success"]","$h["session_reorders"]}' "$f")
      echo "$sys,$d,$row" | tee -a "$RESULT"
    else
      echo "$sys,$d,NA,NA,NA,NA" | tee -a "$RESULT"
    fi
    pkill -KILL -x embarlet 2>/dev/null || true
    for g in /dev/shm/CXL_*; do [ -e "$g" ] && [ "$(stat -c '%U' "$g")" = "domin" ] && rm -f "$g"; done 2>/dev/null
    sleep 2
  done
  clear_netem
done
echo ""; echo "=== Panel A: CAS rejections vs skew ==="; cat "$RESULT"; echo "Done. $RESULT"
