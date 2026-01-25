#!/bin/bash
# Analyze existing performance data from result.csv
# Quick analysis without running new tests

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RESULT_CSV="$PROJECT_ROOT/data/throughput/pub/result.csv"

echo "=========================================="
echo "Performance Data Analysis"
echo "=========================================="
echo ""

if [ ! -f "$RESULT_CSV" ]; then
    echo "ERROR: Result CSV not found: $RESULT_CSV"
    echo "Run a test first to generate data."
    exit 1
fi

# Count lines (excluding header)
TOTAL_LINES=$(tail -n +2 "$RESULT_CSV" | wc -l)

if [ "$TOTAL_LINES" -eq "0" ]; then
    echo "No performance data found in result.csv"
    exit 1
fi

echo "Found $TOTAL_LINES test result(s)"
echo ""

# Extract bandwidth values
python3 << EOF
import csv
import statistics
import sys

results = []
with open('$RESULT_CSV', 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        try:
            bandwidth = float(row.get('pub_bandwidth_mbps', 0))
            if bandwidth > 0:
                results.append(bandwidth)
        except (ValueError, KeyError):
            continue

if len(results) == 0:
    print("ERROR: No valid bandwidth data found!")
    sys.exit(1)

mean = statistics.mean(results)
median = statistics.median(results)
stdev = statistics.stdev(results) if len(results) > 1 else 0.0
results_sorted = sorted(results)
p95_idx = int(len(results_sorted) * 0.95)
p99_idx = int(len(results_sorted) * 0.99)
p95 = results_sorted[p95_idx] if p95_idx < len(results_sorted) else results_sorted[-1]
p99 = results_sorted[p99_idx] if p99_idx < len(results_sorted) else results_sorted[-1]
min_val = min(results)
max_val = max(results)

print("Bandwidth Statistics (MB/s):")
print(f"  Samples:   {len(results)}")
print(f"  Mean:      {mean:.2f} ({mean/1024:.2f} GB/s)")
print(f"  Median:    {median:.2f} ({median/1024:.2f} GB/s)")
print(f"  StdDev:    {stdev:.2f}")
print(f"  Min:       {min_val:.2f} ({min_val/1024:.2f} GB/s)")
print(f"  Max:       {max_val:.2f} ({max_val/1024:.2f} GB/s)")
print(f"  P95:       {p95:.2f} ({p95/1024:.2f} GB/s)")
print(f"  P99:       {p99:.2f} ({p99/1024:.2f} GB/s)")
print("")
print(f"Coefficient of Variation: {stdev/mean*100:.1f}%")
print("")

# Assessment
print("Assessment:")
if stdev/mean < 0.10:
    print("  ✓ Low variance (<10%) - Performance is stable")
elif stdev/mean < 0.20:
    print("  ⚠ Moderate variance (10-20%) - Some system load variation")
else:
    print("  ✗ High variance (>20%) - Investigate system load or bottlenecks")

if mean >= 8000 and mean <= 12000:
    print("  ✓ Bandwidth within target range (8-12 GB/s)")
elif mean < 8000:
    print(f"  ⚠ Bandwidth below target: {mean/1024:.2f} GB/s (target: 8-12 GB/s)")
else:
    print(f"  ⚠ Bandwidth above target: {mean/1024:.2f} GB/s (target: 8-12 GB/s)")

print("")
print("Recommendations:")
if len(results) < 5:
    print("  • Run more iterations (10+) for statistical significance")
if stdev/mean > 0.15:
    print("  • Investigate variance sources (system load, NUMA effects)")
if mean < 8000:
    print("  • Profile hot paths to identify bottlenecks")
if mean >= 8000 and mean <= 12000 and stdev/mean < 0.15:
    print("  • Performance is stable - proceed with profiling and mutex contention measurement")
EOF

echo ""
echo "=========================================="
echo "Analysis Complete"
echo "=========================================="
