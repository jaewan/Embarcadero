#!/bin/bash

# run_clients.sh on mos181



SERVER_IP="10.10.10.143"

DURATION=30

STREAMS=8



echo "Starting $STREAMS parallel iperf3 instances to $SERVER_IP..."

echo "Running test for $DURATION seconds. Please wait..."



for i in $(seq 1 $STREAMS); do

# -c: client, -p: port, -t: time, -f g: output in Gbps, -Z: zero-copy (saves CPU)

numactl --cpunode=0 --membind=0 iperf3 -c $SERVER_IP -p $((5200+i)) -t $DURATION -f g -Z > /tmp/iperf_client_${i}.log &

done



# Wait for all background client processes to finish

wait



echo "--- Test Complete ---"

# Parse the logs to extract the final sender bitrate and sum them up

TOTAL_BW=$(grep "sender" /tmp/iperf_client_*.log | awk '{sum += $7} END {print sum}')



echo "Individual Stream Results (Gbps):"

grep "sender" /tmp/iperf_client_*.log | awk '{print "Stream: " $7 " Gbps"}'

echo "---------------------------------"

echo "TOTAL MAXIMUM BANDWIDTH: $TOTAL_BW Gbits/sec"
