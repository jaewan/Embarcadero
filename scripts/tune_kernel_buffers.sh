#!/bin/bash
# Tune kernel TCP/socket buffer limits so SO_RCVBUF/SO_SNDBUF (e.g. 32 MB) take effect.
# Without this, Linux caps socket buffers to net.core.rmem_max/wmem_max (often ~208 KB).
# Run as root (e.g. sudo). See docs/SEND_PAYLOAD_EPOLL_AND_RECV_INVESTIGATION.md.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# 32 MB minimum for broker/publisher ACK; 128 MB so client data sockets (128 MB) also take effect
SIZE_MB="${EMBARCADERO_KERNEL_BUF_MB:-128}"
SIZE_BYTES=$((SIZE_MB * 1024 * 1024))

echo "Tuning kernel socket buffers to allow up to ${SIZE_MB} MB per socket (bytes=$SIZE_BYTES)..."

# rmem_max / wmem_max: max size for SO_RCVBUF / SO_SNDBUF
sudo sysctl -w net.core.rmem_max=$SIZE_BYTES
sudo sysctl -w net.core.wmem_max=$SIZE_BYTES

# Optional: raise TCP default/max (min, default, max) so new sockets get larger buffers by default
sudo sysctl -w net.ipv4.tcp_rmem="4096 87380 $SIZE_BYTES"
sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 $SIZE_BYTES"

echo "Current values:"
sysctl net.core.rmem_max net.core.wmem_max net.ipv4.tcp_rmem net.ipv4.tcp_wmem

echo "To make persistent, add to /etc/sysctl.d/99-embarcadero.conf:"
echo "  net.core.rmem_max=$SIZE_BYTES"
echo "  net.core.wmem_max=$SIZE_BYTES"
echo "  net.ipv4.tcp_rmem=4096 87380 $SIZE_BYTES"
echo "  net.ipv4.tcp_wmem=4096 65536 $SIZE_BYTES"
