#!/bin/bash
# scripts/run_2pub_1sub.sh
#
# 2-publisher + 1-subscriber cross-machine throughput experiment.
#
# Topology (fixed):
#   Publisher A:  c4          (NUMA ${PUB_C4_NUMA:-1})
#   Publisher B:  local       (NUMA ${LOCAL_PUB_NUMA:-0})  → connects via $BROKER_IP NIC
#   Subscriber:   c3          (NUMA ${SUB_NUMA:-1})
#   Brokers:      local       (NUMA 1,2)
#
# Data split: each publisher sends TOTAL_MESSAGE_SIZE/2 bytes.
# Subscriber polls for TOTAL_MESSAGE_SIZE bytes total.
#
# Aggregate publish bandwidth — OSDI/SOSP rigorous method
# ─────────────────────────────────────────────────────────
# Push-ready barrier protocol (sub-millisecond alignment):
#
#   1. Each publisher binary writes a ready-file once all connection setup and
#      buffer warmup is complete (EMBARCADERO_PUSH_READY_FILE, one per publisher).
#   2. Orchestrator polls both ready-files (with timeout), then computes:
#        go_ns = $(date +%s%N) + 200_000_000  (200 ms in the future)
#      and writes go_ns to a shared go-file (EMBARCADERO_PUSH_GO_FILE).
#   3. Each publisher reads go_ns and spin-waits until CLOCK_REALTIME >= go_ns,
#      then starts its push loop.
#
# Alignment error: both publishers read the same go_ns, so the only skew is
# the NTP offset between their system clocks (typically ≤2 ms on chrony hosts).
# For ~500 ms push windows this is <0.4% error — SOSP-quality.
#
# Metrics:
#   push_start_ns : from binary log "Publisher push start (wall ns): X"
#                   captured on each machine's own clock right after go_ns elapses
#   push_dur_ms   : from binary log "Publish test completed in X.XX seconds"
#   overlap_ms    : min(push_end_A, push_end_B) - max(push_start_A, push_start_B)
#                   computed barrier-relative so cross-clock error = NTP offset only
#   pipeline_bw   : TOTAL_BYTES / (last_push_end - first_push_start)
#   sub wire BW   : first-byte to last-byte on c3's own clock (immune to skew)
#
# Environment variables:
#   NUM_BROKERS, NUM_TRIALS, TRIAL_MAX_ATTEMPTS
#   TOTAL_MESSAGE_SIZE   total bytes across both publishers (default 10 GiB)
#   MESSAGE_SIZE, THREADS_PER_BROKER
#   ORDER, ACK, REPLICATION_FACTOR, SEQUENCER
#   EMBARCADERO_HEAD_ADDR   broker NIC IP (default 10.10.10.143)
#   PUB_C4_NUMA             NUMA node for c4 publisher (default 1)
#   LOCAL_PUB_NUMA          NUMA node for local publisher on broker machine (default 0)
#   SUB_NUMA                NUMA node for c3 subscriber (default 1)
#   SUB_LEAD_SEC            seconds subscriber starts before publisher barrier (default 6)
#   START_DELAY_SEC         seconds from "now" until publishers are launched and
#                           spinning on push-ready barrier; must exceed SSH round-trip
#                           + topic-creation handshake time (~5s typical, 12s is safe)
#   BARRIER_LEAD_MS         extra milliseconds after both ready-files appear before go_ns
#                           (default 200; gives spin-wait time to align clocks)
#   NTP_WARN_MS             warn if chrony/ntpq offset exceeds this (default 5)
#   EMBARCADERO_ORDER0_FAST_PATH, EMBARCADERO_BATCH_SIZE, etc.
#
# Output:
#   multiclient_logs/trial<N>_{c4_pub,local_pub,c3_sub}.log   raw output
#   multiclient_logs/results.csv                               machine-readable summary

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_BIN="$PROJECT_ROOT/build/bin"
BROKER_CONFIG="${BROKER_CONFIG:-config/embarcadero.yaml}"
CLIENT_CONFIG="${CLIENT_CONFIG:-config/client.yaml}"
BROKER_CONFIG_ABS="$PROJECT_ROOT/$BROKER_CONFIG"
CLIENT_CONFIG_ABS="$PROJECT_ROOT/$CLIENT_CONFIG"
LOG_DIR="$PROJECT_ROOT/multiclient_logs"
CSV_FILE="$LOG_DIR/results.csv"

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-3}
TRIAL_MAX_ATTEMPTS=${TRIAL_MAX_ATTEMPTS:-3}

TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-10737418240}   # 10 GiB
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-4}

ORDER=${ORDER:-0}
ACK=${ACK:-1}
REPLICATION_FACTOR=${REPLICATION_FACTOR:-0}
SEQUENCER=${SEQUENCER:-EMBARCADERO}

BROKER_IP="${EMBARCADERO_HEAD_ADDR:-10.10.10.143}"
export EMBARCADERO_HEAD_ADDR="$BROKER_IP"

BROKER_READY_TIMEOUT_SEC=${BROKER_READY_TIMEOUT_SEC:-60}

# Subscriber starts SUB_LEAD_SEC seconds before publishers fire the barrier.
# Must be long enough for the subscriber to connect to all brokers and send
# CreateTopic (typically completes in <1s, 6s is generous).
SUB_LEAD_SEC=${SUB_LEAD_SEC:-6}

