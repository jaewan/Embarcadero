#!/bin/bash
# Validation script: correctness test + short ablation. Before each run we print
# EXPECTED RUNTIME so you can kill if it exceeds (indicates slowness or hang).
#
# Usage:
#   ./validate.sh           # Full: Phase1 + Phase2a + Phase2b
#   FAST=1 ./validate.sh    # Quick: Phase1 only (~30 s)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

if [ "${FAST:-0}" = "1" ]; then
    echo "FAST=1: running Phase 1 (correctness) only."
    echo "  EXPECTED RUNTIME: 30 s  (kill if significantly longer)"
    echo ""
else
    # Full run: Phase1 ~30s + Phase2a up to 10 min + Phase2b up to 3 min
    echo "TOTAL EXPECTED RUNTIME: Phase1 ~30 s; Phase2a up to 10 min; Phase2b up to 3 min."
    echo "  Kill if any phase exceeds the stated time (slowness or hang)."
    echo ""
fi

mkdir -p build logs
cd build
[[ -f sequencer5_benchmark ]] || { echo "Building..."; cmake .. && make -j"$(nproc)"; }

BIN="./sequencer5_benchmark"

# -----------------------------------------------------------------------------
# Phase 1: Minimal correctness test (--test)
# 2000 + 1500 batches, two sequencer types, validate ordering. Normally < 10s.
# -----------------------------------------------------------------------------
EXPECT_TEST_S=30
echo ""
echo "=============================================="
echo "Phase 1: Correctness test (--test)"
echo "  EXPECTED RUNTIME: ${EXPECT_TEST_S} s  (kill if significantly longer)"
echo "=============================================="
START=$(date +%s)
$BIN --test || { echo "FAIL: correctness test"; exit 1; }
ELAPSED=$(($(date +%s) - START))
echo "  Elapsed: ${ELAPSED} s"
if [ "$ELAPSED" -gt "$EXPECT_TEST_S" ]; then
    echo "  WARNING: Exceeded expected ${EXPECT_TEST_S} s (possible slowness or hang)"
fi

[ "${FAST:-0}" = "1" ] && { echo ""; echo "FAST mode: skipping Phase 2a and 2b."; echo "Validation (Phase 1) complete."; exit 0; }

# -----------------------------------------------------------------------------
# Phase 2a: Single-threaded ablation + levels + scalability (short)
# RUNS=2, 5s/run. Wall clock >> 5s*N because sequencer processes epochs; allow ~5 min.
# -----------------------------------------------------------------------------
RUNS=2
SINGLE_DURATION=5
SINGLE_PBR=4194304
# Realistic: ablation1 (warmup + 2 paired runs) + levels (3 configs * 2 runs) + scalability (4 thread counts * 2 runs). Allow up to 10 min; kill if longer.
EXPECT_SINGLE_S=600
echo ""
echo "=============================================="
echo "Phase 2a: Single-threaded suite (ablation + levels + scalability)"
echo "  Config: RUNS=${RUNS} duration=${SINGLE_DURATION}s"
echo "  EXPECTED RUNTIME: ${EXPECT_SINGLE_S} s (~10 min)  (kill if significantly longer)"
echo "=============================================="
START=$(date +%s)
$BIN 4 1 "${SINGLE_DURATION}" 0.1 "${RUNS}" 0 0 8 0 "${SINGLE_PBR}" \
    --suite=ablation,levels,scalability --validity=max || { echo "FAIL: single-threaded suite"; exit 1; }
ELAPSED=$(($(date +%s) - START))
echo "  Elapsed: ${ELAPSED} s"
if [ "$ELAPSED" -gt "$EXPECT_SINGLE_S" ]; then
    echo "  WARNING: Exceeded expected ${EXPECT_SINGLE_S} s (possible slowness or hang)"
fi

# -----------------------------------------------------------------------------
# Phase 2b: Scatter-gather scaling (short)
# RUNS=2, 10s/run; wall clock includes epoch processing. Allow 2 min.
# -----------------------------------------------------------------------------
SG_DURATION=10
EXPECT_SG_S=120
echo ""
echo "=============================================="
echo "Phase 2b: Scatter-gather scaling"
echo "  Config: RUNS=${RUNS} duration=${SG_DURATION}s"
echo "  EXPECTED RUNTIME: ${EXPECT_SG_S} s (~2 min)  (kill if significantly longer)"
echo "=============================================="
START=$(date +%s)
$BIN 4 2 "${SG_DURATION}" 0.1 "${RUNS}" 0 1 8 1 "${SINGLE_PBR}" \
    --validity=max || { echo "FAIL: scatter-gather suite"; exit 1; }
ELAPSED=$(($(date +%s) - START))
echo "  Elapsed: ${ELAPSED} s"
if [ "$ELAPSED" -gt "$EXPECT_SG_S" ]; then
    echo "  WARNING: Exceeded expected ${EXPECT_SG_S} s (possible slowness or hang)"
fi

echo ""
echo "=============================================="
echo "Validation complete. All phases passed."
echo "=============================================="
