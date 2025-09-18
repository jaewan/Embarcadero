#!/bin/bash

# TC Traffic Control Cleanup Script
# Cleans up traffic shaping rules left by run_tc_emulated_throughput.sh

echo "🧹 Cleaning up TC traffic control configuration..."

# Remove all qdisc rules on loopback interface
echo "Removing qdisc rules on lo interface..."
sudo tc qdisc del dev lo root 2>/dev/null || echo "  No root qdisc found on lo (already clean)"

# Check for any remaining rules
echo "Checking remaining TC configuration:"
tc qdisc show dev lo

# Kill any remaining processes that might be holding ports
echo "Checking for hanging processes..."
pkill -f "embarlet" 2>/dev/null || echo "  No embarlet processes found"
pkill -f "throughput_test" 2>/dev/null || echo "  No throughput_test processes found"

# Check port usage for broker ports (1214-1233)
echo "Checking broker port usage:"
for port in {1214..1233}; do
    if lsof -i :$port >/dev/null 2>&1; then
        echo "  Port $port is still in use:"
        lsof -i :$port
    fi
done

# Check heartbeat ports (12140-12159)
echo "Checking heartbeat port usage:"
for port in {12140..12159}; do
    if lsof -i :$port >/dev/null 2>&1; then
        echo "  Port $port is still in use:"
        lsof -i :$port
    fi
done

# Clean up any shared memory segments
echo "Cleaning up shared memory segments..."
ipcs -m | grep $(whoami) | awk '{print $2}' | xargs -r ipcrm -m 2>/dev/null || echo "  No shared memory segments to clean"

echo "✅ TC cleanup completed!"
echo ""
echo "💡 Usage: Run this script after interrupting run_tc_emulated_throughput.sh"
echo "   Example: bash scripts/cleanup_tc.sh"
