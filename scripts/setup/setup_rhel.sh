#!/bin/bash

function install_dependencies() {
    sudo dnf update
    sudo dnf install -y \
        numactl \
        cmake \
        python-devel \
        libevent \
        libevent-devel \
        fmt \
        fmt-devel \
        boost \
        boost-devel \
        double-conversion \
        double-conversion-devel \
        gflags \
        gflags-devel \
        glog \
        glog-devel \
        folly-devel \
        systemd-devel \
        protobuf-devel \
        protobuf-lite-devel
}

function setup_third_party() {
    cd third_party

    # Setup mimalloc (using v1.8 as mentioned in original TODO)
    if [[ ! -d "/usr/local/lib64/mimalloc-1.8" ]]; then
        git clone --depth 1 --branch v1.8.0 https://github.com/microsoft/mimalloc.git
        cd mimalloc/out/release
        cmake ../..
        make -j$(nproc)
        sudo make install
        cd ../../..
    fi

    # Setup cxxopts
    if [[ ! -d "cxxopts" ]]; then
        git clone --depth 1 --branch v3.2.0 https://github.com/jarro2783/cxxopts
    fi

    cd ..
}
