# E2E Test Execution Guide

## Quick Start

### 1. Preflight Verification (Recommended)
Before running tests, verify your environment:
```bash
cd test/e2e
./verify_preflight.sh
```

This checks:
- NUMA node configuration matches your CXL hardware
- DAX device exists and is accessible
- Hugepages are enabled and sufficient
- Required ports are available
- Test binaries exist

### 2. Run Individual Tests

**Basic publish test (smoke test):**
```bash
cd test/e2e
./test_basic_publish.sh
```

**Explicit replication test (ORDER=5 + ack_level=2):**
```bash
cd test/e2e
./test_explicit_replication_order5_ack2.sh
```

### 3. Run All Tests
```bash
cd test/e2e
./run_all.sh
```

## Test Configuration

### NUMA Node Binding
The test scripts use `numactl` to bind to specific NUMA nodes:
- `test_basic_publish.sh`: Uses node 1 (default)
- `test_explicit_replication_order5_ack2.sh`: Uses node 3 (matches real CXL machine)

**To adjust for your machine:**
Edit the `NUMA_BIND` variable in the test script:
```bash
NUMA_BIND="numactl --cpunodebind=3 --membind=3"  # Change 3 to your CXL NUMA node
```

### Test Output
All test logs are written to: `build/test_output/<test_name>/`
- `broker_*.log` - Broker logs
- `client.log` - Client test output

## What Each Test Validates

### test_basic_publish.sh
- ✅ 4 brokers start successfully
- ✅ Client can connect and publish
- ✅ No crashes or fatal errors
- ✅ Valid broker IDs

### test_explicit_replication_order5_ack2.sh
- ✅ ORDER=5 batch-based ordering
- ✅ ack_level=2 (ACK after replication)
- ✅ replication_factor=1 (one replica)
- ✅ ReplicationMetrics show active replication
- ✅ Replica disk files are written
- ✅ No pwrite errors
- ✅ Client completes without ACK stalls

## Troubleshooting

### Test fails with "DAX device not found"
- Verify `/dev/dax0.0` (or your DAX path) exists
- Check `config/embarcadero.yaml` has correct `device_path`
- Ensure you have permissions to access the device

### Test fails with "NUMA node not found"
- Check your actual NUMA topology: `numactl --hardware`
- Update `NUMA_BIND` in test script to match your CXL NUMA node
- Or update `numa_node` in `config/embarcadero.yaml`

### Test hangs waiting for broker readiness
- Check broker logs in `build/test_output/<test_name>/broker_*.log`
- Verify CXL size is reasonable (64GB may take 60-120s to map)
- Check for hugepage allocation failures

### ReplicationMetrics not found
- Metrics are logged every 10 seconds via `LOG(INFO)`
- Test may be too short - increase `TOTAL_MESSAGES` or wait longer
- Check log level: ensure `FLAGS_v=0` or higher for INFO logs

### Replica disk files not found
- Files are written to: `../../.Replication/disk*/embarcadero_replication_log*.dat`
- Check broker logs for replication thread errors
- Verify `replication_factor > 0` in topic creation

## Running via CTest (Optional)

If you re-enable `add_subdirectory(test)` in top-level `CMakeLists.txt`:
```bash
cd build
ctest -R e2e_explicit_replication_order5_ack2 -V
```

## Performance Notes

- **CXL mapping time**: 64GB CXL takes ~60-120 seconds to map on first broker
- **Test duration**: Basic test ~30s, replication test ~60-90s (includes replication catch-up)
- **Message count**: Keep `TOTAL_MESSAGES` small (1k-10k) for fast iteration during development