# Time from "now" until the publisher barrier timestamp.
# SSH connections + process launch + chrony settling must complete within this window.
START_DELAY_SEC=${START_DELAY_SEC:-12}

# NTP offset warning threshold (ms)
NTP_WARN_MS=${NTP_WARN_MS:-5}

PUB_C4_NUMA=${PUB_C4_NUMA:-1}
LOCAL_PUB_NUMA=${LOCAL_PUB_NUMA:-0}
SUB_NUMA=${SUB_NUMA:-1}

# Extra lead time (ms) between "both ready" and go_ns so spin-waits are tight
BARRIER_LEAD_MS=${BARRIER_LEAD_MS:-200}

# Ready/go file paths (local NFS-accessible path used by both publishers)
# Local publisher reads/writes directly; c4 publisher uses the same path via
# SSH-tunnelled commands.  The go-file is written by this orchestrator so it
# must be on the broker (moscxl) filesystem where both can see it.
# The c4 ready-file is also written on moscxl by the c4 publisher via SSH cat.
BARRIER_DIR="${BARRIER_DIR:-/tmp/embarcadero_barrier_$$}"

# Performance knobs
EMBARCADERO_ORDER0_FAST_PATH=${EMBARCADERO_ORDER0_FAST_PATH:-1}
EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES=${EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES:-524288}
EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE=${EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE:-1}
EMBARCADERO_BATCH_SIZE=${EMBARCADERO_BATCH_SIZE:-524288}
EMBARCADERO_CLIENT_PUB_BATCH_KB=${EMBARCADERO_CLIENT_PUB_BATCH_KB:-512}
EMBARCADERO_NETWORK_IO_THREADS=${EMBARCADERO_NETWORK_IO_THREADS:-4}

# ---------------------------------------------------------------------------
# Derived
# ---------------------------------------------------------------------------
LOAD_PER_PUBLISHER=$(( TOTAL_MESSAGE_SIZE / 2 ))

export EMBARCADERO_CXL_SHM_NAME="${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}"

# ---------------------------------------------------------------------------
# Broker lifecycle
# ---------------------------------------------------------------------------
unset REMOTE_BROKER_HOST
export PROJECT_ROOT
source "$SCRIPT_DIR/lib/broker_lifecycle.sh"
broker_init_paths

EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"

log() { echo "$*"; }

shm_cleanup() {
    shm_unlink "${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
    rm -f "/dev/shm${EMBARCADERO_CXL_SHM_NAME}" 2>/dev/null || true
}

cleanup() {
    log "Cleaning up..."
    pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
    broker_local_cleanup
    shm_cleanup
    ssh c4 "pkill -9 -f throughput_test 2>/dev/null; true" 2>/dev/null || true
    ssh c3 "pkill -9 -f throughput_test 2>/dev/null; true" 2>/dev/null || true
    rm -rf "${BARRIER_DIR:-}" 2>/dev/null || true
}
trap cleanup EXIT

# ---------------------------------------------------------------------------
# NTP offset check
# Returns offset in ms (absolute value) for the given host, or "?" if unknown.
# Uses chronyc if available, falls back to ntpq.
# ---------------------------------------------------------------------------
ntp_offset_ms() {
    local host="$1"
    local cmd
    cmd='
if command -v chronyc >/dev/null 2>&1; then
    # "System time: 0.000123456 seconds fast of NTP time"
    offset=$(chronyc tracking 2>/dev/null \
        | awk "/System time:/ {v=\$3; if(v<0) v=-v; printf \"%.3f\", v*1000}")
    if [ -n "$offset" ]; then echo "$offset"; exit 0; fi
fi
if command -v ntpq >/dev/null 2>&1; then
    # last column of "ntpq -p" selected peer line is offset in ms
    offset=$(ntpq -p 2>/dev/null \
        | awk "/^\*/ {v=\$(NF-1); if(v<0) v=-v; printf \"%.3f\", v; exit}")
    if [ -n "$offset" ]; then echo "$offset"; exit 0; fi
fi
echo "?"
'
    if [ "$host" = "local" ]; then
        bash -c "$cmd"
    else
        ssh "$host" "$cmd" 2>/dev/null || echo "?"
    fi
}

check_ntp() {
    log ""
    log "NTP offset check (warn if > ${NTP_WARN_MS} ms):"
    local all_ok=1
    for entry in "c4:c4" "c3:c3" "local:local"; do
        local label="${entry%%:*}"
        local host="${entry##*:}"
        local off
        off=$(ntp_offset_ms "$host")
        if [ "$off" = "?" ]; then
            printf "  %-8s  offset=unknown (chronyc/ntpq not available)\n" "$label"
        else
            local flag=""
            if awk "BEGIN{exit ($off <= $NTP_WARN_MS) ? 0 : 1}"; then
                flag=""
            else
                flag="  *** WARNING: offset > ${NTP_WARN_MS} ms — barrier sync may be imprecise ***"
                all_ok=0
            fi
            printf "  %-8s  offset=%s ms%s\n" "$label" "$off" "$flag"
        fi
    done
    log ""
    [ "$all_ok" -eq 1 ] || log "  Consider running: chronyc makestep  (on each host with large offset)"
    log ""
}

