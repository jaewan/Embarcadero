#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT/build/bin"

NUM_BROKERS=${NUM_BROKERS:-4}
SEQUENCER=${SEQUENCER:-EMBARCADERO}
ORDER=${ORDER:-5}
ACK=${ACK:-1}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-1073741824}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-4}
TEST_TYPE=${TEST_TYPE:-2}
CONFIG=${CONFIG:-config/embarcadero.yaml}
CLIENT_CONFIG=${CLIENT_CONFIG:-config/client.yaml}
SLOW_BROKER_INDEX=${SLOW_BROKER_INDEX:-3}
INJECT_AFTER_SEC=${INJECT_AFTER_SEC:-2}
PAUSE_SEC=${PAUSE_SEC:-4}
POINT_MAX_ATTEMPTS=${POINT_MAX_ATTEMPTS:-2}
TIMEOUT_SEC=${TIMEOUT_SEC:-$((30 + TOTAL_MESSAGE_SIZE / 5242880))}
RUN_TIMEOUT_SEC=${RUN_TIMEOUT_SEC:-$((TIMEOUT_SEC + 60))}
OUTDIR=${OUTDIR:-data/latency/slow_replica}
if [[ "$OUTDIR" != /* ]]; then
  OUTDIR="$PROJECT_ROOT/$OUTDIR"
fi
mkdir -p "$OUTDIR"

EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-numactl --cpunodebind=0 --membind=0}"
SEQUENCER_PID=""

cleanup() {
  pkill -9 -f "./embarlet" >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  pkill -9 -f "corfu_global_sequencer" >/dev/null 2>&1 || true
  rm -f /tmp/embarlet_*_ready >/dev/null 2>&1 || true
  SEQUENCER_PID=""
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
  if [[ "$SEQUENCER" == "CORFU" ]]; then
    ./corfu_global_sequencer >/tmp/hetero_corfu_sequencer.log 2>&1 &
    SEQUENCER_PID="$!"
    sleep 1
  fi

  $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --head --$SEQUENCER >/tmp/hetero_broker_0.log 2>&1 &
  pids+=("$!")
  for ((i=1; i<NUM_BROKERS; i++)); do
    $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --$SEQUENCER >/tmp/hetero_broker_${i}.log 2>&1 &
    pids+=("$!")
  done
  wait_for_brokers 90 "$NUM_BROKERS"
  rm -f /tmp/embarlet_*_ready
  echo "${pids[*]}"
}

inject_slowdown() {
  local pid=$1
  (
    sleep "$INJECT_AFTER_SEC"
    kill -STOP "$pid" >/dev/null 2>&1 || true
    sleep "$PAUSE_SEC"
    kill -CONT "$pid" >/dev/null 2>&1 || true
  ) &
}

extract_metric() {
  local file="$1"
  local metric="$2"
  local col="$3"
  if [ ! -f "$file" ]; then
    echo "NA"
    return
  fi
  local v
  v=$(awk -F',' -v m="$metric" -v c="$col" 'NR>1 && $1==m {print $c; exit}' "$file")
  if [ -z "$v" ]; then
    echo "NA"
  else
    echo "$v"
  fi
}

run_mode() {
  local mode="$1"
  local inject="$2"
  local run_dir="$OUTDIR/$mode"
  mkdir -p "$run_dir"

  local success=0
  for ((attempt=1; attempt<=POINT_MAX_ATTEMPTS; attempt++)); do
    cleanup
    local broker_pid_line
    broker_pid_line=$(start_cluster)
    read -r -a broker_pids <<<"$broker_pid_line"

    if [ "$SLOW_BROKER_INDEX" -lt 0 ] || [ "$SLOW_BROKER_INDEX" -ge "${#broker_pids[@]}" ]; then
      echo "Invalid SLOW_BROKER_INDEX=$SLOW_BROKER_INDEX (pid count=${#broker_pids[@]})" >&2
      cleanup
      return 1
    fi

    if [ "$inject" = "1" ]; then
      inject_slowdown "${broker_pids[$SLOW_BROKER_INDEX]}"
    fi

    rm -f stage_latency_summary.csv pub_latency_stats.csv latency_stats.csv pub_cdf_latency_us.csv cdf_latency_us.csv

    local run_log="$run_dir/run_attempt${attempt}.log"
    echo "Running mode=$mode attempt=$attempt/$POINT_MAX_ATTEMPTS"
    test_cmd=(
      ./throughput_test
      --config "../../${CLIENT_CONFIG}"
      -n "$THREADS_PER_BROKER"
      -m "$MESSAGE_SIZE"
      -s "$TOTAL_MESSAGE_SIZE"
      -t "$TEST_TYPE"
      -o "$ORDER"
      -a "$ACK"
      --sequencer "$SEQUENCER"
      --record_results
      -l 0
    )
    if command -v timeout >/dev/null 2>&1; then
      if $CLIENT_NUMA_BIND timeout "${RUN_TIMEOUT_SEC}s" "${test_cmd[@]}" 2>&1 | tee "$run_log"; then
        run_ok=1
      else
        run_ok=0
      fi
    else
      if $CLIENT_NUMA_BIND "${test_cmd[@]}" 2>&1 | tee "$run_log"; then
        run_ok=1
      else
        run_ok=0
      fi
    fi

    if [ "${run_ok:-0}" -eq 1 ]; then
      if grep -Eq "Latency test failed|Subscriber poll timeout|Subscriber::Poll timeout|Exception during latency test|not all messages acknowledged" "$run_log"; then
        success=0
      else
        for f in stage_latency_summary.csv pub_latency_stats.csv latency_stats.csv pub_cdf_latency_us.csv cdf_latency_us.csv; do
          if [ -f "$f" ]; then
            cp "$f" "$run_dir/$f"
          fi
        done
        success=1
      fi
    fi

    for pid in "${broker_pids[@]}"; do
      kill -TERM "$pid" >/dev/null 2>&1 || true
    done
    for pid in "${broker_pids[@]}"; do
      wait "$pid" >/dev/null 2>&1 || true
    done
    if [ -n "$SEQUENCER_PID" ]; then
      kill -TERM "$SEQUENCER_PID" >/dev/null 2>&1 || true
      wait "$SEQUENCER_PID" >/dev/null 2>&1 || true
      SEQUENCER_PID=""
    fi

    if [ "$success" -eq 1 ]; then
      break
    fi
  done

  if [ "$success" -ne 1 ]; then
    return 1
  fi
  return 0
}

echo "mode,status,ordered_p99_us,ack_p99_us,deliver_p99_us,pub_bw_mb_s,notes" > "$OUTDIR/summary.csv"

if run_mode "baseline" "0"; then
  ordered=$(extract_metric "$OUTDIR/baseline/stage_latency_summary.csv" "append_send_to_ordered" 4)
  ackm=$(extract_metric "$OUTDIR/baseline/stage_latency_summary.csv" "append_send_to_ack" 4)
  deliver=$(extract_metric "$OUTDIR/baseline/stage_latency_summary.csv" "append_send_to_deliver" 4)
  bw=$(grep -oP 'Bandwidth:\s*\K[0-9.]+(?=\s*MB/s)|Publish completed in .* \K[0-9.]+(?=\s*MB/s)' "$OUTDIR/baseline"/run_attempt*.log 2>/dev/null | tail -1 || true)
  [ -z "$bw" ] && bw="NA"
  echo "baseline,PASS,$ordered,$ackm,$deliver,$bw," >> "$OUTDIR/summary.csv"
else
  echo "baseline,FAIL,NA,NA,NA,NA,run_failed" >> "$OUTDIR/summary.csv"
fi

if run_mode "slow_injected" "1"; then
  ordered=$(extract_metric "$OUTDIR/slow_injected/stage_latency_summary.csv" "append_send_to_ordered" 4)
  ackm=$(extract_metric "$OUTDIR/slow_injected/stage_latency_summary.csv" "append_send_to_ack" 4)
  deliver=$(extract_metric "$OUTDIR/slow_injected/stage_latency_summary.csv" "append_send_to_deliver" 4)
  bw=$(grep -oP 'Bandwidth:\s*\K[0-9.]+(?=\s*MB/s)|Publish completed in .* \K[0-9.]+(?=\s*MB/s)' "$OUTDIR/slow_injected"/run_attempt*.log 2>/dev/null | tail -1 || true)
  [ -z "$bw" ] && bw="NA"
  echo "slow_injected,PASS,$ordered,$ackm,$deliver,$bw,slow_broker_index=$SLOW_BROKER_INDEX pause_sec=$PAUSE_SEC" >> "$OUTDIR/summary.csv"
else
  echo "slow_injected,FAIL,NA,NA,NA,NA,run_failed" >> "$OUTDIR/summary.csv"
fi

awk -F',' '
  NR==1 {next}
  $1=="baseline" {bo=$3; ba=$4; bd=$5}
  $1=="slow_injected" {so=$3; sa=$4; sd=$5}
  END {
    print "metric,baseline,slow_injected,delta,delta_pct"
    if (bo!="" && bo!="NA" && so!="" && so!="NA") {
      d=so-bo; p=(bo!=0)?(100*d/bo):0; printf("ordered_p99_us,%s,%s,%.6f,%.3f\n", bo, so, d, p)
    } else { print "ordered_p99_us,NA,NA,NA,NA" }
    if (ba!="" && ba!="NA" && sa!="" && sa!="NA") {
      d=sa-ba; p=(ba!=0)?(100*d/ba):0; printf("ack_p99_us,%s,%s,%.6f,%.3f\n", ba, sa, d, p)
    } else { print "ack_p99_us,NA,NA,NA,NA" }
    if (bd!="" && bd!="NA" && sd!="" && sd!="NA") {
      d=sd-bd; p=(bd!=0)?(100*d/bd):0; printf("deliver_p99_us,%s,%s,%.6f,%.3f\n", bd, sd, d, p)
    } else { print "deliver_p99_us,NA,NA,NA,NA" }
  }
' "$OUTDIR/summary.csv" > "$OUTDIR/comparison.csv"

cleanup
echo "Slow-replica heterogeneity artifacts saved to $OUTDIR"
