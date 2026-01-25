#!/bin/bash

# Embarcadero Broker Scaling Experiment
# Tests throughput scaling with increasing broker count (1,2,4,8,12,16,20)
# Each broker limited to 4Gbps, client sends 10GB total messages
# Dynamic configuration adjustment based on broker count

set -e

# -- Experiment Configuration --
BROKER_COUNTS=(1 2 4 8 12 16 20)
BROKER_THROTTLE="4gbit"  # Fixed 4Gbps per broker (proven stable)
TOTAL_MESSAGE_SIZE_GB=10  # 10GB total message test
MESSAGE_SIZE=1024         # 1KB messages for consistent testing

# Test parameters
NUM_TRIALS=1
ORDER_LEVEL=5
SEQUENCER=EMBARCADERO

# Results file
RESULTS_FILE="data/broker_scaling_results.csv"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
DETAILED_LOG="data/scaling_experiment_${TIMESTAMP}.log"

echo "🚀 Embarcadero Broker Scaling Experiment (Publish-Only)"
echo "   Testing broker counts: ${BROKER_COUNTS[@]}"
echo "   Per-broker bandwidth: $BROKER_THROTTLE (4Gbps each)"
echo "   Total message size: ${TOTAL_MESSAGE_SIZE_GB}GB"
echo "   Message size: ${MESSAGE_SIZE}B"
echo "   Test type: Publish-only (-t 0) for reliable scaling analysis"
echo "   Results file: $RESULTS_FILE"
echo "   Detailed log: $DETAILED_LOG"

# Initialize results file with header
echo "broker_count,total_bandwidth_gbps,throughput_gbps,throughput_msgs_per_sec,duration_seconds,segment_size_gb,buffer_size_mb,test_timestamp" > "$RESULTS_FILE"

# Cleanup function
cleanup_experiment() {
    echo "🧹 Cleaning up experiment..."
    pkill -f "./embarlet" >/dev/null 2>&1 || true
    pkill -f "./throughput_test" >/dev/null 2>&1 || true
    sudo tc qdisc del dev lo root >/dev/null 2>&1 || true
    echo "✅ Experiment cleanup complete"
}

# Calculate resources based on broker count
calculate_resources() {
    local broker_count=$1
    
    # Calculate segment size per broker
    # 10GB total + 50% overhead for headers = 15GB total
    # Distribute across brokers with minimum 2GB per broker
    local total_data_with_overhead_gb=15
    local segment_size_gb=$(( (total_data_with_overhead_gb + broker_count - 1) / broker_count ))
    
    # Minimum 2GB per broker, maximum 20GB per broker
    if [ $segment_size_gb -lt 2 ]; then
        segment_size_gb=2
    elif [ $segment_size_gb -gt 20 ]; then
        segment_size_gb=20
    fi
    
    # Buffer size calculation
    # For fewer brokers: larger buffers per connection
    # For many brokers: smaller buffers to conserve memory
    local buffer_size_mb
    if [ $broker_count -eq 1 ]; then
        buffer_size_mb=2048  # 2GB for single broker
    elif [ $broker_count -le 4 ]; then
        buffer_size_mb=1024  # 1GB for 2-4 brokers
    elif [ $broker_count -le 8 ]; then
        buffer_size_mb=768   # 768MB for 5-8 brokers
    elif [ $broker_count -le 16 ]; then
        buffer_size_mb=512   # 512MB for 9-16 brokers
    else
        buffer_size_mb=256   # 256MB for 17+ brokers
    fi
    
    echo "$segment_size_gb $buffer_size_mb"
}

# Generate dynamic configuration file
generate_config() {
    local broker_count=$1
    local segment_size_gb=$2
    local buffer_size_mb=$3
    local config_file="config/scaling_${broker_count}_brokers.yaml"
    
    local segment_size_bytes=$(( segment_size_gb * 1024 * 1024 * 1024 ))
    
    cat > "$config_file" << EOF
# Embarcadero Configuration - Scaling Experiment: ${broker_count} Brokers
# Auto-generated for broker scaling experiment
# Segment size: ${segment_size_gb}GB, Buffer size: ${buffer_size_mb}MB

embarcadero:
  version:
    major: 1
    minor: 0

  broker:
    port: 1214
    broker_port: 12140
    heartbeat_interval: 3
    max_brokers: $broker_count
    cgroup_core: 85

  cxl:
    size: 68719476736            # CXL memory size (64GB)
    emulation_size: 34359738368  # CXL emulation memory size (32GB)
    device_path: "/dev/dax0.0"
    numa_node: 2

  storage:
    segment_size: $segment_size_bytes  # ${segment_size_gb}GB per broker
    batch_headers_size: 65536          # 64KB batch headers
    batch_size: 2097152                # 2MB batch size
    num_disks: 2
    max_topics: 32
    topic_name_size: 31

  network:
    io_threads: 4                # 4 network threads per broker
    disk_io_threads: 4
    sub_connections: 1           # Single connection model
    zero_copy_send_limit: 65536

  corfu:
    sequencer_port: 50052
    replication_port: 50053

  scalog:
    sequencer_port: 50051
    replication_port: 50052
    sequencer_ip: "192.168.60.173"
    local_cut_interval: 100

  platform:
    is_intel: false
    is_amd: false

  client:
    publisher:
      threads_per_broker: 1        # Single thread per broker for consistency
      buffer_size_mb: $buffer_size_mb
      batch_size_kb: 2048
      
    subscriber:
      connections_per_broker: 1    # Single connection per broker
      buffer_size_mb: $buffer_size_mb
      
    network:
      connect_timeout_ms: 5000     # Increased timeout for many brokers
      send_timeout_ms: 10000
      recv_timeout_ms: 10000
      
    performance:
      use_hugepages: true
      numa_bind: true
      zero_copy: true
EOF
    
    echo "$config_file"
}

