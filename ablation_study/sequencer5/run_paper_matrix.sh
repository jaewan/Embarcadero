#!/bin/bash
# Paper matrix: 2 cases. Estimated total runtime with defaults (RUNS=5, SINGLE_DURATION=10, SG_DURATION=30):
#   Case 1 (single_ablation_levels_scaling): ~8 min  (ablation1 + levels + scalability, 10s/run)
#   Case 2 (scatter_gather_scaling):         ~20 min (full suite with 30s/run + epoch + broker + sg test)
#   Total: ~28 min. Override: RUNS=2 SINGLE_DURATION=5 SG_DURATION=10 for a quicker ~10 min run.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

mkdir -p build logs
cd build
[[ -f sequencer5_benchmark ]] || { cmake .. && make -j"$(nproc)"; }

timestamp="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="../logs/paper_matrix_${timestamp}"
mkdir -p "$LOG_DIR"

# Default tuning (override via env)
RUNS="${RUNS:-5}"
SINGLE_DURATION="${SINGLE_DURATION:-10}"
SG_DURATION="${SG_DURATION:-30}"
SINGLE_PBR="${SINGLE_PBR:-16777216}"  # 16M per broker (avoid PBR saturation)
SG_PBR="${SG_PBR:-16777216}"          # 16M per broker (avoid PBR saturation)

run_case() {
    local name="$1"
    shift
    local log="$LOG_DIR/${name}.log"
    echo "==> ${name} -> ${log}"
    "$@" > "${log}.tmp" 2>&1
    cat "${log}.tmp" | tee "${log}"
    rm -f "${log}.tmp"
}

# Single-threaded headline suite (algorithm comparison: strict validity)
run_case "single_ablation_levels_scaling" \
    ./sequencer5_benchmark 4 1 "${SINGLE_DURATION}" 0.1 "${RUNS}" 0 0 8 0 "${SINGLE_PBR}" \
    --suite=ablation,levels,scalability --validity=algo

# Scatter-gather scaling (open-loop max throughput)
run_case "scatter_gather_scaling" \
    ./sequencer5_benchmark 4 2 "${SG_DURATION}" 0.1 "${RUNS}" 0 1 8 1 "${SG_PBR}" \
    --validity=max

echo "Done. Logs in ${LOG_DIR}"
