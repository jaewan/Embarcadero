#!/bin/bash
set -euo pipefail

# Throughput-Latency Sweep: varies offered load to generate throughput-latency curves.
# Produces per-point CSV files suitable for plotting hockey-stick curves.
#
# Requires: cmake -DCOLLECT_LATENCY_STATS=ON ..

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# --- Configuration ---
NUM_BROKERS=${NUM_BROKERS:-4}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_BYTES=${TOTAL_BYTES:-524288000}    # 500MB per data point
ORDER=${ORDER:-0}
ACK=${ACK:-1}
SEQUENCER=${SEQUENCER:-EMBARCADERO}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-$([ "$NUM_BROKERS" = "1" ] && echo 1 || echo 3)}
CONFIG=${CONFIG:-config/embarcadero.yaml}
CLIENT_CONFIG=${CLIENT_CONFIG:-config/client.yaml}

# Sweep target throughputs (MB/s). Override via env: SWEEP_TARGETS="500 1000 2000"
# Default targets span from low load to above saturation for 4-broker, 3-thread config
SWEEP_TARGETS=${SWEEP_TARGETS:-"200 500 1000 1500 2000 2500 3000 4000 5000 6000 8000 10000 12000"}
POINT_MAX_ATTEMPTS=${POINT_MAX_ATTEMPTS:-3}
STRICT_BROKER_COUNT=${STRICT_BROKER_COUNT:-0}

# Output directory
OUTDIR=${OUTDIR:-"data/latency/sweep_b${NUM_BROKERS}_o${ORDER}"}
mkdir -p "$OUTDIR"

# --- Environment ---
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
export EMBARCADERO_CXL_ZERO_MODE=${EMBARCADERO_CXL_ZERO_MODE:-metadata}
if [ -z "${EMBARCADERO_CXL_SHM_NAME:-}" ]; then
  export EMBARCADERO_CXL_SHM_NAME="/CXL_SHARED_SWEEP_${UID}"
fi

EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=1,2}"
CLIENT_NUMA_BIND="${CLIENT_NUMA_BIND:-numactl --cpunodebind=0 --membind=0}"

# Timeout: base 30s + 2s per 10MB.  The broker→subscriber export path for
# Order 0 can be slow (~8 MB/s aggregate) under heavy write load, so we
# need generous headroom.
TIMEOUT_SEC=$(( 30 + TOTAL_BYTES / 5242880 ))
SWEEP_TIMEOUT_CAP_SEC=${SWEEP_TIMEOUT_CAP_SEC:-240}
if [ "$SWEEP_TIMEOUT_CAP_SEC" -gt 0 ] && [ "$TIMEOUT_SEC" -gt "$SWEEP_TIMEOUT_CAP_SEC" ]; then
  TIMEOUT_SEC=$SWEEP_TIMEOUT_CAP_SEC
fi
export EMBARCADERO_E2E_TIMEOUT_SEC=$TIMEOUT_SEC
# Keep publisher ACK timeout aligned with latency timeout budget unless caller overrides.
export EMBARCADERO_ACK_TIMEOUT_SEC=${EMBARCADERO_ACK_TIMEOUT_SEC:-$TIMEOUT_SEC}
RUN_TIMEOUT_SEC=${RUN_TIMEOUT_SEC:-$((TIMEOUT_SEC + 60))}

cd build/bin || { echo "Error: build/bin not found. Run cmake/make first."; exit 1; }

# --- Helpers ---
cleanup() {
  local remove_cxl=${1:-false}
  pkill -9 -f "embarlet" >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  pkill -9 -f "corfu_global_sequencer" >/dev/null 2>&1 || true
  rm -f /tmp/embarlet_*_ready 2>/dev/null
  if [ "$remove_cxl" = "true" ]; then
    rm -f "/dev/shm${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
  fi
  # Wait for TCP TIME_WAIT on the broker data port to clear
  local port
  port=$(grep -oP 'port:\s*\K[0-9]+' "$PROJECT_ROOT/$CONFIG" 2>/dev/null | head -1)
  if [ -n "$port" ]; then
    local attempts=0
    while ss -tlnp 2>/dev/null | grep -q ":${port} " && [ $attempts -lt 20 ]; do
      sleep 0.5
      attempts=$((attempts + 1))
    done
  fi
  sleep 1
}

