#!/bin/bash

function check_package_installed() {
    dpkg -l "$1" &> /dev/null
    return $?
}

function check_lib_installed() {
    local lib_name="$1"
    local required_version="$2"

    if ! pkg-config --exists "$lib_name"; then
        printf "Library %s not found by pkg-config.\n" "$lib_name"
        return 1
    fi

    if [[ -n "$required_version" ]]; then
        if ! pkg-config --atleast-version="$required_version" "$lib_name"; then
            printf "Library %s version %s or higher not found.\n" "$lib_name" "$required_version"
            return 1
        fi
    fi

    return 0
}

function install_dependencies() {
    local packages=(
        "numactl"
        "cmake"
        "python3-dev"
        "libevent-dev"
        "libboost-all-dev"
        "libdouble-conversion-dev"
        "libgflags-dev"
        #"libgoogle-glog-dev"
        "libssl-dev"
        "pkg-config"
        "libsystemd-dev"
    )

    # Check and install system packages
    local packages_to_install=()
    for pkg in "${packages[@]}"; do
        if ! check_package_installed "$pkg"; then
            packages_to_install+=("$pkg")
        fi
    done

    if [ ${#packages_to_install[@]} -ne 0 ]; then
        echo "Installing missing packages: ${packages_to_install[*]}"
        sudo apt update
        sudo apt install -y "${packages_to_install[@]}"
    else
        echo "All required system packages are already installed"
    fi

    cd "${PROJECT_ROOT}/third_party"

    # Install fmt if needed
    if ! check_lib_installed "fmt" "/usr/local/lib/libfmt.a"; then
        echo "Installing fmt library..."
        if [ -d "fmt" ]; then
            echo "fmt directory exists, cleaning..."
            rm -rf fmt
        fi
        git clone --depth 1 --branch 10.1.1 https://github.com/fmtlib/fmt.git
        cd fmt
        mkdir -p build && cd build
        cmake ..
        make -j$(nproc)
        sudo make install
        cd "${PROJECT_ROOT}/third_party"
    else
        echo "fmt library is already installed"
    fi
}

function setup_third_party() {
    cd "${PROJECT_ROOT}/third_party"

	# Setup glog if needed
    if ! check_lib_installed "glog" "/usr/local/lib/libglog.a"; then
        echo "Installing glog library..."
        if [ -d "glog" ]; then
            echo "glog directory exists, cleaning..."
            rm -rf glog
        fi
        git clone --depth 1 --branch v0.6.0 https://github.com/google/glog.git
        cd glog
        mkdir -p build && cd build
        cmake .. -DBUILD_SHARED_LIBS=ON
        make -j$(nproc)
        sudo make install
        cd "${PROJECT_ROOT}/third_party"
    else
        echo "glog library is already installed"
    fi

    # Setup folly if needed
    if ! check_lib_installed "folly" "/usr/local/lib/libfolly.a"; then
        echo "Installing folly library..."
        if [ -d "folly" ]; then
            echo "folly directory exists, cleaning..."
            rm -rf folly
        fi
        git clone --depth 1 --branch v2024.03.11.00 https://github.com/facebook/folly.git
        cd folly && mkdir -p build && cd build
		sudo ./fbcode_builder/getdeps.py install-system-deps --recursive
        cmake .. -DCMAKE_PREFIX_PATH="/usr/local"
        make -j$(nproc)
        sudo make install
        cd "${PROJECT_ROOT}/third_party"
    else
        echo "folly library is already installed"
    fi

    # Setup mimalloc if needed
    if ! check_lib_installed "mimalloc" "/usr/local/lib/mimalloc-2.1/libmimalloc.so"; then
        echo "Installing mimalloc library..."
        if [ -d "mimalloc" ]; then
            echo "mimalloc directory exists, cleaning..."
            rm -rf mimalloc
        fi
        git clone --depth 1 --branch v2.1.7 https://github.com/microsoft/mimalloc.git
        cd mimalloc
        mkdir -p out/release
        cd out/release
        cmake ../..
        make -j$(nproc)
        sudo make install
        cd "${PROJECT_ROOT}/third_party"
    else
        echo "mimalloc library is already installed"
    fi

	# Setup cxxopts if needed
	if ! check_lib_installed "cxxopts" "/usr/local/lib/libcxxopts.a"; then
		echo "Installing cxxopts..."
		if [ -d "cxxopts" ]; then
			echo "cxxopts directory exists, cleaning..."
			rm -rf cxxopts
		fi
		git clone --depth 1 --branch v3.2.0 https://github.com/jarro2783/cxxopts.git
		cd cxxopts
		mkdir -p build && cd build
		cmake ..
		make -j$(nproc)
		sudo make install
		echo "cxxopts installation finished" # Add this line
		cd "${PROJECT_ROOT}/third_party"
	else
		echo "cxxopts library is already installed"
	fi
}

# Add version checking function
function check_lib_version() {
    local lib_name=$1
    local required_version=$2
    local version_cmd=$3

    if ! command -v $version_cmd &> /dev/null; then
        echo "$lib_name version check command not found"
        return 1
	fi 

    local installed_version=$($version_cmd)
    if [ "$installed_version" = "$required_version" ]; then
        echo "$lib_name version $installed_version is correct"
        return 0
    else
        echo "$lib_name version mismatch. Required: $required_version, Found: $installed_version"
        return 1
    fi
}

# Add cleanup function
function cleanup_third_party() {
    local dir=$1
    if [ -d "$dir" ]; then
        echo "Cleaning up $dir..."
        rm -rf "$dir"
    fi
}
