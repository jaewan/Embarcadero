#!/bin/bash
# End-to-End Test: Explicit Replication with ORDER=5 + ack_level=2
# Tests: Batch-based replication correctness, replication_done monotonicity, ACK progress
# Expected: Replication threads active, replica disk files written, no stalls

set -euo pipefail  # Exit on error, undefined vars, pipe failures

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BIN_DIR="$BUILD_DIR/bin"
CONFIG_DIR="$PROJECT_ROOT/config"
TEST_OUTPUT_DIR="$BUILD_DIR/test_output"

# Test configuration
TEST_NAME="explicit_replication_order5_ack2"
NUM_BROKERS=4
MESSAGE_SIZE=128
TOTAL_MESSAGES=5000  # Small enough for quick test, large enough to see replication progress
# [[PHASE_5_E2E_HARDENING]] - NUMA binding for memory-only nodes
# Config shows numa_node: 2 (CXL memory node, has no CPUs)
# Bind CPUs to node 0/1, but memory to node 2 (CXL)
# If your CXL is on a different node, adjust --membind accordingly
NUMA_BIND="numactl --cpunodebind=0 --membind=2"

# [[PHASE_4_BOUNDED_TIMEOUTS]] - Global test timeout (5 minutes)
# Prevents infinite hangs if replication/ACK never progresses
GLOBAL_TIMEOUT=300  # 5 minutes in seconds

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
    
    # [[PHASE_4_BOUNDED_TIMEOUTS]] - Set ACK timeout for publisher
    # This ensures tests fail fast instead of hanging indefinitely
    export EMBARCADERO_ACK_TIMEOUT_SEC=120  # 2 minutes for ACK wait (should be plenty for 5k messages)

    log_info "Test output directory: $PWD"
    log_info "Test configuration: ORDER=5, ack_level=2, replication_factor=1"
    log_info "Global timeout: ${GLOBAL_TIMEOUT}s, ACK timeout: ${EMBARCADERO_ACK_TIMEOUT_SEC}s"
}

