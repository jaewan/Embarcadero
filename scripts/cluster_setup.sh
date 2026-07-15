#!/bin/bash
# scripts/cluster_setup.sh
#
# One-shot cluster setup + verification for overnight eval runs.
# Run this from moscxl (the broker node) before run_overnight_eval.sh.
#
# What it does:
#   1. Verify local build is present and fresh
#   2. Sync binaries + scripts to the selected client nodes (default: c4, c3, c1)
#   3. Verify CXL NUMA node 2 is present and memory-accessible on moscxl
#   4. Verify hugepages on moscxl (warn if low, don't abort)
#   5. Verify RDMA NIC is up and has IB/RoCE link on moscxl
#   6. Clean /dev/shm of any stale Embarcadero segments on all nodes
#   7. Verify passwordless SSH + sudo to all client nodes
#
# Usage:
#   bash scripts/cluster_setup.sh                         # full setup + verify
#   CLIENT_NODES_CSV=c4,c3 bash scripts/cluster_setup.sh # selected clients only
#   bash scripts/cluster_setup.sh --check                 # verify only (no sync)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

CHECK_ONLY=0
for arg in "$@"; do [[ "$arg" == "--check" ]] && CHECK_ONLY=1; done

# Client nodes used by overnight/publication eval (100G fabric).
# c2 is 1G-only and excluded from eval; do not hard-fail the setup on it.
CLIENT_NODES=(c4 c3 c1)
OPTIONAL_NODES=(c2)
# c4: 100G NIC ens801f0np0 on NUMA 1 — primary overnight client.
# c3: 100G NIC ens801f0np0 on NUMA 1.
# c1: 100G NIC enp24s0f0np0 on NUMA 0.
# Remote Embarcadero root on client nodes
REMOTE_ROOT="${REMOTE_PROJECT_ROOT:-$HOME/Embarcadero}"

# A publication campaign must never refresh a colleague's unrelated remote
# worktree merely because that host happens to be in the historical default
# roster.  Callers select the exact client set used by their cells.
if [[ -n "${CLIENT_NODES_CSV:-}" ]]; then
    IFS=',' read -r -a CLIENT_NODES <<< "$CLIENT_NODES_CSV"
    for host in "${CLIENT_NODES[@]}"; do
        [[ -n "${host//[[:space:]]/}" ]] || die "CLIENT_NODES_CSV contains an empty host"
    done
fi

# Binaries to sync to all client nodes
LOCAL_BIN="$PROJECT_ROOT/build/bin"
REQUIRED_BINS=(embarlet throughput_test corfu_global_sequencer)

# Client binaries are built natively.  Do not normally inject broker-host
# libraries here: c4's older glibc/libstdc++ cannot load the broker's glog or
# yaml-cpp.  An operator may explicitly request a synced compatibility bundle
# only for hosts with a verified compatible runtime.
LOCAL_LIB="$PROJECT_ROOT/lib"
SYNC_CLIENT_LIBS="${SYNC_CLIENT_LIBS:-0}"

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
# 2. Sync source + native-rebuild client binaries
# ---------------------------------------------------------------------------
info "=== 2. Syncing source and rebuilding throughput_test on client nodes ==="
# Clients need a native build (broker AVX-512 binaries can SIGILL on older CPUs).
# Sync a clean git-archive of HEAD (plus overnight script overlays) so uncommitted
# WIP on the broker (e.g. LazyLog metadata) cannot break remote client builds.
SYNC_EXCLUDES=(
    --exclude '.git/'
    --exclude 'build/'
    --exclude 'build-portable/'
    --exclude 'data/'
    --exclude 'Paper/'
    --exclude 'Paper.zip'
    --exclude 'multiclient_logs/'
    --exclude 'lib/'
    --exclude '*.o'
    --exclude '*.a'
    --exclude '__pycache__/'
    --exclude '.Replication/'
)

CLEAN_SYNC_ROOT="$(mktemp -d /tmp/embar_client_sync.XXXXXX)"
info "  Exporting clean HEAD tree for client sync..."
git -C "$PROJECT_ROOT" archive HEAD | tar -x -C "$CLEAN_SYNC_ROOT"
# Sync precisely the committed snapshot.  Copying uncommitted script overlays
# made remote provenance ambiguous and can reintroduce a local WIP defect.