# Setup traffic control for specific broker count
setup_tc() {
    local broker_count=$1
    
    echo "🔧 Setting up TC for $broker_count brokers..."
    
    # Clean existing rules
    sudo tc qdisc del dev lo root >/dev/null 2>&1 || true
    
    # Create HTB qdisc with optimized settings
    sudo tc qdisc add dev lo root handle 1: htb default 99 r2q 10
    
    # Create classes for each broker
    for i in $(seq 1 $broker_count); do
        sudo tc class add dev lo parent 1: classid 1:1$i htb rate $BROKER_THROTTLE quantum 10000
        echo "   Created class 1:1$i for broker $i: $BROKER_THROTTLE"
    done
    
    # Default unlimited class
    sudo tc class add dev lo parent 1: classid 1:99 htb rate 100gbit quantum 10000
    
    # Apply filters to broker data ports
    for i in $(seq 1 $broker_count); do
        local broker_data_port=$((1213 + i))  # 1214, 1215, ..., 1233
        
        sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip sport $broker_data_port 0xffff flowid 1:1$i
        sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip dport $broker_data_port 0xffff flowid 1:1$i
        
        echo "   Broker $i: port $broker_data_port → $BROKER_THROTTLE"
    done
    
    local total_bandwidth_gbps=$(( broker_count * 4 ))
    echo "✅ TC configured: $broker_count brokers × 4Gbps = ${total_bandwidth_gbps}Gbps total"
}

# Run scaling test for specific broker count
run_scaling_test() {
    local broker_count=$1
    local config_file=$2
    local segment_size_gb=$3
    local buffer_size_mb=$4
    
    echo ""
    echo "=" $(printf '%.0s' {1..80})
    echo "🧪 TESTING: $broker_count Brokers"
    echo "   Config: $config_file"
    echo "   Segment size: ${segment_size_gb}GB per broker"
    echo "   Buffer size: ${buffer_size_mb}MB per connection"
    echo "   Total bandwidth: $(( broker_count * 4 ))Gbps"
    echo "=" $(printf '%.0s' {1..80})
    
    cd build/bin || { echo "Error: build/bin not found"; return 1; }
    
    local config_arg="--config ../../$config_file"
    export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
    
    # NUMA binding for optimal CXL performance
    local numa_bind="numactl --cpunodebind=1 --membind=1,2"
    
    echo "   Starting $broker_count brokers..."
    local broker_pids=()
    
    # Start head broker
    echo "     -> Head broker (NUMA optimized)"
    $numa_bind ./embarlet --head $config_arg &
    broker_pids+=($!)
    sleep 3
    
    # Start follower brokers
    for i in $(seq 2 $broker_count); do
        echo "     -> Broker $i"
        $numa_bind ./embarlet $config_arg &
        broker_pids+=($!)
        sleep 1
    done
    
    # Wait for cluster initialization (longer for more brokers)
    local init_wait=$(( 5 + broker_count / 2 ))
    echo "   Waiting ${init_wait}s for cluster initialization..."
    sleep $init_wait
    
    # Run publish-only test with timing (more reliable for scaling analysis)
    echo "   Starting publish-only test..."
    echo "   Command: ./throughput_test $config_arg -t 5 -o $ORDER_LEVEL --sequencer $SEQUENCER -m $MESSAGE_SIZE -n 1"
    
    local start_time=$(date +%s)
    
    if timeout 300 ./throughput_test $config_arg -t 5 -o $ORDER_LEVEL --sequencer $SEQUENCER -m $MESSAGE_SIZE -n 1 >> "../../$DETAILED_LOG" 2>&1; then
        local end_time=$(date +%s)
        local duration=$((end_time - start_time))
        
        # Calculate throughput metrics
        local total_messages=$(( TOTAL_MESSAGE_SIZE_GB * 1024 * 1024 * 1024 / MESSAGE_SIZE ))
        local msgs_per_sec=$(( total_messages / duration ))
        local throughput_gbps=$(echo "scale=2; $TOTAL_MESSAGE_SIZE_GB / $duration" | bc)
        local total_bandwidth_gbps=$(( broker_count * 4 ))
        local test_timestamp=$(date +"%Y-%m-%d %H:%M:%S")
        
        echo "✅ SUCCESS: $broker_count brokers"
        echo "   Duration: ${duration}s"
        echo "   Throughput: ${throughput_gbps} GB/s"
        echo "   Messages/sec: ${msgs_per_sec}"
        
        # Log results to CSV
        echo "$broker_count,$total_bandwidth_gbps,$throughput_gbps,$msgs_per_sec,$duration,$segment_size_gb,$buffer_size_mb,$test_timestamp" >> "../../$RESULTS_FILE"
        
        # Cleanup brokers
        echo "   Cleaning up brokers..."
        for pid in "${broker_pids[@]}"; do
            kill $pid >/dev/null 2>&1 || true
        done
        sleep 2
        pkill -f "./embarlet" >/dev/null 2>&1 || true
        
        cd - > /dev/null
        return 0
    else
        echo "❌ FAILED: $broker_count brokers (timeout or error)"
        
        # Log failure
        echo "$broker_count,$((broker_count * 4)),0,0,300,$segment_size_gb,$buffer_size_mb,FAILED" >> "../../$RESULTS_FILE"
        
        # Cleanup brokers
        for pid in "${broker_pids[@]}"; do
            kill $pid >/dev/null 2>&1 || true
        done
        pkill -f "./embarlet" >/dev/null 2>&1 || true
        
        cd - > /dev/null
        return 1
    fi
}

