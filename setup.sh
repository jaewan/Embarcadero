#! /bin/bash

# Run this script the first time you create this project

# Install System dependencies
function Install_Dependencies()
{
	echo "Installing Dependencies"
	sudo apt install numactl
	sudo apt install cmake
}

function Create_Third_Party_Directory()
{
	mkdir third_party
	cd third_party
}

# Abseil
function Install_Abseil()
{
	echo "Installing Abseil"
	start_dir=$(pwd)
	git clone https://github.com/abseil/abseil-cpp.git
	cd abseil-cpp
	mkdir build && cd build
	cmake ..
	cmake --build . --target all
	cd $start_dir
}

# gRPC
function Install_gRPC()
{
	echo "Installing gRPC"
	start_dir=$(pwd)
	git clone --recurse-submodules -b v1.62.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc
	cd grpc
	mkdir -p cmake/build
	cd cmake/build
	cmake -DgRPC_INSTALL=ON \
      -DgRPC_BUILD_TESTS=OFF \
      -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR \
      ../..
	make -j 4
	sudo make install
	cd $start_dir
}

# Cxxopts
function Install_Cxxopts()
{
	echo "Installing cxxopts"
	start_dir=$(pwd)
	git clone https://github.com/jarro2783/cxxopts
	cd cxxopts
	mkdir build && cd build
	cmake ..
	cmake --build . --target all
	cd $start_dir
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

function Build_Embarcadero()
{
    cd ~/Embarcadero
	echo "Building Embarcadero"
	mkdir build
	cd build
	cmake ..
	cmake --build . --target all
}


##################### Execute ############################
Install_Dependencies
Create_Third_Party_Directory
Install_Abseil
install_gRPC
Install_Cxxopts
Setup_CXL
Build_Embarcadero
