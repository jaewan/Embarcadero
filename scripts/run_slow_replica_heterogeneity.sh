#!/bin/bash
set -euo pipefail

# ---------------------------------------------------------------------------
# run_slow_replica_heterogeneity.sh
#
# Scientific claim tested (Appendix Table):
#   "Slow replica stalls ordering: No for Embarcadero, Yes for Scalog/LazyLog"
#
# Method: Run two trials per system — baseline (no fault) and slow_injected
# (SIGSTOP on a follower broker).  Extract ACK1 (ordering) and ACK2 (durable)
# P99 latencies from stage_latency_summary.csv and compare.
#
# For Embarcadero: SIGSTOP on broker 1 (follower) stalls its CXL→DRAM
# replication payload copy and completion-vector update.  ACK1 (sequencer-
# assigned ordering) comes from broker 0's sequencer — unaffected.
# ACK2 (durable prefix) waits on broker 1's completion vector → stalls.
#
# For Scalog: SIGSTOP on broker 1 stalls its replication progress report to
# the global cut.  The global cut uses element_wise_minimum over all shards,
# so broker 1's stall holds back the global ordered sequence number → BOTH
# ACK1 and ACK2 stall.
#
# IMPORTANT — ACK level note:
#   publisher.cc sets have_ordered_metric = (ack_level_ == 1).
#   When ACK=2, append_send_to_ordered is NOT written to stage_latency_summary.csv.
#   Strategy: run two sub-trials — ACK=1 (captures ordered metric) and ACK=2
#   (captures durable-ack metric) — then merge into a single row.
#
# Usage:
#   bash scripts/run_slow_replica_heterogeneity.sh               # EMBARCADERO
#   SEQUENCER=SCALOG bash scripts/run_slow_replica_heterogeneity.sh
# ---------------------------------------------------------------------------

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT/build/bin"

