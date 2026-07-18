#!/usr/bin/env bash
# Corfu coordinator-path sensitivity campaign.
#
# This intentionally uses the throughput workload rather than Q3's shared KV
# apply path.  The injected delay is a post-grant token-stage critical-path
# delay, not a packet/network fault injector.  It isolates the throughput cost
# of extending the serialized token stage without changing batching or payload
# transport.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
RUNNER="$PROJECT_ROOT/scripts/run_multiclient.sh"

DELAYS_US_CSV="${DELAYS_US_CSV:-0,100,250,500,1000}"
TRANSPORTS_CSV="${TRANSPORTS_CSV:-grpc,cxl_mailbox}"
CAMPAIGN_ID="${CAMPAIGN_ID:-corfu_token_sensitivity_$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_BASE="${OUT_BASE:-$PROJECT_ROOT/data/corfu_token_sensitivity/$CAMPAIGN_ID}"
NUM_TRIALS="${NUM_TRIALS:-3}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-10737418240}"
MESSAGE_SIZE="${MESSAGE_SIZE:-4096}"
THREADS_PER_BROKER="${THREADS_PER_BROKER:-6}"
CLIENT_HOSTS_CSV="${CLIENT_HOSTS_CSV:-c4}"

mkdir -p "$OUT_BASE"
SUMMARY="$OUT_BASE/campaign_cells.csv"
printf 'transport,token_delay_us,status,log_dir\n' > "$SUMMARY"

IFS=',' read -r -a transports <<< "$TRANSPORTS_CSV"
IFS=',' read -r -a delays <<< "$DELAYS_US_CSV"

for transport in "${transports[@]}"; do
    if [[ "$transport" != "grpc" && "$transport" != "cxl_mailbox" ]]; then
        echo "ERROR: unsupported transport '$transport'" >&2
        exit 2
    fi
    for delay in "${delays[@]}"; do
        if [[ ! "$delay" =~ ^(0|[1-9][0-9]*)$ ]] || (( delay > 10000000 )); then
            echo "ERROR: invalid delay '$delay'" >&2
            exit 2
        fi
        cell="${transport}_delay${delay}us"
        log_dir="$OUT_BASE/$cell"
        echo "=== Corfu token sensitivity: transport=$transport delay=${delay}us ==="
        status=pass
        if ! env \
            SEQUENCER=CORFU ORDER=2 ACK=1 REPLICATION_FACTOR=0 TEST_TYPE=5 \
            NUM_CLIENTS=1 NUM_TRIALS="$NUM_TRIALS" CLIENT_HOSTS_CSV="$CLIENT_HOSTS_CSV" \
            TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE" MESSAGE_SIZE="$MESSAGE_SIZE" \
            THREADS_PER_BROKER="$THREADS_PER_BROKER" \
            EMBARCADERO_BASELINE_CONTROL_TRANSPORT="$transport" \
            EMBARCADERO_CORFU_TOKEN_DELAY_US="$delay" \
            LOG_DIR="$log_dir" BENCHMARK_TAG="$CAMPAIGN_ID/$cell" \
            bash "$RUNNER"; then
            status=fail
        fi
        printf '%s,%s,%s,%s\n' "$transport" "$delay" "$status" "$log_dir" >> "$SUMMARY"
        if [[ "$status" == fail ]]; then
            echo "ERROR: campaign cell failed: $cell" >&2
            exit 1
        fi
    done
done

echo "Corfu token sensitivity campaign complete: $SUMMARY"
