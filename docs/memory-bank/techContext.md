# Technical Context: Build & Runtime Environment

**Document Purpose:** Complete technical stack reference for building, deploying, and debugging Embarcadero
**Status:** Reverse-engineered from codebase (Jan 2026)
**Last Updated:** 2026-01-23

---

## Executive Summary

Embarcadero is a **hardware-aware distributed system** that requires:
- **Compiler:** Modern C++17 with x86-64 SIMD intrinsics
- **Hardware:** NUMA-capable server with hugepages, optional CXL/DAX devices
- **Privileges:** Root/sudo for hugepage allocation, cgroup setup, and cache flush instructions
- **Dependencies:** 12+ external libraries (gRPC, Folly, NUMA, Mimalloc)

**Critical Insight:** This is not a portable "works anywhere" application. It is designed for **high-end server hardware** with specific kernel configurations.

---

## 1. Build System

### 1.1 CMake Configuration

**Build Tool:** CMake 3.20+
**Location:** Root `CMakeLists.txt` (line 27793)

**Compiler Flags:**
```cmake
# CMakeLists.txt:27806-27816
set(CMAKE_CXX_STANDARD 17)          # C++17 required
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS "-Wall -O3")    # Aggressive optimization

# src/CMakeLists.txt:32297-32304 (x86-64 only)
-march=native                        # CPU-specific optimizations
                                     # Enables AVX2, SSE4.2, CLFLUSHOPT
```

**Why `-march=native`?**
- Unlocks x86-64 SIMD instructions (`_mm_clflushopt`, `_mm_sfence`, `_mm_pause`)
- **Warning:** Binaries are NOT portable across different CPU generations
- **Detected at build time:** CMake probes CPU vendor (Intel vs AMD) via `lscpu`

### 1.2 Processor Detection

```cmake
# src/CMakeLists.txt:32306-32322
execute_process(COMMAND lscpu | grep "Vendor ID:" | awk "{print $NF}"
                OUTPUT_VARIABLE CPU_VENDOR)

if(CPU_VENDOR STREQUAL "GenuineIntel")
    set(__INTEL__ 1)
elseif(CPU_VENDOR STREQUAL "AuthenticAMD")
    set(__AMD__ 1)
else()
    message(WARNING "Unknown CPU vendor: ${CPU_VENDOR}")
endif()
```

**Usage:** Enables vendor-specific optimizations (potentially for cache-line prefetching).

### 1.3 Build Targets

| Executable | Description | Location |
|-----------|-------------|----------|
| `embarlet` | Main broker daemon | `build/bin/embarlet` |
| `throughput_test` | Publisher/Subscriber client | `build/bin/throughput_test` |
| `scalog_global_sequencer` | Scalog compatibility sequencer | `build/bin/scalog_global_sequencer` |
| `corfu_global_sequencer` | Corfu compatibility sequencer | `build/bin/corfu_global_sequencer` |
| `performance_test` | Google Benchmark suite | `build/bin/performance_test` |
| `config_test` | YAML config validator | `build/bin/config_test` |

---

## 2. External Dependencies

### 2.1 Core Libraries (Required)

| Library | Version | Purpose | Install Method |
|---------|---------|---------|----------------|
| **gRPC** | v1.55.1 | RPC framework (heartbeat, replication) | FetchContent (auto-download) |
| **Protobuf** | v3.x | Message serialization | Bundled with gRPC |
| **Abseil** | Latest | Google utilities (flat_hash_map, Mutex) | Bundled with gRPC |
| **Folly** | Latest | Facebook utilities (MPMCQueue) | System package |
| **glog** | Latest | Google logging | System package |
| **gflags** | Latest | Command-line flags | System package |
| **Mimalloc** | v1.8.0 | Microsoft memory allocator | Built from source |
| **cxxopts** | v3.2.0 | CLI parser | Git submodule in `third_party/` |
| **yaml-cpp** | Latest | Configuration parsing | System package or script |
| **libnuma** | Latest | NUMA memory binding | System package |
| **Google Benchmark** | Latest | Performance testing (optional) | System package |

**Installation Script:** `scripts/setup/setup_dependencies.sh`

### 2.2 System Libraries

```cmake
# src/CMakeLists.txt:32362-32378
target_link_libraries(embarlet
    numa                    # NUMA memory binding
    glog::glog             # Logging
    gflags                 # Command-line parsing
    mimalloc               # Fast memory allocator
    absl::flat_hash_map    # Hash map (faster than std::unordered_map)
    grpc++                 # gRPC C++ runtime
    protobuf::libprotobuf  # Protocol buffers
    yaml-cpp               # YAML config parser
    Threads::Threads       # POSIX threads
)
```

