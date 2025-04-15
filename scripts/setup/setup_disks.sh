#!/bin/bash

###########################################################################
# Script: setup_disk.sh
#
# User TODO:
# Rename localdisk to disk0 if you want to use localdisk as well.
# Otherwise rename the last disk to disk0 for Embarcadero to correctly run
#
# Description:
#   This script prepares storage directories for high-performance,
#   multi-threaded file writing by detecting, mounting, and organizing
#   available data disks on the system.
#
#   It safely identifies all usable data disks (excluding system/root/EFI),
#   mounts them (if not already mounted), and creates bind-mount directories
#   under the project-local `.Replication/` directory.
#
#   Additionally, it creates a `localdisk` directory under `.Replication/`,
#   which points to the current root disk, allowing the application to
#   optionally write data to the root disk in addition to external ones.
#
# Behavior:
#   - Idempotent: Safe to run multiple times. Skips already-mounted disks.
#   - Non-destructive: No formatting or partitioning without user confirmation.
#   - Safe: Skips system disks and EFI partitions.
#   - Organized: All mounts are placed under `../../.Replication/` relative to
#     the current working directory.
#
# Structure Created:
#   .Replication/
#   ├── disk1/        <-- bind-mounted usable data disk
#   ├── disk2/        <-- another usable data disk (if available)
#   ├── localdisk/    <-- directory on current root disk
#   └── .raw/         <-- internal mount location (not used directly)
#
# Usage:
#   Run this script from within your project directory before starting
#   any file-writing jobs. Your application can then write files evenly across:
#
#     ../../.Replication/disk1
#     ../../.Replication/disk2
#     ../../.Replication/localdisk
#
# Requirements:
#   - Linux (tested on Ubuntu)
#   - Must be run with permissions to use `mount`, `parted`, and `mkfs.ext4`
#
# Note:
#   If new disks are added later, re-running this script will detect and mount them.
#
###########################################################################

set -euo pipefail

# Get absolute path to the .Replication directory (relative to current directory)
BASE_DIR="$(realpath ../../.Replication)"
RAW_MOUNT_BASE="$BASE_DIR/.raw"
MOUNT_BASE="$BASE_DIR"
DISK_PREFIX="disk"

# Get current user (for chown)
CURRENT_USER="${SUDO_USER:-$USER}"
CURRENT_UID=$(id -u "$CURRENT_USER")
CURRENT_GID=$(id -g "$CURRENT_USER")

# Create base directories
mkdir -p "$RAW_MOUNT_BASE"
mkdir -p "$MOUNT_BASE"

echo "[INFO] Scanning for available disks..."

# Get list of non-loop, non-ram disks
mapfile -t disk_list < <(lsblk -dpno NAME,TYPE | awk '$2 == "disk" && $1 !~ /loop/ && $1 !~ /ram/ { print $1 }')

