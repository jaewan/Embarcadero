#! /bin/bash

# Run this script the first time you create this project


# Install System dependencies
function Install_Dependencies()
{
	echo "Installing Dependencies"
	sudo apt install numactl
}

# Abseil
function Install_Abseil()
{
	echo "Installing Abseil"
	git clone https://github.com/abseil/abseil-cpp.git
	cd abseil-cpp
	mkdir build && cd build
	git clone https://github.com/abseil/abseil-cpp.git
	cmake --build . --target all
}

# Mount Node:1 memory by tmpfs and create 30GB of file
function Setup_CXL()
{
	echo "Setting up CXL Emulation"
	mkdir ~/.CXL_EMUL
	mount -t tmpfs -o size=31g tmpfs ~/.CXL_EMUL
	sudo mount -o remount,mpol=bind:1 ~/.CXL_EMUL/
	truncate -s 30G ~/.CXL_EMUL/cxl
}


##################### Execute ############################
Setup_CXL