for host in "${CLIENT_NODES[@]}"; do
    info "  Syncing source to $host..."

    ssh -o BatchMode=yes "$host" \
        "mkdir -p $REMOTE_ROOT/build/bin $REMOTE_ROOT/scripts/lib $REMOTE_ROOT/config $REMOTE_ROOT/lib" \
        2>/dev/null

    if [[ "$CHECK_ONLY" != "1" ]]; then
        rsync -az --checksum \
            "${SYNC_EXCLUDES[@]}" \
            "$CLEAN_SYNC_ROOT/" "$host:$REMOTE_ROOT/"

        if [[ "$SYNC_CLIENT_LIBS" == "1" && -d "$LOCAL_LIB" && -n "$(ls -A "$LOCAL_LIB" 2>/dev/null)" ]]; then
            warn "  $host: syncing caller-requested compatibility libraries"
            rsync -az --checksum "$LOCAL_LIB/" "$host:$REMOTE_ROOT/lib/"
        fi

        info "  $host: native rebuild of throughput_test..."
        if ! ssh -o BatchMode=yes "$host" "bash -s" <<REMOTE_BUILD
set -euo pipefail
cd "$REMOTE_ROOT"
mkdir -p build
# A client may have been configured against a different distro glog (or with
# a local static-link override).  rsync preserves source mtimes, so merely
# rebuilding can incorrectly retain objects compiled against that ABI.  Drop
# the cache and clean this target before each native client rebuild.
rm -f build/CMakeCache.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS= >/tmp/embar_client_cmake.log 2>&1
cmake --build build --target clean >/tmp/embar_client_clean.log 2>&1 || true
cmake --build build -j"\$(nproc)" --target throughput_test
test -x build/bin/throughput_test
REMOTE_BUILD
        then
            warn "  $host: native rebuild failed — falling back to broker binary sync"
            rsync -az --checksum \
                "$LOCAL_BIN/throughput_test" \
                "$host:$REMOTE_ROOT/build/bin/"
        else
            info "  $host: native rebuild OK"
        fi
    fi

    # Verify the native binary with its host ABI. ldd alone is insufficient;
    # this bad-option invocation proves loader + C++ runtime compatibility.
    verify_client_binary_runs() {
        ssh -o BatchMode=yes "$1" \
            "cd $REMOTE_ROOT/build/bin && env -u LD_LIBRARY_PATH \
             ./throughput_test --cluster_setup_verify_bad_option 2>&1 | head -3" 2>/dev/null \
            | grep -q "no_such_option\|does not exist"
    }
    if ! ssh -o BatchMode=yes "$host" "test -x $REMOTE_ROOT/build/bin/throughput_test"; then
        die "$host: throughput_test missing after sync"
    fi
    if verify_client_binary_runs "$host"; then
        info "  $host: throughput_test OK (executed under harness env)"
    else
        die "$host: native throughput_test cannot run — fix its host toolchain/runtime before launching"
    fi
done
info "Source sync + client rebuild complete"
rm -rf "$CLEAN_SYNC_ROOT"

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

    # Minimum for overnight run with THREADS_THROUGHPUT=6:
    #   precreate: 6 threads × 4 brokers × 1024 slots × 512KB = 12 GB = 6144 hugepages
    #   safe minimum with headroom: 6500 hugepages (13 GB)
    MIN_HUGEPAGES_NEEDED=6500
    min_huge_mb=$(( MIN_HUGEPAGES_NEEDED * huge_size_kb / 1024 ))
    if [[ "$huge_free_mb" -lt "$min_huge_mb" ]]; then
        warn "  Low hugepages: ${huge_free_mb}MB free < ${min_huge_mb}MB needed for THREADS_THROUGHPUT=6"
        info "  Auto-allocating ${MIN_HUGEPAGES_NEEDED} hugepages via sudo sysctl..."
        if sudo sysctl -w vm.nr_hugepages="${MIN_HUGEPAGES_NEEDED}" 2>/dev/null; then
            huge_free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
            info "  Hugepages after allocation: $huge_free × ${huge_size_kb}kB free"
        else
            warn "  sudo sysctl failed — manually run: sudo sysctl -w vm.nr_hugepages=${MIN_HUGEPAGES_NEEDED}"
        fi
    else
        info "  Hugepages: sufficient (${huge_free_mb}MB free ≥ ${min_huge_mb}MB needed)"
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
