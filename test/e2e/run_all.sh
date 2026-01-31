#!/bin/bash
# Run all E2E tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

TESTS=(
    "test_basic_publish.sh"
    "test_explicit_replication_order5_ack2.sh"
    "test_sequencer_only_topology.sh"
    # Add more tests here as they're created:
    # "test_ordering.sh"
    # "test_durability.sh"
    # "test_broker_failure.sh"
)

PASSED=0
FAILED=0
FAILED_TESTS=()

echo "=========================================="
echo "Running Embarcadero E2E Test Suite"
echo "=========================================="
echo ""

for test in "${TESTS[@]}"; do
    test_path="$SCRIPT_DIR/$test"

    if [ ! -f "$test_path" ]; then
        echo "⚠️  SKIP: $test (not found)"
        continue
    fi

    echo "Running: $test"
    echo "------------------------------------------"

    if bash "$test_path"; then
        echo "✓ PASSED: $test"
        PASSED=$((PASSED + 1))
    else
        echo "✗ FAILED: $test"
        FAILED=$((FAILED + 1))
        FAILED_TESTS+=("$test")
    fi

    echo ""
done

echo "=========================================="
echo "Test Suite Summary"
echo "=========================================="
echo "Passed: $PASSED"
echo "Failed: $FAILED"

if [ $FAILED -gt 0 ]; then
    echo ""
    echo "Failed tests:"
    for test in "${FAILED_TESTS[@]}"; do
        echo "  - $test"
    done
    exit 1
fi

echo ""
echo "✓ All tests passed!"
exit 0
