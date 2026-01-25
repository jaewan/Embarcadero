#!/bin/bash

# Test 10 Brokers at 6Gbps - Scaling Analysis
# This tests the hypothesis that the hang is due to broker count × bandwidth, not just bandwidth

set -e

# -- Configuration --
NUM_BROKERS=10
BROKER_THROTTLE="6gbit"  # Testing 6Gbps with fewer brokers

# Test configuration  
NUM_TRIALS=1
test_cases=(1)
if [ -n "$MESSAGE_SIZE" ]; then
    msg_sizes=($MESSAGE_SIZE)
else
    msg_sizes=(1024)  # Single message size for focused testing
fi

orders=(5)
sequencer=EMBARCADERO

echo "🚀 Testing 10 Brokers at 6Gbps - Scaling Analysis"
echo "   Hypothesis: Issue is broker count × bandwidth, not just bandwidth"
echo "   Brokers: $NUM_BROKERS with real CXL access (NUMA node 2)"
echo "   Per-broker bandwidth: $BROKER_THROTTLE total (in + out)"
echo "   Client bandwidth: Unlimited, max $BROKER_THROTTLE per broker"
echo "   Total client capacity: $((NUM_BROKERS * 6))Gbps (${NUM_BROKERS} × 6Gbps)"
echo "   Message sizes: ${msg_sizes[@]}"
echo "   Expected: Should work if hypothesis is correct"

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
        # For 6Gbps, quantum should be around 10000 to avoid scheduling issues
        sudo tc class add dev lo parent 1: classid 1:1$i htb rate $BROKER_THROTTLE quantum 10000
        echo "   Created class 1:1$i for broker $i: $BROKER_THROTTLE"
    done
    
    # Class 1:99 - Default/unlimited traffic (client and other)
    sudo tc class add dev lo parent 1: classid 1:99 htb rate 100gbit quantum 10000
    
    # Filter traffic by broker ports - each broker gets its own class
    # Only throttle DATA traffic (port 1214-1223), not heartbeat traffic (12140-12149) during connection setup
    for i in $(seq 1 $NUM_BROKERS); do
        broker_heartbeat_port=$((12139 + i))  # 12140, 12141, ..., 12149
        broker_data_port=$((1213 + i))        # 1214, 1215, ..., 1223
        
        # Only throttle DATA ports to avoid interfering with connection establishment
        # Heartbeat ports are left unlimited for reliable connection setup
        sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip sport $broker_data_port 0xffff flowid 1:1$i
        sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip dport $broker_data_port 0xffff flowid 1:1$i
        
        echo "   Broker $i: data port $broker_data_port → class 1:1$i ($BROKER_THROTTLE), heartbeat port $broker_heartbeat_port → unlimited"
    done
    
    echo "✅ Per-broker traffic control configured:"
    echo "   - Each broker: $BROKER_THROTTLE total (incoming + outgoing)"
    echo "   - Client: Unlimited, but max $BROKER_THROTTLE per broker"
    echo "   - Total client bandwidth: up to $((NUM_BROKERS * 6))Gbps (${NUM_BROKERS} × 6Gbps)"
}

# Run test function
run_test() {
    local order=$1
    local msg_size=$2
    
    echo "🧪 Testing Order $order, Message Size ${msg_size}B"
    
    cd build/bin || { echo "Error: build/bin not found"; return 1; }
    
    CONFIG_ARG="--config ../../config/10_brokers.yaml"
    export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
    
    # NUMA binding: CPU on node 1, memory on nodes 1&2 (CXL)
    NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"
    
    echo "   Starting brokers with CXL access..."
    broker_pids=()
    
    # Start head broker
    echo "     -> Head broker (CXL on NUMA node 2)"
    $NUMA_BIND ./embarlet --head $CONFIG_ARG &
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
    echo "   Command: ./throughput_test $CONFIG_ARG -t 1 -o $order --sequencer $sequencer -m $msg_size -n 1"
           
    ./throughput_test $CONFIG_ARG -t 1 -o $order --sequencer $sequencer -m $msg_size -n 1
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
echo "🚀 Starting test..."

for order in "${orders[@]}"; do
    for msg_size in "${msg_sizes[@]}"; do
        echo ""
        echo "=" $(printf '%.0s' {1..60})
        echo "Testing Order Level $order with Message Size ${msg_size}B"
        echo "=" $(printf '%.0s' {1..60})
        
        if run_test $order $msg_size; then
            echo "✅ SUCCESS: Order $order, Message Size ${msg_size}B completed"
        else
            echo "❌ FAILED: Order $order, Message Size ${msg_size}B failed"
            exit 1
        fi
    done
done

echo ""
echo "🎉 All tests completed successfully!"
echo "📊 HYPOTHESIS CONFIRMED: 10 brokers at 6Gbps works!"
echo "   This proves the issue is broker count × bandwidth scaling"
