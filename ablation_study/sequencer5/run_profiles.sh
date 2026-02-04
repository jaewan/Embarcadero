#!/bin/bash
# Run three publication-ready profiles:
#   1) Algorithm ablation (strict validity)
#   2) Max throughput (open-loop)
#   3) Scatter-gather scaling (open-loop)
#
# Usage:
#   ./run_profiles.sh                 # Runs all three profiles
#   PROFILE=algo ./run_profiles.sh    # Runs only profile 1
#   PROFILE=max  ./run_profiles.sh    # Runs only profile 2
#   PROFILE=sg   ./run_profiles.sh    # Runs only profile 3
#
# Override defaults via env (RUNS, DURATION, PBR, SHARDS, PRODUCERS).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

mkdir -p build logs
cd build
[[ -f sequencer5_benchmark ]] || { cmake .. && make -j"$(nproc)"; }

timestamp="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="../logs/profiles_${timestamp}"
mkdir -p "$LOG_DIR"

PROFILE="${PROFILE:-all}"

run_case() {
    local name="$1"
    shift
    local log="$LOG_DIR/${name}.log"
    echo "==> ${name} -> ${log}"
    "$@" > "${log}.tmp" 2>&1
    cat "${log}.tmp" | tee "${log}"
    rm -f "${log}.tmp"
}

if [[ "$PROFILE" == "all" || "$PROFILE" == "algo" ]]; then
    # Profile 1: Algorithm ablation (strict validity)
    # Goal: no PBR saturation, stable comparison.
    RUNS="${RUNS:-5}"
    DURATION="${DURATION:-10}"
    PBR="${PBR:-16777216}"  # 16M per broker
    PRODUCERS="${PRODUCERS:-1}"
    echo ""
    echo "Profile 1 (algo): RUNS=$RUNS, DURATION=${DURATION}s, PRODUCERS=$PRODUCERS, PBR=$PBR"
    run_case "profile_algo_ablation_levels_scaling" \
        ./sequencer5_benchmark 4 "$PRODUCERS" "$DURATION" 0.1 "$RUNS" 0 0 8 0 "$PBR" \
        --suite=ablation,levels,scalability --validity=algo
fi

if [[ "$PROFILE" == "all" || "$PROFILE" == "max" ]]; then
    # Profile 2: Max throughput (open-loop)
    # Goal: peak throughput; allows higher PBR_full; not for algorithm comparisons.
    RUNS="${RUNS:-5}"
    DURATION="${DURATION:-10}"
    PBR="${PBR:-33554432}"  # 32M per broker
    PRODUCERS="${PRODUCERS:-8}"
    echo ""
    echo "Profile 2 (max): RUNS=$RUNS, DURATION=${DURATION}s, PRODUCERS=$PRODUCERS, PBR=$PBR"
    run_case "profile_max_throughput" \
        ./sequencer5_benchmark 4 "$PRODUCERS" "$DURATION" 0.1 "$RUNS" 0 0 8 0 "$PBR" \
        --suite=levels,scalability --validity=max
fi

if [[ "$PROFILE" == "all" || "$PROFILE" == "sg" ]]; then
    # Profile 3: Scatter-gather scaling (open-loop)
    RUNS="${RUNS:-5}"
    DURATION="${DURATION:-30}"
    PBR="${PBR:-16777216}"   # 16M per broker
    PRODUCERS="${PRODUCERS:-2}"
    SHARDS="${SHARDS:-8}"
    echo ""
    echo "Profile 3 (sg): RUNS=$RUNS, DURATION=${DURATION}s, PRODUCERS=$PRODUCERS, SHARDS=$SHARDS, PBR=$PBR"
    run_case "profile_scatter_gather_scaling" \
        ./sequencer5_benchmark 4 "$PRODUCERS" "$DURATION" 0.1 "$RUNS" 0 1 "$SHARDS" 1 "$PBR" \
        --validity=max
fi

echo "Done. Logs in ${LOG_DIR}"