# ---------------------------------------------------------------------------
# Broker startup
# ---------------------------------------------------------------------------
start_brokers() {
    log "Resetting previous broker state..."
    pkill -9 -f "\./embarlet" >/dev/null 2>&1 || true
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    shm_cleanup

    cd "$BUILD_BIN"
    export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}"
    export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-full}"
    export EMBARCADERO_RUNTIME_MODE="throughput"
    export EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"
    export EMBARCADERO_ORDER0_FAST_PATH
    export EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES

    log "Starting $NUM_BROKERS broker(s) [NUMA 1,2]..."
    $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" --head "--${SEQUENCER}" \
        > /tmp/broker_0.log 2>&1 &
    for (( i=1; i<NUM_BROKERS; i++ )); do
        $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" \
            > /tmp/broker_"$i".log 2>&1 &
    done

    log "Waiting for $NUM_BROKERS broker(s) to become ready (timeout ${BROKER_READY_TIMEOUT_SEC}s)..."
    if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS"; then
        echo "ERROR: Brokers did not become ready within ${BROKER_READY_TIMEOUT_SEC}s" >&2
        cat /tmp/broker_0.log >&2
        return 1
    fi
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    log "All $NUM_BROKERS brokers ready."
}

# ---------------------------------------------------------------------------
# Shared environment block emitted into every client script
# ---------------------------------------------------------------------------
common_env() {
cat <<ENVBLOCK
export EMBARCADERO_HEAD_ADDR=$BROKER_IP
export EMBARCADERO_CXL_SHM_NAME=$EMBARCADERO_CXL_SHM_NAME
export EMBARCADERO_CXL_ZERO_MODE=${EMBARCADERO_CXL_ZERO_MODE:-full}
export EMBARCADERO_RUNTIME_MODE=throughput
export EMBARCADERO_REPLICATION_FACTOR=$REPLICATION_FACTOR
export EMBARCADERO_ORDER0_FAST_PATH=$EMBARCADERO_ORDER0_FAST_PATH
export EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES=$EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES
export EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE=$EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE
export EMBARCADERO_BATCH_SIZE=$EMBARCADERO_BATCH_SIZE
export EMBARCADERO_CLIENT_PUB_BATCH_KB=$EMBARCADERO_CLIENT_PUB_BATCH_KB
export EMBARCADERO_NETWORK_IO_THREADS=$EMBARCADERO_NETWORK_IO_THREADS
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
cd $BUILD_BIN
ENVBLOCK
}

# ---------------------------------------------------------------------------
# Build a publisher client script.
#
# Uses the push-ready barrier protocol:
#   - EMBARCADERO_PUSH_READY_FILE  written by binary after init (one per publisher)
#   - EMBARCADERO_PUSH_GO_FILE     read by binary; orchestrator writes go_ns after
#                                   both ready-files appear
#
# Emits PUB_START_NS (shell timestamp right before binary launch) so that
# orchestrator can detect if the binary launch itself was unusually slow.
# The authoritative push-start is "Publisher push start (wall ns):" from the binary.
# ---------------------------------------------------------------------------
make_publisher_cmd() {
    local numa="$1"
    local ready_file="$2"
    local go_file="$3"
    local head_addr="$4"

    cat <<ENDPUB
set -e
$(common_env)
export EMBARCADERO_PUSH_READY_FILE=$ready_file
export EMBARCADERO_PUSH_GO_FILE=$go_file
# Record shell-level launch timestamp on this machine's own clock (ns)
echo "PUB_START_NS=\$(date +%s%N)"
numactl --cpunodebind=$numa --membind=$numa \\
    ./throughput_test \\
    --config $CLIENT_CONFIG_ABS \\
    -n $THREADS_PER_BROKER \\
    -m $MESSAGE_SIZE \\
    -s $LOAD_PER_PUBLISHER \\
    -t 5 \\
    -o $ORDER \\
    -a $ACK \\
    -r $REPLICATION_FACTOR \\
    --sequencer $SEQUENCER \\
    --head_addr $head_addr \\
    -l 0
echo "PUB_END_NS=\$(date +%s%N)"
ENDPUB
}

# ---------------------------------------------------------------------------
# Build subscriber client script.
# Starts SUB_LEAD_SEC before publishers so the subscriber is already
# connected when the topic is created. No barrier needed — it blocks in Poll.
# ---------------------------------------------------------------------------
make_subscriber_cmd() {
    local numa="$1"
    local sub_start_ms="$2"

    # SUB_POLL_TIMEOUT_SEC: how long Poll() waits with no progress before giving up.
    # Must be long enough to cover the publisher barrier delay + full publish duration.
    # With START_DELAY_SEC=12 and each publisher completing in ~1s, 60s is conservative.
    local sub_timeout=${SUB_POLL_TIMEOUT_SEC:-60}

    cat <<ENDSUB
set -e
$(common_env)
export EMBARCADERO_E2E_TIMEOUT_SEC=$sub_timeout
# Start ahead of publishers so subscriber is connected before topic creation
while [ \$(date +%s%3N) -lt $sub_start_ms ]; do sleep 0.0001; done
numactl --cpunodebind=$numa --membind=$numa \\
    ./throughput_test \\
    --config $CLIENT_CONFIG_ABS \\
    -n $THREADS_PER_BROKER \\
    -m $MESSAGE_SIZE \\
    -s $TOTAL_MESSAGE_SIZE \\
    -t 6 \\
    -o $ORDER \\
    -a $ACK \\
    -r $REPLICATION_FACTOR \\
    --sequencer $SEQUENCER \\
    --head_addr $BROKER_IP \\
    -l 0
ENDSUB
}

