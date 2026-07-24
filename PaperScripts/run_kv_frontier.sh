#!/usr/bin/env bash
# PaperScripts/run_kv_frontier.sh
#
# Correctness-gated latency-throughput frontier for the SMR/KV store, per system.
# Closed-loop single client: the knob is in-flight depth (ops between ACK
# barriers). depth=1 is synchronous commit-and-wait (exposes each system's
# per-op critical path -- e.g. Corfu's token round-trip); large depth pipelines
# toward the throughput ceiling. Each point captures publish->ACK P50/P99
# (commit latency) and sustained throughput, and is Valid-checked. Reuses
# run_smr_fifo_eval.sh's cluster lifecycle + cleanup + Valid audit via its
# `frontier` mode.
#
# Latency axis is APPLY latency (submit->applied, the end-to-end commit latency
# a client observes), not publish-enqueue latency. Output:
# data/paper_eval/kv_frontier_<ts>/frontier.csv
#   system,depth,throughput_ops_med,apply_p50_us_med,apply_p99_us_med,valid_all,n
set -euo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

SYSTEMS="${SYSTEMS:-EMBARCADERO CORFU SCALOG}"
DEPTHS="${DEPTHS:-1 4 16 64 256 0}"     # 0 = full pipeline (apply barrier)
TRIALS="${TRIALS:-3}"   # >=3 for a stable median; per-point trials are high-variance
BUILD_BEFORE_RUN="${BUILD_BEFORE_RUN:-1}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
TS="$(date +%Y%m%dT%H%M%SZ)"
FRONTIER_ROOT="${OUT_ROOT:-$PROJECT_ROOT/data/paper_eval/kv_frontier_${TS}}"
mkdir -p "$FRONTIER_ROOT"
CSV="$FRONTIER_ROOT/frontier.csv"
echo "system,depth,throughput_ops_med,apply_p50_us_med,apply_p99_us_med,valid_all,n" > "$CSV"

if ! [[ "$TRIALS" =~ ^[0-9]+$ ]] || (( TRIALS < 3 )); then
  echo "ERROR: TRIALS must be an integer >= 3 (got '$TRIALS')" >&2
  exit 2
fi

# A source commit alone is not sufficient provenance: stale binaries can silently
# survive several source commits. Build the exact paper executables before hashing
# them, then require a clean tracked tree so the manifest identifies their source.
if [[ "$BUILD_BEFORE_RUN" == "1" ]]; then
  cmake --build "$PROJECT_ROOT/build" \
    --target embarlet kv_ycsb_bench --parallel "$BUILD_JOBS"
fi
for binary in "$PROJECT_ROOT/build/bin/embarlet" "$PROJECT_ROOT/build/bin/kv_ycsb_bench"; do
  if [[ ! -x "$binary" ]]; then
    echo "ERROR: missing benchmark executable: $binary" >&2
    exit 2
  fi
done
if [[ -n "$(git status --porcelain --untracked-files=no)" ]]; then
  echo "ERROR: refusing paper campaign from a dirty tracked worktree" >&2
  git status --short --untracked-files=no >&2
  exit 2
fi

GIT_COMMIT="$(git rev-parse HEAD)"
EMBARLET_SHA256="$(sha256sum "$PROJECT_ROOT/build/bin/embarlet" | awk '{print $1}')"
KV_BENCH_SHA256="$(sha256sum "$PROJECT_ROOT/build/bin/kv_ycsb_bench" | awk '{print $1}')"
{
  echo "campaign=kv_frontier"
  echo "created_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "git_commit=$GIT_COMMIT"
  echo "git_dirty=clean"
  echo "embarlet_sha256=$EMBARLET_SHA256"
  echo "kv_ycsb_bench_sha256=$KV_BENCH_SHA256"
  echo "systems=$SYSTEMS"
  echo "depths=$DEPTHS"
  echo "trials=$TRIALS"
  echo "rf=1"
  echo "ack=1"
  echo "latency_tracking=1"
} > "$FRONTIER_ROOT/manifest.txt"

