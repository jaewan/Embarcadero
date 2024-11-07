# Run this script the first time you create this project
#!/bin/bash

set -xe

DO_CLEAN=false

function Clean_Previous_Artifacts()
{
	rm -rf build
	rm -rf third_party
	rm -rf ~/.CXL_EMUL
	rm ~/embarc.disklog
}

# Install System dependencies
function Install_Ubuntu_Dependencies()
{
	echo "Installing Ubuntu Dependencies"
	sudo apt update
	sudo apt install -y numactl cmake
	sudo apt install -y python3-dev
	sudo apt install -y libevent-dev
	sudo apt install -y libfmt-dev
	sudo apt install -y libboost-all-dev
	sudo apt install -y libdouble-conversion-dev
	sudo apt install -y libgflags-dev libgoogle-glog-dev

	# for folly
	sudo apt install -y libssl-dev libfmt-dev pkg-config

	# for grpc
	sudo apt install -y libsystemd-dev
	sudo apt install -y protobuf-compiler
	
	# install folly manually
	cd third_party
	if [ -f "/usr/local/lib/libfolly.a" ]; then
		echo "folly already installed; skipping build"
	else
		if [ -d "folly" ]; then
			echo "folly already cloned; not re-cloning"
		else
			git clone --depth 1 --branch v2024.03.11.00 https://github.com/facebook/folly.git
		fi
		cd folly/build
		cmake ..
		sudo cmake --build . --target install
		cd ../..
	fi

	# install mi_malloc manually
	if [ -d "/usr/local/lib/mimalloc-2.1" ]; then
		echo "mimalloc already installed; skipping build"
	else
		if [ -d "mimalloc" ]; then
			echo "mimalloc already cloned; not re-cloning"
		else
			git clone --depth 1 --branch v2.1.7 https://github.com/microsoft/mimalloc.git
		fi
		cd mimalloc
		mkdir -p out/release
		cd out/release
		cmake ../..
		make
		sudo make install
		cd ../../..
	fi
	cd ..
}

function Install_RHEL_Dependencies()
{
	echo "Installing RHEL Dependencies"
	sudo dnf update
	sudo dnf install -y numactl cmake
	sudo dnf install -y python-devel
	sudo dnf install -y libevent libevent-devel
	sudo dnf install -y fmt fmt-devel
	sudo dnf install -y boost boost-devel
	sudo dnf install -y double-conversion double-conversion-devel
	sudo dnf install -y gflags gflags-devel glog glog-devel
	sudo dnf install -y folly-devel

	# for grpc
	sudo dnf install -y systemd-devel
	sudo dnf install -y protobuf-devel protobuf-lite-devel

	# TODO: install mi_malloc, currently running on test machines with v1.8
}

function Download_Dependency_Source_Code()
{
	cd third_party
	if [ -d "cxxopts" ]; then
		echo "cxxopts already cloned; not re-cloning"
	else
		git clone --depth 1 --branch v3.2.0 https://github.com/jarro2783/cxxopts
	fi
	cd ..
}

function Build_Project()
{
	mkdir -p build
	cd build
	cmake ..
	cmake --build .
}

# Mount Node:1 memory by tmpfs and create 128GB of file
function Setup_CXL()
{
	echo "Setting up CXL Emulation on numa node 1"
	sudo mkdir /mnt/CXL_DIR
	sudo chown $USER /mnt/CXL_DIR
	sudo numactl --membind=1 mount -t tmpfs tmpfs /mnt/CXL_DIR/ -o size=128G
}
function Setup_CXL()

##################### Execute ############################
if $DO_CLEAN; then
	echo "Cleaning up artifacts from previous setup/build..."
	Clean_Previous_Artifacts
else
	echo "Not cleaning artifacts from prevous setup/build"
fi

mkdir -p third_party
touch ~/embarc.disklog

MY_DISTRO=$(awk -F= '/^NAME/{print $2}' /etc/os-release)
echo "Distro: $MY_DISTRO"

if [ "$MY_DISTRO" = "\"Ubuntu\"" ]; then
	echo "Ubuntu distribution, using apt"
	Install_Ubuntu_Dependencies
else
	echo "Not Ubuntu, assuming RHEL..."
	Install_RHEL_Dependencies
fi

Download_Dependency_Source_Code
#Setup_CXL
Build_Project
