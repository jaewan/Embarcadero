#!/bin/bash

NUM_BROKERS=20
CLIENT_NS="client-ns"
BROKER_PREFIX="broker-ns-"
BRIDGE_NAME="br0"

echo "🧹 Cleaning up emulation environment..."

# It's safer to check if the namespace exists before trying to delete it
if sudo ip netns list | grep -q $CLIENT_NS; then
    echo "  -> Deleting client namespace: $CLIENT_NS"
    sudo ip netns del $CLIENT_NS
fi

for i in $(seq 1 $NUM_BROKERS)
do
  NS_NAME="${BROKER_PREFIX}${i}"
  if sudo ip netns list | grep -q $NS_NAME; then
    echo "  -> Deleting broker namespace: $NS_NAME"
    sudo ip netns del $NS_NAME
  fi
done

# It's safer to check if the bridge exists before trying to delete it
if ip link show $BRIDGE_NAME > /dev/null 2>&1; then
    echo "  -> Deleting bridge: $BRIDGE_NAME"
    # The veth pairs are deleted automatically when the namespaces are deleted.
    # We just need to delete the bridge itself.
    sudo ip link set $BRIDGE_NAME down
    sudo ip link del $BRIDGE_NAME
fi

echo "✅ Cleanup complete."
