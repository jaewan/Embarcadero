#!/bin/bash

# Clean up dependencies

# abseil
rm -rf abseil-cpp
# folly: must use sudo due to python cache permissions
sudo rm -rf folly

# TODO: cleanup CXL emulation
