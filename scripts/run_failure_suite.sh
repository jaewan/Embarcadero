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
#   bash scripts/run_failure_suite.sh            # full suite
#   SUITE=E4a bash scripts/run_failure_suite.sh  # single experiment
#   SMOKE=1 bash scripts/run_failure_suite.sh    # quick sanity (1 trial, 2 GiB)
#
# Prerequisites:
#   - Brokers running on moscxl (broker node), publishers on remote clients
#   - BROKER_IP=10.10.10.10 set to the CXL server dataplane IP
#   - 4 brokers + sequencer started via run_multiclient.sh or manually

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
    TOTAL_BYTES=$((2 * 1024 * 1024 * 1024))
    NUM_TRIALS=1
    echo "[failure_suite] SMOKE mode"
else
    TOTAL_BYTES=$((8 * 1024 * 1024 * 1024))
    NUM_TRIALS=3
    echo "[failure_suite] FULL mode"
fi

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
# E4a: broker kill with M concurrent sessions
# ---------------------------------------------------------------------------
run_e4a() {
    log "E4a: broker kill — M=$NUM_SESSIONS concurrent sessions, 4 brokers"
    local cell_log="$OUT_BASE/logs/e4a_broker_kill.log"

    # Start M publishers on the client, each with their own session
    # Kill broker 1 after 2s, measure per-session stall
    # Implementation note: throughput_test supports multiple threads (THREADS_PER_BROKER
    # controls publisher parallelism; each thread opens its own session).
    # We launch with THREADS_PER_BROKER=NUM_SESSIONS to get M concurrent sessions.
    {
        echo "=== E4a: broker kill with M=$NUM_SESSIONS sessions ==="
        echo "=== $(stamp): Starting publishers ==="

        # Launch 4-broker cluster (reuse run_multiclient harness)
        NUM_CLIENTS=1 \
        CLIENT_HOSTS_CSV="$CLIENT_HOST" \
        CLIENT_NUMAS_CSV=1 \
        NUM_BROKERS="$NUM_BROKERS" \
        NUM_TRIALS="$NUM_TRIALS" \
        TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
        MESSAGE_SIZE=1024 \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        THREADS_PER_BROKER="$NUM_SESSIONS" \
        SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=0 \
        TEST_TYPE=5 \
        EMBARCADERO_CXL_ZERO_MODE=metadata \
        EMBARCADERO_CXL_MAP_POPULATE=0 \
        BROKER_KILL_AFTER_SEC=3 \
        BROKER_KILL_ID=1 \
        OUT_BASE="$OUT_BASE/e4a" \
        BENCHMARK_TAG="$RUN_TAG/e4a" \
        bash "$SCRIPT_DIR/run_multiclient.sh"
        echo "=== $(stamp): E4a complete ==="
    } > "$cell_log" 2>&1
    log "E4a done — see $cell_log"
    # Note: run_multiclient.sh does not currently support BROKER_KILL_AFTER_SEC.
    # TODO: implement broker-kill injection inside run_multiclient.sh:
    #   After START_DELAY_SEC barrier, sleep BROKER_KILL_AFTER_SEC then
    #   kill launched_broker_pids[BROKER_KILL_ID]. Record kill timestamp.
    #   The per-session ACK stall is recoverable from the publisher timeseries CSV.
    log "E4a TODO: BROKER_KILL_AFTER_SEC injection not yet implemented in run_multiclient.sh"
    log "E4a MANUAL: Kill broker PID manually during a run and record timeseries CSVs."
}

