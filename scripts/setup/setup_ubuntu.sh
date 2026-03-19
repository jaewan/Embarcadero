#!/bin/bash

function check_package_installed() {
    dpkg -l "$1" &> /dev/null
    return $?
}

function ensure_apt_packages() {
    local packages=("$@")
    local packages_to_install=()
    for pkg in "${packages[@]}"; do
        if ! check_package_installed "$pkg"; then
            packages_to_install+=("$pkg")
        fi
    done

    if [ ${#packages_to_install[@]} -ne 0 ]; then
        echo "Installing missing packages: ${packages_to_install[*]}"
        sudo apt update
        sudo DEBIAN_FRONTEND=noninteractive apt install -y "${packages_to_install[@]}"
    else
        echo "All required APT packages are already installed"
    fi
}

function header_exists() {
    local header="$1"
    [[ -f "/usr/local/include/${header}" || -f "/usr/include/${header}" ]]
}

function command_exists() {
    command -v "$1" >/dev/null 2>&1
}

function build_and_install_cmake_project() {
    local repo_dir="$1"
    shift
    local cmake_args=("$@")

    mkdir -p build
    pushd build >/dev/null
    cmake .. "${cmake_args[@]}"
    cmake --build . -j"$(nproc)"
    sudo cmake --install .
    popd >/dev/null
}

function install_dependencies() {
    local packages=(
        "cmake"
        "build-essential"
        "git"
        "numactl"
        "cmake"
        "ninja-build"
        "python3-dev"
        "python3-pip"
        "libevent-dev"
        "libboost-all-dev"
        "libdouble-conversion-dev"
        "libgflags-dev"
        "libgoogle-glog-dev"
        "libssl-dev"
        "pkg-config"
        "libsystemd-dev"
        "libyaml-cpp-dev"
        "libunwind-dev"
        "liblz4-dev"
        "libzstd-dev"
        "libsodium-dev"
        "libsnappy-dev"
        "zlib1g-dev"
        "libbz2-dev"
        "liblzma-dev"
        "libjemalloc-dev"
        "autoconf"
        "automake"
        "libtool"
        "curl"
        "wget"
        "ca-certificates"
    )

    ensure_apt_packages "${packages[@]}"

    cd "${PROJECT_ROOT}/third_party"
}

function setup_third_party() {
    cd "${PROJECT_ROOT}/third_party"

    # Setup fmt if needed
    if ! header_exists "fmt/core.h"; then
        echo "Installing fmt library..."
        rm -rf fmt
        git clone --depth 1 --branch 10.1.1 https://github.com/fmtlib/fmt.git
        pushd fmt >/dev/null
        build_and_install_cmake_project "$PWD" -DCMAKE_BUILD_TYPE=Release
        popd >/dev/null
    else
        echo "fmt library is already installed"
    fi

    # Setup glog if needed
    if ! header_exists "glog/logging.h"; then
        echo "Installing glog library..."
        rm -rf glog
        git clone --depth 1 --branch v0.6.0 https://github.com/google/glog.git
        pushd glog >/dev/null
        build_and_install_cmake_project "$PWD" -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=ON
        popd >/dev/null
    else
        echo "glog library is already installed"
    fi

    # Setup folly if needed
    if ! header_exists "folly/FBString.h"; then
        echo "Installing folly library..."
        rm -rf folly
        git clone --depth 1 --branch v2024.03.11.00 https://github.com/facebook/folly.git
        pushd folly >/dev/null
        python3 ./build/fbcode_builder/getdeps.py install-system-deps --recursive
        build_and_install_cmake_project "$PWD" -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/usr/local"
        popd >/dev/null
    else
        echo "folly library is already installed"
    fi

    # Setup mimalloc if needed
    if ! header_exists "mimalloc.h"; then
        echo "Installing mimalloc library..."
        rm -rf mimalloc
        git clone --depth 1 --branch v2.1.7 https://github.com/microsoft/mimalloc.git
        pushd mimalloc >/dev/null
        mkdir -p out/release
        pushd out/release >/dev/null
        cmake ../.. -DCMAKE_BUILD_TYPE=Release
        cmake --build . -j"$(nproc)"
        sudo cmake --install .
        popd >/dev/null
        popd >/dev/null
    else
        echo "mimalloc library is already installed"
    fi

    # Setup cxxopts if needed
    if ! header_exists "cxxopts.hpp"; then
        echo "Installing cxxopts..."
        rm -rf cxxopts
        git clone --depth 1 --branch v3.2.0 https://github.com/jarro2783/cxxopts.git
        pushd cxxopts >/dev/null
        build_and_install_cmake_project "$PWD" -DCMAKE_BUILD_TYPE=Release
        popd >/dev/null
    else
        echo "cxxopts library is already installed"
    fi
}