if [[ ${#disk_list[@]} -eq 0 ]]; then
    echo "[ERROR] No physical disks found."
    exit 1
fi

# Find next available disk number (monotonic)
find_next_disk_number() {
    local max=0
    for d in "$MOUNT_BASE"/${DISK_PREFIX}[0-9]*; do
        [[ -e "$d" ]] || continue
        num="${d##*$DISK_PREFIX}"
        if [[ "$num" =~ ^[0-9]+$ && "$num" -gt "$max" ]]; then
            max="$num"
        fi
    done
    echo $((max + 1))
}

for disk in "${disk_list[@]}"; do
    echo "[INFO] Processing disk: $disk"

    # Skip root/system disk
    if lsblk -lnpo MOUNTPOINT "$disk" | grep -qE '^/$'; then
        echo "[INFO] $disk is the root disk. Skipping for mounting..."
        continue
    fi

    # Skip if mounted elsewhere
    if lsblk -no MOUNTPOINT "$disk" | grep -q '/' && ! lsblk -no MOUNTPOINT "$disk" | grep -q "$BASE_DIR"; then
        echo "[INFO] $disk is in use outside of .Replication. Skipping..."
        continue
    fi

    # Get partitions
    mapfile -t children < <(lsblk -lnpo NAME "$disk" | tail -n +2)

    if [[ ${#children[@]} -eq 0 ]]; then
        echo "[WARNING] Disk $disk has no partitions."
        read -p "Do you want to partition and format $disk as ext4? [y/N]: " confirm
        if [[ "$confirm" =~ ^[Yy]$ ]]; then
            sudo parted -s "$disk" mklabel gpt
            sudo parted -s "$disk" mkpart primary ext4 0% 100%
            sleep 2
            mapfile -t children < <(lsblk -lnpo NAME "$disk" | tail -n +2)
        else
            echo "[INFO] Skipping $disk"
            continue
        fi
    fi

    # Choose the largest ext4 partition
    device=""
    largest_size=0
    for part in "${children[@]}"; do
        size=$(lsblk -bno SIZE "$part")
        fstype=$(blkid -s TYPE -o value "$part" 2>/dev/null || true)

        if [[ "$fstype" == "ext4" && "$size" -gt "$largest_size" ]]; then
            device="$part"
            largest_size="$size"
        fi
    done

    if [[ -z "$device" ]]; then
        echo "[WARNING] No usable ext4 partition found on $disk."
        read -p "Do you want to format the largest partition as ext4? [y/N]: " confirm
        if [[ "$confirm" =~ ^[Yy]$ ]]; then
            largest_part=""
            largest_size=0
            for part in "${children[@]}"; do
                size=$(lsblk -bno SIZE "$part")
                if [[ "$size" -gt "$largest_size" ]]; then
                    largest_part="$part"
                    largest_size="$size"
                fi
            done

            if [[ -n "$largest_part" ]]; then
                echo "[INFO] Formatting $largest_part as ext4"
                sudo mkfs.ext4 -F "$largest_part"
                device="$largest_part"
            else
                echo "[ERROR] No partition found to format on $disk"
                continue
            fi
        else
            echo "[INFO] Skipping $disk"
            continue
        fi
    fi

    # Check if already mounted under .Replication
    if grep -q "$device" /proc/mounts && mount | grep -q "$MOUNT_BASE"; then
        echo "[INFO] $device already mounted under .Replication. Skipping..."
        continue
    fi

    # Mount if not mounted
    mount_path=$(lsblk -no MOUNTPOINT "$device")
    if [[ -z "$mount_path" ]]; then
        next_num=$(find_next_disk_number)
        raw_mount_point="$RAW_MOUNT_BASE/${DISK_PREFIX}${next_num}"
        echo "[INFO] Mounting $device to $raw_mount_point"
        sudo mkdir -p "$raw_mount_point"
        sudo mount "$device" "$raw_mount_point"
        sudo chown "$CURRENT_UID:$CURRENT_GID" "$raw_mount_point"
        mount_path="$raw_mount_point"
    fi

    # Bind mount to final location
    next_num=$(find_next_disk_number)
    final_mount_point="$MOUNT_BASE/${DISK_PREFIX}${next_num}"
    if [[ ! -d "$final_mount_point" || ! $(mount | grep "on $final_mount_point ") ]]; then
        echo "[INFO] Bind mounting $mount_path to $final_mount_point"
        sudo mkdir -p "$final_mount_point"
        sudo mount --bind "$mount_path" "$final_mount_point"
        sudo chown "$CURRENT_UID:$CURRENT_GID" "$final_mount_point"
    else
        echo "[INFO] $final_mount_point already mounted. Skipping bind mount."
    fi
done

# Add localdisk on root disk (if not exists)
localdisk_path="$MOUNT_BASE/localdisk"
if [[ ! -d "$localdisk_path" ]]; then
    echo "[INFO] Creating localdisk directory at $localdisk_path"
    mkdir -p "$localdisk_path"
else
    echo "[INFO] localdisk directory already exists at $localdisk_path"
fi

# Ensure ownership
sudo chown "$CURRENT_UID:$CURRENT_GID" "$localdisk_path"

echo "[INFO] Final disk directories created:"
ls -l "$MOUNT_BASE" | grep -E "${DISK_PREFIX}[0-9]+|localdisk"