# ---------------------------------------------------------------------------
# E4b: sequencer failover MTTR
# ---------------------------------------------------------------------------
run_e4b() {
    log "E4b: sequencer failover MTTR"
    local cell_log="$OUT_BASE/logs/e4b_sequencer_failover.log"
    {
        echo "=== E4b: sequencer failover ==="
        echo "=== $(stamp): Starting cluster ==="
        # 1. Start 4-broker cluster normally
        # 2. After 3s, kill the sequencer process (it's co-located with broker 0 in the embarlet)
        #    For ORDER=5, the sequencer IS broker 0's Sequencer5 thread.
        #    Killing broker 0 forces a new election.
        # 3. Measure time from kill to first new ACK on surviving sessions.
        #
        # NOTE: In the current architecture the sequencer is a thread inside broker 0's
        # embarlet process, not a separate binary. Killing broker 0 = killing the sequencer.
        # Recovery: surviving brokers detect via heartbeat (~3s with gRPC heartbeat,
        # or ~1ms with CXL lease if implemented). New sequencer elected by membership
        # service, scans PBRs from committed frontier, resumes.
        #
        # Measure: time from kill signal to first successfully ACKed batch on a
        # publisher that was mid-flight when broker 0 was killed.
        #
        # TODO: implement a scripted sequencer-kill + MTTR measurement harness.
        # Until then, run manually and record timestamps from broker logs:
        #   t_kill = kill -TERM <broker_0_pid>
        #   t_resume = first "[SESSION_OPEN_ACK]" in broker_1.log after t_kill
        #   MTTR = t_resume - t_kill
        echo "=== E4b MANUAL PROCEDURE ==="
        echo "1. Start 4-broker cluster: bash scripts/run_multiclient.sh ..."
        echo "2. Wait for steady state (at least 5s of throughput)"
        echo "3. Kill broker 0 (the sequencer): kill -TERM \$(pgrep -x embarlet | head -1)"
        echo "4. Record kill timestamp from broker_0.log: grep 'Shutdown requested'"
        echo "5. Record recovery from broker_1.log: grep 'Starting Sequencer5'"
        echo "6. MTTR = time between kill and first new ACK on publisher"
        echo "Expected: <200 ms for session-table read + PBR rescan"
        echo "Caveat: gRPC heartbeat detection adds 3s timeout; CXL lease reduces to <1ms once implemented"
    } > "$cell_log" 2>&1
    log "E4b procedure documented — see $cell_log"
    log "E4b TODO: implement automated MTTR measurement script"
}

# ---------------------------------------------------------------------------
# E4f: baselines under failure
# ---------------------------------------------------------------------------
run_e4f() {
    log "E4f: baselines under failure — Scalog seal/cut stall, Corfu reconfig"
    local cell_log="$OUT_BASE/logs/e4f_baselines_failure.log"
    {
        echo "=== E4f: baseline failure behavior ==="
        # Scalog: kill the global sequencer (scalog_global_sequencer process)
        #   Expected: local cuts stop delivering; per-broker ordering halts at cut cadence
        #   Measure: throughput timeline from all publishers, hole duration
        # Corfu: kill the corfu_global_sequencer
        #   Expected: all publishers stall at token acquisition; no forward progress until
        #   reconfiguration completes (Corfu uses epoch-based reconfiguration over gRPC)
        #   Measure: throughput hole duration from kill to first ACK after reconfiguration
        #
        # TODO: implement automated kill + timeline collection for each baseline.
        # The baseline sequencers run as separate processes (corfu_global_sequencer,
        # scalog_global_sequencer from broker_lifecycle.sh), so killing them is safe.
        echo "=== E4f MANUAL PROCEDURE ==="
        echo "Scalog:"
        echo "  1. Run: SEQUENCER=SCALOG bash scripts/run_multiclient.sh ..."
        echo "  2. Kill: pkill -x scalog_global_sequencer"
        echo "  3. Measure throughput hole in timeseries CSV"
        echo "Corfu:"
        echo "  1. Run: SEQUENCER=CORFU bash scripts/run_multiclient.sh ..."
        echo "  2. Kill: pkill -x corfu_global_sequencer"
        echo "  3. Measure throughput hole (expect full stall during reconfiguration)"
    } > "$cell_log" 2>&1
    log "E4f procedure documented — see $cell_log"
    log "E4f TODO: automated sequencer-kill + timeline collection"
}

# ---------------------------------------------------------------------------
# Dispatch
# ---------------------------------------------------------------------------
case "$SUITE" in
    E4a|e4a) run_e4a ;;
    E4b|e4b) run_e4b ;;
    E4f|e4f) run_e4f ;;
    all)
        run_e4a
        run_e4b
        run_e4f
        ;;
    *) echo "Unknown SUITE=$SUITE. Use E4a, E4b, E4f, or all." >&2; exit 1 ;;
esac

log ""
log "===== Failure suite COMPLETE — $RUN_TAG ====="
log "Results: $OUT_BASE"
log ""
log "NOTE: E4a and E4b require manual broker-kill injection."
log "The automated kill injection is not yet implemented in run_multiclient.sh."
log "Run E4a/E4b manually following the procedures in the cell logs."
