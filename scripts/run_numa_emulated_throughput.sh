#!/bin/bash

# NUMA-based Embarcadero Network Emulation Throughput Test
# This script uses network namespaces with proper NUMA and shared memory access

set -e

# -- Configuration --
NUM_BROKERS=4
CLIENT_NS="client-ns"
BROKER_PREFIX="broker-ns-"
BRIDGE_NAME="br0"
BROKER_RATE="4gbit"  # 4 Gigabit/s per broker (0.5 GB/s)

# Test configuration
NUM_TRIALS=1
test_cases=(1)
# Use MESSAGE_SIZE environment variable or default to multiple sizes
if [ -n "$MESSAGE_SIZE" ]; then
    msg_sizes=($MESSAGE_SIZE)
else
    msg_sizes=(128 1024 4096)
fi

orders=(5)  # Sequencer 5 (batch-level ordering)
sequencer=EMBARCADERO

echo "🚀 Starting NUMA-based Embarcadero Network Emulation Throughput Test"
echo "   Brokers: $NUM_BROKERS (throttled to $BROKER_RATE each)"
echo "   Client: Unlimited bandwidth"
echo "   Message sizes: ${msg_sizes[@]}"
echo "   Order level: ${orders[@]}"
echo "   CXL: Real NUMA node 2 access (no emulation)"

# Function to cleanup network emulation
cleanup_emulation() {
    echo "🧹 Cleaning up network emulation..."
    
    # Kill any running processes
    pkill -f "./embarlet" >/dev/null 2>&1 || true
    pkill -f "./throughput_test" >/dev/null 2>&1 || true
    
    # Cleanup bind mounts for broker namespaces
    for i in $(seq 1 $NUM_BROKERS); do
        NS_NAME="${BROKER_PREFIX}${i}"
        
        # Unmount bind mounts if they exist
        sudo umount /var/run/netns/$NS_NAME/dev/shm >/dev/null 2>&1 || true
        
        # Remove namespace
        sudo ip netns del "$NS_NAME" >/dev/null 2>&1 || true
    done
    
    # Remove client namespace
    sudo ip netns del $CLIENT_NS >/dev/null 2>&1 || true
    
    # Remove bridge
    sudo ip link del $BRIDGE_NAME >/dev/null 2>&1 || true
    
    echo "✅ Cleanup complete"
}

# Function to setup network emulation
setup_emulation() {
    echo "🔌 Setting up network emulation..."
    
    # Clean up any existing setup
    cleanup_emulation >/dev/null 2>&1 || true
    
    # 1. Create the virtual switch (Linux Bridge)
    echo "   Creating virtual switch: $BRIDGE_NAME"
    sudo ip link add name $BRIDGE_NAME type bridge
    sudo ip link set dev $BRIDGE_NAME up
    
    # 2. Setup the Client Namespace (No Bandwidth Limit)
    echo "   Setting up client namespace: $CLIENT_NS (unlimited bandwidth)"
    sudo ip netns add $CLIENT_NS
    sudo ip link add veth-client type veth peer name veth-client-br
    sudo ip link set veth-client netns $CLIENT_NS
    sudo ip link set veth-client-br master $BRIDGE_NAME
    sudo ip netns exec $CLIENT_NS ip addr add 10.0.0.100/24 dev veth-client
    sudo ip netns exec $CLIENT_NS ip link set dev veth-client up
    sudo ip netns exec $CLIENT_NS ip link set dev lo up
    sudo ip link set dev veth-client-br up
    
    # 3. Setup Broker Namespaces with bandwidth throttling
    echo "   Setting up $NUM_BROKERS broker namespaces (throttled to $BROKER_RATE each)..."
    for i in $(seq 1 $NUM_BROKERS); do
        NS_NAME="${BROKER_PREFIX}${i}"
        VETH_NS="veth-b${i}"
        VETH_BR="veth-b${i}-br"
        IP_ADDR="10.0.0.${i}/24"
        
        echo "     -> Creating $NS_NAME ($IP_ADDR)"
        
        sudo ip netns add $NS_NAME
        sudo ip link add $VETH_NS type veth peer name $VETH_BR
        sudo ip link set $VETH_NS netns $NS_NAME
        sudo ip link set $VETH_BR master $BRIDGE_NAME
        sudo ip netns exec $NS_NAME ip addr add $IP_ADDR dev $VETH_NS
        sudo ip netns exec $NS_NAME ip link set dev $VETH_NS up
        sudo ip netns exec $NS_NAME ip link set dev lo up
        sudo ip link set dev $VETH_BR up
        
        # Apply traffic shaping rule for the broker
        sudo ip netns exec $NS_NAME tc qdisc add dev $VETH_NS root handle 1: htb default 10
        sudo ip netns exec $NS_NAME tc class add dev $VETH_NS parent 1: classid 1:10 htb rate $BROKER_RATE
        
        # CRITICAL: Bind mount /dev/shm for shared memory access
        # This allows CXL shared memory to work across namespaces
        sudo mkdir -p /var/run/netns/$NS_NAME/dev
        sudo mount --bind /dev/shm /var/run/netns/$NS_NAME/dev/shm
        
        echo "     -> Network and shared memory setup complete for $NS_NAME"
    done
    
    echo "✅ Network emulation setup complete"
}