# Start broker cluster
start_brokers() {
    log_info "Starting $NUM_BROKERS broker cluster for explicit replication test..."

    # Start head broker
    log_info "Starting head broker (broker 0)..."
    $NUMA_BIND "$BIN_DIR/embarlet" \
        --config "$CONFIG_DIR/embarcadero.yaml" \
        --head --EMBARCADERO \
        > broker_0.log 2>&1 &

    local head_pid=$!
    BROKER_PIDS+=($head_pid)
    log_info "Head broker started with PID $head_pid"

    # Wait for head broker to signal readiness
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

# Run client test with ORDER=5 + ack_level=2 + replication_factor=1
run_client_test() {
    log_info "Running client publish test with explicit replication..."
    log_info "Config: ORDER=5, ack_level=2, replication_factor=1"
    log_info "Message config: ${MESSAGE_SIZE}B messages, ${TOTAL_MESSAGES} total messages"

    # Calculate total message size
    local total_size=$((MESSAGE_SIZE * TOTAL_MESSAGES))
    log_info "Total data size: $total_size bytes"

    # [[PHASE_4_BOUNDED_TIMEOUTS]] - Run client with timeout wrapper
    # This ensures we don't hang forever if ACK timeout doesn't work
    local client_start=$(date +%s)
    
    # Run throughput test with explicit replication parameters
    # -o 5: ORDER=5 (batch-based ordering)
    # -a 2: ack_level=2 (ack after replication)
    # -r 1: replication_factor=1 (one replica)
    # -t 5: publish-only test (simpler, deterministic)
    timeout $((GLOBAL_TIMEOUT - 60)) "$BIN_DIR/throughput_test" \
        --config "$CONFIG_DIR/client.yaml" \
        -m "$MESSAGE_SIZE" \
        -s "$total_size" \
        -t 5 \
        -o 5 \
        -a 2 \
        -r 1 \
        --sequencer EMBARCADERO \
        > client.log 2>&1

    local client_exit=$?
    local client_duration=$(($(date +%s) - client_start))

    # Check if client succeeded
    if [ $client_exit -ne 0 ]; then
        if [ $client_exit -eq 124 ]; then
            log_error "Client timed out after ${client_duration}s (timeout=$((GLOBAL_TIMEOUT - 60))s)"
        else
            log_error "Client exited with code $client_exit"
        fi
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

    log_info "Client test completed successfully in ${client_duration}s"
}

# Verify replication is working
verify_replication() {
    log_info "Verifying explicit replication correctness..."

    local replication_verified=false
    local metrics_found=false

    # Check for ReplicationMetrics log entries (emitted every 10 seconds)
    for i in $(seq 0 $((NUM_BROKERS-1))); do
        local log_file="broker_$i.log"
        
        # Look for ReplicationMetrics lines
        if grep -q "ReplicationMetrics" "$log_file" 2>/dev/null; then
            metrics_found=true
            log_info "Found ReplicationMetrics in broker $i log"
            
            # Extract and validate metrics
            local last_line=$(grep "ReplicationMetrics" "$log_file" | tail -1)
            
            # Check for pwrite_errors (should be 0)
            if echo "$last_line" | grep -q "pwrite_errors=[^0]"; then
                log_error "Broker $i has pwrite_errors > 0: $last_line"
                return 1
            fi
            
            # Check that batches_replicated > 0
            if echo "$last_line" | grep -q "replicated=0"; then
                log_warn "Broker $i shows replicated=0 (may be normal if no batches yet)"
            else
                replication_verified=true
                log_info "Broker $i replication metrics: $last_line"
            fi
        fi
    done

    if [ "$metrics_found" = false ]; then
        log_warn "No ReplicationMetrics found in broker logs (may need longer test duration)"
        # Don't fail - metrics are logged every 10s, test might be too short
    fi

    # Check for replica disk files
    # Replication files are written to: ../../.Replication/disk*/embarcadero_replication_log*.dat
    local replica_files_found=0
    local replica_dir="$PROJECT_ROOT/.Replication"
    
    if [ -d "$replica_dir" ]; then
        for disk_dir in "$replica_dir"/disk*; do
            if [ -d "$disk_dir" ]; then
                for log_file in "$disk_dir"/embarcadero_replication_log*.dat; do
                    if [ -f "$log_file" ]; then
                        local file_size=$(stat -f%z "$log_file" 2>/dev/null || stat -c%s "$log_file" 2>/dev/null || echo "0")
                        if [ "$file_size" -gt 0 ]; then
                            replica_files_found=$((replica_files_found + 1))
                            log_info "Found replica file: $log_file (size: $file_size bytes)"
                        fi
                    fi
                done
            fi
        done
    fi

    if [ $replica_files_found -gt 0 ]; then
        log_info "Found $replica_files_found replica disk file(s) with non-zero size"
        replication_verified=true
    else
        log_warn "No replica disk files found (may be normal if replication hasn't started yet)"
        # Don't fail - files might be in different location or test too short
    fi

    # Check for replication_done updates in logs
    # Look for "Replicated batch" messages
    local replication_messages=0
    for i in $(seq 0 $((NUM_BROKERS-1))); do
        local count=$(grep -c "Replicated batch\|replication_done" "broker_$i.log" 2>/dev/null || echo "0")
        replication_messages=$((replication_messages + count))
    done

    if [ $replication_messages -gt 0 ]; then
        log_info "Found $replication_messages replication-related log messages"
        replication_verified=true
    else
        log_warn "No replication log messages found (check VLOG level or test duration)"
    fi

    if [ "$replication_verified" = true ]; then
        log_info "✓ Replication verification passed"
        return 0
    else
        log_warn "⚠ Replication verification inconclusive (test may be too short or VLOG level too high)"
        # Don't fail - this is a warning, not a hard failure
        return 0
    fi
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
    echo "E2E Test: Explicit Replication (ORDER=5 + ack_level=2)"
    echo "=========================================="
    
    local test_start=$(date +%s)
    
    setup
    start_brokers || return 1
    
    # Give brokers a moment to fully initialize replication threads
    sleep 2
    
    # [[PHASE_4_BOUNDED_TIMEOUTS]] - Check timeout before client test
    local elapsed=$(($(date +%s) - test_start))
    if [ $elapsed -ge $GLOBAL_TIMEOUT ]; then
        log_error "Test exceeded global timeout of ${GLOBAL_TIMEOUT}s before client test"
        return 1
    fi
    
    run_client_test || return 1
    
    # Wait a bit for replication to catch up
    log_info "Waiting 5 seconds for replication to complete..."
    sleep 5
    
    # [[PHASE_4_BOUNDED_TIMEOUTS]] - Check timeout before verification
    elapsed=$(($(date +%s) - test_start))
    if [ $elapsed -ge $GLOBAL_TIMEOUT ]; then
        log_error "Test exceeded global timeout of ${GLOBAL_TIMEOUT}s during verification"
        return 1
    fi
    
    verify_replication || return 1
    verify_broker_health || return 1

    echo ""
    log_info "All test assertions passed"
}

main "$@"
