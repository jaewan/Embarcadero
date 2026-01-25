#!/bin/bash

# Quick test to validate scaling experiment setup
# Tests single broker configuration before running full experiment

set -e

echo "🧪 Testing Scaling Experiment Setup"
echo "   This will test 4-broker configuration to validate the setup"

# Test with 4 brokers (known working configuration)
TEST_BROKERS=4
MESSAGE_SIZE=1024

# Create test directory
mkdir -p data

cd "$(dirname "$0")/.."

# Generate test configuration
echo "📝 Generating test configuration for $TEST_BROKERS brokers..."

# Calculate resources (same logic as main script)
SEGMENT_SIZE_GB=4  # 15GB / 4 brokers ≈ 4GB per broker
BUFFER_SIZE_MB=1024  # 1GB for 4 brokers

cat > config/scaling_test.yaml << EOF
embarcadero:
  version:
    major: 1
    minor: 0
  broker:
    port: 1214
    broker_port: 12140
    heartbeat_interval: 3
    max_brokers: $TEST_BROKERS
    cgroup_core: 85
  cxl:
    size: 68719476736
    emulation_size: 34359738368
    device_path: "/dev/dax0.0"
    numa_node: 2
  storage:
    segment_size: 4294967296  # 4GB
    batch_headers_size: 65536
    batch_size: 2097152
    num_disks: 2
    max_topics: 32
    topic_name_size: 31
  network:
    io_threads: 4
    disk_io_threads: 4
    sub_connections: 1
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
      threads_per_broker: 1
      buffer_size_mb: $BUFFER_SIZE_MB
      batch_size_kb: 2048
    subscriber:
      connections_per_broker: 1
      buffer_size_mb: $BUFFER_SIZE_MB
    network:
      connect_timeout_ms: 5000
      send_timeout_ms: 10000
      recv_timeout_ms: 10000
    performance:
      use_hugepages: true
      numa_bind: true
      zero_copy: true
EOF

echo "✅ Test configuration generated: config/scaling_test.yaml"

# Setup TC for test
echo "🔧 Setting up TC for $TEST_BROKERS brokers..."
sudo tc qdisc del dev lo root >/dev/null 2>&1 || true
sudo tc qdisc add dev lo root handle 1: htb default 99 r2q 10

for i in $(seq 1 $TEST_BROKERS); do
    sudo tc class add dev lo parent 1: classid 1:1$i htb rate 4gbit quantum 10000
    echo "   Created class 1:1$i for broker $i: 4gbit"
done

sudo tc class add dev lo parent 1: classid 1:99 htb rate 100gbit quantum 10000

for i in $(seq 1 $TEST_BROKERS); do
    broker_data_port=$((1213 + i))
    sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip sport $broker_data_port 0xffff flowid 1:1$i
    sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip dport $broker_data_port 0xffff flowid 1:1$i
    echo "   Broker $i: port $broker_data_port → 4gbit"
done

echo "✅ TC configured for $TEST_BROKERS brokers"

# Cleanup function
cleanup_test() {
    echo "🧹 Cleaning up test..."
    pkill -f "./embarlet" >/dev/null 2>&1 || true
    pkill -f "./throughput_test" >/dev/null 2>&1 || true
    sudo tc qdisc del dev lo root >/dev/null 2>&1 || true
    echo "✅ Test cleanup complete"
}

trap cleanup_test EXIT

# Run test
echo "🚀 Running scaling setup test..."

cd build/bin || { echo "Error: build/bin not found"; exit 1; }

CONFIG_ARG="--config ../../config/scaling_test.yaml"
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
NUMA_BIND="numactl --cpunodebind=1 --membind=1,2"

echo "   Starting $TEST_BROKERS brokers..."
broker_pids=()

# Start head broker
echo "     -> Head broker"
$NUMA_BIND ./embarlet --head $CONFIG_ARG &
broker_pids+=($!)
sleep 3

# Start follower brokers
for i in $(seq 2 $TEST_BROKERS); do
    echo "     -> Broker $i"
    $NUMA_BIND ./embarlet $CONFIG_ARG &
    broker_pids+=($!)
    sleep 1
done

echo "   Waiting for cluster initialization..."
sleep 8

echo "   Running publish-only test..."
start_time=$(date +%s)

if timeout 120 ./throughput_test $CONFIG_ARG -t 5 -o 5 --sequencer EMBARCADERO -m $MESSAGE_SIZE -n 1; then
    end_time=$(date +%s)
    duration=$((end_time - start_time))
    
    echo ""
    echo "✅ SCALING SETUP TEST SUCCESSFUL!"
    echo "   Duration: ${duration}s"
    echo "   Configuration: $TEST_BROKERS brokers, 4GB segments, 1GB buffers"
    echo ""
    echo "🎯 Ready to run full scaling experiment:"
    echo "   ./scripts/broker_scaling_experiment.sh"
    
else
    echo ""
    echo "❌ SCALING SETUP TEST FAILED!"
    echo "   Check configuration and try again"
    exit 1
fi
