#!/bin/bash
set -euo pipefail

# Get absolute path of the script directory
SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
PROJECT_ROOT="$( cd "${SCRIPT_DIR}/../../" && pwd )"

FORCE_INSTALL=false
WITH_CXL=0
CLIENT_ONLY=0
SKIP_SYSTEM_TUNING=0
ENABLE_LATENCY_BUILD=${ENABLE_LATENCY_BUILD:-0}

# Common setup tasks
function setup_common() {
    mkdir -p "${PROJECT_ROOT}/third_party"
    touch "${HOME}/embarc.disklog"
}

# Clean function
function clean_all() {
    rm -rf "${PROJECT_ROOT}/build"
    rm -rf "${PROJECT_ROOT}/third_party/folly"
    rm -rf "${PROJECT_ROOT}/third_party/cxxopts"
    rm -rf "${PROJECT_ROOT}/third_party/fmt"
    rm -rf "${PROJECT_ROOT}/third_party/glog"
    rm -rf "${PROJECT_ROOT}/third_party/mimalloc"
    rm -rf "${HOME}/.CXL_EMUL"
    rm -f "${HOME}/embarc.disklog"
}

function hugepage_setup() {
    # Configurable HugeTLB (2MB pages) target in GB. Only needed on experiment
    # hosts that run brokers / large shared-memory paths. Remote non-CXL client
    # nodes should use --client-only to skip this.
    HUGETLB_GB=${HUGETLB_GB:-26}
    PAGES=$(( (HUGETLB_GB * 1024) / 2 ))

    echo "Configuring HugeTLB/system tuning: ${HUGETLB_GB}GB (${PAGES} pages of 2MB)"
    echo "${PAGES}" | sudo tee /proc/sys/vm/nr_hugepages >/dev/null

    sudo mkdir -p /dev/hugepages
    if ! mount | grep -q "on /dev/hugepages type hugetlbfs"; then
        sudo mount -t hugetlbfs -o pagesize=2M none /dev/hugepages || true
    fi

    if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
        echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null || true
    fi

    sudo sysctl -w net.core.wmem_max=134217728
    sudo sysctl -w net.core.rmem_max=134217728
    sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 134217728"
    sudo sysctl -w net.ipv4.tcp_rmem="4096 65536 134217728"
}

function usage() {
    cat <<EOF
Usage: $0 [options]

Options:
  --clean               Remove build and third_party worktrees used by this script
  --force               Reinstall third_party dependencies from scratch
  --with-cxl            Run CXL setup after build (broker/CXL host only)
  --client-only         Skip hugepage/system tuning and CXL setup (remote client node)
  --skip-system-tuning  Skip hugepage + sysctl tuning even on broker hosts
  -h, --help            Show this help

Environment:
  ENABLE_LATENCY_BUILD=1   Configure with -DCOLLECT_LATENCY_STATS=ON
  HUGETLB_GB=<n>           HugeTLB target size for broker/CXL hosts
EOF
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
        --client-only)
            CLIENT_ONLY=1
            SKIP_SYSTEM_TUNING=1
            shift
            ;;
        --skip-system-tuning)
            SKIP_SYSTEM_TUNING=1
            shift
            ;;
        --force)
            FORCE_INSTALL=true
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1"
            usage
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
if [[ "${CLIENT_ONLY}" != "1" && "${SKIP_SYSTEM_TUNING}" != "1" ]]; then
    hugepage_setup
else
    echo "Skipping hugepage/system tuning (client-only or explicit skip)."
fi
install_dependencies
setup_third_party

# Build project
mkdir -p "${PROJECT_ROOT}/build" && cd "${PROJECT_ROOT}/build"
cmake_args=(..)
if [[ "${ENABLE_LATENCY_BUILD}" == "1" ]]; then
    cmake_args+=(-DCOLLECT_LATENCY_STATS=ON)
fi
cmake "${cmake_args[@]}"
cmake --build . -j$(nproc)

if [[ "${WITH_CXL}" == "1" && "${CLIENT_ONLY}" != "1" ]]; then
    source "${SCRIPT_DIR}/setup_cxl.sh"
fi
