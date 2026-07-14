# Embarcadero Test Suite

## Current Status

- ✅ **E2E Tests:** Implemented in `e2e/` - Run actual broker clusters
- 🟡 **Unit Tests:** Archived in `archive/` - Disabled, need update for current architecture
- ❌ **Integration Tests:** Not yet implemented
- ❌ **Property Tests:** Not yet implemented (Paper Spec guarantees)

## Quick Start

```bash
# Build the project
cd build
cmake ..
make -j$(nproc)

# Run E2E tests
cd ../test/e2e
./run_all.sh

# Publication-style end-to-end cells
cd ../..
bash scripts/run_multiclient.sh
```

## Test Organization

```
test/
├── e2e/                    # End-to-end tests (working)
│   ├── run_all.sh
│   └── README.md
├── archive/                # Archived unit tests (disabled)
│   ├── embarlet/
│   │   ├── buffer_manager_test.cc
│   │   ├── callback_manager_test.cc
│   │   └── message_ordering_test.cc
│   ├── cxl_manager.cc
│   └── publish_test.cc
└── CMakeLists.txt
```

## Why Unit Tests Are Archived

The unit tests were written for an older architecture (v0 with TInode) and are currently broken:
- Interfaces changed (BufferManager, SegmentManager APIs)
- Data structures changed (TInode → Bmeta/Blog migration in progress)
- Mocks don't match current implementation

They're **archived, not deleted** because:
- Show good testing patterns (gtest, gmock, concurrency tests)
- Can be updated when architecture stabilizes
- Reference for future unit test development

## Test Coverage

### What's Tested (E2E)
- ✅ Scenario-specific broker/client correctness tests listed in `e2e/run_all.sh`
- ✅ Publication-style broker/client runs through `scripts/run_multiclient.sh`

### What's NOT Tested (High Priority Gaps)
- ❌ FIFO ordering enforcement (Property 3d)
- ❌ f+1 durability (Property 4a)
- ❌ Broker failure recovery
- ❌ Sequencer failover
- ❌ Cache coherence primitives
- ❌ CXL memory allocation correctness
- ❌ Network partition handling

## Adding New Tests

### E2E Test (Recommended)
1. Copy a current scenario script from `e2e/` as a template; do not use the
   retired `test_basic_publish.sh`, which is intentionally excluded from CTest.
2. Modify test scenario
3. Add to `e2e/run_all.sh`
4. Add to `CMakeLists.txt`

### Unit Test (When Architecture Stabilizes)
1. Create `<component>_test.cc` using gtest
2. Add to `CMakeLists.txt`
3. Run: `cd build && make && ctest`

## Future Test Roadmap

### Phase 1: E2E Coverage (Current)
- [x] Basic publish flow
- [ ] Ordering guarantees
- [ ] Durability guarantees
- [ ] Failure scenarios

### Phase 2: Unit Test Revival
- [ ] Update BufferManager tests
- [ ] Add CXLManager tests
- [ ] Add HeartBeatManager tests
- [ ] Add Topic tests

### Phase 3: Property-Based Tests
- [ ] Verify Property 3d (Strong Total Ordering)
- [ ] Verify Property 4a (Full Durability)
- [ ] Verify cache coherence laws

### Phase 4: CI Integration
- [ ] GitHub Actions workflow
- [ ] Automated test runs on PR
- [ ] Code coverage reporting

## Test Configuration

For faster tests, use smaller CXL sizes in `config/embarcadero.yaml`:

```yaml
cxl:
  size: 4294967296             # 4GB (fast) vs 68719476736 (64GB, slow)
  emulation_size: 4294967296
```

4GB CXL maps in ~4 seconds vs 66 seconds for 64GB.

## Resources

- Test output: `build/test_output/<test_name>/`
- E2E test guide: `e2e/README.md`
- Test assessment: `../TEST_ASSESSMENT.md`
- Paper spec: `../docs/memory-bank/paper_spec.md`
