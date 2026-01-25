#!/bin/bash
# End-to-End Test: Basic Publish Flow
# Tests: Broker startup → Client publish → Message delivery
# Expected: 4 brokers start successfully, client publishes, no errors

set -euo pipefail  # Exit on error, undefined vars, pipe failures

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BIN_DIR="$BUILD_DIR/bin"
CONFIG_DIR="$PROJECT_ROOT/config"
TEST_OUTPUT_DIR="$BUILD_DIR/test_output"

# Test configuration
TEST_NAME="basic_publish"
NUM_BROKERS=4
MESSAGE_SIZE=128
TOTAL_MESSAGES=1000  # Small for quick test
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

wait_for_log_message() {
    local log_file=$1
    local pattern=$2
    local timeout=$3
    local start=$(date +%s)

    while [ $(($(date +%s) - start)) -lt $timeout ]; do
        if grep -q "$pattern" "$log_file" 2>/dev/null; then
            return 0
        fi
        sleep 0.5
    done

    log_error "Timeout waiting for '$pattern' in $log_file"
    return 1
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

    # Check for connection failures
    if grep -i "failed to connect\|connection refused\|connection timed out" "$log_file" 2>/dev/null; then
        log_error "$component had connection errors"
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
    assert_file_exists "$CONFIG_DIR/client.yaml"

    # Clean up any stale processes and ready signal files
    pkill -f "embarlet" 2>/dev/null || true
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    sleep 1

    # Enable hugepages
    export EMBAR_USE_HUGETLB=1

    log_info "Test output directory: $PWD"
}

# Start broker cluster
start_brokers() {
    log_info "Starting $NUM_BROKERS broker cluster..."

    # Start head broker
    log_info "Starting head broker (broker 0)..."
    $NUMA_BIND "$BIN_DIR/embarlet" \
        --config "$CONFIG_DIR/embarcadero.yaml" \
        --head --EMBARCADERO \
        > broker_0.log 2>&1 &

    local head_pid=$!
    BROKER_PIDS+=($head_pid)
    log_info "Head broker started with PID $head_pid"

    # Wait for head broker to signal readiness (uses file-based polling)
    # Timeout is generous to handle large CXL allocations (up to 120s for 64GB)
    if ! wait_for_ready_file "$head_pid" 120; then
        log_error "Head broker failed to initialize"
        cat broker_0.log
        return 1
    fi

    # Verify head broker is still running
    if ! assert_process_running "$head_pid" "Head broker"; then
        cat broker_0.log
        return 1
    fi

    # Start follower brokers
    for ((i=1; i<NUM_BROKERS; i++)); do
        log_info "Starting broker $i..."
        $NUMA_BIND "$BIN_DIR/embarlet" \
            --config "$CONFIG_DIR/embarcadero.yaml" \
            > "broker_$i.log" 2>&1 &

        local broker_pid=$!
        BROKER_PIDS+=($broker_pid)
        log_info "Broker $i started with PID $broker_pid"

        # Wait for broker to signal readiness
        # Follower brokers should be much faster (no CXL allocation)
        if ! wait_for_ready_file "$broker_pid" 30; then
            log_error "Broker $i failed to initialize"
            cat "broker_$i.log"
            return 1
        fi

        # Verify broker got valid ID (not -1)
        if grep -q "broker_id: -1" "broker_$i.log"; then
            log_error "Broker $i failed to register (got broker_id=-1)"
            cat "broker_$i.log"
            return 1
        fi
    done

    # Final cluster health check
    sleep 2
    for i in "${!BROKER_PIDS[@]}"; do
        if ! assert_process_running "${BROKER_PIDS[$i]}" "Broker $i"; then
            cat "broker_$i.log"
            return 1
        fi
    done

    log_info "All $NUM_BROKERS brokers running successfully"
}

# Run client test
run_client_test() {
    log_info "Running client publish test..."
    log_info "Config: ${MESSAGE_SIZE}B messages, ${TOTAL_MESSAGES} total messages"

    # Calculate total message size (MESSAGE_SIZE * TOTAL_MESSAGES)
    local total_size=$((MESSAGE_SIZE * TOTAL_MESSAGES))
    log_info "Total data size: $total_size bytes"

    # Run throughput test
    "$BIN_DIR/throughput_test" \
        --config "$CONFIG_DIR/client.yaml" \
        -m "$MESSAGE_SIZE" \
        -s "$total_size" \
        -t 5 \
        -o 0 \
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

    # Verify client didn't report errors
    if grep -i "error\|failed\|timeout" client.log 2>/dev/null; then
        log_error "Client reported errors in log"
        cat client.log
        return 1
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

    for i in $(seq 0 $((NUM_BROKERS-1))); do
        # Check if broker is still running
        if ! assert_process_running "${BROKER_PIDS[$i]}" "Broker $i"; then
            return 1
        fi

        # Check for errors in broker logs
        if ! check_log_for_errors "broker_$i.log" "Broker $i"; then
            return 1
        fi
    done

    log_info "All brokers healthy"
}

# Main test flow
main() {
    echo "=========================================="
    echo "E2E Test: Basic Publish Flow"
    echo "=========================================="

    setup
    start_brokers || return 1
    run_client_test || return 1
    verify_broker_health || return 1

    echo ""
    log_info "All test assertions passed"
}

main "$@"
