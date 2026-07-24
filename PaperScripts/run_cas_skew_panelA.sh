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
# Reproducibility (matches PaperScripts/run_kv_frontier.sh): fail-fast; builds
# the exact paper binaries; refuses a dirty tracked tree; records binary SHA-256s
# in a manifest; requires exactly TRIALS parseable trials per cell whose per-trial
# metadata carries the same clean commit.
#
#   TRIALS=3 DELAYS="0 1 2 5 10" SYSTEMS="EMBARCADERO SCALOG CORFU" \
#     bash PaperScripts/run_cas_skew_panelA.sh
#
# Output: data/paper_eval/cas/<TAG>/panelA.csv (one row per trial) + manifest.txt.
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

TRIALS="${TRIALS:-3}"
DELAYS="${DELAYS:-0 1 2 5 10}"
SYSTEMS="${SYSTEMS:-EMBARCADERO SCALOG CORFU}"
SKEW_PORT="${SKEW_PORT:-1217}"        # broker 3 data port (1214 + broker_id)
OPS="${OPS:-100000}"; KEYS="${KEYS:-5000}"
BUILD_BEFORE_RUN="${BUILD_BEFORE_RUN:-1}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
TAG="${TAG:-cas_skew_panelA}"
OUT="$PROJECT_ROOT/data/paper_eval/cas/$TAG"; mkdir -p "$OUT"
RESULT="$OUT/panelA.csv"

if ! [[ "$TRIALS" =~ ^[0-9]+$ ]] || (( TRIALS < 3 )); then
  echo "ERROR: TRIALS must be an integer >= 3 (got '$TRIALS')" >&2; exit 2
fi

# Build the exact paper binaries, then require a clean tracked tree so the
# manifest's commit identifies their source (stale binaries can outlive commits).
if [[ "$BUILD_BEFORE_RUN" == "1" ]]; then
  cmake --build "$PROJECT_ROOT/build" --clean-first \
    --target embarlet kv_ycsb_bench --parallel "$BUILD_JOBS"
fi
for binary in "$PROJECT_ROOT/build/bin/embarlet" "$PROJECT_ROOT/build/bin/kv_ycsb_bench"; do
  [[ -x "$binary" ]] || { echo "ERROR: missing executable: $binary" >&2; exit 2; }
done
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "ERROR: refusing paper campaign from a dirty tracked worktree" >&2
  git status --short --untracked-files=no >&2; exit 2
fi

GIT_COMMIT="$(git rev-parse HEAD)"
EMBARLET_SHA256="$(sha256sum "$PROJECT_ROOT/build/bin/embarlet" | awk '{print $1}')"
KV_BENCH_SHA256="$(sha256sum "$PROJECT_ROOT/build/bin/kv_ycsb_bench" | awk '{print $1}')"
{
  echo "campaign=cas_skew_panelA"
  echo "created_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "hostname=$(hostname)"
  echo "git_commit=$GIT_COMMIT"
  echo "git_dirty=clean"
  echo "embarlet_sha256=$EMBARLET_SHA256"
  echo "kv_ycsb_bench_sha256=$KV_BENCH_SHA256"
  echo "trials=$TRIALS"; echo "delays_ms=$DELAYS"; echo "systems=$SYSTEMS"
  echo "ops=$OPS"; echo "keys=$KEYS"; echo "rf=1"; echo "ack=1"
  echo "skew_tool=tc netem on lo dport $SKEW_PORT"
} > "$OUT/manifest.txt"

echo "system,delay_ms,trial,valid,cas_rejections,cas_success,session_reorders" > "$RESULT"

clear_netem() { sudo -n tc qdisc del dev lo root >/dev/null 2>&1 || true; }
set_netem() {  # $1 = delay ms; 0 => clean baseline (no qdisc)
  clear_netem
  [[ "$1" == "0" ]] && return 0
  sudo -n tc qdisc add dev lo root handle 1: prio >/dev/null 2>&1 || true
  sudo -n tc qdisc add dev lo parent 1:3 handle 30: netem delay "${1}ms" >/dev/null 2>&1 || true
  sudo -n tc filter add dev lo parent 1: protocol ip u32 match ip dport "$SKEW_PORT" 0xffff flowid 1:3 >/dev/null 2>&1 || true
}
# SAFETY: always clear netem on any exit (including a set -e abort).
trap 'clear_netem' EXIT INT TERM
sudo -n modprobe sch_netem >/dev/null 2>&1 || true

for d in $DELAYS; do
  set_netem "$d"
  echo ">>> delay=${d}ms port $SKEW_PORT (netem=$(sudo -n tc qdisc show dev lo | grep -c netem || true)) ($(date -u +%H:%M:%SZ))"
  for sys in $SYSTEMS; do
    pt="$OUT/${sys}_d${d}"; rm -rf "$pt"; mkdir -p "$pt"
    if ! SMR_FIFO_SEQUENCERS="$sys" SMR_FIFO_MODES="pipe" SMR_FIFO_CAS=1 \
         SMR_FIFO_NUM_TRIALS="$TRIALS" \
         SMR_FIFO_RECORD_COUNT="$KEYS" SMR_FIFO_OPERATION_COUNT="$OPS" SMR_FIFO_WARMUP_OPS=5000 \
         BENCH_TIMEOUT_SEC=200 OUT_ROOT="$pt" \
         EMBARCADERO_CORFU_SEQ_IP=10.10.10.10 \
         bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$pt/driver.log" 2>&1; then
      echo "ERROR: benchmark driver failed for $sys delay=$d (see $pt/driver.log)" >&2; exit 1
    fi
    n=0
    for t in $(seq 1 "$TRIALS"); do
      f=$(ls "$pt"/${sys}_*pipe_trial${t}_s0/summary.csv 2>/dev/null | head -1)
      [[ -n "$f" && -e "$f" ]] || { echo "ERROR: $sys delay=$d missing trial $t summary" >&2; exit 1; }
      meta="${f%/summary.csv}/metadata.txt"
      if [[ ! -f "$meta" ]] || ! grep -qx "git_commit=$GIT_COMMIT" "$meta" || ! grep -qx "git_dirty=clean" "$meta"; then
        echo "ERROR: provenance mismatch for $f" >&2; exit 1
      fi
      row=$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{print $h["valid"]","$h["cas_rejections"]","$h["cas_success"]","$h["session_reorders"]}' "$f")
      IFS=, read -r v rej succ reo <<<"$row"
      if ! [[ "$v" =~ ^[01]$ && "$rej" =~ ^[0-9]+$ && "$succ" =~ ^[0-9]+$ ]]; then
        echo "ERROR: malformed summary row in $f: $row" >&2; exit 1
      fi
      echo "$sys,$d,$t,$v,$rej,$succ,$reo" | tee -a "$RESULT"
      n=$((n+1))
    done
    (( n == TRIALS )) || { echo "ERROR: $sys delay=$d produced $n/$TRIALS trials" >&2; exit 1; }
    pkill -KILL -x embarlet 2>/dev/null || true
    for g in /dev/shm/CXL_*; do [[ -e "$g" && "$(stat -c '%U' "$g")" == "domin" ]] && rm -f "$g"; done 2>/dev/null || true
    sleep 2
  done
  clear_netem
done
echo ""; echo "=== Panel A: per-trial CAS rejections vs skew ==="; cat "$RESULT"; echo "Done. $RESULT"
