# Embarcadero Configuration System

## Overview

The Embarcadero configuration system provides a flexible, hierarchical configuration management solution that supports:
- YAML-based configuration files
- Environment variable overrides
- Command-line argument overrides
- Runtime configuration validation

## Configuration Hierarchy

Configuration values are resolved in the following priority order (highest to lowest):
1. Command-line arguments
2. Environment variables
3. Configuration file values
4. Default values in code

## Configuration File Format

The configuration file uses YAML format with the following structure:

```yaml
embarcadero:
  version:
    major: 1
    minor: 0
  
  broker:
    port: 1214
    broker_port: 12140
    heartbeat_interval: 3
    max_brokers: 4
    cgroup_core: 85
  
  cxl:
    size: 34359738368  # 32GB
    emulation_size: 34359738368
    device_path: "/dev/dax0.0"
    numa_node: 2
  
  storage:
    segment_size: 17179869184  # 16GB
    batch_headers_size: 65536  # 64KB
    batch_size: 524288  # 512KB
    num_disks: 2
    max_topics: 32
    topic_name_size: 31
  
  network:
    io_threads: 8
    disk_io_threads: 4
    sub_connections: 3
    zero_copy_send_limit: 8388608  # 8MB
  
  corfu:
    sequencer_port: 50052
    replication_port: 50053
  
  scalog:
    sequencer_port: 50051
    replication_port: 50052
    sequencer_ip: "192.168.60.173"
    local_cut_interval: 100
  
  platform:
    is_intel: false
    is_amd: false
```

## Batch Size Alignment (Client vs Broker)

**Contract:** Brokers and throughput clients must use the same logical batch size for correct behavior and full ACKs.

- **Broker:** `embarcadero.storage.batch_size` (e.g. 2097152 = 2 MB in `config/embarcadero.yaml`). Used for CXL allocation and sequencer batching.
- **Client:** Throughput client uses `BATCH_SIZE` from config (see `src/common/config.h.in` → `storage.batch_size`) or, when using `config/client.yaml`, `client.publisher.batch_size_kb` (e.g. 2048 ⇒ 2 MB).

**Recommendation:** Use one source of truth. For throughput tests, ensure:
- Broker config: `storage.batch_size: 2097152` (2 MB).
- Client config: `client.publisher.batch_size_kb: 2048` (2 MB), or ensure the client loads a config where `storage.batch_size` equals the broker’s value.

Mismatch can cause under-filled batches, extra round-trips, or ring/ACK issues at high load.

## Acknowledgment Levels (ack_level)

Embarcadero supports three acknowledgment levels that control when publishers receive ACKs:

### ack_level = 0
**No acknowledgments**: Publisher never waits for ACKs. Fastest but no delivery guarantees.

### ack_level = 1  
**Memory acknowledgment**: Publisher receives ACK after message is:
- Written to CXL shared memory
- Globally ordered (if `order > 0`)

**Durability**: Messages are in shared memory but **not yet durable** to disk. Suitable for high-throughput scenarios where some data loss is acceptable.

### ack_level = 2
**Replication acknowledgment**: Publisher receives ACK only after:
- Message is written to CXL shared memory
- Message is globally ordered
- Message is **replicated to disk** on all replicas (up to `replication_factor`)

**Durability Semantics**:
- **Eventual durability within periodic sync window**
- Default sync policy: `fdatasync()` every **250ms** OR every **64 MiB** written
- This means `ack_level=2` provides "eventual durability" - data is durable within ~250ms, not immediately
- For stronger guarantees (immediate fsync per batch), consider:
  - Configurable sync thresholds (future enhancement)
  - Explicit `fsync()` calls in replication path
  - Alternative durability policies

**Configuration**:
- Set `replication_factor > 0` for replication to be active
- Sync thresholds are currently hardcoded but documented in `src/disk_manager/disk_manager.cc`:
  - `kSyncBytesThreshold = 64 * 1024 * 1024` (64 MiB)
  - `kSyncTimeThreshold = 250ms`

**Use Cases**:
- **ack_level=1**: High-throughput logging, metrics, non-critical data
- **ack_level=2**: Transaction logs, critical state, data requiring durability guarantees

**Performance Trade-offs**:
- `ack_level=0`: Lowest latency, highest throughput
- `ack_level=1`: Moderate latency increase (~microseconds for ordering)
- `ack_level=2`: Higher latency (~250ms worst-case for sync window), but provides durability

## Environment Variables

All configuration values can be overridden using environment variables. The naming convention is:
`EMBARCADERO_<SECTION>_<KEY>`

Examples:
- `EMBARCADERO_BROKER_PORT=9999`
- `EMBARCADERO_CXL_SIZE=68719476736`
- `EMBARCADERO_NETWORK_IO_THREADS=16`

## Command-Line Arguments

The following command-line arguments are supported:

- `--config <path>`: Path to configuration file (default: config/embarcadero.yaml)
- `--broker-port <port>`: Override broker port
- `--heartbeat-interval <seconds>`: Override heartbeat interval
- `--cxl-size <bytes>`: Override CXL memory size
- `--batch-size <bytes>`: Override batch size
- `--network-threads <count>`: Override network IO threads
- `--max-topics <count>`: Override maximum topics

## Usage in Code

### Loading Configuration

```cpp
#include "common/configuration.h"

// Get configuration instance (singleton)
Embarcadero::Configuration& config = Embarcadero::Configuration::getInstance();

// Load from file
if (!config.loadFromFile("config/embarcadero.yaml")) {
    LOG(ERROR) << "Failed to load configuration";
    auto errors = config.getValidationErrors();
    for (const auto& error : errors) {
        LOG(ERROR) << "Config error: " << error;
    }
    return 1;
}

// Override with command line
config.overrideFromCommandLine(argc, argv);
```

### Accessing Configuration Values

```cpp
// Direct access
int port = config.config().broker.port.get();
size_t cxl_size = config.config().cxl.size.get();

// Using helper methods
int broker_port = config.getBrokerPort();
size_t batch_size = config.getBatchSize();

// Legacy macro compatibility
int old_port = PORT;  // Still works, uses new config system
```

### Validation

The configuration system automatically validates:
- Port ranges (1024-65535)
- Memory sizes (minimum thresholds)
- Thread counts (minimum 1)
- Logical constraints (e.g., batch_size <= segment_size)

## Migration from Legacy System

The old `config.h.in` macros are maintained for backward compatibility but now use the new configuration system internally. This allows gradual migration of existing code.

Legacy macros that are now configuration-backed:
- `PORT`, `BROKER_PORT`, `HEARTBEAT_INTERVAL`
- `CXL_SIZE`, `CXL_EMUL_SIZE`, `SEGMENT_SIZE`
- `BATCH_SIZE`, `BATCHHEADERS_SIZE`
- `NUM_NETWORK_IO_THREADS`, `NUM_DISK_IO_THREADS`
- All Corfu and Scalog configuration macros

## Best Practices

1. **Use configuration files for deployment**: Keep environment-specific settings in separate YAML files
2. **Environment variables for containers**: Use env vars when deploying in containers/Kubernetes
3. **Command-line for testing**: Use CLI args for quick testing and development
4. **Validate early**: Always check validation errors after loading configuration
5. **Gradual migration**: Use legacy macros during transition, migrate to direct config access over time
