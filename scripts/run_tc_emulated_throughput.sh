#!/bin/bash

# Traffic Control Based Embarcadero Network Emulation
# This script uses Linux TC to throttle broker communication while preserving CXL access

set -e

# -- Configuration --
NUM_BROKERS=20
BROKER_THROTTLE="4gbit"  # STABLE: Reverting to 4Gbps which works correctly

# Test configuration  
NUM_TRIALS=1
test_cases=(1)
if [ -n "$MESSAGE_SIZE" ]; then
    msg_sizes=($MESSAGE_SIZE)
else
    msg_sizes=(4096)
fi

orders=(0)
sequencer=EMBARCADERO

echo "🚀 Starting Traffic Control Based Network Emulation"
echo "   Brokers: $NUM_BROKERS with real CXL access (NUMA node 2)"
echo "   Per-broker bandwidth: $BROKER_THROTTLE total (in + out)"
echo "   Client bandwidth: Unlimited, max $BROKER_THROTTLE per broker"
echo "   Total client capacity: $((NUM_BROKERS * 4))Gbps (${NUM_BROKERS} × 4Gbps)"
echo "   Message sizes: ${msg_sizes[@]}"
echo "   Test: Full 10GB message test"

# Cleanup function
cleanup_tc() {
    echo "🧹 Cleaning up..."
    pkill -f "./embarlet" >/dev/null 2>&1 || true
    pkill -f "./throughput_test" >/dev/null 2>&1 || true
    sudo tc qdisc del dev lo root >/dev/null 2>&1 || true
    echo "✅ Cleanup complete"
}

# Setup traffic control for per-broker bandwidth limits
setup_tc() {
    echo "🔧 Setting up per-broker traffic control..."
    
    # Clean existing rules
    sudo tc qdisc del dev lo root >/dev/null 2>&1 || true
    
    # Create HTB qdisc with adjusted r2q for high bandwidth rates
    # r2q=10 helps with high bandwidth rates to avoid quantum warnings
    sudo tc qdisc add dev lo root handle 1: htb default 99 r2q 10
    
    # Create individual classes for each broker with proper quantum
    for i in $(seq 1 $NUM_BROKERS); do
        # Class 1:1X - Broker X traffic with explicit quantum to avoid warnings
        # For 8Gbps, quantum should be around 10000 to avoid scheduling issues
        sudo tc class add dev lo parent 1: classid 1:1$i htb rate $BROKER_THROTTLE quantum 10000
        echo "   Created class 1:1$i for broker $i: $BROKER_THROTTLE"
    done
    
    # Class 1:99 - Default/unlimited traffic (client and other)
    sudo tc class add dev lo parent 1: classid 1:99 htb rate 100gbit quantum 10000
    
    # Filter traffic by broker ports - each broker gets its own class
    # Only throttle DATA traffic (port 1214-1233), not heartbeat traffic (12140-12159) during connection setup
    for i in $(seq 1 $NUM_BROKERS); do
        broker_heartbeat_port=$((12139 + i))  # 12140-12159 for 20 brokers
        broker_data_port=$((1213 + i))        # 1214-1233 for 20 brokers
        
        # Only throttle DATA ports to avoid interfering with connection establishment
        # Heartbeat ports are left unlimited for reliable connection setup
        sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip sport $broker_data_port 0xffff flowid 1:1$i
        sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip dport $broker_data_port 0xffff flowid 1:1$i
        
        echo "   Broker $i: data port $broker_data_port → class 1:1$i ($BROKER_THROTTLE), heartbeat port $broker_heartbeat_port → unlimited"
    done
    
    echo "✅ Per-broker traffic control configured:"
    echo "   - Each broker: $BROKER_THROTTLE total (incoming + outgoing)"
    echo "   - Client: Unlimited, but max $BROKER_THROTTLE per broker"
    echo "   - Total client bandwidth: up to $((NUM_BROKERS * 4))Gbps (${NUM_BROKERS} × 4Gbps)"
}

# Run test function
run_test() {
    local order=$1
    local msg_size=$2
    
    echo "🧪 Testing Order $order, Message Size ${msg_size}B"
    
    cd build/bin || { echo "Error: build/bin not found"; return 1; }
    
    CONFIG_ARG="--config ../../config/20_brokers_optimized.yaml"
    export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
    
    # NUMA binding: CPU on node 1, memory on nodes 1&2 (CXL)
    NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"
    
    echo "   Starting brokers with CXL access..."
    broker_pids=()
    
    # Start head broker
    echo "     -> Head broker (CXL on NUMA node 2)"
    $NUMA_BIND ./embarlet --head $CONFIG_ARG --emul &
    broker_pids+=($!)
    sleep 3
    
    # Start follower brokers
    for i in $(seq 2 $NUM_BROKERS); do
        echo "     -> Broker $i (CXL on NUMA node 2)"
        $NUMA_BIND ./embarlet $CONFIG_ARG &
        broker_pids+=($!)
        sleep 1
    done
    
    echo "   Waiting for cluster initialization..."
    sleep 10  # Increased wait time for full service initialization
    
    echo "   Starting throughput test (connections will establish first)..."
           echo "   Command: ./throughput_test --config ../../config/20_brokers_optimized.yaml -o $order --sequencer $sequencer -m $msg_size -t 5 (publish-only test)"
           
           ./throughput_test --config ../../config/20_brokers_optimized.yaml -o $order --sequencer $sequencer -m $msg_size -t 5
    result=$?
    
    echo "   Cleaning up brokers..."
    for pid in "${broker_pids[@]}"; do
        kill $pid >/dev/null 2>&1 || true
    done
    sleep 2
    pkill -f "./embarlet" >/dev/null 2>&1 || true
    
    return $result
}

# Main execution
trap cleanup_tc EXIT

setup_tc

echo ""
echo "🚀 Starting tests..."

for order in "${orders[@]}"; do
    for msg_size in "${msg_sizes[@]}"; do
        echo ""
        echo "=================================================="
        echo "Testing Order $order, Message Size ${msg_size}B"
        echo "Network: Inter-broker throttled to $BROKER_THROTTLE"
        echo "CXL: Real NUMA node 2 access"
        echo "=================================================="
        
        if run_test $order $msg_size; then
            echo "✅ Test completed successfully"
        else
            echo "❌ Test failed"
        fi
        
        echo "Waiting 3 seconds..."
        sleep 3
    done
done

echo ""
echo "🎉 All tests completed!"
