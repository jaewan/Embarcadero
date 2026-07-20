#!/bin/bash
# scripts/run_failure_suite.sh
#
# E4 Failure suite: the thesis instrument.
# Measures Embarcadero's per-session recovery vs baseline stall behavior.
#
# E4a — broker kill, M independent sessions
#   - Kill one of 4 brokers mid-run with M=4 concurrent publisher sessions
#   - Measure: per-session stall-duration CDF, reconvergence transient on survivors
#   - Expected: Embarcadero ≈δ (single-digit ms); baselines stall globally
#
# E4b — sequencer failover MTTR
#   - Kill the sequencer process; measure time to resumption including
#     session-table recovery + PBR rescan
#   - Expected: <200 ms from detection to first new ACK
#
# E4c — SIGSTOP false positive (lease false alarm)
#   - SIGSTOP a broker (simulate slow/paused, not crashed)
#   - Verify dedup: retransmitted batches are not delivered twice
#   - Expected: exactly-once delivery under false alarm
#
# E4f — baselines under failure
#   - Same broker kill as E4a, applied to Scalog and Corfu
#   - Measure: global throughput hole duration (seal/cut stall vs reconfig stall)
#   - Expected: Scalog stalls for cut cadence; Corfu stalls for reconfiguration MTTR
#
# Usage:
#   bash scripts/run_failure_suite.sh            # full suite (E4a, E4b, E4f)
#   SUITE=E4a bash scripts/run_failure_suite.sh  # single experiment
#   SMOKE=1 bash scripts/run_failure_suite.sh    # quick sanity (1 trial, 8 GiB)
#
# Each cell launches its own cluster via run_multiclient.sh with timed
# broker-kill injection (BROKER_KILL_AFTER_SEC) and runs the per-session
# stall analyzer. Knobs: E4_CLIENT_HOSTS, E4_CLIENT_NUMAS,
# E4_TARGET_MBPS_PER_SESSION, E4_KILL_AFTER_SEC, E4_TS_INTERVAL_MS,
# E4_TOTAL_BYTES, E4A_KILL_BROKER_ID, E4F_KILL_BROKER_ID.
#
# Prerequisites:
#   - Run from moscxl (broker node); clients reachable over SSH (c4/c3/c1)
#   - BROKER_IP=10.10.10.10 (CXL server dataplane IP)
#   - No other run holding /tmp/embarcadero_run_multiclient.lock

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

BROKER_IP="${BROKER_IP:-10.10.10.10}"
CLIENT_HOST="${CLIENT_HOST:-c1}"
NUM_BROKERS="${NUM_BROKERS:-4}"
NUM_SESSIONS="${NUM_SESSIONS:-4}"
SUITE="${SUITE:-all}"

if [[ "${SMOKE:-0}" == "1" ]]; then
    NUM_TRIALS=1
    echo "[failure_suite] SMOKE mode"
else
    NUM_TRIALS=3
    echo "[failure_suite] FULL mode"
fi
# Per-cell data volume is set inside run_kill_cell (E4_TOTAL_BYTES; SMOKE-aware)

RUN_TAG="${RUN_TAG:-$(date -u +%Y%m%dT%H%M%SZ)_failure}"
OUT_BASE="$PROJECT_ROOT/data/failure_suite/$RUN_TAG"
mkdir -p "$OUT_BASE/logs"
SUMMARY_LOG="$OUT_BASE/sweep_summary.log"
touch "$SUMMARY_LOG"

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
log() { local msg="[$(stamp)] $*"; echo "$msg"; echo "$msg" >> "$SUMMARY_LOG"; }

log "===== Failure suite START — $RUN_TAG ====="
log "Broker IP: $BROKER_IP  Client: $CLIENT_HOST  Sessions: $NUM_SESSIONS"

