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
	# Configurable HugeTLB (2MB pages) target in GB. Default: 26GB so client QueueBuffer (12 queues × 1024 capacity × ~2MB slot) + brokers fit; 24GB was short by ~3MB (docs/HUGEPAGE_NEED_INVESTIGATION.md).
	HUGETLB_GB=${HUGETLB_GB:-26}
	PAGES=$(( (HUGETLB_GB * 1024) / 2 ))
	
	echo "Configuring HugeTLB: ${HUGETLB_GB}GB (${PAGES} pages of 2MB)"
	# Set global nr_hugepages (system-wide pool)
	echo ${PAGES} | sudo tee /proc/sys/vm/nr_hugepages >/dev/null

	# Mount hugetlbfs if not already mounted
	sudo mkdir -p /dev/hugepages
	if ! mount | grep -q "on /dev/hugepages type hugetlbfs"; then
		sudo mount -t hugetlbfs -o pagesize=2M none /dev/hugepages || true
	fi

	# Prefer THP madvise to reduce interference while still benefiting non-HugeTLB allocations
	if [ -f /sys/kernel/mm/transparent_hugepage/enabled ]; then
		echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled >/dev/null || true
	fi

	# NIC/Socket buffer tuning
	sudo sysctl -w net.core.wmem_max=134217728  # 128 MB
	sudo sysctl -w net.core.rmem_max=134217728  # 128 MB
	sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 134217728"
	sudo sysctl -w net.ipv4.tcp_rmem="4096 65536 134217728"
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
