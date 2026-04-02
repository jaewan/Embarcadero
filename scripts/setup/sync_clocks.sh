#!/bin/bash
# Synchronize clocks across the cluster using chrony
# Local node (10.10.10.143) acts as the NTP master
# c1, c2, c3, c4 act as NTP clients

set -e

MASTER_IP="10.10.10.143"
NETWORK="10.10.10.0/24"
CLIENTS=("c1" "c2" "c3" "c4")

# Prompt for the sudo password securely once
echo -n "Enter sudo password for all machines: "
read -s SUDO_PASS
echo ""

echo "=== 1. Configuring Local Node (Master) ==="
# Check if allow rule already exists
if ! grep -q "allow $NETWORK" /etc/chrony/chrony.conf; then
    echo "Adding allow rule for $NETWORK to /etc/chrony/chrony.conf"
    echo "$SUDO_PASS" | sudo -S bash -c "echo 'allow $NETWORK' >> /etc/chrony/chrony.conf"
    echo "$SUDO_PASS" | sudo -S systemctl restart chrony || echo "$SUDO_PASS" | sudo -S systemctl restart chronyd
    echo "Local chrony restarted."
else
    echo "Local node already configured to allow $NETWORK."
fi

# Ensure local chrony is synced
echo "Forcing local clock sync..."
echo "$SUDO_PASS" | sudo -S chronyc makestep || true

echo ""
echo "=== 2. Configuring Client Nodes ==="
for CLIENT in "${CLIENTS[@]}"; do
    echo "-> Configuring $CLIENT..."
    
    # Create a configuration script to run on the client
    # We pass the password via stdin to sudo -S on the remote machine
    ssh -o StrictHostKeyChecking=no "$CLIENT" "sudo -S bash -s" <<EOF
$SUDO_PASS
        set -e
        CONF_FILE="/etc/chrony/chrony.conf"
        if [ ! -f "\$CONF_FILE" ]; then
            CONF_FILE="/etc/chrony.conf"
        fi
        
        # Backup original config if not already backed up
        if [ ! -f "\${CONF_FILE}.bak" ]; then
            cp "\$CONF_FILE" "\${CONF_FILE}.bak"
        fi
        
        # Comment out existing pool/server lines
        sed -i 's/^pool/#pool/g' "\$CONF_FILE"
        sed -i 's/^server/#server/g' "\$CONF_FILE"
        
        # Add our master server if not already there
        if ! grep -q "server $MASTER_IP iburst" "\$CONF_FILE"; then
            echo "server $MASTER_IP iburst" >> "\$CONF_FILE"
        fi
        
        # Restart chrony service
        systemctl restart chrony 2>/dev/null || systemctl restart chronyd 2>/dev/null
        
        # Force immediate sync
        sleep 1
        chronyc makestep || true
        
        # Show tracking status
        echo "Status on $CLIENT:"
        chronyc tracking | grep -E "Reference ID|System time|Last offset"
EOF
    echo "----------------------------------------"
done

echo "=== Clock Synchronization Complete ==="
echo "You can verify the offsets from the clients to the master by running:"
echo "for c in c1 c2 c3 c4; do echo \"\$c:\"; ssh \$c chronyc sources; done"