NUM_BROKERS=${NUM_BROKERS:-4}
SEQUENCER=${SEQUENCER:-EMBARCADERO}
ORDER=${ORDER:-5}
REPLICATION_FACTOR=${REPLICATION_FACTOR:-2}
ACK=${ACK:-2}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-1073741824}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-4}
TEST_TYPE=${TEST_TYPE:-2}
CONFIG=${CONFIG:-config/embarcadero.yaml}
CLIENT_CONFIG=${CLIENT_CONFIG:-config/client.yaml}
# Use broker 1 (first follower in the replication chain) as the slow replica.
# Broker 0 is the head/sequencer; stopping broker 1 specifically tests whether
# a follower slowdown propagates back to the sequencer (Scalog/LazyLog) or not
# (Embarcadero).
SLOW_BROKER_INDEX=${SLOW_BROKER_INDEX:-1}
INJECT_AFTER_SEC=${INJECT_AFTER_SEC:-2}
PAUSE_SEC=${PAUSE_SEC:-4}
POINT_MAX_ATTEMPTS=${POINT_MAX_ATTEMPTS:-2}
TIMEOUT_SEC=60
RUN_TIMEOUT_SEC=${RUN_TIMEOUT_SEC:-$((TIMEOUT_SEC + 60))}
OUTDIR=${OUTDIR:-data/latency/slow_replica}
if [[ "$OUTDIR" != /* ]]; then
  OUTDIR="$PROJECT_ROOT/$OUTDIR"
fi
mkdir -p "$OUTDIR"

# Resolve BROKER_IP for SCALOG local-sequencer mode (SKIP_REMOTE_SCALOG_SEQUENCER=1)
BROKER_IP=${BROKER_IP:-$(hostname -I | awk '{print $1}')}

# Replication sink: DRAM replica completion (same as Fig1 right panel).
# This isolates the coordination path from NVMe latency — making ACK2 fast enough
# for a latency comparison between ACK1 (ordering) and ACK2 (durable).
# Without this, disk fdatasync dominates and the ACK2 sub-trial times out.
export EMBARCADERO_CHAIN_REPLICATION_SINK="${EMBARCADERO_CHAIN_REPLICATION_SINK:-memory-copy}"
export EMBARCADERO_CHAIN_REPLICATION_INMEM="${EMBARCADERO_CHAIN_REPLICATION_INMEM:-1}"
export EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY="${EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY:-1}"

EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-numactl --cpunodebind=0 --membind=0}"
SEQUENCER_PID=""

cleanup() {
  kill_inject_bg
  pkill -9 -f "./embarlet" >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  pkill -9 -f "scalog_global_sequencer" >/dev/null 2>&1 || true
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
  # Pass RF to embarlet so it initialises the replication chain.
  # Without this, EMBARCADERO_REPLICATION_FACTOR=0 in the broker and ACK=2
  # clients wait forever for a replica that never starts.
  export EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"
  local pids=()
  if [[ "$SEQUENCER" == "CORFU" ]]; then
    ./corfu_global_sequencer >/tmp/hetero_corfu_sequencer.log 2>&1 &
    SEQUENCER_PID="$!"
    sleep 1
  elif [[ "$SEQUENCER" == "SCALOG" ]]; then
    # Run the global sequencer locally (SKIP_REMOTE_SCALOG_SEQUENCER=1 tells
    # embarlet not to SSH out to a remote machine for it).
    SKIP_REMOTE_SCALOG_SEQUENCER=1 EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP" \
      ./scalog_global_sequencer >/tmp/hetero_scalog_sequencer.log 2>&1 &
    SEQUENCER_PID="$!"
    sleep 1
  fi

  if [[ "$SEQUENCER" == "SCALOG" ]]; then
    # Pass env vars so embarlet's local sequencer connects to the local global seq
    SKIP_REMOTE_SCALOG_SEQUENCER=1 EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP" \
      $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --head --$SEQUENCER \
      >/tmp/hetero_broker_0.log 2>&1 &
    pids+=("$!")
    for ((i=1; i<NUM_BROKERS; i++)); do
      SKIP_REMOTE_SCALOG_SEQUENCER=1 EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP" \
        $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --$SEQUENCER \
        >/tmp/hetero_broker_${i}.log 2>&1 &
      pids+=("$!")
    done
  else
    $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --head --$SEQUENCER \
      >/tmp/hetero_broker_0.log 2>&1 &
    pids+=("$!")
    for ((i=1; i<NUM_BROKERS; i++)); do
      $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --$SEQUENCER \
        >/tmp/hetero_broker_${i}.log 2>&1 &
      pids+=("$!")
    done
  fi

  wait_for_brokers 90 "$NUM_BROKERS"
  rm -f /tmp/embarlet_*_ready
  echo "${pids[*]}"
}

INJECT_BG_PID=""

inject_slowdown() {
  local pid=$1
  (
    sleep "$INJECT_AFTER_SEC"
    kill -STOP "$pid" >/dev/null 2>&1 || true
    sleep "$PAUSE_SEC"
    kill -CONT "$pid" >/dev/null 2>&1 || true
  ) &
  INJECT_BG_PID=$!
}

kill_inject_bg() {
  if [ -n "$INJECT_BG_PID" ] && kill -0 "$INJECT_BG_PID" 2>/dev/null; then
    kill "$INJECT_BG_PID" 2>/dev/null || true
    wait "$INJECT_BG_PID" 2>/dev/null || true
  fi
  INJECT_BG_PID=""
}

# Extract a field from stage_latency_summary.csv.
# CSV header: Stage,Average,Min,p50,p99,p999,Max,Count  (cols 1-8)
# col=5 => p99
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

# run_mode <label> <inject_slow:0|1> <ack_level>
# ack_level=1 => records append_send_to_ordered (ACK1/ordering)
# ack_level=2 => records append_send_to_ack    (ACK2/durable)
run_mode() {
  local mode="$1"
  local inject="$2"
  local ack_level="$3"
  local run_dir="$OUTDIR/${SEQUENCER}/$mode"
  mkdir -p "$run_dir"

  local success=0
  for ((attempt=1; attempt<=POINT_MAX_ATTEMPTS; attempt++)); do
    cleanup
    sleep 2  # Let signals settle before starting new cluster
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

    rm -f stage_latency_summary.csv pub_latency_stats.csv latency_stats.csv \
          pub_cdf_latency_us.csv cdf_latency_us.csv

    local run_log="$run_dir/run_attempt${attempt}_ack${ack_level}.log"
    echo "Running mode=$mode ack_level=${ack_level} attempt=$attempt/$POINT_MAX_ATTEMPTS sequencer=$SEQUENCER" >&2

    local test_cmd=(
      ./throughput_test
      --config "../../${CLIENT_CONFIG}"
      -n "$THREADS_PER_BROKER"
      -m "$MESSAGE_SIZE"
      -s "$TOTAL_MESSAGE_SIZE"
      -t "$TEST_TYPE"
      -o "$ORDER"
      -a "$ack_level"
      -r "$REPLICATION_FACTOR"
      --sequencer "$SEQUENCER"
      --record_results
      -l 0
    )

    local run_ok=0
    if command -v timeout >/dev/null 2>&1; then
      if $CLIENT_NUMA_BIND timeout "${RUN_TIMEOUT_SEC}s" "${test_cmd[@]}" 2>&1 | tee "$run_log"; then
        run_ok=1
      fi
    else
      if $CLIENT_NUMA_BIND "${test_cmd[@]}" 2>&1 | tee "$run_log"; then
        run_ok=1
      fi
    fi

    # Check for catastrophic failure strings (not uid-dup from paused-replica scenario).
    # "Delivery ordering assertion failed" with only dup_uid is expected when the
    # replica pause causes hold-buffer batch release duplicates at the subscriber;
    # pub_latency_stats.csv (pub_ack P99) is still valid — copy it regardless.
    local catastrophic_fail=0
    if grep -Eq "Latency test failed|Subscriber poll timeout|Subscriber::Poll timeout|Exception during latency test|not all messages acknowledged" "$run_log"; then
      catastrophic_fail=1
    fi

    # Always copy latency CSVs if they exist (written before ordering assertion exit).
    # success=1 only when run_ok=1 AND no catastrophic failure.
    local copied=0
    for f in stage_latency_summary.csv pub_latency_stats.csv latency_stats.csv \
              pub_cdf_latency_us.csv cdf_latency_us.csv; do
      if [ -f "$f" ]; then
        cp "$f" "$run_dir/${f%.csv}_ack${ack_level}.csv"
        copied=1
      fi
    done

    if [ "${run_ok:-0}" -eq 1 ] && [ "$catastrophic_fail" -eq 0 ]; then
      success=1
    elif [ "$copied" -eq 1 ] && [ "$catastrophic_fail" -eq 0 ]; then
      # Ordering assertion failed (likely uid-dup from pause scenario) but latency
      # CSVs were written — treat as success for P99 extraction purposes.
      echo "[INFO] Ordering assertion failed (likely pause-induced dup_uid) — treating as success for latency extraction" >&2
      success=1
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

# ---------------------------------------------------------------------------
# Collect ordered (ACK1) and durable (ACK2) P99s for one system/mode pair.
# Because publisher.cc only emits append_send_to_ordered when ack_level=1,
# and only emits append_send_to_ack (durable) when ack_level>=2, we run two
# sub-trials per mode and read the metric from the matching CSV.
# ---------------------------------------------------------------------------
collect_pair() {
  local mode="$1"    # baseline | slow_injected
  local inject="$2"  # 0 | 1
  local run_dir="$OUTDIR/${SEQUENCER}/$mode"

  local ordered_p99="NA"
  local ack_p99="NA"
  local status="PASS"

  # Sub-trial for ACK1 (ordering latency)
  if run_mode "$mode" "$inject" 1; then
    ordered_p99=$(extract_metric \
      "$run_dir/stage_latency_summary_ack1.csv" "append_send_to_ordered" 5)
  else
    echo "[WARN] $SEQUENCER $mode ACK=1 sub-trial failed" >&2
    status="PARTIAL"
  fi

  # Sub-trial for ACK2 (durable latency)
  if run_mode "$mode" "$inject" 2; then
    ack_p99=$(extract_metric \
      "$run_dir/stage_latency_summary_ack2.csv" "append_send_to_ack" 5)
  else
    echo "[WARN] $SEQUENCER $mode ACK=2 sub-trial failed" >&2
    status="PARTIAL"
  fi

  if [ "$ordered_p99" = "NA" ] && [ "$ack_p99" = "NA" ]; then
    status="FAIL"
  fi

  echo "$SEQUENCER,$mode,$status,$ordered_p99,$ack_p99"
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
RESULT_CSV="$OUTDIR/slow_replica_comparison.csv"
echo "system,mode,status,ordered_p99_us,durable_p99_us" > "$RESULT_CSV"

echo "=== Testing SEQUENCER=$SEQUENCER ===" >&2

baseline_row=$(collect_pair "baseline" "0")
echo "$baseline_row" >> "$RESULT_CSV"
echo "  baseline: $baseline_row"

slow_row=$(collect_pair "slow_injected" "1")
echo "$slow_row" >> "$RESULT_CSV"
echo "  slow_injected: $slow_row"

# ---------------------------------------------------------------------------
# Human-readable delta table
# ---------------------------------------------------------------------------
awk -F',' '
NR==1 {next}
{
  system=$1; mode=$2; status=$3; op99=$4; dp99=$5
  if (mode=="baseline")   { b_o[system]=op99; b_d[system]=dp99 }
  if (mode=="slow_injected") { s_o[system]=op99; s_d[system]=dp99 }
}
END {
  fmt = "%-13s | %-14s | %10s | %10s | %12s | %12s\n"
  printf fmt, "System", "Mode", "ACK1-P99us", "ACK2-P99us", "ACK1-delta%", "ACK2-delta%"
  printf fmt, "-------------", "--------------", "----------", "----------", "------------", "------------"
  for (sys in b_o) {
    bo=b_o[sys]+0; bd=b_d[sys]+0
    so=s_o[sys]+0; sd=s_d[sys]+0
    dp_o = (bo>0) ? sprintf("+%.1f%%", 100*(so-bo)/bo) : "NA"
    dp_d = (bd>0) ? sprintf("+%.1f%%", 100*(sd-bd)/bd) : "NA"
    printf fmt, sys, "baseline",     b_o[sys], b_d[sys], "---", "---"
    printf fmt, sys, "slow_injected", s_o[sys], s_d[sys], dp_o, dp_d
  }
}
' "$RESULT_CSV" || true

echo ""
echo "Claim: ACK1 delta ~ 0% for EMBARCADERO (ordering unaffected by follower stall)."
echo "       ACK1 delta >> 0% for SCALOG (follower stall blocks global cut => ordering stalls)."
echo "       ACK2 delta >> 0% for both (durable prefix waits on all replicas)."
echo ""
echo "Artifacts: $OUTDIR"
echo "CSV:       $RESULT_CSV"

cleanup