# ---------------------------------------------------------------------------
# Shared kill-cell runner
#
# Sessions are per client PROCESS (one client_id + session_epoch per Publisher
# instance — publisher.cc IsOrder5SessionMode / SendSessionOpenOnSocket), NOT
# per publisher thread. M independent sessions therefore = NUM_CLIENTS=M
# processes, each with its own per-session timeseries CSV on the shared
# ORIGIN_MS axis. Load is paced (--target_mbps --steady_rate) so the send
# phase is still active at T_kill and the post-kill stall window is visible.
#
# Kill: broker $kill_id gets SIGKILL at push-start + E4_KILL_AFTER_SEC via
# run_multiclient.sh injection (waits for Publisher::Init to finish first);
# kill wall/rel timestamps land in multiclient_logs/trial<N>_broker_kill.csv
# on the timeseries axis.
#
# Usage: run_kill_cell <label> <kill_broker_id> [extra ENV=VAL ...]
# Extra env pairs are appended last, so they can override the defaults
# (e.g. SEQUENCER=SCALOG ORDER=1 for baseline cells).
# ---------------------------------------------------------------------------
run_kill_cell() {
    local label="$1" kill_id="$2"
    shift 2
    local cell_log="$OUT_BASE/logs/${label}.log"
    local cell_dir="$OUT_BASE/$label"

    local hosts="${E4_CLIENT_HOSTS:-c4,c4,c3,c3}"
    local numas="${E4_CLIENT_NUMAS:-1,1,1,1}"
    local per_session_mbps="${E4_TARGET_MBPS_PER_SESSION:-500}"
    local kill_after="${E4_KILL_AFTER_SEC:-3}"
    local ts_interval_ms="${E4_TS_INTERVAL_MS:-10}"
    # Paced run duration = total/(M*pace). Default 16 GiB → 4 GiB/session →
    # ~8.6 s at 500 MB/s: kill at push-go+3 s leaves a >5 s post-kill window.
    # SMOKE uses 12 GiB (~6.4 s/session) so kill at +3 s still leaves ~3 s post.
    local total_bytes="${E4_TOTAL_BYTES:-$(( 16 * 1024 * 1024 * 1024 ))}"
    if [[ "${SMOKE:-0}" == "1" ]]; then
        total_bytes="${E4_TOTAL_BYTES:-$(( 12 * 1024 * 1024 * 1024 ))}"
    fi

    log "[$label] M=$NUM_SESSIONS sessions on $hosts, kill broker $kill_id at T+${kill_after}s, pace ${per_session_mbps}MB/s/session"
    mkdir -p "$cell_dir"
    local marker="$cell_dir/.start_marker"
    touch "$marker"

    local rc=0
    {
        echo "=== [$label] broker kill with M=$NUM_SESSIONS sessions ==="
        echo "hosts=$hosts numas=$numas pace=${per_session_mbps}MB/s/session kill=broker${kill_id}@T+${kill_after}s ts=${ts_interval_ms}ms extra_env=$*"
        echo "=== $(stamp): Starting cluster + $NUM_SESSIONS publisher sessions ==="

        # Four 8-GiB segments do not fit after layout-v2 metadata in the
        # configured 64-GiB CXL extent. Six GiB leaves four admitted segments
        # and comfortably holds this paced failure workload.
        env \
        NUM_CLIENTS="$NUM_SESSIONS" \
        CLIENT_HOSTS_CSV="$hosts" \
        CLIENT_NUMAS_CSV="$numas" \
        NUM_BROKERS="$NUM_BROKERS" \
        NUM_TRIALS="$NUM_TRIALS" \
        TRIAL_MAX_ATTEMPTS=1 \
        TOTAL_MESSAGE_SIZE="$total_bytes" \
        MESSAGE_SIZE=1024 \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=0 \
        TEST_TYPE=5 \
        EMBARCADERO_CXL_ZERO_MODE=metadata \
        EMBARCADERO_CXL_MAP_POPULATE=0 \
        EMBARCADERO_SEGMENT_SIZE="${EMBARCADERO_SEGMENT_SIZE:-6442450944}" \
        EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS="$ts_interval_ms" \
        EMBARCADERO_THROUGHPUT_TIMESERIES_ON_SENT=1 \
        CLIENT_EXTRA_ARGS="--target_mbps $per_session_mbps --steady_rate" \
        BROKER_KILL_AFTER_SEC="$kill_after" \
        BROKER_KILL_ID="$kill_id" \
        BROKER_KILL_SIGNAL=KILL \
        START_DELAY_SEC="${START_DELAY_SEC:-8}" \
        MIN_REMOTE_START_DELAY_SEC="${MIN_REMOTE_START_DELAY_SEC:-8}" \
        EMBARCADERO_QUEUE_DRAIN_TIMEOUT_SEC="${EMBARCADERO_QUEUE_DRAIN_TIMEOUT_SEC:-30}" \
        EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-30}" \
        EMBARCADERO_SESSION_LEASE_MS="${EMBARCADERO_SESSION_LEASE_MS:-180000}" \
        EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS="${EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS:-180000}" \
        EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-0}" \
        "$@" \
        bash "$SCRIPT_DIR/run_multiclient.sh"
        echo "=== $(stamp): [$label] run complete ==="
    } > "$cell_log" 2>&1 || rc=$?

    # Preserve this run's per-session timeseries + kill records (only files
    # written after the marker — multiclient_logs is shared across runs),
    # plus the LAST trial's broker logs (for broker-side recovery markers).
    find "$PROJECT_ROOT/multiclient_logs" -maxdepth 1 -type f -newer "$marker" \
        -exec cp -f {} "$cell_dir/" \; 2>/dev/null || true
    local b
    for (( b=0; b<NUM_BROKERS; b++ )); do
        cp -f "/tmp/broker_${b}.log" "$cell_dir/last_trial_broker${b}.log" 2>/dev/null || true
    done

    # Per-session stall CDF (non-zero if any session lacks a usable baseline/recovery).
    local analysis_rc=0
    local -a analysis_args=()
    if [[ "$label" == "e4a_broker_kill" ]]; then
        analysis_args=(--affected-session c40 --control-session c41)
    elif [[ "$label" == "e4a_session_gap" ]]; then
        # Publisher ACK samples are per-broker aggregates and are retained only
        # as offered-load corroboration. The separate sequencer trace below is
        # the authoritative per-session commit-isolation contract.
        analysis_args=(--gap-event-session c40)
    fi
    if python3 "$SCRIPT_DIR/analyze_e4a_stall.py" "$cell_dir" \
        --output "$cell_dir/stall_summary.csv" "${analysis_args[@]}" \
        >> "$cell_log" 2>&1; then
        log "[$label] stall analysis written to $cell_dir/stall_summary.csv"
    else
        analysis_rc=$?
        log "[$label] WARNING: stall analysis failed (rc=$analysis_rc) — inspect $cell_log"
    fi
    if [[ "$label" == "e4a_session_gap" ]]; then
        if python3 "$SCRIPT_DIR/analyze_e4a_commit_isolation.py" "$cell_dir" \
            >> "$cell_log" 2>&1; then
            log "[$label] sequencer commit-isolation contract passed"
        else
            analysis_rc=$?
            log "[$label] WARNING: sequencer commit-isolation contract failed (rc=$analysis_rc)"
        fi
    fi
    if [[ "$rc" -eq 0 && "$analysis_rc" -ne 0 ]]; then
        rc=$analysis_rc
    fi
    log "[$label] done (rc=$rc) — see $cell_log"
    return "$rc"
}