# Op count per depth so each point measures over a comparable wall-time window
# (low depth = synchronous = slow; high depth = pipelined = fast).
ops_for_depth() {
  case "$1" in
    1)   echo 20000 ;;
    4)   echo 40000 ;;
    16)  echo 100000 ;;
    64)  echo 300000 ;;
    256) echo 500000 ;;
    0)   echo 500000 ;;
    *)   echo 100000 ;;
  esac
}

median() { printf '%s\n' "$@" | sort -n | awk '{a[NR]=$1} END{if(NR==0){print "NA";exit} m=int((NR+1)/2); if(NR%2)print a[m]; else printf "%.0f\n",(a[m]+a[m+1])/2}'; }

echo "=== KV frontier sweep: systems=[$SYSTEMS] depths=[$DEPTHS] trials=$TRIALS ==="
echo "    out=$FRONTIER_ROOT"

for sys in $SYSTEMS; do
  for depth in $DEPTHS; do
    ops="$(ops_for_depth "$depth")"
    point_out="$FRONTIER_ROOT/${sys}_d${depth}"
    rm -rf "$point_out"; mkdir -p "$point_out"
    echo ">>> $sys depth=$depth ops=$ops ($(date -u +%H:%M:%SZ))"
    if ! SMR_FIFO_SEQUENCERS="$sys" \
      SMR_FIFO_MODES="frontier" \
      SMR_FIFO_NUM_TRIALS="$TRIALS" \
      SMR_FIFO_TRACK_LATENCY=1 \
      SMR_FIFO_SYNC_INTERVAL_OVERRIDE="$depth" \
      SMR_FIFO_OPERATION_COUNT="$ops" \
      BENCH_TIMEOUT_SEC=200 BENCH_TIMEOUT_SCALOG=300 \
      OUT_ROOT="$point_out" \
      bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$point_out/driver.log" 2>&1; then
      echo "ERROR: benchmark driver failed for $sys depth=$depth" >&2
      exit 1
    fi

    # Aggregate this point's per-trial rows (the harness writes one summary.csv
    # per trial dir). Compute medians; valid_all = 1 iff every trial valid.
    tps=(); p50s=(); p99s=(); valid_all=1; n=0
    for f in "$point_out"/*_frontier_trial*_s0/summary.csv; do
      [[ -e "$f" ]] || continue
      metadata="${f%/summary.csv}/metadata.txt"
      if [[ ! -f "$metadata" ]] ||
         ! grep -qx "git_commit=$GIT_COMMIT" "$metadata" ||
         ! grep -qx "git_dirty=clean" "$metadata"; then
        echo "ERROR: provenance mismatch for $f" >&2
        exit 1
      fi
      row="$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{print $h["throughput_ops_sec"]":"$h["apply_p50_us"]":"$h["apply_p99_us"]":"$h["valid"]}' "$f")"
      [[ -z "$row" ]] && continue
      IFS=: read -r tp p50 p99 v <<<"$row"
      if ! [[ "$tp" =~ ^[0-9]+([.][0-9]+)?$ &&
              "$p50" =~ ^[0-9]+([.][0-9]+)?$ &&
              "$p99" =~ ^[0-9]+([.][0-9]+)?$ &&
              "$v" =~ ^[01]$ ]]; then
        echo "ERROR: malformed summary row in $f: $row" >&2
        exit 1
      fi
      tps+=("$tp"); p50s+=("$p50"); p99s+=("$p99"); n=$((n+1))
      [[ "$v" != "1" ]] && valid_all=0
    done
    if (( n != TRIALS )); then
      echo "ERROR: $sys depth=$depth produced $n/$TRIALS complete trials" >&2
      exit 1
    fi
    echo "$sys,$depth,$(median "${tps[@]}"),$(median "${p50s[@]}"),$(median "${p99s[@]}"),$valid_all,$n" | tee -a "$CSV"
  done
done

echo ""
echo "=== frontier.csv ==="
cat "$CSV"
echo ""
echo "Done. $CSV"
