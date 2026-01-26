#!/bin/bash
# E2E Regression Test Runner for BlogMessageHeader
# Runs e2e tests with BlogHeader off and on, checks for error signatures

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Error signatures to check for in logs
ERROR_PATTERNS=(
    "FATAL"
    "SIGSEGV"
    "Invalid paddedSize"
    "boundary mismatch"
    "payload size exceeds max"
    "would walk past batch end"
    "assert"
)

# Colors
GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo "=========================================="
echo "E2E Regression Test: BlogMessageHeader"
echo "=========================================="
echo ""

# Test 1: Baseline (BlogHeader disabled)
echo -e "${YELLOW}[TEST 1/2] Running E2E tests with EMBARCADERO_USE_BLOG_HEADER=0${NC}"
export EMBARCADERO_USE_BLOG_HEADER=0

cd test/e2e
if bash run_all.sh > /tmp/e2e_baseline.log 2>&1; then
    echo -e "${GREEN}✓ Baseline tests passed${NC}"
    BASELINE_PASSED=1
else
    echo -e "${RED}✗ Baseline tests failed${NC}"
    BASELINE_PASSED=0
    cat /tmp/e2e_baseline.log | tail -50
fi
cd "$PROJECT_ROOT"

# Check baseline logs for error signatures
echo "Checking baseline logs for error signatures..."
BASELINE_ERRORS=0
for pattern in "${ERROR_PATTERNS[@]}"; do
    if grep -r "$pattern" build/test_output/ 2>/dev/null | grep -v "grep:" > /dev/null; then
        echo -e "${RED}  ✗ Found error pattern: $pattern${NC}"
        grep -r "$pattern" build/test_output/ 2>/dev/null | head -5
        BASELINE_ERRORS=1
    fi
done

if [ $BASELINE_ERRORS -eq 0 ]; then
    echo -e "${GREEN}  ✓ No error signatures found in baseline logs${NC}"
fi

# Cleanup before next test
pkill -9 -f "embarlet|throughput_test" 2>/dev/null || true
sleep 3

# Test 2: BlogHeader enabled
echo ""
echo -e "${YELLOW}[TEST 2/2] Running E2E tests with EMBARCADERO_USE_BLOG_HEADER=1${NC}"
export EMBARCADERO_USE_BLOG_HEADER=1

cd test/e2e
if bash run_all.sh > /tmp/e2e_blog_v2.log 2>&1; then
    echo -e "${GREEN}✓ BlogHeader v2 tests passed${NC}"
    BLOG_PASSED=1
else
    echo -e "${RED}✗ BlogHeader v2 tests failed${NC}"
    BLOG_PASSED=0
    cat /tmp/e2e_blog_v2.log | tail -50
fi
cd "$PROJECT_ROOT"

# Check BlogHeader logs for error signatures
echo "Checking BlogHeader v2 logs for error signatures..."
BLOG_ERRORS=0
for pattern in "${ERROR_PATTERNS[@]}"; do
    if grep -r "$pattern" build/test_output/ 2>/dev/null | grep -v "grep:" > /dev/null; then
        echo -e "${RED}  ✗ Found error pattern: $pattern${NC}"
        grep -r "$pattern" build/test_output/ 2>/dev/null | head -5
        BLOG_ERRORS=1
    fi
done

if [ $BLOG_ERRORS -eq 0 ]; then
    echo -e "${GREEN}  ✓ No error signatures found in BlogHeader v2 logs${NC}"
fi

# Final summary
echo ""
echo "=========================================="
echo "E2E Regression Test Summary"
echo "=========================================="
echo "Baseline (BlogHeader=0): $([ $BASELINE_PASSED -eq 1 ] && [ $BASELINE_ERRORS -eq 0 ] && echo -e "${GREEN}PASS${NC}" || echo -e "${RED}FAIL${NC}")"
echo "BlogHeader v2 (BlogHeader=1): $([ $BLOG_PASSED -eq 1 ] && [ $BLOG_ERRORS -eq 0 ] && echo -e "${GREEN}PASS${NC}" || echo -e "${RED}FAIL${NC}")"
echo ""

if [ $BASELINE_PASSED -eq 1 ] && [ $BASELINE_ERRORS -eq 0 ] && [ $BLOG_PASSED -eq 1 ] && [ $BLOG_ERRORS -eq 0 ]; then
    echo -e "${GREEN}✓ All E2E regression tests passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ E2E regression tests failed${NC}"
    exit 1
fi
