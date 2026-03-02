#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT/build/bin"

NUM_BROKERS=${NUM_BROKERS:-4}
ACK=${ACK:-1}
TEST_TYPE=${TEST_TYPE:-5}
SEQUENCER=${SEQUENCER:-EMBARCADERO}
ORDER=${ORDER:-5}
CLIENT_CONFIG=${CLIENT_CONFIG:-../../config/client.yaml}
BROKER_CONFIG=${BROKER_CONFIG:-../../config/embarcadero.yaml}

NORMAL_MESSAGE_SIZE=${NORMAL_MESSAGE_SIZE:-1024}
NORMAL_TOTAL_BYTES=${NORMAL_TOTAL_BYTES:-1073741824}
NORMAL_THREADS_PER_BROKER=${NORMAL_THREADS_PER_BROKER:-1}

STRESS_MESSAGE_SIZE=${STRESS_MESSAGE_SIZE:-256}
STRESS_TOTAL_BYTES=${STRESS_TOTAL_BYTES:-2147483648}
STRESS_THREADS_PER_BROKER=${STRESS_THREADS_PER_BROKER:-4}
CASE_MAX_ATTEMPTS=${CASE_MAX_ATTEMPTS:-3}

OUTDIR=${OUTDIR:-../../data/latency/order5_anomaly_checks}
mkdir -p "$OUTDIR"
SUMMARY="$OUTDIR/summary.csv"
echo "mode,message_size,total_bytes,threads_per_broker,fifo_violations,ack_order_violations,skipped_batches,scanner_timeout_skips,hold_timeout_skips,hold_buffer_forced_skips,stale_epoch_skips" > "$SUMMARY"

EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-numactl --cpunodebind=0 --membind=0}"

cleanup_force() {
  pkill -9 -f "./embarlet" >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  rm -f /tmp/embarlet_*_ready >/dev/null 2>&1 || true
}

wait_for_brokers() {
  local timeout_s=$1
  local expected=$2
  local start_ts
  start_ts=$(date +%s)
  while true; do
    local ready_files=(/tmp/embarlet_*_ready)
    local count=0
    if [ -e "${ready_files[0]}" ]; then
      count=${#ready_files[@]}
    fi
    if [ "$count" -ge "$expected" ]; then
      return 0
    fi
    if [ $(( $(date +%s) - start_ts )) -ge "$timeout_s" ]; then
      return 1
    fi
    sleep 0.1
  done
}

start_cluster() {
  local pids=()
  $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG" --head --$SEQUENCER >/tmp/order5_anom_broker_0.log 2>&1 &
  pids+=("$!")
  for ((i=1; i<NUM_BROKERS; i++)); do
    $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG" --$SEQUENCER >/tmp/order5_anom_broker_${i}.log 2>&1 &
    pids+=("$!")
  done
  if ! wait_for_brokers 90 "$NUM_BROKERS"; then
    echo "ERROR: brokers did not become ready" >&2
    cleanup_force
    return 1
  fi
  rm -f /tmp/embarlet_*_ready
  sleep 1
  echo "${pids[*]}"
}

stop_cluster_graceful() {
  local pids=("$@")
  for pid in "${pids[@]}"; do
    kill -TERM "$pid" >/dev/null 2>&1 || true
  done
  local deadline=$(( $(date +%s) + 20 ))
  for pid in "${pids[@]}"; do
    while kill -0 "$pid" >/dev/null 2>&1; do
      if [ "$(date +%s)" -ge "$deadline" ]; then
        kill -KILL "$pid" >/dev/null 2>&1 || true
        break
      fi
      sleep 0.1
    done
  done
  wait >/dev/null 2>&1 || true
}

collect_counters() {
  local mode=$1
  local msg_size=$2
  local total_bytes=$3
  local threads=$4
  local csvs=(order5_anomaly_counters_broker*.csv)
  if [ ! -e "${csvs[0]}" ]; then
    echo "ERROR: no anomaly CSV files produced" >&2
    return 1
  fi
  awk -F',' -v mode="$mode" -v msg_size="$msg_size" -v total_bytes="$total_bytes" -v threads="$threads" '
    FNR==1 {next}
    {
      fifo += $5; ack += $6; skipped += $7; scanner += $8; hold_to += $9; hold_buf += $10; stale += $11;
    }
    END {
      print mode "," msg_size "," total_bytes "," threads "," fifo "," ack "," skipped "," scanner "," hold_to "," hold_buf "," stale;
    }
  ' "${csvs[@]}" | tee -a "$SUMMARY"
}

run_case() {
  local mode=$1
  local msg_size=$2
  local total_bytes=$3
  local threads=$4

  for ((attempt=1; attempt<=CASE_MAX_ATTEMPTS; attempt++)); do
    rm -f order5_anomaly_counters_broker*.csv
    cleanup_force
    local pids_line
    pids_line=$(start_cluster)
    read -r -a broker_pids <<<"$pids_line"

    local case_log="$OUTDIR/${mode}_throughput_attempt${attempt}.log"
    if $CLIENT_NUMA_BIND ./throughput_test --config "$CLIENT_CONFIG" -n "$threads" -m "$msg_size" -s "$total_bytes" \
      -t "$TEST_TYPE" -o "$ORDER" -a "$ACK" --sequencer "$SEQUENCER" -l 0 -r 0 2>&1 | tee "$case_log"; then
      cp "$case_log" "$OUTDIR/${mode}_throughput.log"
      stop_cluster_graceful "${broker_pids[@]}"
      collect_counters "$mode" "$msg_size" "$total_bytes" "$threads"
      return 0
    fi

    stop_cluster_graceful "${broker_pids[@]}"
    echo "WARNING: mode=${mode} failed on attempt ${attempt}/${CASE_MAX_ATTEMPTS}" >&2
  done

  echo "ERROR: mode=${mode} failed after ${CASE_MAX_ATTEMPTS} attempts" >&2
  return 1
}

run_case normal "$NORMAL_MESSAGE_SIZE" "$NORMAL_TOTAL_BYTES" "$NORMAL_THREADS_PER_BROKER"
run_case cross_broker_stress "$STRESS_MESSAGE_SIZE" "$STRESS_TOTAL_BYTES" "$STRESS_THREADS_PER_BROKER"

NORMAL_FIFO=$(awk -F',' 'NR==2 {print $5+0}' "$SUMMARY")
NORMAL_ACK=$(awk -F',' 'NR==2 {print $6+0}' "$SUMMARY")
if [ "$NORMAL_FIFO" -ne 0 ] || [ "$NORMAL_ACK" -ne 0 ]; then
  echo "ERROR: normal mode has anomalies (fifo=${NORMAL_FIFO}, ack=${NORMAL_ACK})" >&2
  exit 2
fi

echo "Anomaly checks completed. Summary: $SUMMARY"