### 2.3 Dependency Installation (Ubuntu)

**Automated Setup:**
```bash
cd /path/to/Embarcadero
./scripts/setup/setup_dependencies.sh
```

**What it does:**
1. Sources `setup_ubuntu.sh` or `setup_rhel.sh` based on OS
2. Installs system packages via `apt-get` or `dnf`
3. Builds Mimalloc v1.8.0 from source
4. Clones cxxopts v3.2.0 into `third_party/`
5. Configures hugepages (see Section 3.2)
6. Runs CMake build

**Manual Installation (Ubuntu):**
```bash
sudo apt-get update
sudo apt-get install -y \
    cmake build-essential \
    libnuma-dev \
    libgoogle-glog-dev \
    libgflags-dev \
    libboost-all-dev \
    libdouble-conversion-dev \
    libevent-dev \
    libfmt-dev \
    libfolly-dev \
    libyaml-cpp-dev

# Mimalloc (manual build)
cd third_party
git clone --depth 1 --branch v1.8.0 https://github.com/microsoft/mimalloc.git
cd mimalloc && mkdir -p out/release && cd out/release
cmake ../.. && make -j$(nproc) && sudo make install
```

### 2.4 CXL Simulation Libraries

**Critical Check:** No explicit CXL simulation libraries found (e.g., `libvmmalloc`, `libpmem`).

**Current CXL Emulation:**
- Uses **NUMA node binding** (`numactl --membind=1`)
- Mounts `tmpfs` on NUMA node 1 to simulate disaggregated memory
- See `scripts/setup/setup_cxl.sh:1807-1834`

**Real CXL Hardware:**
- Code checks for `/dev/dax0.0` device (src/cxl_manager/cxl_manager.cc:37204-37207)
- Uses `mmap(MAP_SHARED)` for DAX device mapping
- Falls back to `shm_open("/CXL_SHARED_FILE")` if DAX unavailable

---

## 3. Hardware Requirements

### 3.1 Minimum Specifications

| Component | Requirement | Rationale |
|-----------|------------|-----------|
| **CPU** | x86-64 with AVX2 | SIMD instructions, cache flush intrinsics |
| **Cores** | 8+ cores | Multi-threaded sequencer, network I/O threads |
| **RAM** | 32GB+ | Client buffers, CXL emulation (24GB hugepages) |
| **NUMA Nodes** | 2+ | CXL emulation binds to node 1 |
| **Disk** | SSD (NVMe preferred) | Replication backend (fsync performance critical) |
| **Network** | 10GbE+ | High-throughput pub/sub workloads |

### 3.2 Hugepage Configuration

**Required:** 24GB of 2MB hugepages (12,288 pages)
**Location:** `scripts/setup/setup_dependencies.sh:18004-18045`

**Setup:**
```bash
# Automated (via setup script)
HUGETLB_GB=24 ./scripts/setup/setup_dependencies.sh

# Manual
sudo bash -c 'echo 12288 > /proc/sys/vm/nr_hugepages'
sudo mkdir -p /dev/hugepages
sudo mount -t hugetlbfs -o pagesize=2M none /dev/hugepages
```

**Verification:**
```bash
cat /proc/meminfo | grep Huge
# Expected output:
# HugePages_Total:   12288
# HugePages_Free:    12288 (initially)
# Hugepagesize:       2048 kB
```

**Why Hugepages?**
1. Reduces TLB misses (critical for 9GB/s throughput)
2. Eliminates page faults in hot path
3. Pre-touched buffers (see `Publisher::WarmupBuffers()` in src/client/publisher.h:27920)

**Transparent Hugepages (THP):**
```bash
# Set to "madvise" mode (recommended)
echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
```
- `always`: Too aggressive, causes memory bloat
- `madvise`: Application explicitly requests THP
- `never`: Disables THP (degrades performance)

### 3.3 NUMA Configuration

**CXL Emulation Topology:**
```
NUMA Node 0: System RAM (for application code, stack)
NUMA Node 1: CXL Emulation (tmpfs mount at /mnt/CXL_DIR)
NUMA Node 2: Alternative CXL binding (if Node 1 unavailable)
```

**Setup Script:** `scripts/setup/setup_cxl.sh`

