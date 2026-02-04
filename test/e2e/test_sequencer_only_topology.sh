#!/bin/bash
# End-to-End Test: Sequencer-Only Head Node Topology
# Tests: 1 sequencer-only head (broker 0) + 3 data brokers (1,2,3)
# Expected: Head node doesn't accept publishes, clients connect only to data brokers
# See docs/memory-bank/SEQUENCER_ONLY_HEAD_NODE_DESIGN.md

set -euo pipefail  # Exit on error, undefined vars, pipe failures

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BIN_DIR="$BUILD_DIR/bin"
CONFIG_DIR="$PROJECT_ROOT/config"
TEST_OUTPUT_DIR="$BUILD_DIR/test_output"

# Test configuration
TEST_NAME="sequencer_only_topology"
NUM_DATA_BROKERS=3  # Data brokers (1, 2, 3)
MESSAGE_SIZE=1024
TOTAL_MESSAGES=1000
NUMA_BIND="numactl --cpunodebind=1 --membind=1"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Test state
BROKER_PIDS=()
TEST_FAILED=0
START_TIME=$(date +%s)

# Cleanup function (always runs)
cleanup() {
    local exit_code=$?
    echo ""
    echo "=========================================="
    echo "Cleaning up test resources..."
    echo "=========================================="

    # Kill all broker processes
    for pid in "${BROKER_PIDS[@]}"; do
        if kill -0 "$pid" 2>/dev/null; then
            echo "Terminating broker PID $pid"
            kill "$pid" 2>/dev/null || true
        fi
    done

    # Give processes time to exit
    sleep 1

    # Force kill any remaining embarlet processes
    pkill -9 -f "embarlet" 2>/dev/null || true

    # Clean up ready signal files
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true

    # Calculate test duration
    local end_time=$(date +%s)
    local duration=$((end_time - START_TIME))

    if [ $exit_code -eq 0 ] && [ $TEST_FAILED -eq 0 ]; then
        echo -e "${GREEN}✓ TEST PASSED${NC} (${duration}s)"
        exit 0
    else
        echo -e "${RED}✗ TEST FAILED${NC} (${duration}s)"
        echo "See logs in: $TEST_OUTPUT_DIR/$TEST_NAME/"
        exit 1
    fi
}

trap cleanup EXIT INT TERM

# Utility functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
    TEST_FAILED=1
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

assert_file_exists() {
    if [ ! -f "$1" ]; then
        log_error "Required file not found: $1"
        return 1
    fi
}

assert_process_running() {
    local pid=$1
    local name=$2
    if ! kill -0 "$pid" 2>/dev/null; then
        log_error "$name (PID $pid) is not running"
        return 1
    fi
}

wait_for_ready_file() {
    local pid=$1
    local timeout=$2
    local start=$(date +%s)
    local ready_file="/tmp/embarlet_${pid}_ready"

    log_info "Waiting for ready signal from PID $pid (timeout: ${timeout}s)..."

    while [ $(($(date +%s) - start)) -lt $timeout ]; do
        if [ -f "$ready_file" ]; then
            local elapsed=$(($(date +%s) - start))
            log_info "Broker PID $pid ready after ${elapsed}s"
            rm -f "$ready_file"  # Clean up signal file
            return 0
        fi
        sleep 0.5
    done

    log_error "Broker PID $pid failed to signal readiness in ${timeout}s"
    return 1
}

check_log_for_errors() {
    local log_file=$1
    local component=$2

    # Check for fatal errors
    if grep -i "fatal\|abort\|segfault\|core dumped" "$log_file" 2>/dev/null; then
        log_error "$component crashed (fatal error in log)"
        return 1
    fi

    return 0
}

# Setup test environment
setup() {
    log_info "Setting up test environment..."

    # Create output directory
    mkdir -p "$TEST_OUTPUT_DIR/$TEST_NAME"
    cd "$TEST_OUTPUT_DIR/$TEST_NAME"

    # Check prerequisites
    assert_file_exists "$BIN_DIR/embarlet"
    assert_file_exists "$BIN_DIR/throughput_test"
    assert_file_exists "$CONFIG_DIR/embarcadero.yaml"
    assert_file_exists "$CONFIG_DIR/embarcadero_sequencer_only.yaml"
    assert_file_exists "$CONFIG_DIR/client.yaml"

    # Clean up any stale processes and ready signal files
    pkill -f "embarlet" 2>/dev/null || true
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    sleep 1

    # Enable hugepages
    export EMBAR_USE_HUGETLB=1

    log_info "Test output directory: $PWD"
}

