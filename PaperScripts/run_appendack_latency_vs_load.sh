#!/usr/bin/env bash
# PaperScripts/run_appendack_latency_vs_load.sh
#
# Co-located append->ACK latency vs offered-load sweep at RF=2/ACK=2 for the
# three ordered log systems: EMBARCADERO(ORDER=5), CORFU(ORDER=2), SCALOG(ORDER=1).
# This is the co-located companion to the (remote-only) Fig2 campaign
# (PaperScripts/run_fig2_latency_vs_load.sh): it drives scripts/run_latency_vs_load.sh
# directly with SCENARIO=local (no ssh) and the memory-copy fast-durable RF2 sink,
# so it runs entirely on one host (e.g. moscxl).
#
# Primary metric: publisher send->ACK (append->ACK) p50/p99 in microseconds,
# read from each point's trial_results.csv (pub_ack_p50_us / pub_ack_p99_us).
#
# Why this exists (see also the fig2 harness): run_fig2_point hardcodes
# SCENARIO=remote and ssh's to CLIENT_HOST, so CLIENT_HOST=local resolves the
# literal host "local" and fails. Co-located therefore goes through
# run_latency_vs_load.sh, which self-launches the corfu/scalog sequencers.
#
# Requirements:
#   - build/bin/{embarlet,throughput_test,corfu_global_sequencer,scalog_global_sequencer}
#   - throughput_test built with -DCOLLECT_LATENCY_STATS=ON (emits pub_latency_stats.csv)
#   - 72 GiB CXL: the 64 GB config default only fits 3 of 4 broker segments
#     (8 GB each) plus ~34 GB GOI metadata -> broker 3 "CXL memory exhausted".
#
# Usage:
#   bash PaperScripts/run_appendack_latency_vs_load.sh
#   SYSLIST="EMBARCADERO" LOADS="250" bash PaperScripts/run_appendack_latency_vs_load.sh   # smoke
#   LOADS="10 25 50 100 250 500 1000" bash PaperScripts/run_appendack_latency_vs_load.sh   # full curve
#
# Output: data/paper_eval/appendack/<TAG>/appendack.csv
#   system,target_mbps,achieved_mbps,pub_ack_p50_us,pub_ack_p99_us,deliver_p50_us,status
set -uo pipefail
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

SYSLIST="${SYSLIST:-EMBARCADERO CORFU SCALOG}"
LOADS="${LOADS:-10 25 50 100 250 500 1000}"
TAG="${TAG:-appendack_rf2_$(date -u +%Y%m%dT%H%M%SZ)}"
TOTAL_BYTES="${TOTAL_BYTES:-$((1 * 1024 * 1024 * 1024))}"
BROKER_NIC_IP="${BROKER_NIC_IP:-10.10.10.10}"   # real NIC (Corfu needs non-loopback)
OUT_BASE="$PROJECT_ROOT/data/paper_eval/appendack/$TAG"
mkdir -p "$OUT_BASE"
RESULT_CSV="$OUT_BASE/appendack.csv"
echo "system,target_mbps,achieved_mbps,pub_ack_p50_us,pub_ack_p99_us,deliver_p50_us,status" > "$RESULT_CSV"

common_env() {
  export SCENARIO=local NUM_BROKERS=4 MSG_SIZE=1024
  export TOTAL_MESSAGE_SIZE="$TOTAL_BYTES"
  export NUM_TRIALS="${NUM_TRIALS:-1}" WARMUP_TRIALS=0 PACING_MODE=steady
  export EMBARCADERO_CXL_SIZE=77309411328   # 72 GiB (see header)
  export EMBARCADERO_CXL_ZERO_MODE=metadata EMBARCADERO_CXL_MAP_POPULATE=0 EMBAR_USE_HUGETLB=1
  export BROKER_READY_TIMEOUT_SEC=900 BROKER_REACHABILITY_TIMEOUT_SEC=60
  export EMBARCADERO_ACK_TIMEOUT_SEC=300 EMBAR_ORDER5_EPOCH_US=500
  export EMBARCADERO_LATENCY_ACK_PRIMARY=1
  export CLIENT_LD_LIBRARY_PATH="${CLIENT_LD_LIBRARY_PATH:-$PROJECT_ROOT/third_party/glog-0.6/lib:$PROJECT_ROOT/third_party/yaml-cpp-0.8/lib}"
  # RF2 memory-copy fast-durable sink (CXL + DRAM copy, no fdatasync)
  export EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy
  export EMBARCADERO_CHAIN_REPLICATION_INMEM=1 EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1
  unset EMBARCADERO_REPLICA_DISK_DIRS EMBARCADERO_CHAIN_SYNC_BYTES EMBARCADERO_CHAIN_SYNC_INTERVAL_MS 2>/dev/null || true
}

per_system_env() {
  case "$1" in
    EMBARCADERO) export SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=2 REPLICATION_FACTOR=2 ;;
    CORFU)       export SEQUENCER=CORFU ORDER=2 ACK_LEVEL=2 REPLICATION_FACTOR=2 EMBARCADERO_CORFU_SEQ_IP="$BROKER_NIC_IP" ;;
    SCALOG)      export SEQUENCER=SCALOG ORDER=1 ACK_LEVEL=2 REPLICATION_FACTOR=2 SCALOG_CXL_MODE=1 EMBARCADERO_SCALOG_SEQ_IP=127.0.0.1 ;;
    *) echo "unknown system $1" >&2; return 1 ;;
  esac
}

for sys in $SYSLIST; do
  ( common_env; per_system_env "$sys"
    export SYSTEM_LABEL="${sys}_o${ORDER}_ack2_rf2_mem" BENCHMARK_TAG="$TAG" RUN_ID="${sys}_run"
    export OUT_BASE="$OUT_BASE/raw" LOAD_POINTS_MBPS="$LOADS"
    echo ">>> $sys order=$ORDER ack=2 rf=2 loads=[$LOADS] ($(date -u +%H:%M:%SZ))"
    bash scripts/run_latency_vs_load.sh
  ) > "$OUT_BASE/${sys}_driver.log" 2>&1 || echo "  ($sys returned nonzero — parsing whatever landed)"

  for pt in "$OUT_BASE/raw/$TAG/${sys}_o"*"_ack2_rf2_mem/run_${sys}_run"/points/*/; do
    tr="$pt/trial_results.csv"
    [[ -e "$tr" ]] || { echo "$sys,?,,,,,MISSING_TRIAL" >> "$RESULT_CSV"; continue; }
    python3 - "$tr" "$sys" >> "$RESULT_CSV" <<'PY'
import csv,sys
tr,system=sys.argv[1],sys.argv[2]
for r in csv.DictReader(open(tr)):
    tgt=(r.get("target_mbps") or "").strip()
    ach=(r.get("achieved_offered_load_mbps") or r.get("achieved_e2e_goodput_mbps") or "").strip()
    p50=(r.get("pub_ack_p50_us") or "").strip(); p99=(r.get("pub_ack_p99_us") or "").strip()
    d50=(r.get("publish_to_deliver_p50_us") or "").strip()
    print(f"{system},{tgt},{ach},{p50},{p99},{d50},{'ok' if (p50 and p99) else 'no_pub_ack'}")
PY
  done
done

echo ""; echo "=== appendack.csv ==="; cat "$RESULT_CSV"; echo "Done. $RESULT_CSV"
