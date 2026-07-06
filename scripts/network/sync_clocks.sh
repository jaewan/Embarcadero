#!/bin/bash
LOCAL_IP="192.168.60.8"

for host in c1 c2 c3 c4; do
    echo "=== Configuring $host ==="
    ssh -t $host "sudo systemctl stop chronyd 2>/dev/null; sudo systemctl disable chronyd 2>/dev/null; sudo bash -c 'echo -e \"[Time]\nNTP=$LOCAL_IP\" > /etc/systemd/timesyncd.conf' && sudo systemctl enable systemd-timesyncd && sudo systemctl restart systemd-timesyncd && sudo timedatectl set-ntp true"
done
