#!/bin/bash
# Ensure .Replication/disk* maps onto distinct physical NVMe devices.
# Idempotent. Prefer bind-mounting /mnt/nvme0/replication/disk1 onto
# $PROJECT_ROOT/.Replication/disk1 when the secondary NVMe is present.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REPL_ROOT="${EMBARCADERO_REPLICA_DISK_ROOT:-$PROJECT_ROOT/.Replication}"
DISK0="$REPL_ROOT/disk0"
DISK1="$REPL_ROOT/disk1"
NVME_DISK1="${EMBARCADERO_NVME_DISK1:-/mnt/nvme0/replication/disk1}"

mkdir -p "$DISK0"
chmod u+rwx "$DISK0" 2>/dev/null || true

if [ -d "$NVME_DISK1" ] || [ -b /dev/nvme0n1 ]; then
  mkdir -p "$NVME_DISK1"
  if mountpoint -q "$DISK1" 2>/dev/null; then
    echo "[repair_replica_disks] $DISK1 already mounted"
  else
    if [ -d "$DISK1" ]; then
      # Replace empty/non-writable stub only.
      if [ -n "$(find "$DISK1" -mindepth 1 -maxdepth 1 2>/dev/null)" ]; then
        echo "[repair_replica_disks] ERROR: $DISK1 is not empty; refusing to replace" >&2
        exit 1
      fi
      if [ ! -w "$DISK1" ]; then
        sudo rm -rf "$DISK1"
      else
        rmdir "$DISK1" 2>/dev/null || sudo rm -rf "$DISK1"
      fi
    fi
    sudo mkdir -p "$DISK1"
    sudo mount --bind "$NVME_DISK1" "$DISK1"
    sudo chown "${SUDO_UID:-$(id -u)}:${SUDO_GID:-$(id -g)}" "$DISK1" 2>/dev/null \
      || sudo chown "$(id -u):$(id -g)" "$DISK1"
    FSTAB_LINE="$NVME_DISK1 $DISK1 none bind 0 0"
    if ! grep -Fqs "$DISK1" /etc/fstab; then
      echo "$FSTAB_LINE" | sudo tee -a /etc/fstab >/dev/null
      echo "[repair_replica_disks] added fstab entry for $DISK1"
    fi
  fi
fi

echo "[repair_replica_disks] layout:"
for d in "$DISK0" "$DISK1"; do
  [ -d "$d" ] || continue
  dev=$(df -P "$d" 2>/dev/null | awk 'NR==2 {print $1}')
  writable=no
  [ -w "$d" ] && writable=yes
  echo "  $d -> $dev writable=$writable"
done