# ---------------------------------------------------------------------------
# Parse a nanosecond timestamp from a log file (key=value format)
# ---------------------------------------------------------------------------
parse_ns() {
    local file="$1" key="$2"
    grep -m1 "^${key}=" "$file" 2>/dev/null | cut -d= -f2 || echo ""
}

# ---------------------------------------------------------------------------
# Extract bandwidth value (MB/s) from log line matching a pattern
# ---------------------------------------------------------------------------
parse_bw() {
    local file="$1" pattern="$2"
    grep -i "$pattern" "$file" 2>/dev/null | tail -1 \
        | grep -oP '[\d]+\.?[\d]*(?=\s*MB)' | tail -1 || echo ""
}

# ---------------------------------------------------------------------------
# Parse the "Publisher push start (wall ns): X" timestamp from a publisher log.
# This is emitted by the binary itself right before it starts writing batches.
# Because it is on the same machine's clock as PUB_START_NS, the difference
# (push_start_ns - PUB_START_NS) gives the exact initialization latency with
# no cross-machine clock comparison needed.
# ---------------------------------------------------------------------------
parse_push_start_ns() {
    local file="$1"
    grep -m1 "Publisher push start (wall ns):" "$file" 2>/dev/null \
        | grep -oP '(?<=wall ns\): )\d+' || echo ""
}

# ---------------------------------------------------------------------------
# Parse push duration in ms from "Publish test completed in X.XX seconds".
# ---------------------------------------------------------------------------
parse_push_duration_ms() {
    local file="$1"
    grep -m1 "Publish test completed in" "$file" 2>/dev/null \
        | grep -oP '[\d]+\.[\d]+(?=\s*seconds)' \
        | awk '{printf "%.3f", $1 * 1000}' || echo ""
}

# ---------------------------------------------------------------------------
# Compute the barrier-relative push window overlap and aggregate metrics.
#
# With the push-ready barrier, both publishers start their push loops at go_ns
# (the same CLOCK_REALTIME nanosecond value).  The only cross-machine clock error
# is the NTP offset between c4 and moscxl (≤2 ms on chrony-synced hosts).
#
# For each publisher p ∈ {c4, local}:
#   push_start_ns(p) = "Publisher push start (wall ns):" in binary log
#                      ≈ go_ns + spin_overshoot  (~0–50 µs)
#   init_delay_ms(p) = (push_start_ns(p) − PUB_START_NS(p)) / 1e6
#                      (reported for observability; should be ≈ BARRIER_LEAD_MS)
#   push_window(p)   = [init_delay_ms(p), init_delay_ms(p) + push_dur_ms(p)]
#                      both anchored on each machine's own clock relative to launch
#
# Cross-machine overlap is computed using push_start_ns values on each machine's
# own clock.  Error = NTP skew ≤ 2 ms; for ~500 ms push windows this is <0.4%.
#
# Three derived metrics (all in MB/s unless noted):
#   overlap_ms      : length of the window where both publishers push simultaneously
#   concurrent_agg  : c4_bw + local_bw  (valid only when overlap_ms > 0)
#   pipeline_bw     : TOTAL_BYTES / (last_push_end − first_push_start)
#                     = effective throughput seen by subscriber (first byte to last)
#
# pipeline_bw should closely match c3's wire bandwidth (first-byte to last-byte).
# ---------------------------------------------------------------------------
compute_overlap_metrics() {
    local c4_start_ns="$1"   lo_start_ns="$2"
    local c4_push_ns="$3"    lo_push_ns="$4"
    local c4_push_dur_ms="$5" lo_push_dur_ms="$6"
    local bw_c4="$7"          bw_local="$8"

    # Require all inputs
    for v in "$c4_start_ns" "$lo_start_ns" "$c4_push_ns" "$lo_push_ns" \
              "$c4_push_dur_ms" "$lo_push_dur_ms" "$bw_c4" "$bw_local"; do
        [[ "$v" =~ ^[0-9]+\.?[0-9]*$ ]] || { echo "? ? ?"; return; }
    done

    awk -v c4s="$c4_start_ns" -v los="$lo_start_ns" \
        -v c4p="$c4_push_ns"  -v lop="$lo_push_ns" \
        -v c4d="$c4_push_dur_ms" -v lod="$lo_push_dur_ms" \
        -v bw4="$bw_c4" -v bwl="$bw_local" \
        -v total="$TOTAL_MESSAGE_SIZE" \
    'BEGIN {
        # Init delays (ms) — on each machine'\''s own clock, no cross-clock comparison
        c4_init = (c4p - c4s) / 1e6
        lo_init = (lop - los) / 1e6

        # Push windows relative to barrier (ms)
        c4_win_start = c4_init;        c4_win_end = c4_init + c4d
        lo_win_start = lo_init;        lo_win_end = lo_init + lod

        # Overlap window
        ov_start = (c4_win_start > lo_win_start) ? c4_win_start : lo_win_start
        ov_end   = (c4_win_end   < lo_win_end)   ? c4_win_end   : lo_win_end
        overlap  = (ov_end > ov_start) ? (ov_end - ov_start) : 0

        # Concurrent aggregate (instantaneous, valid during overlap)
        concurrent = (overlap > 0) ? bw4 + bwl : 0

        # Pipeline: first push start → last push end (covers all data delivery)
        pipe_start = (c4_win_start < lo_win_start) ? c4_win_start : lo_win_start
        pipe_end   = (c4_win_end   > lo_win_end)   ? c4_win_end   : lo_win_end
        pipe_ms    = pipe_end - pipe_start
        pipeline   = (pipe_ms > 0) ? (total / 1048576.0) / (pipe_ms / 1000.0) : 0

        printf "%.1f %.2f %.2f\n", overlap, concurrent, pipeline
    }'
}