```bash
#!/bin/bash
# scripts/setup/setup_cxl.sh:1810-1834
numactl --membind=1 mount -t tmpfs tmpfs /mnt/CXL_DIR/ -o size=128G
```

**Manual NUMA Binding:**
```bash
# Check NUMA topology
numactl --hardware

# Run embarlet on specific NUMA node
numactl --membind=1 --cpunodebind=1 ./build/bin/embarlet --head
```

**Code Reference:**
- CXL memory binding: `src/cxl_manager/cxl_manager.cc:37240-37273`
- Uses `mbind(MPOL_BIND)` syscall to pin CXL region to NUMA node 2

### 3.4 Kernel Parameters

**Network Buffer Tuning:**
```bash
# scripts/setup/setup_dependencies.sh:18020-18044
sudo sysctl -w net.core.wmem_max=134217728   # 128 MB send buffer
sudo sysctl -w net.core.rmem_max=134217728   # 128 MB recv buffer
sudo sysctl -w net.ipv4.tcp_wmem="4096 65536 134217728"
sudo sysctl -w net.ipv4.tcp_rmem="4096 65536 134217728"
```

**Persistent Configuration:**
```bash
# Add to /etc/sysctl.conf
net.core.wmem_max = 134217728
net.core.rmem_max = 134217728
net.ipv4.tcp_wmem = 4096 65536 134217728
net.ipv4.tcp_rmem = 4096 65536 134217728
```

---

## 4. Privilege Requirements

### 4.1 Root/Sudo Operations

**Setup Phase (Requires Root):**
1. Hugepage allocation (`/proc/sys/vm/nr_hugepages`)
2. CXL emulation mount (`mount -t tmpfs`)
3. Cgroup creation (`/sys/fs/cgroup/embarcadero_cgroup*`)
4. Network buffer tuning (`sysctl -w`)

**Runtime (Can Run as User with Capabilities):**
```bash
# Grant capabilities to embarlet binary
sudo setcap cap_sys_admin,cap_dac_override,cap_dac_read_search=eip ./build/bin/embarlet
```

**Required Capabilities:**
- `CAP_SYS_ADMIN`: mmap with MAP_POPULATE, cgroup attachment
- `CAP_DAC_OVERRIDE`: Access to `/dev/hugepages`, `/dev/dax0.0`
- `CAP_DAC_READ_SEARCH`: Read system-wide NUMA topology

### 4.2 Cgroup Setup (Optional)

**Purpose:** CPU core throttling for multi-broker testing
**Location:** `scripts/setup/create_cgroup.sh`

```bash
# Create cgroup for broker 0 (4 cores)
sudo cgcreate -g cpu:/embarcadero_cgroup0
sudo cgset -r cpu.cfs_quota_us=400000 embarcadero_cgroup0
sudo cgset -r cpu.cfs_period_us=100000 embarcadero_cgroup0

# Attach process
echo $PID | sudo tee /sys/fs/cgroup/embarcadero_cgroup0/cgroup.procs
```

**Usage in Embarlet:**
```bash
./embarlet --head --run_cgroup=0  # Attach to cgroup0
```

---

## 5. Development Workflow

### 5.1 Initial Setup

```bash
# 1. Clone repository
git clone <repo_url> && cd Embarcadero

# 2. Install dependencies (requires sudo)
./scripts/setup/setup_dependencies.sh

# 3. Verify hugepages
cat /proc/meminfo | grep HugePages_Total  # Should be 12288

# 4. Build project (already done by setup script)
# If rebuilding manually:
mkdir -p build && cd build
cmake .. && cmake --build . -j$(nproc)
```

### 5.2 Running Brokers

**Single-Node Testing:**
```bash
# Terminal 1: Start head broker
./build/bin/embarlet --head

# Terminal 2: Run client test
./build/bin/throughput_test --head_addr 127.0.0.1:1214 --topic test
```

**Multi-Node Deployment:**
```bash
# Node 1 (Head broker, IP: 192.168.1.10)
./embarlet --head

# Node 2 (Follower)
./embarlet --follower 192.168.1.10:1214

# Node 3 (Follower)
./embarlet --follower 192.168.1.10:1214
```

### 5.3 Configuration Files

**Default Locations:**
- Broker config: `config/embarcadero.yaml`
- Client config: `config/client.yaml`

**Override at Runtime:**
```bash
./embarlet --head --config config/custom.yaml
```

