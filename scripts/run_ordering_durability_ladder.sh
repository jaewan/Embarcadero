#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

SYSTEM=${SYSTEM:-EMBARCADERO}
SEQUENCER=${SEQUENCER:-EMBARCADERO}
ORDERS=${ORDERS:-"0 5"}
ACK_LEVELS=${ACK_LEVELS:-"0 1 2"}
NUM_CLIENTS=${NUM_CLIENTS:-3}
NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-3}
TEST_TYPE=${TEST_TYPE:-5}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-10737418240}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-4}
TRIAL_MAX_ATTEMPTS=${TRIAL_MAX_ATTEMPTS:-3}
ACK_TIMEOUT_SEC=${EMBARCADERO_ACK_TIMEOUT_SEC:-120}
ACK2_REPLICATION_FACTOR=${ACK2_REPLICATION_FACTOR:-2}
NON_REPLICATED_RF=${NON_REPLICATED_RF:-1}
BROKER_IP=${BROKER_IP:-10.10.10.143}
START_DELAY_SEC=${START_DELAY_SEC:-8}
LOCAL_CLIENT_NUMA=${LOCAL_CLIENT_NUMA:-0}
TAG=${TAG:-ordering_durability_ladder_publication}
PUBLICATION_BROKER_CONFIG=${PUBLICATION_BROKER_CONFIG:-config/embarcadero.yaml}
PUBLICATION_CLIENT_CONFIG=${PUBLICATION_CLIENT_CONFIG:-config/client.yaml}
OUTDIR=${OUTDIR:-data/latency/ordering_durability_ladder}

if [ -z "${EMBARCADERO_REPLICA_DISK_DIRS:-}" ] && \
   [ -d "$PROJECT_ROOT/.Replication/disk0" ] && \
   [ -d "/mnt/nvme0/replication/disk1" ]; then
  export EMBARCADERO_REPLICA_DISK_DIRS="$PROJECT_ROOT/.Replication/disk0,/mnt/nvme0/replication/disk1"
fi

mkdir -p "$OUTDIR"
CSV="$OUTDIR/ladder.csv"

echo "order,ack,replication_factor,status,bandwidth_mbps,notes" > "$CSV"

for order in $ORDERS; do
  for ack in $ACK_LEVELS; do
    rf=$NON_REPLICATED_RF
    if [ "$ack" -eq 2 ]; then
      rf=$ACK2_REPLICATION_FACTOR
    fi

    echo "=== Ladder point: order=$order ack=$ack rf=$rf ==="
    run_id="o${order}_a${ack}"
    cmd=(
      env
      TAG="$TAG"
      RUN_ID="$run_id"
      SYSTEM="$SYSTEM"
      ORDER="$order"
      SEQUENCER="$SEQUENCER"
      NUM_CLIENTS="$NUM_CLIENTS"
      REPLICATION_FACTOR="$rf"
      ACK_LEVEL="$ack"
      NUM_TRIALS="$NUM_TRIALS"
      NUM_BROKERS="$NUM_BROKERS"
      TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE"
      MESSAGE_SIZE="$MESSAGE_SIZE"
      TEST_TYPE="$TEST_TYPE"
      THREADS_PER_BROKER="$THREADS_PER_BROKER"
      BROKER_IP="$BROKER_IP"
      START_DELAY_SEC="$START_DELAY_SEC"
      LOCAL_CLIENT_NUMA="$LOCAL_CLIENT_NUMA"
      PUBLICATION_BROKER_CONFIG="$PUBLICATION_BROKER_CONFIG"
      PUBLICATION_CLIENT_CONFIG="$PUBLICATION_CLIENT_CONFIG"
      EMBARCADERO_ACK_TIMEOUT_SEC="$ACK_TIMEOUT_SEC"
      TRIAL_MAX_ATTEMPTS="$TRIAL_MAX_ATTEMPTS"
      REQUIRE_FIRST_ATTEMPT_PASS=0
      "$SCRIPT_DIR/publication/run_throughput_cell.sh"
    )

    LOG_FILE="$OUTDIR/o${order}_a${ack}.log"
    if "${cmd[@]}" >"$LOG_FILE" 2>&1; then
      SUMMARY_CSV="$PROJECT_ROOT/data/publication/throughput/$TAG/${SYSTEM}_order${order}_rf${rf}_n${NUM_CLIENTS}/run_${run_id}/summary.csv"
      if [ ! -f "$SUMMARY_CSV" ]; then
        echo "$order,$ack,$rf,FAIL,,missing summary.csv" >> "$CSV"
        echo "  FAIL: missing summary.csv"
        tail -n 20 "$LOG_FILE" || true
        continue
      fi

      avg_gbps="$(awk -F',' 'NR>1 && $8=="ok" {sum+=$9; n++} END {if (n>0) printf("%.6f", sum/n);}' "$SUMMARY_CSV")"
      if [ -z "$avg_gbps" ]; then
        echo "$order,$ack,$rf,FAIL,,missing throughput rows" >> "$CSV"
        echo "  FAIL: missing throughput rows"
        tail -n 20 "$LOG_FILE" || true
        continue
      fi

      bandwidth_mbps="$(awk "BEGIN {printf \"%.2f\", $avg_gbps * 128}")"
      echo "$order,$ack,$rf,PASS,$bandwidth_mbps," >> "$CSV"
      echo "  PASS: ${bandwidth_mbps} MB/s"
    else
      note=$(grep -E "ACK Timeout|Publish test failed|ERROR:" "$LOG_FILE" | tail -1 | tr ',' ';' || true)
      echo "$order,$ack,$rf,FAIL,,$note" >> "$CSV"
      echo "  FAIL: ${note:-see $LOG_FILE}"
      tail -n 20 "$LOG_FILE" || true
    fi
  done
done

echo "Ladder results written to $CSV"