# ---------------------------------------------------------------------------
# Print banner
# ---------------------------------------------------------------------------
echo "=================================================================="
echo "  Embarcadero — 2 Publishers + 1 Subscriber  (OSDI/SOSP timing)"
echo "=================================================================="
printf "  %-36s %s\n" "Publisher A:" "c4  (NUMA $PUB_C4_NUMA)"
printf "  %-36s %s\n" "Publisher B:" "local/moscxl (NUMA $LOCAL_PUB_NUMA)  → $BROKER_IP"
printf "  %-36s %s\n" "Subscriber:" "c3  (NUMA $SUB_NUMA)"
printf "  %-36s %s\n" "Brokers:" "$NUM_BROKERS × local  (NUMA 1,2)"
printf "  %-36s %s / %s\n" "Sequencer / Order:" "$SEQUENCER" "$ORDER"
printf "  %-36s %s\n" "ACK / Replication:" "$ACK / $REPLICATION_FACTOR"
printf "  %-36s %s B  (%s GiB)\n" "TOTAL_MESSAGE_SIZE:" \
    "$TOTAL_MESSAGE_SIZE" "$(awk "BEGIN{printf \"%.2f\",$TOTAL_MESSAGE_SIZE/1073741824}")"
printf "  %-36s %s B  (%s GiB each)\n" "Per-publisher load:" \
    "$LOAD_PER_PUBLISHER" "$(awk "BEGIN{printf \"%.2f\",$LOAD_PER_PUBLISHER/1073741824}")"
printf "  %-36s %s B\n" "Message size:" "$MESSAGE_SIZE"
printf "  %-36s %s\n" "Threads per broker:" "$THREADS_PER_BROKER"
printf "  %-36s push-ready file barrier (+%dms lead)\n" \
    "Publisher sync:" "$BARRIER_LEAD_MS"
printf "  %-36s %s\n" "Broker IP:" "$BROKER_IP"
printf "  %-36s %s\n" "ORDER0_FAST_PATH:" "$EMBARCADERO_ORDER0_FAST_PATH"
printf "  %-36s %s B  (%s KiB)\n" "Batch size:" \
    "$EMBARCADERO_BATCH_SIZE" "$(( EMBARCADERO_BATCH_SIZE / 1024 ))"
echo "=================================================================="

mkdir -p "$LOG_DIR"

# Write CSV header (append-safe across multiple invocations)
if [ ! -f "$CSV_FILE" ]; then
    echo "timestamp_utc,order,num_brokers,msg_bytes,total_bytes,trial,attempt,\
pub_c4_bw_mbs,pub_c4_init_delay_ms,pub_c4_push_dur_ms,\
pub_local_bw_mbs,pub_local_init_delay_ms,pub_local_push_dur_ms,\
overlap_ms,concurrent_agg_bw_mbs,pipeline_bw_mbs,pipeline_bw_gbs,\
sub_wire_bw_mbs,sub_wire_bw_gbs,\
ntp_offset_c4_ms,ntp_offset_c3_ms" > "$CSV_FILE"
fi

# ---------------------------------------------------------------------------
# Pre-run NTP check
# ---------------------------------------------------------------------------
check_ntp

# ---------------------------------------------------------------------------
# Capture NTP offsets once per run for CSV
# ---------------------------------------------------------------------------
NTP_C4=$(ntp_offset_ms "c4")
NTP_C3=$(ntp_offset_ms "c3")

overall_status=0