# ---------------------------------------------------------------------------
# E4a: kill a non-head broker with M concurrent sessions (Embarcadero ORDER=5)
# Headline: per-session stall CDF — sessions homed on survivors should see
# ~zero stall; only sessions rerouting off the dead broker pay the repair.
# ---------------------------------------------------------------------------
run_e4a() {
    local kill_id="${E4A_KILL_BROKER_ID:-1}"
    if [[ "$NUM_SESSIONS" -ne 2 ]]; then
        echo "ERROR: E4a isolation contract requires NUM_SESSIONS=2 (affected + control)." >&2
        return 1
    fi
    if [[ "$kill_id" -ne 1 || "$NUM_BROKERS" -ne 4 ]]; then
        echo "ERROR: E4a explicit routing contract currently requires four brokers and kill_id=1." >&2
        return 1
    fi
    # Both sessions fully stripe across the same four brokers and share the
    # same sequencer. Hold only session 0's batch 64 for three seconds; later
    # batches make its missing prefix observable while session 1 remains live.
    run_kill_cell "e4a_session_gap" "$kill_id" \
        BROKER_KILL_AFTER_SEC=0 \
        CLIENT_ORDER5_GAP_DELAYS_MS_PIPE="3000|0" \
        CLIENT_ORDER5_GAP_BATCH_SEQS_PIPE="64|0" \
        EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1
}

