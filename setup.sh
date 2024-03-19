#! /bin/bash

# Run this script the first time you create this project
set -ex

# Install System dependencies
function Install_Dependencies()
{
	echo "Installing Dependencies"
	sudo apt update
	sudo apt install -y numactl
	sudo apt install -y cmake
	sudo apt install -y libboost-all-dev
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
	sudo apt install -y libssl-dev libfmt-dev pkg-config

	# Build and install folly
	#cd build
	#cmake ..
	#cmake --build . --target all
	sudo ./build.sh --install-dir /usr/local/
	cd $start_dir
}


# gRPC
function Install_gRPC()
{
	echo "Installing gRPC"
	start_dir=$(pwd)

	# These packages are required to build gRPC per the documentation: https://grpc.io/docs/languages/cpp/quickstart/
	# Please do not remove this from the build script.
	sudo apt install -y build-essential autoconf libtool pkg-config

	git clone --recurse-submodules -b v1.62.0 --depth 1 --shallow-submodules https://github.com/grpc/grpc
	cd grpc
	mkdir -p cmake/build
	cd cmake/build

	# Not sure if this issue is still relevant: https://github.com/grpc/grpc/issues/13841
	# But seems to have install errors if you don't use packages for everything.
	cmake -DgRPC_INSTALL=ON \
		-DgRPC_BUILD_TESTS=OFF \
		-DgRPC_PROTOBUF_PROVIDER=package \
		-DgRPC_ZLIB_PROVIDER=package \
		-DgRPC_CARES_PROVIDER=package \
		-DgRPC_SSL_PROVIDER=package \
		-DCMAKE_BUILD_TYPE=Release \
		-DgRPC_ABSL_PROVIDER=package \
	  	-DgRPC_BUILD_GRPC_CSHARP_PLUGIN=off \
      	-DgRPC_BUILD_GRPC_NODE_PLUGIN=off \
      	-DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=off \
      	-DgRPC_BUILD_GRPC_PHP_PLUGIN=off \
	  	-DgRPC_BUILD_GRPC_PYTHON_PLUGIN=off \
	  	-DgRPC_BUILD_GRPC_RUBY_PLUGIN=off \
		../.. 

	make -j 4
	sudo make install
	cd $start_dir
}

# Cxxopts
function Install_Cxxopts()
{
	echo "Installing cxxopts"
	#start_dir=$(pwd)
	git clone https://github.com/jarro2783/cxxopts

	# Note: we install this in the CMakeLists by adding this as a subdirectory
	# So there is no need to build here.

	#cd cxxopts
	#mkdir build && cd build
	#cmake ..
	#cmake --build . --target all
	#cd $start_dir
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
	echo "Building Embarcadero"
	mkdir build
	cd build
	cmake ..
	cmake --build . --target all
}



##################### Execute ############################
current_dir=$(pwd)
Install_Dependencies
Create_Third_Party_Directory
Install_Abseil
Install_gRPC
Install_Folly
Install_Cxxopts
Setup_CXL
cd $current_dir
Build_Embarcadero
