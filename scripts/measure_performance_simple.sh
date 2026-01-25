#!/bin/bash
# Simple Performance Baseline Measurement
# Reads from result.csv after each test run

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

# Configuration
NUM_ITERATIONS=${NUM_ITERATIONS:-10}
ORDER=${ORDER:-5}
ACK=${ACK:-1}
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-10737418240}  # 10GB

# Output files
RESULTS_DIR="$PROJECT_ROOT/data/performance_baseline"
mkdir -p "$RESULTS_DIR"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RESULTS_FILE="$RESULTS_DIR/baseline_${TIMESTAMP}.csv"
SUMMARY_FILE="$RESULTS_DIR/summary_${TIMESTAMP}.txt"
RESULT_CSV="$PROJECT_ROOT/data/throughput/pub/result.csv"

echo "=========================================="
echo "Performance Baseline Measurement (Simple)"
echo "=========================================="
echo "Iterations: $NUM_ITERATIONS"
echo "Order Level: $ORDER"
echo "ACK Level: $ACK"
echo "Message Size: $MESSAGE_SIZE bytes"
echo "Total Data: $TOTAL_MESSAGE_SIZE bytes"
echo "Results: $RESULTS_FILE"
echo "=========================================="
echo ""

# Cleanup function
cleanup() {
    pkill -9 -f "throughput_test|embarlet" 2>/dev/null || true
    sleep 2
    rm -f /tmp/embarlet_*_ready build/bin/broker_*.log 2>/dev/null || true
}

# Initialize results file
echo "iteration,bandwidth_mbps,duration_seconds,status" > "$RESULTS_FILE"

# Backup existing result.csv
if [ -f "$RESULT_CSV" ]; then
    cp "$RESULT_CSV" "$RESULT_CSV.backup"
fi

# Run iterations
SUCCESSFUL_ITERATIONS=0
FAILED_ITERATIONS=0

for i in $(seq 1 $NUM_ITERATIONS); do
    echo "[$i/$NUM_ITERATIONS] Running iteration..."
    
    cleanup
    
    # Clear result CSV
    rm -f "$RESULT_CSV"
    
    # Run test
    export ORDER=$ORDER ACK=$ACK MESSAGE_SIZE=$MESSAGE_SIZE TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE
    
    START_TIME=$(date +%s)
    if bash "$SCRIPT_DIR/run_throughput.sh" > /tmp/test_output_${i}.log 2>&1; then
        END_TIME=$(date +%s)
        DURATION=$((END_TIME - START_TIME))
        
        # Wait a moment for CSV to be written
        sleep 1
        
        # Read from result.csv
        if [ -f "$RESULT_CSV" ]; then
            # Get last line (most recent result)
            LAST_LINE=$(tail -1 "$RESULT_CSV")
            BANDWIDTH=$(echo "$LAST_LINE" | cut -d',' -f12 || echo "0")
            
            if [ -n "$BANDWIDTH" ] && [ "$BANDWIDTH" != "0" ] && [ "$BANDWIDTH" != "pub_bandwidth_mbps" ]; then
                echo "$i,$BANDWIDTH,$DURATION,success" >> "$RESULTS_FILE"
                echo "  ✓ Bandwidth: ${BANDWIDTH} MB/s, Duration: ${DURATION}s"
                ((SUCCESSFUL_ITERATIONS++))
            else
                echo "$i,0,$DURATION,failed" >> "$RESULTS_FILE"
                echo "  ✗ Failed: Invalid bandwidth in CSV"
                ((FAILED_ITERATIONS++))
            fi
        else
            echo "$i,0,$DURATION,failed" >> "$RESULTS_FILE"
            echo "  ✗ Failed: Result CSV not found"
            ((FAILED_ITERATIONS++))
        fi
    else
        END_TIME=$(date +%s)
        DURATION=$((END_TIME - START_TIME))
        echo "$i,0,$DURATION,error" >> "$RESULTS_FILE"
        echo "  ✗ Failed: Test script error"
        tail -5 /tmp/test_output_${i}.log | sed 's/^/    /'
        ((FAILED_ITERATIONS++))
    fi
    
    # Brief pause between iterations
    sleep 2
done

cleanup

# Calculate statistics
echo ""
echo "=========================================="
echo "Calculating Statistics..."
echo "=========================================="

python3 << EOF
import csv
import statistics
import sys

results = []
with open('$RESULTS_FILE', 'r') as f:
    reader = csv.DictReader(f)
    for row in reader:
        if row['status'] == 'success':
            results.append(float(row['bandwidth_mbps']))

if len(results) == 0:
    print("ERROR: No successful iterations!")
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

print(f"Successful Iterations: {len(results)}/{$NUM_ITERATIONS}")
print(f"Failed Iterations: {$FAILED_ITERATIONS}")
print("")
print("Bandwidth Statistics (MB/s):")
print(f"  Mean:   {mean:.2f}")
print(f"  Median: {median:.2f}")
print(f"  StdDev: {stdev:.2f}")
print(f"  Min:    {min_val:.2f}")
print(f"  Max:    {max_val:.2f}")
print(f"  P95:    {p95:.2f}")
print(f"  P99:    {p99:.2f}")
print("")
print(f"Variance: {stdev/mean*100:.1f}% (CV)")

# Write summary
with open('$SUMMARY_FILE', 'w') as f:
    f.write("Performance Baseline Summary\n")
    f.write("=" * 40 + "\n")
    f.write(f"Timestamp: $TIMESTAMP\n")
    f.write(f"Iterations: {len(results)}/{$NUM_ITERATIONS} successful\n")
    f.write(f"Configuration: ORDER=$ORDER, ACK=$ACK, MSG_SIZE=$MESSAGE_SIZE\n")
    f.write("\n")
    f.write("Bandwidth Statistics (MB/s):\n")
    f.write(f"  Mean:   {mean:.2f}\n")
    f.write(f"  Median: {median:.2f}\n")
    f.write(f"  StdDev: {stdev:.2f}\n")
    f.write(f"  Min:    {min_val:.2f}\n")
    f.write(f"  Max:    {max_val:.2f}\n")
    f.write(f"  P95:    {p95:.2f}\n")
    f.write(f"  P99:    {p99:.2f}\n")
    f.write(f"\nVariance: {stdev/mean*100:.1f}% (Coefficient of Variation)\n")
    f.write("\n")
    f.write("Assessment:\n")
    if stdev/mean < 0.10:
        f.write("  ✓ Low variance (<10%) - Performance is stable\n")
    elif stdev/mean < 0.20:
        f.write("  ⚠ Moderate variance (10-20%) - Some system load variation\n")
    else:
        f.write("  ✗ High variance (>20%) - Investigate system load or bottlenecks\n")
    
    if mean >= 8000 and mean <= 12000:
        f.write("  ✓ Bandwidth within target range (8-12 GB/s)\n")
    else:
        f.write(f"  ⚠ Bandwidth outside target range: {mean/1024:.2f} GB/s\n")

print("\nSummary written to: $SUMMARY_FILE")
EOF

echo ""
echo "=========================================="
echo "Measurement Complete"
echo "=========================================="
echo "Results: $RESULTS_FILE"
echo "Summary: $SUMMARY_FILE"
echo ""
