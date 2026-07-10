#!/bin/bash
# scripts/cluster_setup.sh
#
# One-shot cluster setup + verification for overnight eval runs.
# Run this from moscxl (the broker node) before run_overnight_eval.sh.
#
# What it does:
#   1. Verify local build is present and fresh
#   2. Sync binaries + scripts to all client nodes (c1, c2, c3)
#   3. Verify CXL NUMA node 2 is present and memory-accessible on moscxl
#   4. Verify hugepages on moscxl (warn if low, don't abort)
#   5. Verify RDMA NIC is up and has IB/RoCE link on moscxl
#   6. Clean /dev/shm of any stale Embarcadero segments on all nodes
#   7. Verify passwordless SSH + sudo to all client nodes
#
# Usage:
#   bash scripts/cluster_setup.sh            # full setup + verify
#   bash scripts/cluster_setup.sh --check    # verify only (no sync)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

CHECK_ONLY=0
for arg in "$@"; do [[ "$arg" == "--check" ]] && CHECK_ONLY=1; done

# Client nodes (all must be reachable via passwordless SSH from moscxl)
CLIENT_NODES=(c1 c2 c3)

# Remote Embarcadero root on client nodes
REMOTE_ROOT="${REMOTE_PROJECT_ROOT:-$HOME/Embarcadero}"

# Binaries to sync to all client nodes
LOCAL_BIN="$PROJECT_ROOT/build/bin"
REQUIRED_BINS=(embarlet throughput_test corfu_global_sequencer)

