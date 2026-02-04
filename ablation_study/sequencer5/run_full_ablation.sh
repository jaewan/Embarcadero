#!/bin/bash
# Run full ablation study (publication-ready defaults).
# NOTE: For algorithm-comparison runs, use fewer producers and larger PBR
# (see run_profiles.sh profile 1). This script targets throughput.
# Uses larger PBR to reduce saturation risk.
# Takes ~15–25 minutes depending on machine.
#
# Usage:
#   ./run_full_ablation.sh           # Full run (5 runs, 10s)
#   ABLATION_SHORT=1 ./run_full_ablation.sh   # Short run (2 runs, 5s) for quick end-to-end check (~5–10 min)
#
# For paper-ready algorithm comparison only: ./sequencer5_benchmark --clean
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"
mkdir -p build
cd build
[[ -f sequencer5_benchmark ]] || { cmake .. && make -j"$(nproc)"; }

# Short mode: 2 runs, 5s duration (quick end-to-end verification)
if [[ "${ABLATION_SHORT:-0}" == "1" ]]; then
    NUM_RUNS=2
    DURATION=5
    LOG="../ablation_short_$(date +%Y%m%d_%H%M%S).log"
    echo "Short full ablation (2 runs, 5s) -> $LOG (output buffered; for live view: tail -f $LOG)"
else
    NUM_RUNS=5
    DURATION=10
    LOG="../ablation_full_$(date +%Y%m%d_%H%M%S).log"
    echo "Full ablation -> $LOG (output buffered; for live view: tail -f $LOG)"
fi

BENCH_EXIT=0
cleanup() {
    if [[ -n "${LOG:-}" && -f "$LOG" ]]; then
        echo "" >> "$LOG"
        echo "=== Exit status: ${BENCH_EXIT} ===" >> "$LOG"
    fi
    if [[ "$BENCH_EXIT" -eq 0 ]]; then
        echo "Done. Results in $LOG"
    else
        echo "FAILED (exit $BENCH_EXIT). See $LOG"
    fi
}
trap cleanup EXIT

# Run benchmark; capture real exit code (pipe would give tee's status, hiding segfault/signal).
# brokers=4 producers=8 duration level5_ratio=0.1 num_runs radix=0 sg=0 shards=8 sg_only=0 pbr_entries=16M
TMPLOG="${LOG}.tmp"
./sequencer5_benchmark 4 8 "$DURATION" 0.1 "$NUM_RUNS" 0 0 8 0 16777216 > "$TMPLOG" 2>&1
BENCH_EXIT=$?
cat "$TMPLOG" | tee "$LOG"
rm -f "$TMPLOG"
exit "$BENCH_EXIT"
