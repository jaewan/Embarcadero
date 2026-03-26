 # ── Hugepages ────────────────────────────────────────────────────────────────
  # Need ≥ 8200 on node 1 (used by numactl --membind=1). Current node1 = 6656.
  echo 8200 | sudo tee /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages
  # Verify
  grep -E 'HugePages_(Total|Free)|Hugepagesize' /proc/meminfo
  cat /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

  # ── /dev/hugepages permissions (alternative to MAP_HUGETLB anonymous) ────────
  sudo chmod 1777 /dev/hugepages    # sticky + world-writable (same as /tmp)
  # or add the domin user to a hugepages group if your distro uses one

  # ── Socket buffer limits ─────────────────────────────────────────────────────
  sudo sysctl -w net.core.rmem_max=268435456
  sudo sysctl -w net.core.wmem_max=268435456
  sudo sysctl -w net.ipv4.tcp_rmem='4096 65536 268435456'
  sudo sysctl -w net.ipv4.tcp_wmem='4096 65536 268435456'

  # ── NIC ring buffers (100G: ens801f0np0) ─────────────────────────────────────
  sudo ethtool -G ens801f0np0 rx 8192 tx 8192

  # ── Make hugepages and sysctl persistent across reboots ──────────────────────
  # Add to /etc/sysctl.conf:
  echo "vm.nr_hugepages = 16400" | sudo tee -a /etc/sysctl.conf
  echo "net.core.rmem_max = 268435456" | sudo tee -a /etc/sysctl.conf
  echo "net.core.wmem_max = 268435456" | sudo tee -a /etc/sysctl.conf
  echo "net.ipv4.tcp_rmem = 4096 65536 268435456" | sudo tee -a /etc/sysctl.conf
  echo "net.ipv4.tcp_wmem = 4096 65536 268435456" | sudo tee -a /etc/sysctl.conf

  # Add to /etc/rc.local or a systemd unit for NIC ring buffer:
  # ethtool -G ens801f0np0 rx 8192 tx 8192
