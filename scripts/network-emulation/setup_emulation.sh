#!/bin/bash

# -- Configuration --
NUM_BROKERS=20
CLIENT_NS="client-ns"
BROKER_PREFIX="broker-ns-"
BRIDGE_NAME="br0"
# 0.5 GigaBytes/s = 4 Gigabit/s
BROKER_RATE="4gbit" 

# Exit on any error
set -e

echo "🚀 Starting emulation setup for 1 client and $NUM_BROKERS brokers..."

# 1. Create the virtual switch (Linux Bridge)
echo "🔌 Creating virtual switch: $BRIDGE_NAME"
sudo ip link add name $BRIDGE_NAME type bridge
sudo ip link set dev $BRIDGE_NAME up

# 2. Setup the Client Namespace (No Bandwidth Limit)
echo "👤 Setting up client namespace: $CLIENT_NS"
sudo ip netns add $CLIENT_NS
# Create veth pair for the client
sudo ip link add veth-client type veth peer name veth-client-br
# Move one end into the namespace
sudo ip link set veth-client netns $CLIENT_NS
# Attach the other end to the bridge
sudo ip link set veth-client-br master $BRIDGE_NAME
# Configure the interface inside the namespace
sudo ip netns exec $CLIENT_NS ip addr add 10.0.0.100/24 dev veth-client
sudo ip netns exec $CLIENT_NS ip link set dev veth-client up
sudo ip netns exec $CLIENT_NS ip link set dev lo up
# Bring up the bridge-facing interface
sudo ip link set dev veth-client-br up

echo "✅ Client namespace is ready without any rate limits."

# 3. Loop to Setup Broker Namespaces
echo "🤖 Setting up $NUM_BROKERS broker namespaces..."
for i in $(seq 1 $NUM_BROKERS)
do
  NS_NAME="${BROKER_PREFIX}${i}"
  VETH_NS="veth-b${i}"
  VETH_BR="veth-b${i}-br"
  IP_ADDR="10.0.0.${i}/24"

  echo "  -> Creating $NS_NAME ($IP_ADDR) with a $BROKER_RATE rate limit"

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
done

echo "✅ Emulation environment is ready."