**Sample Configuration:**
```yaml
# config/embarcadero.yaml (snippet)
cxl:
  size: 137438953472       # 128GB
  numa_node: 2             # NUMA binding
  use_hugepages: true
  numa_bind: true

broker:
  num_network_io_threads: 12
  batch_size: 4194304      # 4MB batches

replication:
  factor: 3                # f+1 = 3 replicas
  ack_level: 2             # Wait for 2 ACKs
```

### 5.4 Testing & Benchmarking

**Throughput Test:**
```bash
# Publisher-only benchmark
./build/bin/throughput_test \
  --mode pub \
  --message_size 1024 \
  --total_size 10GB \
  --order 5

# End-to-end (pub + sub)
./build/bin/throughput_test \
  --mode both \
  --message_size 1024 \
  --total_size 10GB \
  --order 5 \
  --num_sub 4
```

**Latency Test:**
```bash
./scripts/run_latency.sh
```

**Ordering Microbenchmark:**
```bash
cd build/bin
./order_micro_bench \
  --brokers 4 \
  --message_size 256 \
  --duration_s 30 \
  --verify 1
```

### 5.5 Debugging

**Enable Verbose Logging:**
```bash
# Set VLOG level (0=errors only, 3=detailed trace)
./embarlet --head --log_level=3
```

**Attach GDB:**
```bash
# Compile with debug symbols
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .

# Run under GDB
gdb --args ./build/bin/embarlet --head
```

**Check Memory Layout:**
```bash
# Inspect /proc/meminfo during run
watch -n 1 'cat /proc/meminfo | grep -E "Huge|Numa"'

# Trace mmap calls
strace -e mmap,munmap ./embarlet --head 2>&1 | grep CXL
```

---

## 6. Common Issues & Solutions

### 6.1 Build Failures

**Issue:** `mimalloc not found`
**Solution:**
```bash
cd third_party
git clone --depth 1 --branch v1.8.0 https://github.com/microsoft/mimalloc.git
cd mimalloc/out/release && cmake ../.. && make -j$(nproc) && sudo make install
```

**Issue:** `folly headers not found`
**Solution:**
```bash
# Ubuntu
sudo apt-get install libfolly-dev

# RHEL/CentOS
sudo dnf install folly-devel
```

**Issue:** `CMake version too old`
**Solution:**
```bash
# Install CMake 3.20+ from Kitware APT repository
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc | sudo apt-key add -
sudo apt-add-repository 'deb https://apt.kitware.com/ubuntu/ focal main'
sudo apt-get update && sudo apt-get install cmake
```

### 6.2 Runtime Errors

**Issue:** `Failed to allocate hugepages`
**Check:**
```bash
# 1. Verify hugepages are allocated
cat /proc/meminfo | grep HugePages_Free
# If HugePages_Free = 0, increase allocation:
sudo bash -c 'echo 12288 > /proc/sys/vm/nr_hugepages'

# 2. Ensure hugetlbfs is mounted
mount | grep hugetlbfs
# If not mounted:
sudo mount -t hugetlbfs -o pagesize=2M none /dev/hugepages
```

**Issue:** `Permission denied: /dev/dax0.0`
**Solution:**
```bash
# Option 1: Grant capabilities
sudo setcap cap_dac_override=eip ./embarlet

# Option 2: Change device permissions
sudo chmod 666 /dev/dax0.0
```

**Issue:** `NUMA node 2 not available`
**Solution:**
```bash
# Check NUMA topology
numactl --hardware

# If only 1 NUMA node, modify code to use node 0
# Edit: src/cxl_manager/cxl_manager.cc:37242
# Change: numa_bitmask_setbit(bitmask, 2);
# To:     numa_bitmask_setbit(bitmask, 0);
```

### 6.3 Performance Issues

**Issue:** Throughput < 1GB/s
**Checklist:**
1. ✅ Hugepages allocated? (`cat /proc/meminfo | grep HugePages_Free`)
2. ✅ Network buffers tuned? (`sysctl net.core.rmem_max`)
3. ✅ Running with `-march=native`? (`strings embarlet | grep -i avx2`)
4. ✅ CXL emulation on correct NUMA node? (`numastat -p $(pidof embarlet)`)
5. ✅ CPU governor set to `performance`? (`cpupower frequency-info`)

**Set CPU Governor:**
```bash
# Check current governor
cat /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor

# Set to performance mode
sudo cpupower frequency-set -g performance
```

---

## 7. Hardware Platform Profiles

### 7.1 Development (Local Laptop)

