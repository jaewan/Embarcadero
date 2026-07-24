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
set -uo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

SYSTEMS="${SYSTEMS:-EMBARCADERO CORFU SCALOG}"
DEPTHS="${DEPTHS:-1 4 16 64 256 0}"     # 0 = full pipeline (apply barrier)
TRIALS="${TRIALS:-3}"   # >=3 for a stable median; per-point trials are high-variance
TS="$(date +%Y%m%dT%H%M%SZ)"
FRONTIER_ROOT="${OUT_ROOT:-$PROJECT_ROOT/data/paper_eval/kv_frontier_${TS}}"
mkdir -p "$FRONTIER_ROOT"
CSV="$FRONTIER_ROOT/frontier.csv"
echo "system,depth,throughput_ops_med,apply_p50_us_med,apply_p99_us_med,valid_all,n" > "$CSV"

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
    SMR_FIFO_SEQUENCERS="$sys" \
    SMR_FIFO_MODES="frontier" \
    SMR_FIFO_NUM_TRIALS="$TRIALS" \
    SMR_FIFO_TRACK_LATENCY=1 \
    SMR_FIFO_SYNC_INTERVAL_OVERRIDE="$depth" \
    SMR_FIFO_OPERATION_COUNT="$ops" \
    BENCH_TIMEOUT_SEC=200 BENCH_TIMEOUT_SCALOG=300 \
    OUT_ROOT="$point_out" \
    bash benchmarks/kv_store/run_smr_fifo_eval.sh > "$point_out/driver.log" 2>&1 || true

    # Aggregate this point's per-trial rows (the harness writes one summary.csv
    # per trial dir). Compute medians; valid_all = 1 iff every trial valid.
    tps=(); p50s=(); p99s=(); valid_all=1; n=0
    for f in "$point_out"/*_frontier_trial*_s0/summary.csv; do
      [[ -e "$f" ]] || continue
      row="$(awk -F, 'NR==1{for(i=1;i<=NF;i++)h[$i]=i} NR==2{print $h["throughput_ops_sec"]":"$h["apply_p50_us"]":"$h["apply_p99_us"]":"$h["valid"]}' "$f")"
      [[ -z "$row" ]] && continue
      IFS=: read -r tp p50 p99 v <<<"$row"
      tps+=("$tp"); p50s+=("$p50"); p99s+=("$p99"); n=$((n+1))
      [[ "$v" != "1" ]] && valid_all=0
    done
    if [[ "$n" -gt 0 ]]; then
      echo "$sys,$depth,$(median "${tps[@]}"),$(median "${p50s[@]}"),$(median "${p99s[@]}"),$valid_all,$n" | tee -a "$CSV"
    else
      echo "$sys,$depth,NA,NA,NA,0,0" | tee -a "$CSV"
    fi
  done
done

echo ""
echo "=== frontier.csv ==="
cat "$CSV"
echo ""
echo "Done. $CSV"