# Function to run throughput test in emulated environment
run_emulated_test() {
    local order=$1
    local msg_size=$2
    
    echo "🧪 Testing Order $order, Message Size ${msg_size}B"
    
    # Navigate to build/bin directory
    if [ -d "build/bin" ]; then
        cd build/bin
    elif [ -d "../build/bin" ]; then
        cd ../build/bin
    else
        echo "Error: Cannot find build/bin directory"
        return 1
    fi
    
    # Configuration
    CONFIG_ARG="--config ../../config/embarcadero.yaml"
    
    # PERF OPTIMIZED: Enable hugepages for 9GB/s+ performance
    export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
    
    # NUMA Optimization: Bind embarlet processes to node 1 (with access to CXL node 2)
    EMBARLET_NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"
    
    # Start brokers in their respective namespaces
    echo "   Starting $NUM_BROKERS brokers in throttled namespaces..."
    broker_pids=()
    
    # Start head broker in broker-ns-1
    echo "     -> Starting head broker in ${BROKER_PREFIX}1 (10.0.0.1)"
    sudo ip netns exec "${BROKER_PREFIX}1" $EMBARLET_NUMA_BIND ./embarlet --head $CONFIG_ARG &
    broker_pids+=($!)
    sleep 3  # Give head broker time to initialize
    
    # Start remaining brokers
    for i in $(seq 2 $NUM_BROKERS); do
        echo "     -> Starting broker in ${BROKER_PREFIX}${i} (10.0.0.${i})"
        sudo ip netns exec "${BROKER_PREFIX}${i}" $EMBARLET_NUMA_BIND ./embarlet --follower 10.0.0.1:12140 $CONFIG_ARG &
        broker_pids+=($!)
        sleep 1
    done
    
    echo "   Waiting for brokers to initialize..."
    sleep 5
    
    # Run throughput test from client namespace (unlimited bandwidth)
    echo "   Running throughput test from client namespace (unlimited bandwidth)..."
    echo "   Command: sudo ip netns exec $CLIENT_NS ./throughput_test -t 1 -o $order --sequencer $sequencer -m $msg_size"
    
    # Run the test in client namespace
    sudo ip netns exec $CLIENT_NS ./throughput_test -t 1 -o $order --sequencer $sequencer -m $msg_size
    test_result=$?
    
    # Cleanup brokers
    echo "   Cleaning up brokers..."
    for pid in "${broker_pids[@]}"; do
        sudo kill $pid >/dev/null 2>&1 || true
    done
    
    # Wait for processes to exit
    sleep 2
    pkill -f "./embarlet" >/dev/null 2>&1 || true
    
    return $test_result
}

# Trap to ensure cleanup on exit
trap cleanup_emulation EXIT

# Main execution
echo "🔧 Setting up network emulation environment..."
setup_emulation

echo ""
echo "🚀 Starting throughput tests..."

# Run tests for each configuration
for order in "${orders[@]}"; do
    for msg_size in "${msg_sizes[@]}"; do
        echo ""
        echo "=================================================="
        echo "Testing Order Level $order, Message Size ${msg_size}B"
        echo "Network: Brokers throttled to $BROKER_RATE, Client unlimited"
        echo "CXL: Real NUMA node 2 access via shared memory"
        echo "=================================================="
        
        if run_emulated_test $order $msg_size; then
            echo "✅ Test completed successfully"
        else
            echo "❌ Test failed"
        fi
        
        echo "Waiting 3 seconds before next test..."
        sleep 3
    done
done

echo ""
echo "🎉 All emulated throughput tests completed!"
echo "   Network emulation will be cleaned up automatically"