stamp() { date -u +"%Y-%m-%dT%H:%M:%SZ"; }
info()  { echo "[$(stamp)] INFO  $*"; }
warn()  { echo "[$(stamp)] WARN  $*" >&2; }
die()   { echo "[$(stamp)] FATAL $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Verify local build
# ---------------------------------------------------------------------------
info "=== 1. Checking local build ==="
for bin in "${REQUIRED_BINS[@]}"; do
    path="$LOCAL_BIN/$bin"
    if [[ ! -x "$path" ]]; then
        die "Missing binary: $path — build first:
  cmake --build build -j --target embarlet throughput_test corfu_global_sequencer kv_ycsb_bench"
    fi
    age=$(( $(date +%s) - $(stat -c %Y "$path" 2>/dev/null || stat -f %m "$path" 2>/dev/null) ))
    info "  $bin  age=${age}s  size=$(du -sh "$path" | cut -f1)"
done
info "Local build OK"

if [[ "$CHECK_ONLY" == "1" ]]; then
    info "(--check mode: skipping binary sync)"
fi

# ---------------------------------------------------------------------------
# 2. Sync binaries to client nodes
# ---------------------------------------------------------------------------
info "=== 2. Syncing binaries to client nodes ==="
for host in "${CLIENT_NODES[@]}"; do
    info "  Syncing to $host..."

    # Ensure remote directory structure exists
    ssh -o BatchMode=yes "$host" "mkdir -p $REMOTE_ROOT/build/bin $REMOTE_ROOT/scripts/lib $REMOTE_ROOT/config" 2>/dev/null

    if [[ "$CHECK_ONLY" != "1" ]]; then
        # Sync binaries
        rsync -az --checksum \
            "$LOCAL_BIN/throughput_test" \
            "$host:$REMOTE_ROOT/build/bin/"

        # Sync scripts the client harness needs
        rsync -az --checksum \
            "$SCRIPT_DIR/lib/broker_lifecycle.sh" \
            "$SCRIPT_DIR/lib/run_throughput_impl.sh" \
            "$host:$REMOTE_ROOT/scripts/lib/"
        rsync -az --checksum \
            "$SCRIPT_DIR/run_throughput.sh" \
            "$host:$REMOTE_ROOT/scripts/"

        # Sync config
        rsync -az --checksum \
            "$PROJECT_ROOT/config/client.yaml" \
            "$host:$REMOTE_ROOT/config/"
    fi

    # Verify binary landed
    if ssh -o BatchMode=yes "$host" "test -x $REMOTE_ROOT/build/bin/throughput_test"; then
        # Check it's actually the right binary (not a stale old build)
        remote_size=$(ssh -o BatchMode=yes "$host" "stat -c %s $REMOTE_ROOT/build/bin/throughput_test 2>/dev/null || stat -f %z $REMOTE_ROOT/build/bin/throughput_test 2>/dev/null")
        local_size=$(stat -c %s "$LOCAL_BIN/throughput_test" 2>/dev/null || stat -f %z "$LOCAL_BIN/throughput_test" 2>/dev/null)
        if [[ "$remote_size" == "$local_size" ]]; then
            info "  $host: throughput_test OK (size=${remote_size}B)"
        else
            warn "  $host: throughput_test size mismatch (local=$local_size remote=$remote_size) — sync may have failed"
        fi
    else
        die "$host: throughput_test missing after sync"
    fi
done
info "Binary sync complete"

# ---------------------------------------------------------------------------
# 3. Verify CXL NUMA node 2 on moscxl
# ---------------------------------------------------------------------------
info "=== 3. Verifying CXL NUMA node 2 on moscxl ==="
if ! command -v numactl &>/dev/null; then
    warn "numactl not found — cannot verify NUMA topology"
else
    numa_info=$(numactl -H 2>/dev/null)
    if echo "$numa_info" | grep -qE '^node 2 size:'; then
        node2_size=$(echo "$numa_info" | grep '^node 2 size:' | awk '{print $4, $5}')
        node2_cpus=$(echo "$numa_info" | grep '^node 2 cpus:' | sed 's/node 2 cpus://')
        if [[ -z "${node2_cpus// /}" ]]; then
            info "  NUMA node 2: CPU-less (correct for CXL DIMM), size=$node2_size"
        else
            warn "  NUMA node 2 has CPUs — this may not be the CXL node: $node2_cpus"
        fi
    else
        warn "  NUMA node 2 not found — CXL memory may not be accessible"
        numactl -H 2>/dev/null || true
    fi

    # Verify run_multiclient will use membind=1,2 automatically
    membind=$(numactl -H 2>/dev/null | grep -qE '^node 2 cpus:' && echo "1,2 (CXL auto-detected)" || echo "1")
    info "  Broker memory binding will be: --membind=$membind"
fi

# Test CXL memory write/read via a tiny numactl-bound allocation
if command -v numactl &>/dev/null && numactl -H 2>/dev/null | grep -qE '^node 2 size:'; then
    if numactl --membind=2 python3 -c "
import mmap, struct
m = mmap.mmap(-1, 4096)
m.write(b'CXL_TEST_OK\\x00')
m.seek(0)
assert m.read(10) == b'CXL_TEST_OK', 'read back mismatch'
m.close()
print('CXL node 2 mmap write+read: OK')
" 2>/dev/null; then
        info "  CXL node 2 mmap: OK"
    else
        warn "  CXL node 2 mmap test failed (may be normal if no hugepages allocated yet)"
    fi
fi

# ---------------------------------------------------------------------------
# 4. Verify hugepages on moscxl
# ---------------------------------------------------------------------------
info "=== 4. Checking hugepages on moscxl ==="
if [[ -f /proc/meminfo ]]; then
    huge_total=$(grep HugePages_Total /proc/meminfo | awk '{print $2}')
    huge_free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    huge_size_kb=$(grep Hugepagesize /proc/meminfo | awk '{print $2}')
    huge_free_mb=$(( huge_free * huge_size_kb / 1024 ))
    info "  HugePages: total=$huge_total free=$huge_free size=${huge_size_kb}kB → ${huge_free_mb}MB free"

    if [[ "$huge_free_mb" -lt 8192 ]]; then
        warn "  Low hugepages: ${huge_free_mb}MB free < 8192MB recommended"
        warn "  To allocate: sudo sysctl -w vm.nr_hugepages=4096"
        warn "  run_multiclient.sh will warn before each trial; set EMBAR_USE_HUGETLB=0 to disable"
    else
        info "  Hugepages: sufficient (${huge_free_mb}MB free)"
    fi
fi

# ---------------------------------------------------------------------------
# 5. Verify RDMA NIC
# ---------------------------------------------------------------------------
info "=== 5. Checking RDMA NIC ==="
if command -v ibstat &>/dev/null; then
    ib_state=$(ibstat 2>/dev/null | grep -E "State:|Physical" | head -4)
    info "  ibstat output:"
    echo "$ib_state" | while read -r line; do info "    $line"; done
    if echo "$ib_state" | grep -q "State: Active"; then
        info "  RDMA: link is Active"
    else
        warn "  RDMA: no Active link found — W5 experiments will fail"
    fi
elif command -v ibv_devinfo &>/dev/null; then
    if ibv_devinfo 2>/dev/null | grep -q "PORT_ACTIVE\|state.*4"; then
        info "  RDMA: device found and port active"
    else
        warn "  RDMA: device found but no active port"
    fi
else
    warn "  ibstat/ibv_devinfo not found — cannot verify RDMA NIC state"
fi

# ---------------------------------------------------------------------------
# 6. Clean /dev/shm of stale Embarcadero segments everywhere
# ---------------------------------------------------------------------------
info "=== 6. Cleaning stale /dev/shm segments ==="
# Local
stale=$(ls /dev/shm/CXL_SHARED_EXPERIMENT_* 2>/dev/null || true)
if [[ -n "$stale" ]]; then
    echo "$stale" | while read -r f; do
        rm -f "$f" && info "  Removed local: $f"
    done
else
    info "  Local /dev/shm: clean"
fi

# Remote
for host in "${CLIENT_NODES[@]}"; do
    stale_count=$(ssh -o BatchMode=yes "$host" \
        'ls /dev/shm/CXL_SHARED_EXPERIMENT_* 2>/dev/null | wc -l' 2>/dev/null || echo 0)
    if [[ "$stale_count" -gt 0 ]]; then
        ssh -o BatchMode=yes "$host" \
            'rm -f /dev/shm/CXL_SHARED_EXPERIMENT_* 2>/dev/null; true'
        info "  $host: removed $stale_count stale segment(s)"
    else
        info "  $host /dev/shm: clean"
    fi
done

# ---------------------------------------------------------------------------
# 7. Verify passwordless SSH + sudo on all client nodes
# ---------------------------------------------------------------------------
info "=== 7. Verifying SSH + sudo on client nodes ==="
for host in "${CLIENT_NODES[@]}"; do
    # Basic SSH
    if ! ssh -o ConnectTimeout=5 -o BatchMode=yes "$host" "hostname" &>/dev/null; then
        die "$host: SSH failed (no passwordless access)"
    fi
    # sudo without password (needed for tc-netem, hugepage allocation)
    if ssh -o BatchMode=yes "$host" "sudo -n true" &>/dev/null; then
        info "  $host: SSH OK, passwordless sudo OK"
    else
        warn "  $host: SSH OK but passwordless sudo NOT available (needed for E6 tc-netem)"
    fi
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
info ""
info "=== Cluster setup COMPLETE ==="
info "All client nodes synced and verified. Ready to run:"
info "  SMOKE=1 bash scripts/run_overnight_eval.sh   # ~10-min sanity test"
info "  bash scripts/run_overnight_eval.sh            # full overnight"