**Specs:**
- CPU: Intel Core i7-10750H (6 cores, 12 threads)
- RAM: 16GB DDR4
- Disk: 512GB NVMe SSD

**Limitations:**
- ❌ No real CXL hardware
- ❌ Single NUMA node
- ⚠️ Limited hugepage allocation (8GB max recommended)

**Configuration:**
```yaml
# config/dev.yaml
cxl:
  size: 8589934592  # 8GB (reduced from 128GB)
  numa_node: 0      # Use system NUMA node
broker:
  num_network_io_threads: 4
```

### 7.2 Production (Cloudlab c6525-100g)

**Specs:**
- CPU: AMD EPYC 7452 (2x 32-core, 128 threads total)
- RAM: 256GB DDR4
- CXL: Emulated via NUMA node 1
- Network: 100GbE

**Configuration:**
```yaml
# config/production.yaml
cxl:
  size: 137438953472  # 128GB
  numa_node: 1
broker:
  num_network_io_threads: 16
```

**NUMA Binding:**
```bash
# Pin broker to NUMA node 0 (CPU), CXL to node 1 (memory)
numactl --cpunodebind=0 --membind=1 ./embarlet --head
```

### 7.3 CXL-Enabled Server (Future)

**Specs:**
- CPU: Intel Sapphire Rapids (CXL 1.1 support)
- CXL Device: /dev/dax0.0 (256GB)
- RAM: 512GB DDR5

**Configuration:**
```yaml
# config/cxl_hw.yaml
cxl:
  size: 274877906944  # 256GB
  device: "/dev/dax0.0"  # Real DAX device
  numa_node: 2
```

**Verification:**
```bash
# Check DAX device
ls -lh /dev/dax0.0
ndctl list

# Verify CXL memory mode
cat /sys/bus/dax/devices/dax0.0/size
```

---

## 8. Container/Docker Support

**Current Status:** ❌ Not officially supported

**Challenges:**
1. Hugepage allocation requires `--privileged` mode or specific capabilities
2. NUMA binding needs access to `/sys/devices/system/node/`
3. CXL DAX devices must be exposed via `--device` flag

**Experimental Dockerfile:**
```dockerfile
FROM ubuntu:22.04

# Install dependencies
RUN apt-get update && apt-get install -y \
    cmake build-essential libnuma-dev \
    libgoogle-glog-dev libgflags-dev \
    libfolly-dev libyaml-cpp-dev

# Build Embarcadero
COPY . /embarcadero
WORKDIR /embarcadero/build
RUN cmake .. && cmake --build . -j$(nproc)

# Run with hugepages and NUMA
CMD ["numactl", "--membind=1", "./bin/embarlet", "--head"]
```

**Run Container:**
```bash
docker run --privileged \
  --cap-add=SYS_ADMIN \
  --device=/dev/hugepages \
  --volume=/mnt/CXL_DIR:/mnt/CXL_DIR \
  embarcadero:latest
```

---

## 9. CI/CD Integration

**Build Matrix:**
| OS | Compiler | CXL Mode | Status |
|----|----------|----------|--------|
| Ubuntu 22.04 | GCC 11 | Emulated | ✅ |
| Ubuntu 20.04 | GCC 9 | Emulated | ✅ |
| RHEL 8 | GCC 8 | Emulated | ⚠️ (folly version) |
| Ubuntu 22.04 | Clang 14 | Emulated | ❌ (mimalloc conflict) |

**GitHub Actions Workflow (Example):**
```yaml
name: Build & Test
on: [push, pull_request]

jobs:
  build:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v3

      - name: Install dependencies
        run: |
          sudo apt-get update
          sudo apt-get install -y libnuma-dev libgoogle-glog-dev libfolly-dev

      - name: Build
        run: |
          mkdir build && cd build
          cmake .. && cmake --build . -j$(nproc)

      - name: Unit Tests
        run: |
          cd build
          ctest --output-on-failure
```

---

## References

- **CMakeLists.txt:** Root build configuration (line 27792)
- **src/CMakeLists.txt:** Main target definitions (line 32282)
- **setup_dependencies.sh:** Automated setup script (line 17980)
- **setup_cxl.sh:** CXL emulation setup (line 1807)
- **README.md:** Quick start guide (line 23571)

---

**Document Maintenance:**
- Update this document when adding new dependencies
- Verify hardware requirements after major performance optimizations
- Test setup scripts on new Linux distributions (Ubuntu LTS, RHEL)
