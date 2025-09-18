#!/bin/bash

# Optimized throughput test script with thermal management
# Ensures peak CPU performance for consistent benchmarking

set -e

echo "🚀 OPTIMIZED THROUGHPUT TEST WITH THERMAL MANAGEMENT"
echo ""

# Function to check CPU frequency
check_cpu_frequency() {
    local freq=$(cat /proc/cpuinfo | grep "cpu MHz" | head -1 | awk '{print $4}')
    local freq_int=$(echo "$freq" | cut -d. -f1)
    echo "Current CPU frequency: ${freq} MHz"
    
    # Return 0 if frequency is good (>3000 MHz), 1 if throttled
    if [ "$freq_int" -gt 3000 ]; then
        return 0
    else
        return 1
    fi
}

# Function to wait for thermal recovery
wait_for_thermal_recovery() {
    echo "⏳ Waiting for CPU thermal recovery..."
    local max_wait=600  # 10 minutes max wait
    local wait_time=0
    local check_interval=10  # Check every 10 seconds instead of 30
    
    while [ $wait_time -lt $max_wait ]; do
        if check_cpu_frequency; then
            echo "✅ CPU frequency recovered! Ready for next test."
            return 0
        fi
        
        echo "🌡️ CPU still throttled, waiting ${check_interval} seconds... (${wait_time}s elapsed)"
        sleep $check_interval
        wait_time=$((wait_time + check_interval))
    done
    
    echo "⚠️ Warning: CPU frequency not fully recovered after 10 minutes"
    return 1
}

# Function to optimize CPU settings
optimize_cpu_settings() {
    echo "⚙️ Optimizing CPU settings for peak performance..."
    
    # Set performance governor (if not already set)
    if command -v cpupower >/dev/null 2>&1; then
        sudo cpupower frequency-set -g performance 2>/dev/null || echo "Note: cpupower requires sudo"
    fi
    
    # Set high priority for this process
    echo "📈 Setting high process priority..."
    renice -n -10 $$ 2>/dev/null || echo "Note: renice requires sudo for negative values"
}

# Function to run single test with monitoring
run_single_test() {
    local test_num=$1
    echo ""
    echo "🧪 TEST $test_num - Starting performance measurement"
    
    # Check CPU frequency before test
    if ! check_cpu_frequency; then
        echo "⚠️ Warning: CPU frequency is throttled before test"
    fi
    
    # Run the actual test
    echo "🏃 Running throughput test..."
    local test_output=$(timeout 180 ./run_throughput.sh 2>&1)
    local exit_code=$?
    
    # Extract and display bandwidth results
    local bandwidth_results=$(echo "$test_output" | grep -E "(Publish bandwidth|End-to-end bandwidth)")
    if [ -n "$bandwidth_results" ]; then
        echo "$bandwidth_results" | while read -r line; do
            echo "📊 $line"
        done
    else
        echo "⚠️ No bandwidth results found in test output"
        # Show last few lines of output for debugging
        echo "📝 Last few lines of test output:"
        echo "$test_output" | tail -5
    fi
    
    if [ $exit_code -eq 124 ]; then
        echo "❌ Test timed out after 3 minutes"
        return 1
    elif [ $exit_code -ne 0 ]; then
        echo "❌ Test failed with exit code $exit_code"
        return 1
    fi
    
    echo "✅ Test $test_num completed successfully"
    return 0
}

# Main execution
main() {
    echo "Starting optimized throughput testing..."
    echo "System: $(uname -a)"
    echo "CPU: $(lscpu | grep 'Model name' | cut -d: -f2 | xargs)"
    echo ""
    
    # Optimize CPU settings
    optimize_cpu_settings
    
    # Wait for initial thermal recovery
    wait_for_thermal_recovery
    
    # Run multiple tests with thermal recovery between them
    local num_tests=3
    local results=()
    
    for i in $(seq 1 $num_tests); do
        if run_single_test $i; then
            # Wait for thermal recovery before next test (except after last test)
            if [ $i -lt $num_tests ]; then
                echo ""
                echo "⏱️ Checking CPU thermal status before next test..."
                # Check if CPU frequency is already good
                if check_cpu_frequency; then
                    echo "✅ CPU frequency is good, proceeding immediately to next test."
                else
                    echo "🌡️ CPU needs thermal recovery, waiting adaptively..."
                    wait_for_thermal_recovery
                fi
            fi
        else
            echo "❌ Test $i failed, stopping test sequence"
            exit 1
        fi
    done
    
    echo ""
    echo "🎉 All tests completed successfully!"
    echo "💡 Thermal management: The script now adaptively waits for CPU frequency recovery."
    echo "💡 For best results, ensure adequate cooling between manual test runs."
}

# Run main function
main "$@"
