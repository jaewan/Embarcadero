#!/bin/bash
# Preflight verification script for E2E tests on real CXL machine
# Verifies: NUMA node, hugepages, DAX device, ports

set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

ERRORS=0
WARNINGS=0

log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
    ERRORS=$((ERRORS + 1))
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
    WARNINGS=$((WARNINGS + 1))
}

echo "=========================================="
echo "Preflight Verification for E2E Tests"
echo "=========================================="
echo ""

# Check NUMA node configuration
log_info "Checking NUMA node configuration..."
CONFIG_FILE="${1:-config/embarcadero.yaml}"
if [ -f "$CONFIG_FILE" ]; then
    NUMA_NODE=$(grep -A 5 "cxl:" "$CONFIG_FILE" | grep "numa_node:" | awk '{print $2}' || echo "")
    if [ -n "$NUMA_NODE" ]; then
        log_info "Config specifies NUMA node: $NUMA_NODE"
        
        # Check if NUMA node exists
        if numactl --hardware 2>/dev/null | grep -q "node $NUMA_NODE"; then
            log_info "NUMA node $NUMA_NODE exists on this system"
        else
            log_warn "NUMA node $NUMA_NODE not found on this system"
        fi
    else
        log_warn "Could not find numa_node in config file"
    fi
else
    log_warn "Config file not found: $CONFIG_FILE"
fi

# Check DAX device
log_info "Checking DAX device..."
DAX_PATH=$(grep -A 5 "cxl:" "$CONFIG_FILE" 2>/dev/null | grep "device_path:" | awk '{print $2}' | tr -d '"' || echo "/dev/dax0.0")
if [ -e "$DAX_PATH" ]; then
    log_info "DAX device exists: $DAX_PATH"
    if [ -c "$DAX_PATH" ]; then
        log_info "DAX device is a character device (correct)"
    else
        log_warn "DAX device exists but is not a character device"
    fi
else
    log_error "DAX device not found: $DAX_PATH"
fi

# Check hugepages
log_info "Checking hugepages..."
if [ -f /proc/meminfo ]; then
    HUGEPAGE_SIZE=$(grep "Hugepagesize:" /proc/meminfo | awk '{print $2}')
    HUGEPAGE_FREE=$(grep "HugePages_Free:" /proc/meminfo | awk '{print $2}')
    HUGEPAGE_TOTAL=$(grep "HugePages_Total:" /proc/meminfo | awk '{print $2}')
    
    if [ -n "$HUGEPAGE_SIZE" ]; then
        log_info "Hugepage size: ${HUGEPAGE_SIZE} KB"
        log_info "Hugepages free: $HUGEPAGE_FREE / $HUGEPAGE_TOTAL"
        
        # Calculate free hugepage memory (assuming 2GB minimum for 4GB CXL)
        FREE_GB=$((HUGEPAGE_FREE * HUGEPAGE_SIZE / 1024 / 1024))
        if [ $FREE_GB -ge 2 ]; then
            log_info "Sufficient hugepages available: ${FREE_GB} GB free"
        else
            log_warn "Low hugepages: only ${FREE_GB} GB free (recommend at least 2GB for 4GB CXL)"
        fi
    else
        log_warn "Could not read hugepage info from /proc/meminfo"
    fi
else
    log_warn "Could not access /proc/meminfo"
fi

# Check ports
log_info "Checking broker ports..."
PORTS=(12140 12141 12142 12143)
for port in "${PORTS[@]}"; do
    if lsof -i :$port >/dev/null 2>&1 || netstat -tuln 2>/dev/null | grep -q ":$port "; then
        log_warn "Port $port is already in use"
    else
        log_info "Port $port is available"
    fi
done

# Check binaries
log_info "Checking test binaries..."
BUILD_DIR="${BUILD_DIR:-build}"
if [ -f "$BUILD_DIR/bin/embarlet" ]; then
    log_info "embarlet binary exists: $BUILD_DIR/bin/embarlet"
else
    log_error "embarlet binary not found: $BUILD_DIR/bin/embarlet"
fi

if [ -f "$BUILD_DIR/bin/throughput_test" ]; then
    log_info "throughput_test binary exists: $BUILD_DIR/bin/throughput_test"
else
    log_error "throughput_test binary not found: $BUILD_DIR/bin/throughput_test"
fi

# Summary
echo ""
echo "=========================================="
echo "Preflight Summary"
echo "=========================================="
if [ $ERRORS -eq 0 ] && [ $WARNINGS -eq 0 ]; then
    echo -e "${GREEN}✓ All checks passed${NC}"
    exit 0
elif [ $ERRORS -eq 0 ]; then
    echo -e "${YELLOW}⚠ $WARNINGS warning(s) (may still work)${NC}"
    exit 0
else
    echo -e "${RED}✗ $ERRORS error(s), $WARNINGS warning(s)${NC}"
    exit 1
fi
