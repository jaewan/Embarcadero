#!/bin/bash

# Clean up dependencies
# Note: some things from folly require the sudo to rm them
sudo rm -rf third_party

# Clean up build directory
rm -rf build

# TODO: cleanup CXL emulation
