# End-to-End Tests

These tests validate the complete Embarcadero system by running actual brokers and clients.

## Test Structure

```
test/e2e/
├── test_basic_publish.sh       - Basic publish flow (4 brokers, simple throughput)
├── test_ordering.sh            - FIFO ordering guarantee (Property 3d)
├── test_durability.sh          - f+1 replication (Property 4a)
├── test_broker_failure.sh      - Broker crash recovery
├── test_sequencer_failover.sh  - Sequencer migration
└── run_all.sh                  - Run all E2E tests
```

## Running Tests

### Single Test
```bash
cd test/e2e
./test_basic_publish.sh
```

### All Tests
```bash
cd test/e2e
./run_all.sh
```

### From Build Directory
```bash
cd build
make test  # Runs all configured tests
```

## Test Requirements

1. **Built binaries:**
   - `build/bin/embarlet`
   - `build/bin/throughput_test`

2. **Configuration files:**
   - `config/embarcadero.yaml`
   - `config/client.yaml`

3. **System resources:**
   - Hugepages enabled (2GB minimum for 4GB CXL config)
   - NUMA node 1 available (for memory binding)
   - Ports 12140-12143 available

## Test Output

Logs are written to: `build/test_output/<test_name>/`
- `broker_*.log` - Broker logs
- `client.log` - Client test output

## What Each Test Validates

### test_basic_publish.sh
- ✅ Brokers start successfully
- ✅ Cluster formation (head + 3 followers)
- ✅ Client can connect and publish
- ✅ No crashes or fatal errors
- ✅ Valid broker IDs (not -1)

### test_ordering.sh (TODO)
- ✅ FIFO ordering per publisher
- ✅ Out-of-order messages buffered
- ✅ Weak total ordering enforced

### test_durability.sh (TODO)
- ✅ Messages replicated to f+1 brokers
- ✅ ACK only after disk fsync
- ✅ Data survives broker crash

### test_broker_failure.sh (TODO)
- ✅ Broker crash detected
- ✅ Cluster continues operating
- ✅ Clients redirect to other brokers

### test_sequencer_failover.sh (TODO)
- ✅ Sequencer migrates to new broker
- ✅ Ordering state recovered from CXL
- ✅ No message loss during failover

## Adding New Tests

1. Create `test_<name>.sh` in this directory
2. Follow the template from `test_basic_publish.sh`
3. Add to `run_all.sh`
4. Document what it validates above

## Test Best Practices

- **Always cleanup:** Use `trap cleanup EXIT` to kill brokers
- **Check exit codes:** `set -euo pipefail` catches failures
- **Assert explicitly:** Don't just run commands, verify output
- **Timeout guards:** All waits have timeout (90s for broker init)
- **Log validation:** Check for errors in logs, not just exit codes
- **Fast by default:** Use small CXL sizes (4GB) and message counts (1000)