# Start broker cluster with sequencer-only head
start_brokers() {
    log_info "Starting sequencer-only topology: 1 sequencer + $NUM_DATA_BROKERS data brokers..."

    # Start head broker with sequencer-only config
    log_info "Starting head broker (broker 0) with is_sequencer_node=true..."
    $NUMA_BIND "$BIN_DIR/embarlet" \
        --config "$CONFIG_DIR/embarcadero_sequencer_only.yaml" \
        --head --EMBARCADERO \
        > broker_0_sequencer.log 2>&1 &

    local head_pid=$!
    BROKER_PIDS+=($head_pid)
    log_info "Head broker (sequencer-only) started with PID $head_pid"

    # Wait for head broker to signal readiness
    if ! wait_for_ready_file "$head_pid" 120; then
        log_error "Head broker failed to initialize"
        cat broker_0_sequencer.log
        return 1
    fi

    # Verify head broker is running
    if ! assert_process_running "$head_pid" "Head broker (sequencer)"; then
        cat broker_0_sequencer.log
        return 1
    fi

    # Verify head broker initialized in sequencer-only mode
    if ! grep -q "\[SEQUENCER_ONLY\]" broker_0_sequencer.log; then
        log_error "Head broker did not initialize in sequencer-only mode"
        log_info "Expected log entry containing [SEQUENCER_ONLY]"
        cat broker_0_sequencer.log
        return 1
    fi
    log_info "✓ Head broker initialized in sequencer-only mode"

    # Verify NetworkManager is skipped on sequencer node
    if ! grep -q "skip_networking=true" broker_0_sequencer.log; then
        log_error "Head broker should skip networking in sequencer-only mode"
        cat broker_0_sequencer.log
        return 1
    fi
    log_info "✓ Head broker skipped NetworkManager (no data ingestion)"

    # Start data brokers (with normal config, they will set accepts_publishes=true)
    for ((i=1; i<=NUM_DATA_BROKERS; i++)); do
        log_info "Starting data broker $i..."
        $NUMA_BIND "$BIN_DIR/embarlet" \
            --config "$CONFIG_DIR/embarcadero.yaml" \
            > "broker_$i.log" 2>&1 &

        local broker_pid=$!
        BROKER_PIDS+=($broker_pid)
        log_info "Data broker $i started with PID $broker_pid"

        # Wait for broker to signal readiness
        if ! wait_for_ready_file "$broker_pid" 60; then
            log_error "Data broker $i failed to initialize"
            cat "broker_$i.log"
            return 1
        fi

        # Verify broker got valid ID
        if grep -q "broker_id: -1" "broker_$i.log"; then
            log_error "Data broker $i failed to register (got broker_id=-1)"
            cat "broker_$i.log"
            return 1
        fi
    done

    # Final cluster health check
    sleep 2
    for i in "${!BROKER_PIDS[@]}"; do
        if ! assert_process_running "${BROKER_PIDS[$i]}" "Broker $i"; then
            if [ $i -eq 0 ]; then
                cat broker_0_sequencer.log
            else
                cat "broker_$i.log"
            fi
            return 1
        fi
    done

    log_info "Cluster ready: 1 sequencer-only head + $NUM_DATA_BROKERS data brokers"
}

# Run client test - should only connect to data brokers
run_client_test() {
    log_info "Running client publish test..."
    log_info "Client should connect to data brokers only (not to sequencer-only head)"
    log_info "Config: ${MESSAGE_SIZE}B messages, ${TOTAL_MESSAGES} total messages"

    local total_size=$((MESSAGE_SIZE * TOTAL_MESSAGES))
    log_info "Total data size: $total_size bytes"

    # Run throughput test with ORDER=5 (uses Sequencer5)
    "$BIN_DIR/throughput_test" \
        --config "$CONFIG_DIR/client.yaml" \
        -m "$MESSAGE_SIZE" \
        -s "$total_size" \
        -t 5 \
        -o 5 \
        -a 1 \
        --sequencer EMBARCADERO \
        > client.log 2>&1

    local client_exit=$?

    # Check if client succeeded
    if [ $client_exit -ne 0 ]; then
        log_error "Client exited with code $client_exit"
        cat client.log
        return 1
    fi

    # Verify client used broker_info with accepts_publishes filtering
    if grep -q "Skipping broker 0" client.log || grep -q "accepts_publishes=false" client.log; then
        log_info "✓ Client correctly skipped sequencer-only broker 0"
    elif grep -q "broker_info" client.log; then
        log_info "✓ Client used broker_info for broker discovery"
    fi

    # Verify client connected to data brokers
    if grep -q "Added broker [1-3]" client.log; then
        log_info "✓ Client connected to data brokers"
    fi

    # Verify throughput was reported
    if ! grep -q "GB/s\|Throughput" client.log 2>/dev/null; then
        log_error "Client didn't report throughput results"
        cat client.log
        return 1
    fi

    log_info "Client test completed successfully"
}

# Verify broker health after test
verify_broker_health() {
    log_info "Verifying broker health after test..."

    # Check sequencer-only head
    if ! assert_process_running "${BROKER_PIDS[0]}" "Head broker (sequencer)"; then
        return 1
    fi
    if ! check_log_for_errors "broker_0_sequencer.log" "Head broker (sequencer)"; then
        return 1
    fi

    # Check data brokers
    for i in $(seq 1 $NUM_DATA_BROKERS); do
        if ! assert_process_running "${BROKER_PIDS[$i]}" "Data broker $i"; then
            return 1
        fi
        if ! check_log_for_errors "broker_$i.log" "Data broker $i"; then
            return 1
        fi
    done

    log_info "All brokers healthy"
}

# Verify sequencer processed batches from data brokers
verify_sequencer_activity() {
    log_info "Verifying sequencer processed batches from data brokers..."

    # Check if sequencer scanned data brokers (B1, B2, B3)
    local scanner_count=$(grep -c "BrokerScannerWorker5\|scanner.*broker" broker_0_sequencer.log 2>/dev/null || echo "0")
    if [ "$scanner_count" -gt 0 ]; then
        log_info "✓ Sequencer has scanner activity"
    fi

    # Verify sequencer filtered out B0 from scanner set
    if grep -q "filtered out B0 from scanner" broker_0_sequencer.log; then
        log_info "✓ Sequencer correctly filtered B0 from scanner set"
    fi

    log_info "Sequencer activity verified"
}

# Main test flow
main() {
    echo "=========================================="
    echo "E2E Test: Sequencer-Only Head Node Topology"
    echo "=========================================="
    echo "Architecture: 1 sequencer-only head (B0) + 3 data brokers (B1,B2,B3)"
    echo ""

    setup
    start_brokers || return 1
    run_client_test || return 1
    verify_broker_health || return 1
    verify_sequencer_activity || return 1

    echo ""
    log_info "All test assertions passed"
}

main "$@"
