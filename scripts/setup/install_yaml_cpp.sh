#!/bin/bash
# Install yaml-cpp library for configuration management

set -e

echo "Installing yaml-cpp library..."

# Check if running as root or with sudo
if [ "$EUID" -ne 0 ]; then 
    echo "This script needs to be run with sudo"
    exit 1
fi

# Install yaml-cpp from package manager
if command -v apt-get &> /dev/null; then
    # Debian/Ubuntu
    apt-get update
    apt-get install -y libyaml-cpp-dev
elif command -v yum &> /dev/null; then
    # RHEL/CentOS
    yum install -y yaml-cpp-devel
elif command -v dnf &> /dev/null; then
    # Fedora
    dnf install -y yaml-cpp-devel
elif command -v pacman &> /dev/null; then
    # Arch Linux
    pacman -S --noconfirm yaml-cpp
else
    echo "Unsupported package manager. Please install yaml-cpp manually."
    exit 1
fi

echo "yaml-cpp installation completed successfully!"