# Main experiment execution
main() {
    echo "🔬 Starting Embarcadero Broker Scaling Experiment" | tee "$DETAILED_LOG"
    echo "Timestamp: $(date)" | tee -a "$DETAILED_LOG"
    
    trap cleanup_experiment EXIT
    
    for broker_count in "${BROKER_COUNTS[@]}"; do
        echo "" | tee -a "$DETAILED_LOG"
        echo "🎯 Preparing test for $broker_count brokers..." | tee -a "$DETAILED_LOG"
        
        # Calculate optimal resources
        local resources=($(calculate_resources $broker_count))
        local segment_size_gb=${resources[0]}
        local buffer_size_mb=${resources[1]}
        
        echo "   Calculated resources: ${segment_size_gb}GB segments, ${buffer_size_mb}MB buffers" | tee -a "$DETAILED_LOG"
        
        # Generate configuration
        local config_file=$(generate_config $broker_count $segment_size_gb $buffer_size_mb)
        echo "   Generated config: $config_file" | tee -a "$DETAILED_LOG"
        
        # Setup traffic control
        setup_tc $broker_count
        
        # Run the test
        if run_scaling_test $broker_count $config_file $segment_size_gb $buffer_size_mb; then
            echo "✅ Completed: $broker_count brokers" | tee -a "$DETAILED_LOG"
        else
            echo "❌ Failed: $broker_count brokers" | tee -a "$DETAILED_LOG"
            # Continue with next test instead of failing completely
        fi
        
        # Brief pause between tests
        sleep 5
    done
    
    echo "" | tee -a "$DETAILED_LOG"
    echo "🎉 Broker Scaling Experiment Complete!" | tee -a "$DETAILED_LOG"
    echo "📊 Results saved to: $RESULTS_FILE" | tee -a "$DETAILED_LOG"
    echo "📝 Detailed log: $DETAILED_LOG" | tee -a "$DETAILED_LOG"
    
    # Display summary
    echo ""
    echo "📈 SCALING RESULTS SUMMARY:"
    echo "Broker Count | Total BW | Throughput | Duration"
    echo "-------------|----------|------------|----------"
    tail -n +2 "$RESULTS_FILE" | while IFS=, read -r brokers total_bw throughput msgs_sec duration segment buffer timestamp; do
        if [ "$throughput" != "0" ]; then
            printf "%12s | %8s | %10s | %8ss\n" "${brokers}" "${total_bw}Gbps" "${throughput}GB/s" "$duration"
        else
            printf "%12s | %8s | %10s | %8s\n" "${brokers}" "${total_bw}Gbps" "FAILED" "TIMEOUT"
        fi
    done
}

# Check dependencies
if ! command -v bc &> /dev/null; then
    echo "Error: bc calculator not found. Please install: sudo apt-get install bc"
    exit 1
fi

# Run the experiment
main "$@"