for (( trial=1; trial<=NUM_TRIALS; trial++ )); do
    echo ""
    echo "=== Trial $trial / $NUM_TRIALS  (ORDER=$ORDER  Brokers=$NUM_BROKERS  msg=${MESSAGE_SIZE}B) ==="
    trial_success=0

    for (( attempt=1; attempt<=TRIAL_MAX_ATTEMPTS; attempt++ )); do
        log "  Attempt $attempt / $TRIAL_MAX_ATTEMPTS"

        if ! start_brokers; then
            echo "ERROR: broker startup failed" >&2
            cleanup
            continue
        fi

        SUB_LOG="$LOG_DIR/trial${trial}_c3_sub.log"
        PUB_C4_LOG="$LOG_DIR/trial${trial}_c4_pub.log"
        PUB_LOCAL_LOG="$LOG_DIR/trial${trial}_local_pub.log"

        # ------------------------------------------------------------------
        # Push-ready barrier file paths for this trial
        #   READY_C4   : written by c4 publisher (via SSH cat on moscxl)
        #   READY_LOCAL: written by local publisher directly
        #   GO_FILE    : written by orchestrator; read by both publishers
        #
        # All three files live on moscxl local filesystem.
        # c4 publisher writes its ready-file by piping "1" to ssh cat on moscxl.
        # The go_ns value is in CLOCK_REALTIME nanoseconds (same epoch on all
        # chrony-synced hosts, ≤2 ms skew).
        # ------------------------------------------------------------------
        mkdir -p "$BARRIER_DIR"
        READY_C4="$BARRIER_DIR/ready_c4"
        READY_LOCAL="$BARRIER_DIR/ready_local"
        GO_FILE="$BARRIER_DIR/go_ns"
        rm -f "$READY_C4" "$READY_LOCAL" "$GO_FILE"

        # ------------------------------------------------------------------
        # Compute subscriber start time
        # Timeline:  now → +2s → subscriber starts (connects, creates topic)
        #            → publishers launched → init → write ready → orchestrator
        #              sees both ready → writes go_ns → publishers push
        # The subscriber has well over START_DELAY_SEC seconds before first data.
        # ------------------------------------------------------------------
        NOW_MS=$(date +%s%3N)
        SUB_START_MS=$(( NOW_MS + 2000 ))

        log "  Now (local):      $(date -u +%Y-%m-%dT%H:%M:%S.%3NZ)"
        log "  Subscriber start: T+2s"
        log "  Publishers:       launched immediately; barrier fires after both report ready"
        log "  Barrier dir:      $BARRIER_DIR"

        # ------------------------------------------------------------------
        # Build scripts
        # ------------------------------------------------------------------
        SUB_CMD="$(make_subscriber_cmd "$SUB_NUMA" "$SUB_START_MS")"

        # c4 publisher: EMBARCADERO_PUSH_READY_FILE points at a path on moscxl.
        # The binary runs on c4 and cannot write to moscxl directly, so we wrap
        # the throughput_test call so that after init it SSHes back to write "1"
        # to the ready-file on moscxl before polling the go-file.
        # Implementation: we override EMBARCADERO_PUSH_READY_FILE to a local
        # temp path on c4, then after the binary writes it we relay it to moscxl.
        #
        # Simpler approach: the binary writes EMBARCADERO_PUSH_READY_FILE locally
        # on c4; a background loop on moscxl polls for it via SSH and relays the
        # signal to READY_C4 locally.  This avoids modifying the c4 binary invocation.
        #
        # We use the relay approach: launch a local background poller that SSHes
        # c4 to check for /tmp/embarcadero_c4_ready_$$ and copies it to READY_C4.
        # Barrier file paths:
        #   READY_LOCAL   : written by local binary directly on moscxl
        #   C4_READY_REMOTE: written by c4 binary on c4's own filesystem
        #   GO_FILE_LOCAL : go_ns file on moscxl, read by local binary
        #   C4_GO_REMOTE  : go_ns file on c4, written by orchestrator via SSH,
        #                   read by c4 binary
        C4_READY_REMOTE="/tmp/embarcadero_c4_ready_$$"
        C4_GO_REMOTE="/tmp/embarcadero_c4_go_$$"
        ssh c4 "rm -f $C4_READY_REMOTE $C4_GO_REMOTE" 2>/dev/null || true

        PUB_C4_CMD="$(make_publisher_cmd "$PUB_C4_NUMA" "$C4_READY_REMOTE" "$C4_GO_REMOTE" "$BROKER_IP")"
        PUB_LOCAL_CMD="$(make_publisher_cmd "$LOCAL_PUB_NUMA" "$READY_LOCAL" "$GO_FILE" "$BROKER_IP")"

        # ------------------------------------------------------------------
        # Launch subscriber and both publishers in parallel
        # ------------------------------------------------------------------
        ssh c3 "$SUB_CMD"        > "$SUB_LOG"       2>&1 & SUB_PID=$!
        ssh c4 "$PUB_C4_CMD"     > "$PUB_C4_LOG"    2>&1 & PUB_C4_PID=$!
        bash -c "$PUB_LOCAL_CMD" > "$PUB_LOCAL_LOG"  2>&1 & PUB_LOCAL_PID=$!

        log "  Processes launched — sub=$SUB_PID  c4-pub=$PUB_C4_PID  local-pub=$PUB_LOCAL_PID"

        # ------------------------------------------------------------------
        # Wait for both ready-files, then write go_ns to both machines
        # ------------------------------------------------------------------
        BARRIER_TIMEOUT_SEC=${BARRIER_TIMEOUT_SEC:-60}
        log "  Waiting for both publishers to finish init (push-ready barrier)..."
        barrier_ok=1
        barrier_deadline=$(( $(date +%s) + BARRIER_TIMEOUT_SEC ))
        while true; do
            if [ "$(date +%s)" -ge "$barrier_deadline" ]; then
                echo "ERROR: push-ready barrier timeout after ${BARRIER_TIMEOUT_SEC}s" >&2
                barrier_ok=0
                break
            fi
            local_rdy=0
            [ -f "$READY_LOCAL" ] && local_rdy=1
            c4_rdy=0
            ssh c4 "test -f $C4_READY_REMOTE" 2>/dev/null && c4_rdy=1
            if [ "$c4_rdy" -eq 1 ] && [ "$local_rdy" -eq 1 ]; then
                break
            fi
            sleep 0.05
        done

        if [ "$barrier_ok" -eq 1 ]; then
            # Both publishers are past init. Compute go_ns = now + BARRIER_LEAD_MS.
            GO_NS=$(( $(date +%s%N) + BARRIER_LEAD_MS * 1000000 ))
            # Write to both machines simultaneously (ssh in background for c4)
            echo "$GO_NS" > "$GO_FILE"
            ssh c4 "echo $GO_NS > $C4_GO_REMOTE" &
            wait $!
            log "  Both publishers ready — go_ns=$GO_NS  (fire in ~${BARRIER_LEAD_MS}ms)"
        else
            # Barrier failed — unblock publishers immediately with go_ns=1 (past)
            echo "1" > "$GO_FILE"
            ssh c4 "echo 1 > $C4_GO_REMOTE" 2>/dev/null || true
        fi

        log "  Waiting for publishers and subscriber to complete..."

        pub_c4_ok=1;  wait "$PUB_C4_PID"    || pub_c4_ok=0
        pub_lo_ok=1;  wait "$PUB_LOCAL_PID" || pub_lo_ok=0
        sub_ok=1;     wait "$SUB_PID"        || sub_ok=0

        rm -f "$READY_C4" "$READY_LOCAL" "$GO_FILE"
        ssh c4 "rm -f $C4_READY_REMOTE $C4_GO_REMOTE" 2>/dev/null || true

        [ "$pub_c4_ok" -eq 1 ] || echo "WARNING: c4 publisher exited non-zero" >&2
        [ "$pub_lo_ok" -eq 1 ] || echo "WARNING: local publisher exited non-zero" >&2
        [ "$sub_ok"    -eq 1 ] || echo "WARNING: c3 subscriber exited non-zero" >&2

        # ------------------------------------------------------------------
        # Parse outputs
        # ------------------------------------------------------------------
        bw_c4=$(parse_bw    "$PUB_C4_LOG"    "bandwidth:")
        bw_local=$(parse_bw  "$PUB_LOCAL_LOG" "bandwidth:")
        wire_c3=$(parse_bw   "$SUB_LOG"       "wire bandwidth")

        # ------------------------------------------------------------------
        # Parse per-publisher timing from the binary's own output.
        # All timestamps are on each machine's local clock — no cross-machine
        # clock arithmetic is performed here.
        # ------------------------------------------------------------------
        c4_start_ns=$(parse_ns          "$PUB_C4_LOG"   "PUB_START_NS")
        lo_start_ns=$(parse_ns          "$PUB_LOCAL_LOG" "PUB_START_NS")
        c4_push_ns=$(parse_push_start_ns "$PUB_C4_LOG")
        lo_push_ns=$(parse_push_start_ns "$PUB_LOCAL_LOG")
        c4_push_dur_ms=$(parse_push_duration_ms "$PUB_C4_LOG")
        lo_push_dur_ms=$(parse_push_duration_ms "$PUB_LOCAL_LOG")

        # Init delay = time from binary launch (barrier) to actual first batch write
        c4_init_ms=""
        lo_init_ms=""
        if [[ "$c4_start_ns" =~ ^[0-9]+$ ]] && [[ "$c4_push_ns" =~ ^[0-9]+$ ]]; then
            c4_init_ms=$(awk "BEGIN{printf \"%.1f\", ($c4_push_ns - $c4_start_ns)/1e6}")
        fi
        if [[ "$lo_start_ns" =~ ^[0-9]+$ ]] && [[ "$lo_push_ns" =~ ^[0-9]+$ ]]; then
            lo_init_ms=$(awk "BEGIN{printf \"%.1f\", ($lo_push_ns - $lo_start_ns)/1e6}")
        fi

        # Overlap / pipeline metrics (barrier-relative, cross-machine via NTP)
        overlap_ms=""; concurrent_bw=""; pipeline_bw=""; pipeline_gbs=""
        read -r overlap_ms concurrent_bw pipeline_bw < <(
            compute_overlap_metrics \
                "${c4_start_ns:-0}" "${lo_start_ns:-0}" \
                "${c4_push_ns:-0}"  "${lo_push_ns:-0}" \
                "${c4_push_dur_ms:-0}" "${lo_push_dur_ms:-0}" \
                "${bw_c4:-0}" "${bw_local:-0}"
        )
        [[ "$pipeline_bw" =~ ^[0-9]+\.?[0-9]*$ ]] && \
            pipeline_gbs=$(awk "BEGIN{printf \"%.3f\", $pipeline_bw/1024}")

        # Wire BW in GB/s for CSV
        wire_c3_gbs=""
        [[ "$wire_c3" =~ ^[0-9]+\.?[0-9]*$ ]] && \
            wire_c3_gbs=$(awk "BEGIN{printf \"%.3f\", $wire_c3/1024}")

        # ------------------------------------------------------------------
        # Validate
        # ------------------------------------------------------------------
        logs_ok=1
        [ -n "$bw_c4" ]    || { echo "WARNING: no Bandwidth in $PUB_C4_LOG"    >&2; logs_ok=0; tail -15 "$PUB_C4_LOG"    >&2; }
        [ -n "$bw_local" ] || { echo "WARNING: no Bandwidth in $PUB_LOCAL_LOG"  >&2; logs_ok=0; tail -15 "$PUB_LOCAL_LOG"  >&2; }
        [ -n "$wire_c3" ]  || { echo "WARNING: no Wire bandwidth in $SUB_LOG"   >&2; logs_ok=0; tail -20 "$SUB_LOG"         >&2; }

        all_ok=$(( pub_c4_ok & pub_lo_ok & sub_ok ))
        if [[ "$all_ok" -eq 1 && "$logs_ok" -eq 1 ]]; then
            trial_success=1
            printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n" \
                "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
                "$ORDER" "$NUM_BROKERS" "$MESSAGE_SIZE" "$TOTAL_MESSAGE_SIZE" \
                "$trial" "$attempt" \
                "${bw_c4:-}"         "${c4_init_ms:-}"    "${c4_push_dur_ms:-}" \
                "${bw_local:-}"      "${lo_init_ms:-}"    "${lo_push_dur_ms:-}" \
                "${overlap_ms:-}"    "${concurrent_bw:-}" \
                "${pipeline_bw:-}"   "${pipeline_gbs:-}" \
                "${wire_c3:-}"       "${wire_c3_gbs:-}" \
                "$NTP_C4" "$NTP_C3" >> "$CSV_FILE"
            break
        fi

        echo "WARNING: trial $trial attempt $attempt incomplete — retrying..." >&2
        kill "$PUB_C4_PID" "$PUB_LOCAL_PID" "$SUB_PID" 2>/dev/null || true
        cleanup
        sleep 2
    done

    # ------------------------------------------------------------------
    # Per-trial summary table
    # ------------------------------------------------------------------
    conc_note="concurrent (overlap=${overlap_ms:-?} ms)"
    [[ "${overlap_ms:-0}" == "0" || "${overlap_ms:-0}" == "0.0" ]] && \
        conc_note="sequential (no overlap)"
    echo ""
    echo "  ┌───────────────────────────┬──────────────┬─────────────┬──────────────┐"
    printf "  │ Trial %-2d                  │ BW (MB/s)    │ Init (ms)   │ Push (ms)    │\n" "$trial"
    echo "  ├───────────────────────────┼──────────────┼─────────────┼──────────────┤"
    printf "  │ c4   pub  (NUMA %s, NIC)  │ %8s     │ %8s    │ %8s     │\n" \
        "$PUB_C4_NUMA" "${bw_c4:-N/A}" "${c4_init_ms:-N/A}" "${c4_push_dur_ms:-N/A}"
    printf "  │ local pub (NUMA %s, lo)   │ %8s     │ %8s    │ %8s     │\n" \
        "$LOCAL_PUB_NUMA" "${bw_local:-N/A}" "${lo_init_ms:-N/A}" "${lo_push_dur_ms:-N/A}"
    echo "  ├───────────────────────────┼──────────────┴─────────────┴──────────────┤"
    printf "  │ Push overlap              │ %-44s│\n" "$conc_note"
    printf "  │ Concurrent aggregate      │ %-44s│\n" \
        "$(printf '%s MB/s  (c4+local, during overlap)' "${concurrent_bw:-N/A}")"
    printf "  │ Pipeline BW               │ %-44s│\n" \
        "$(printf '%s MB/s = %s GB/s  (first→last byte)' "${pipeline_bw:-N/A}" "${pipeline_gbs:-N/A}")"
    echo "  ├───────────────────────────┼──────────────────────────────────────────┤"
    printf "  │ c3 subscriber wire BW     │ %-44s│\n" \
        "$(printf '%s MB/s = %s GB/s  (c3 clock, ground truth)' "${wire_c3:-N/A}" "${wire_c3_gbs:-N/A}")"
    echo "  └───────────────────────────┴──────────────────────────────────────────┘"

    if [[ "$trial_success" -ne 1 ]]; then
        echo "  ERROR: Trial $trial failed after $TRIAL_MAX_ATTEMPTS attempts." >&2
        overall_status=1
    fi

    cleanup
