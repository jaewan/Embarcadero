#!/bin/bash
set -e

function setup_cxl() {
    echo "Setting up CXL Emulation on numa node 1"
    
    # Check if running as root or with sudo
    if [[ $EUID -ne 0 ]]; then
        echo "This script must be run with sudo privileges"
        exit 1
    fi

    # Check if numa is available
    if ! command -v numactl &> /dev/null; then
        echo "numactl is not installed. Please install it first."
        exit 1
    fi

    # Create CXL directory if it doesn't exist
    if [[ ! -d "/mnt/CXL_DIR" ]]; then
        mkdir -p /mnt/CXL_DIR
    fi

    # Set ownership
    chown $SUDO_USER:$SUDO_USER /mnt/CXL_DIR

    # Mount tmpfs with numa node 1 binding
    numactl --membind=1 mount -t tmpfs tmpfs /mnt/CXL_DIR/ -o size=128G

    echo "CXL emulation setup complete"
    echo "Mounted 128GB tmpfs on /mnt/CXL_DIR bound to NUMA node 1"
}

setup_cxl
