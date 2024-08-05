#!/bin/bash

sudo ip netns exec embarcadero_netns0 iperf -s > server.log &
sleep 1

sudo ip netns exec embarcadero_netns1 iperf -c 172.18.0.1 > client.single.log
sleep 1

sudo ip netns exec embarcadero_netns1 iperf -c 172.18.0.1 > client1.multiple.log &
IPERF_CLIENT1_PID=$!
sudo ip netns exec embarcadero_netns2 iperf -c 172.18.0.1 > client2.multiple.log &
IPERF_CLIENT2_PID=$!

wait $IPERF_CLIENT1_PID
wait $IPERF_CLIENT2_PID

sudo kill $(pgrep -f "iperf -s")