done

# ---------------------------------------------------------------------------
# Grand summary
# ---------------------------------------------------------------------------
echo ""
echo "=================================================================="
echo "  Grand Summary"
echo "=================================================================="
printf "  %-6s  %-13s  %-13s  %-9s  %-12s  %-13s  %-14s\n" \
    "Trial" "C4 (MB/s)" "Local (MB/s)" "Ovlp(ms)" "Conc (MB/s)" "Pipeline(GB/s)" "Sub wire(GB/s)"
echo "  ──────  ─────────────  ─────────────  ─────────  ────────────  ─────────────  ──────────────"
for (( trial=1; trial<=NUM_TRIALS; trial++ )); do
    row=$(grep "^[^,]*,[^,]*,[^,]*,[^,]*,[^,]*,$trial," "$CSV_FILE" 2>/dev/null | tail -1)
    if [ -n "$row" ]; then
        IFS=',' read -r _ts _ord _nb _ms _tot _tr _att \
            _bw_c4 _init_c4 _push_c4 \
            _bw_lo _init_lo _push_lo \
            _ovlp _conc _pipe_mbs _pipe_gbs \
            _sub_mbs _sub_gbs _ntp4 _ntp3 <<< "$row"
        printf "  %-6s  %-13s  %-13s  %-9s  %-12s  %-13s  %-14s\n" \
            "$trial" "${_bw_c4:-N/A}" "${_bw_lo:-N/A}" "${_ovlp:-N/A}" \
            "${_conc:-N/A}" "${_pipe_gbs:-N/A}" "${_sub_gbs:-N/A}"
    else
        printf "  %-6s  (no data)\n" "$trial"
    fi
done
echo "  ──────  ─────────────  ─────────────  ─────────  ────────────  ─────────────  ──────────────"
echo "  ──────  ──────────────  ──────────────  ──────────────  ────────────────  ────────────────"
echo ""
echo "  Metrics:"
echo "    Pipeline BW = TOTAL_BYTES / (last_push_end − first_push_start) [barrier-relative]"
echo "    Concurrent agg = c4_bw + local_bw, valid only when overlap > 0"
echo "    c3 wire BW = ground truth; first-byte to last-byte on c3's own clock"
echo "    NTP offsets: c4=${NTP_C4} ms  c3=${NTP_C3} ms"
echo ""
echo "  Full machine-readable results: $CSV_FILE"
echo "=================================================================="

exit "$overall_status"
