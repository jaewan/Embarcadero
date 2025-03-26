#!/bin/bash
set -xe

# Get absolute path of the script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "${SCRIPT_DIR}/../../" && pwd )"

# Add force flag
FORCE_INSTALL=false

# Common setup tasks
function setup_common() {
	mkdir -p third_party || exit 1
    touch ~/embarc.disklog || exit 1
}

# Clean function
function clean_all() {
    rm -rf build
    rm -rf third_party/folly
    rm -rf third_party/cxxopts
    rm -rf third_party/fmt
    rm -rf third_party/glog
    rm -rf third_party/mimalloc
    rm -rf ~/.CXL_EMUL
    rm ~/embarc.disklog
}

function hugepage_setup() {
	echo 10420 | sudo tee /proc/sys/vm/nr_hugepages # 20GB of huge pages
	sudo sysctl -w net.core.wmem_max=16777216  # 16 MB
	sudo sysctl -w net.core.rmem_max=16777216  # 16 MB
	sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 16777216"
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --clean)
            clean_all
            shift
            ;;
        --with-cxl)
            WITH_CXL=1
            shift
            ;;
        --force)
            FORCE_INSTALL=true
            shift
            ;;
        *)
            echo "Unknown option: $1"
            exit 1
            ;;
    esac
done

# Source the correct distribution script
if grep -q Ubuntu /etc/os-release; then
    source "${SCRIPT_DIR}/setup_ubuntu.sh"
else
    source "${SCRIPT_DIR}/setup_rhel.sh"
fi

# If force install is requested, clean third_party
if $FORCE_INSTALL; then
    echo "Force install requested, cleaning third_party directory..."
    rm -rf "${PROJECT_ROOT}/third_party"
fi

setup_common
hugepage_setup
install_dependencies
setup_third_party

# Build project
mkdir -p "${PROJECT_ROOT}/build" && cd "${PROJECT_ROOT}/build"
cmake ..
cmake --build . -j$(nproc)

if [[ "${WITH_CXL}" == "1" ]]; then
    source "${SCRIPT_DIR}/setup_cxl.sh"
fi
