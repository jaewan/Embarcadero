#! /bin/bash

# Run this script the first time you create this project

EXTERNAL_DEP_DIR="external"

# Install System dependencies
function Install_Dependencies()
{
	sudo apt install -y cmake numactl
}

# Abseil
function Install_Abseil()
{
	echo "Installing Abseil"
	cd $EXTERNAL_DEP_DIR
	git clone https://github.com/abseil/abseil-cpp.git
	cd abseil-cpp
	mkdir build && cd build
	cmake ..
	cmake --build . --target all
}

# Mount Node:1 memory by tmpfs and create 30GB of file
function Setup_CXL()
{
	echo "Setting up CXL Emulation"
	mkdir -p ~/.CXL_EMUL
	mount -t tmpfs -o size=31g tmpfs ~/.CXL_EMUL
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
Setup_CXL
Build_Embarcadero