wait_for_brokers() {
  local timeout=$1
  local expected_count=$2
  local start=$(date +%s)
  echo "  Waiting for $expected_count broker(s)..."
  while true; do
    local ready_files=(/tmp/embarlet_*_ready)
    local ready=0
    if [ -e "${ready_files[0]}" ]; then
      ready=${#ready_files[@]}
    fi
    [ "$ready" -ge "$expected_count" ] && return 0
    [ $(($(date +%s) - start)) -gt "$timeout" ] && { echo "  ERROR: broker startup timeout"; return 1; }
    sleep 0.2
  done
}

start_brokers() {
  if [[ "$SEQUENCER" == "CORFU" ]]; then
    ./corfu_global_sequencer > /tmp/corfu_sequencer.log 2>&1 &
    sleep 1
  fi

  # Start head broker first — it creates and populates the CXL shared memory file.
  $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" --head --$SEQUENCER > /tmp/embarlet_head.log 2>&1 &

  if [ "$NUM_BROKERS" -gt 1 ]; then
    # Wait for head to finish CXL init before launching followers.
    # Followers MAP_POPULATE the 64GB region; starting after the head
    # avoids doubling memory-init pressure and cuts ~30s off wall time.
    if ! wait_for_brokers 120 1; then
      cleanup
      return 1
    fi
    rm -f /tmp/embarlet_*_ready

    for ((i=1; i<NUM_BROKERS; i++)); do
      $EMBARLET_NUMA_BIND ./embarlet --config "../../${CONFIG}" > /tmp/embarlet_${i}.log 2>&1 &
    done

    # Wait for the remaining followers (head ready file already consumed).
    local followers=$((NUM_BROKERS - 1))
    if ! wait_for_brokers 180 "$followers"; then
      cleanup
      return 1
    fi
  else
    if ! wait_for_brokers 120 1; then
      cleanup
      return 1
    fi
  fi
  rm -f /tmp/embarlet_*_ready
  sleep 5
}

# --- Summary CSV header ---
SUMMARY="$PROJECT_ROOT/$OUTDIR/summary.csv"
echo "target_mbps,actual_pub_mbps,actual_e2e_mbps,pub_p50_us,pub_p99_us,pub_p999_us,e2e_p50_us,e2e_p99_us,e2e_p999_us" > "$SUMMARY"

# --- Main Sweep ---
echo "========================================================"
echo " Throughput-Latency Sweep"
echo " Order=$ORDER  Sequencer=$SEQUENCER  Brokers=$NUM_BROKERS"
echo " MsgSize=$MESSAGE_SIZE  TotalBytes=$TOTAL_BYTES"
echo " Timeouts: ACK=${EMBARCADERO_ACK_TIMEOUT_SEC}s E2E=${EMBARCADERO_E2E_TIMEOUT_SEC}s"
echo " Targets: $SWEEP_TARGETS"
echo " Output:  $OUTDIR/"
echo "========================================================"

cleanup true   # initial: remove stale CXL

for target in $SWEEP_TARGETS; do
  echo ""
  echo "--- Target: ${target} MB/s ---"
  point_ok=0
  for ((attempt=1; attempt<=POINT_MAX_ATTEMPTS; attempt++)); do
    echo "  Attempt ${attempt}/${POINT_MAX_ATTEMPTS}"
    cleanup        # between points: keep CXL file resident for fast re-mmap

    if ! start_brokers; then
      echo "  WARN: broker start failed"
      continue
    fi

    # Run latency test with open-loop pacing; capture stdout for throughput extraction
    TEST_LOG="/tmp/throughput_test_${target}.log"
    cmd=(
      ./throughput_test
      --config "../../${CLIENT_CONFIG}"
      -n "$THREADS_PER_BROKER"
      -m "$MESSAGE_SIZE"
      -s "$TOTAL_BYTES"
      -t 2
      -o "$ORDER"
      -a "$ACK"
      --sequencer "$SEQUENCER"
      --target_mbps "$target"
      --record_results
      -l 0
    )
    if command -v timeout >/dev/null 2>&1; then
      if $CLIENT_NUMA_BIND timeout "${RUN_TIMEOUT_SEC}s" "${cmd[@]}" >"$TEST_LOG" 2>&1; then
        run_ok=1
      else
        run_ok=0
      fi
    else
      if $CLIENT_NUMA_BIND "${cmd[@]}" >"$TEST_LOG" 2>&1; then
        run_ok=1
      else
        run_ok=0
      fi
    fi
    cat "$TEST_LOG"

    if [ "${run_ok:-0}" -eq 1 ]; then

      if grep -Eq "Latency test failed|Subscriber::Poll timeout|Exception during latency test|not all messages acknowledged" "$TEST_LOG"; then
        echo "  WARN: latency run failed despite zero exit status; retrying target"
        cleanup
        continue
      fi

      data_brokers=$(grep -oP 'data_brokers=\K[0-9]+' "$TEST_LOG" 2>/dev/null | tail -1 || true)
      if [ -z "$data_brokers" ]; then
        data_brokers="unknown"
      fi
      if [ "$STRICT_BROKER_COUNT" = "1" ] && [ "$data_brokers" != "unknown" ] && [ "$data_brokers" -lt "$NUM_BROKERS" ]; then
        echo "  WARN: run used only ${data_brokers}/${NUM_BROKERS} brokers; retrying target"
        cleanup
        continue
      fi

      POINT_DIR="$PROJECT_ROOT/$OUTDIR/${target}mbps"
      mkdir -p "$POINT_DIR"
      for f in pub_latency_stats.csv pub_cdf_latency_us.csv latency_stats.csv cdf_latency_us.csv; do
        [ -f "$f" ] && mv "$f" "$POINT_DIR/"
      done
      cp "$TEST_LOG" "$POINT_DIR/test.log"

      PUB_STATS="$POINT_DIR/pub_latency_stats.csv"
      E2E_STATS="$POINT_DIR/latency_stats.csv"

      # CSV columns: Average,Min,Median,p90,p95,p99,p999,Max,...
      pub_p50="" pub_p99="" pub_p999=""
      if [ -f "$PUB_STATS" ]; then
        row=$(grep -m1 "submit_to_ack" "$PUB_STATS" 2>/dev/null || grep -m1 "send_to_ack" "$PUB_STATS" 2>/dev/null || true)
        if [ -n "$row" ]; then
          pub_p50=$(echo "$row" | awk -F',' '{print $3}')   # Median
          pub_p99=$(echo "$row" | awk -F',' '{print $6}')
          pub_p999=$(echo "$row" | awk -F',' '{print $7}')
        fi
      fi

      e2e_p50="" e2e_p99="" e2e_p999=""
      if [ -f "$E2E_STATS" ]; then
        row=$(grep -m1 "publish_to_deliver_latency" "$E2E_STATS" 2>/dev/null || tail -1 "$E2E_STATS" 2>/dev/null || true)
        if [ -n "$row" ]; then
          e2e_p50=$(echo "$row" | awk -F',' '{print $3}')
          e2e_p99=$(echo "$row" | awk -F',' '{print $6}')
          e2e_p999=$(echo "$row" | awk -F',' '{print $7}')
        fi
      fi

      # Extract actual submit-phase throughput from log
      actual_pub=$(grep -oP 'Open-loop submit phase: \K[0-9.]+' "$TEST_LOG" 2>/dev/null || echo "N/A")
      actual_e2e=$(grep -oP 'Publish completed in .* \K[0-9.]+(?=\s*MB/s)' "$TEST_LOG" 2>/dev/null | tail -1 || true)
      if [ -z "$actual_e2e" ]; then
        actual_e2e=$(grep -oP 'Publish bandwidth:\s*\K[0-9.]+(?=\s*MB/s)' "$TEST_LOG" 2>/dev/null | tail -1 || true)
      fi
      [ -z "$actual_e2e" ] && actual_e2e="N/A"

      echo "${target},${actual_pub},${actual_e2e},${pub_p50:-N/A},${pub_p99:-N/A},${pub_p999:-N/A},${e2e_p50:-N/A},${e2e_p99:-N/A},${e2e_p999:-N/A}" >> "$SUMMARY"

      echo "  OK: results in $POINT_DIR/ (data_brokers=${data_brokers})"
      point_ok=1
      cleanup
      break
    else
      echo "  WARN: throughput_test returned non-zero"
      cleanup
    fi
  done

  if [ "$point_ok" -ne 1 ]; then
    echo "  FAIL: target ${target} MB/s failed after ${POINT_MAX_ATTEMPTS} attempts"
    echo "${target},FAIL,FAIL,,,,,," >> "$SUMMARY"
  fi
done

cleanup true   # final: remove CXL file

echo ""
echo "========================================================"
echo " Sweep complete. Summary: $OUTDIR/summary.csv"
echo "========================================================"
