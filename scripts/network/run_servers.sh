#!/bin/bash
# run_servers.sh on moscxl

echo "Cleaning up any old iperf3 instances..."
killall iperf3 2>/dev/null

echo "Starting 8 independent iperf3 servers..."
for i in {1..8}; do
    # -s: server mode, -p: port, -D: run as daemon
    iperf3 -s -p $((5200+i)) -D
done

echo "Servers running in the background on ports 5201 to 5208."
