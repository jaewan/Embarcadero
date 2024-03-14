#! /bin/bash

# Run this script the first time you create this project

set -x

# Install System dependencies
function Install_Dependencies()
{
	sudo apt install -y cmake numactl
}

# Abseil
function Install_Abseil()
{
	echo "Installing Abseil"
	start_dir=$(pwd)
	git clone https://github.com/abseil/abseil-cpp.git
	cd abseil-cpp
	git checkout 20240116.1 
	mkdir build && cd build
	cmake .. -DABSL_ENABLE_INSTALL=ON -DABSL_USE_EXTERNAL_GOOGLETEST=ON -DABSL_FIND_GOOGLETEST=ON
	sudo cmake  --build . --target install
	cd $start_dir
}

# We use folly's MPMCQeue
function Install_Folly()
{
	echo "Installing Folly"
	start_dir=$(pwd)

	# Clone the repo and set the version
	git clone https://github.com/facebook/folly.git
	cd folly
	git checkout v2024.03.11.00
	
	# Install dependencies. I don't know why the script misses some
	sudo ./build/fbcode_builder/getdeps.py install-system-deps --recursive
	sudo apt install -y libssl-dev libfmt-dev
	
	# Build and install folly
	cd build
	cmake ..
	make
	sudo make install

	cd $start_dir
}

# Mount Node:1 memory by tmpfs and create 30GB of file
function Setup_CXL()
{
	echo "Setting up CXL Emulation"
	mkdir -p ~/.CXL_EMUL
	sudo mount -t tmpfs -o size=31g tmpfs ~/.CXL_EMUL
	sudo mount -o remount,mpol=bind:1 ~/.CXL_EMUL/
	truncate -s 30G ~/.CXL_EMUL/cxl
}

function Build_Embarcadero()
{
	echo "Building Embacadero"
	mkdir build
	cd build
	cmake ..
	cmake --build . --target all
}


##################### Execute ############################
Install_Dependencies
Install_Abseil
Install_Folly
Setup_CXL
Build_Embarcadero
