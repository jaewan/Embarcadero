#!/bin/bash
# Throughput with moscxl-local client on NUMA 0 (CPU+mem), remote clients on wire.
# Goal: see if local bypass of the single 100GbE NIC raises aggregate past ~10.6 GB/s.
set +e

TAG=${TAG:-local_mix_$(date -u +%Y%m%dT%H%M%SZ)}
ROOT=/home/domin/Embarcadero/data/$TAG
mkdir -p "$ROOT"
echo "$ROOT" > /tmp/local_mix_dir.txt
echo "campaign root: $ROOT"

run_cell() {
  local label=$1 hosts=$2 numas=$3 n=$4
  local out="$ROOT/$label"
  mkdir -p "$out"
  local total=$(( n * 8 * 1024 * 1024 * 1024 ))
  echo "=== $label hosts=$hosts numas=$numas N=$n total=$((total/1024/1024/1024))GiB ==="
  cd /home/domin/Embarcadero
  env \
    SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
    NUM_CLIENTS=$n NUM_BROKERS=4 NUM_TRIALS=3 TRIAL_MAX_ATTEMPTS=2 \
    TOTAL_MESSAGE_SIZE=$total MESSAGE_SIZE=1024 THREADS_PER_BROKER=6 \
    CLIENT_HOSTS_CSV=$hosts CLIENT_NUMAS_CSV=$numas \
    LOCAL_CLIENT_NUMA=0 \
    EMBARCADERO_HEAD_ADDR=10.10.10.10 \
    EMBARCADERO_RUNTIME_MODE=throughput \
    EMBARCADERO_ORDER5_HOME_BROKERS=0 \
    EMBARCADERO_CXL_ZERO_MODE=metadata \
    SKIP_CLUSTER_SETUP=1 \
    CLIENT_LD_LIBRARY_PATH="$HOME/Embarcadero/lib" \
    OUT_BASE="$out" BENCHMARK_TAG="${TAG}_${label}" \
    bash scripts/run_multiclient.sh > "$out/harness.log" 2>&1
  local rc=$?
  echo "DONE $label rc=$rc"
  rg -n 'TOTAL|overlap window|Trial [123]  :|incomplete|TIMEOUT|Grand Summary' "$out/harness.log" 2>/dev/null | tail -25
}

# 1) c4 + c3 + local (NUMA 0)
run_cell "N3_c4c3local" "c4,c3,local" "1,1,0" 3

# 2) c4 + c3 + c1 + local (NUMA 0)
run_cell "N4_c4c3c1local" "c4,c3,c1,local" "1,1,0,0" 4

echo "LOCAL_MIX_CAMPAIGN_DONE root=$ROOT"