# ---------------------------------------------------------------------------
# E4b: sequencer failover MTTR — kill broker 0 (head). For ORDER=5 the
# sequencer IS broker 0's Sequencer5 thread, so this forces re-election +
# PBR rescan. Client-visible MTTR = stall_ms on surviving sessions (from the
# shared analyzer); broker-side markers are extracted for corroboration.
# ---------------------------------------------------------------------------
run_e4b() {
    run_kill_cell "e4b_head_kill" 0

    # Broker-side recovery markers from the surviving brokers' logs
    # (last trial only — earlier trials' broker logs are overwritten).
    local markers="$OUT_BASE/e4b_head_kill/recovery_markers.txt"
    {
        echo "# Broker-side recovery markers (last trial). Compare against"
        echo "# kill_wall_ms in trial<N>_broker_kill.csv (glog timestamps are local time)."
        grep -Hn "Starting Sequencer5\|sequencer.*elect\|SESSION_OPEN_ACK\|SessionFenced\|head.*takeover\|becomes head\|new head" \
            "$OUT_BASE/e4b_head_kill/"last_trial_broker*.log 2>/dev/null || echo "(no markers found)"
    } > "$markers" 2>&1 || true
    log "E4b broker-side markers: $markers"
}

# ---------------------------------------------------------------------------
# E4f: baselines under the SAME fault (kill broker 1) — Scalog and Corfu.
# Expected: Scalog stalls globally at seal/cut cadence; Corfu publishers
# stall at token/reconfiguration (the client deliberately does NOT reroute
# for CORFU — publisher.cc RendezvousBroker refusal — so unrecovered
# sessions in the analyzer output ARE the measurement).
# Both cells use local sequencers on the broker node (matching the E2
# baseline configuration in run_overnight_eval.sh).
# ---------------------------------------------------------------------------
run_e4f() {
    run_kill_cell "e4f_scalog_broker_kill" "${E4F_KILL_BROKER_ID:-1}" \
        SEQUENCER=SCALOG ORDER=1 SKIP_REMOTE_SCALOG_SEQUENCER=1 \
        EMBARCADERO_SCALOG_SEQ_IP="$BROKER_IP"

    run_kill_cell "e4f_corfu_broker_kill" "${E4F_KILL_BROKER_ID:-1}" \
        SEQUENCER=CORFU ORDER=2 \
        EMBARCADERO_CORFU_SEQ_IP="$BROKER_IP"
}

# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------
suite_rc=0
case "$SUITE" in
    E4a|e4a) run_e4a || suite_rc=$? ;;
    E4b|e4b) run_e4b || suite_rc=$? ;;
    E4f|e4f) run_e4f || suite_rc=$? ;;
    all)
        run_e4a || suite_rc=$?
        run_e4b || suite_rc=$?
        run_e4f || suite_rc=$?
        ;;
    *) echo "Unknown SUITE=$SUITE. Use E4a, E4b, E4f, or all." >&2; exit 1 ;;
esac

log ""
log "===== Failure suite COMPLETE — $RUN_TAG ====="
log "Results: $OUT_BASE"
log ""
log "E4a/E4b/E4f are automated via BROKER_KILL_AFTER_SEC injection in"
log "run_multiclient.sh + analyze_e4a_stall.py. E4c (SIGSTOP false positive)"
log "remains future work. Per-cell results: $OUT_BASE/<cell>/stall_summary.csv"
exit "$suite_rc"
