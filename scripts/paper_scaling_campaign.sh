#!/bin/bash
# Embarcadero-only throughput scaling campaign (paper Q1 Embarcadero curve +
# regression validation of the dead-queue / Init / ReqReceive fix).
# All-remote clients, full stripe (HOME=0), throughput mode, RF=0/ACK=1.
# 8 GiB/client keeps per-broker data < 8 GiB segment boundary for N<=3.
set +e

TAG=${TAG:-paper_scaling_$(date -u +%Y%m%dT%H%M%SZ)}
ROOT=/home/domin/Embarcadero/data/$TAG
mkdir -p "$ROOT"
echo "$ROOT" > /tmp/paper_scaling_dir.txt
echo "campaign root: $ROOT"

run_n() {
  local n=$1 hosts=$2 numas=$3
  local out="$ROOT/N${n}"
  mkdir -p "$out"
  local total=$(( n * 8 * 1024 * 1024 * 1024 ))   # 8 GiB per client
  echo "=== N=$n hosts=$hosts numas=$numas total=$((total/1024/1024/1024))GiB ==="
  cd /home/domin/Embarcadero
  env \
    SEQUENCER=EMBARCADERO ORDER=5 ACK_LEVEL=1 REPLICATION_FACTOR=0 \
    NUM_CLIENTS=$n NUM_BROKERS=4 NUM_TRIALS=3 TRIAL_MAX_ATTEMPTS=2 \
    TOTAL_MESSAGE_SIZE=$total MESSAGE_SIZE=1024 THREADS_PER_BROKER=6 \
    CLIENT_HOSTS_CSV=$hosts CLIENT_NUMAS_CSV=$numas \
    EMBARCADERO_HEAD_ADDR=10.10.10.10 \
    EMBARCADERO_RUNTIME_MODE=throughput \
    EMBARCADERO_ORDER5_HOME_BROKERS=0 \
    EMBARCADERO_CXL_ZERO_MODE=metadata \
    SKIP_CLUSTER_SETUP=1 \
    CLIENT_LD_LIBRARY_PATH="$HOME/Embarcadero/lib" \
    OUT_BASE="$out" BENCHMARK_TAG="${TAG}_N${n}" \
    bash scripts/run_multiclient.sh > "$out/harness.log" 2>&1
  local rc=$?
  cp -a /home/domin/Embarcadero/multiclient_logs/trial*_c*.log "$out/" 2>/dev/null
  echo "DONE N=$n rc=$rc"
  rg -n 'TOTAL|overlap|Bandwidth|ACK_VERIFY 1|ACK_VERIFY [0-9]|FAIL|Trial ' "$out/harness.log" 2>/dev/null | tail -30
}

run_n 1 "c4"        "1"
run_n 2 "c4,c3"     "1,1"
run_n 3 "c4,c3,c1"  "1,1,0"

echo "SCALING_CAMPAIGN_DONE root=$ROOT"
