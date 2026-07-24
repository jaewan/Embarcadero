#!/usr/bin/env bash
# Panel A (Q3 end-to-end): CAS dependent-conditional-update correctness vs
# injected inter-broker skew. Skew is injected with `tc netem` (route 1) on the
# loopback path to ONE broker's data port, so a client's round-robin-striped
# batches to that broker arrive late — the write-before-order skew hazard.
#
# Workload: each op is a compare-and-set on the client's OWN dependent version
# chain per key over a disjoint keyspace, pipelined speculatively (no wait for
# prior results). An out-of-submission-order apply WITHIN a key's chain rejects
# that conditional write, and — because later chained ops for the key are
# precomputed against the expected prior version — subsequent ops for that key
# also fail until the chain re-anchors. So cas_rejections measures the
# downstream application damage caused by same-key inversions (cascade-amplified),
# NOT the raw fraction of operations reordered.
#
# Expected: order-preserving logs (Embarcadero server-side hold; Corfu
# token-before-write) reject 0 across the skew range; Scalog (write-before-order)
# commits past the gap so rejections climb with skew.
#
#   TRIALS=3 DELAYS="0 1 2 5 10" SYSTEMS="EMBARCADERO SCALOG CORFU" \
#     bash PaperScripts/run_cas_skew_panelA.sh
#
# Output: data/paper_eval/cas/<TAG>/panelA.csv  (one row per trial; median +
#   variation computed by PaperScripts/plot_cas_skew_panelA.py) and manifest.txt.
set -uo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

TRIALS="${TRIALS:-3}"
DELAYS="${DELAYS:-0 1 2 5 10}"
SYSTEMS="${SYSTEMS:-EMBARCADERO SCALOG CORFU}"
SKEW_PORT="${SKEW_PORT:-1217}"        # broker 3 data port (1214 + broker_id)
OPS="${OPS:-100000}"; KEYS="${KEYS:-5000}"
TAG="${TAG:-cas_skew_panelA}"
OUT="$PROJECT_ROOT/data/paper_eval/cas/$TAG"; mkdir -p "$OUT"
RESULT="$OUT/panelA.csv"
echo "system,delay_ms,trial,valid,cas_rejections,cas_success,session_reorders" > "$RESULT"

# Reproducibility manifest.
{
  echo "commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "git_dirty=$([[ -n $(git status --porcelain 2>/dev/null) ]] && echo dirty || echo clean)"
  echo "trials=$TRIALS"; echo "delays_ms=$DELAYS"; echo "systems=$SYSTEMS"
  echo "ops=$OPS"; echo "keys=$KEYS"; echo "skew_port=$SKEW_PORT"; echo "rf=1"; echo "ack=1"
  echo "skew_tool=tc netem on lo dport $SKEW_PORT"
} > "$OUT/manifest.txt"

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
    SMR_FIFO_SEQUENCERS="$sys" SMR_FIFO_MODES="pipe" SMR_FIFO_CAS=1 SMR_FIFO_NUM_TRIALS="$TRIALS" \
      SMR_FIFO_RECORD_COUNT="$KEYS" SMR_FIFO_OPERATION_COUNT="$OPS" SMR_FIFO_WARMUP_OPS=5000 \
      BENCH_TIMEOUT_SEC=200 OUT_ROOT="$pt" \
      EMBARCADERO_CORFU_SEQ_IP=10.10.10.10 \
      bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$pt/driver.log" 2>&1 || true
    # One summary per trial: ${sys}_*pipe_trial<t>_s0/summary.csv
    for t in $(seq 1 "$TRIALS"); do
      f=$(ls "$pt"/${sys}_*pipe_trial${t}_s0/summary.csv 2>/dev/null | head -1)
      if [ -n "$f" ]; then
        row=$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{print $h["valid"]","$h["cas_rejections"]","$h["cas_success"]","$h["session_reorders"]}' "$f")
        echo "$sys,$d,$t,$row" | tee -a "$RESULT"
      else
        echo "$sys,$d,$t,NA,NA,NA,NA" | tee -a "$RESULT"
      fi
    done
    pkill -KILL -x embarlet 2>/dev/null || true
    for g in /dev/shm/CXL_*; do [ -e "$g" ] && [ "$(stat -c '%U' "$g")" = "domin" ] && rm -f "$g"; done 2>/dev/null
    sleep 2
  done
  clear_netem
done
echo ""; echo "=== Panel A: per-trial CAS rejections vs skew ==="; cat "$RESULT"; echo "Done. $RESULT"
