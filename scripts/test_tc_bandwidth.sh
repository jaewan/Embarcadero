#!/bin/bash

# Simple test to measure actual TC bandwidth limitations
echo "🔍 Testing TC bandwidth limitations..."

# Clean up any existing TC rules
sudo tc qdisc del dev lo root 2>/dev/null || true

# Set up TC with same configuration as the main script
echo "Setting up TC with 4Gbps limit on port 1214..."
sudo tc qdisc add dev lo root handle 1: htb default 99
sudo tc class add dev lo parent 1: classid 1:1 htb rate 4gbit quantum 10000
sudo tc class add dev lo parent 1: classid 1:99 htb rate 100gbit

# Add filter for port 1214 (broker 1)
sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip sport 1214 0xffff flowid 1:1
sudo tc filter add dev lo protocol ip parent 1:0 prio 1 u32 match ip dport 1214 0xffff flowid 1:1

echo "✅ TC configured for port 1214 with 4Gbps limit"

# Test with iperf3 if available, or use a simple netcat test
if command -v iperf3 >/dev/null 2>&1; then
    echo "Testing bandwidth with iperf3..."
    # Start iperf3 server on port 1214 in background
    iperf3 -s -p 1214 &
    SERVER_PID=$!
    sleep 2
    
    # Run client test for 10 seconds
    echo "Running 10-second bandwidth test..."
    iperf3 -c 127.0.0.1 -p 1214 -t 10
    
    # Clean up
    kill $SERVER_PID 2>/dev/null || true
else
    echo "iperf3 not available, skipping bandwidth test"
fi

# Clean up TC rules
echo "Cleaning up TC rules..."
sudo tc qdisc del dev lo root 2>/dev/null || true

echo "✅ TC bandwidth test completed"
