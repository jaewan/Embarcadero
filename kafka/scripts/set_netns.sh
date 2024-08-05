#!/bin/bash

sudo modprobe ifb
sudo iptables --policy FORWARD ACCEPT

# Create bridge
sudo ip link add embarcadero_br0 type bridge
sudo ip link set embarcadero_br0 up

for i in {0..5}; do
	# Create netns, peer link
	sudo ip netns add embarcadero_netns$i 
	sudo ip link add embar_brid$i type veth peer name embar_veth$i
	sudo ip link set embar_veth$i netns embarcadero_netns$i
	sudo ip netns exec embarcadero_netns$i ip address add 172.18.$i.1/16 dev embar_veth$i
	sudo ip netns exec embarcadero_netns$i ip link set dev lo up
	sudo ip netns exec embarcadero_netns$i ip link set dev embar_veth$i up
	sudo ip link set embar_brid$i master embarcadero_br0
	sudo ip link set dev embar_brid$i up

	# Set tc
	sudo ip link add embar_ifb$i type ifb
	sudo ip link set dev embar_ifb$i up
	sudo tc qdisc add dev embar_brid$i handle ffff: ingress
	sudo tc filter add dev embar_brid$i parent ffff: protocol all u32 match u32 0 0 action mirred egress redirect dev embar_ifb$i
	sudo tc qdisc add dev embar_ifb$i root tbf rate 16gbit burst 200m latency 100ms
	sudo tc qdisc add dev embar_brid$i root tbf rate 16gbit burst 200m latency 100ms
done
