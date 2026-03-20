#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

SEQUENCER=${SEQUENCER:-EMBARCADERO}
ORDERS=${ORDERS:-"0 5"}
ACK_LEVELS=${ACK_LEVELS:-"0 1 2"}
NUM_BROKERS=${NUM_BROKERS:-4}
TEST_TYPE=${TEST_TYPE:-5}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-10737418240}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-4}
TRIAL_MAX_ATTEMPTS=${TRIAL_MAX_ATTEMPTS:-1}
ACK_TIMEOUT_SEC=${EMBARCADERO_ACK_TIMEOUT_SEC:-60}
ACK2_REPLICATION_FACTOR=${ACK2_REPLICATION_FACTOR:-2}
NON_REPLICATED_RF=${NON_REPLICATED_RF:-0}
OUTDIR=${OUTDIR:-data/latency/ordering_durability_ladder}

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
    cmd=(
      env
      NUM_BROKERS="$NUM_BROKERS"
      TEST_TYPE="$TEST_TYPE"
      ORDER="$order"
      ACK="$ack"
      REPLICATION_FACTOR="$rf"
      TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE"
      MESSAGE_SIZE="$MESSAGE_SIZE"
      THREADS_PER_BROKER="$THREADS_PER_BROKER"
      SEQUENCER="$SEQUENCER"
      EMBARCADERO_ACK_TIMEOUT_SEC="$ACK_TIMEOUT_SEC"
      TRIAL_MAX_ATTEMPTS="$TRIAL_MAX_ATTEMPTS"
      "$SCRIPT_DIR/singlenode_run_throughput.sh"
    )

    LOG_FILE="$OUTDIR/o${order}_a${ack}.log"
    if "${cmd[@]}" >"$LOG_FILE" 2>&1; then
      bandwidth=$(grep -E "Publish bandwidth:|Bandwidth:" "$LOG_FILE" | tail -1 | grep -Eo '[0-9]+\.[0-9]+' | tail -1 || true)
      if [ -z "$bandwidth" ]; then
        echo "$order,$ack,$rf,FAIL,,missing bandwidth line" >> "$CSV"
        echo "  FAIL: missing bandwidth line"
        tail -n 20 "$LOG_FILE" || true
        continue
      fi
      echo "$order,$ack,$rf,PASS,$bandwidth," >> "$CSV"
      echo "  PASS: ${bandwidth} MB/s"
    else
      note=$(grep -E "ACK Timeout|Publish test failed|ERROR:" "$LOG_FILE" | tail -1 | tr ',' ';' || true)
      echo "$order,$ack,$rf,FAIL,,$note" >> "$CSV"
      echo "  FAIL: ${note:-see $LOG_FILE}"
      tail -n 20 "$LOG_FILE" || true
    fi
  done
done

echo "Ladder results written to $CSV"
