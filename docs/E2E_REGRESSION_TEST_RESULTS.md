# E2E Regression Test Results - BlogMessageHeader V2

**Date**: January 26, 2026  
**Status**: ✅ Correctness Tests Passed | ⏳ Performance Tests (Scripts Created)

## Test Execution Summary

### ✅ Build Gate - PASSED
- **Binary verification**: Both `embarlet` and `throughput_test` built successfully
- **Location**: `build/bin/embarlet` (19MB), `build/bin/throughput_test` (18MB)
- **Status**: All targets compile without errors

### ✅ E2E Correctness Tests - PASSED

**Test Script**: `scripts/run_e2e_regression.sh`

**Baseline (EMBARCADERO_USE_BLOG_HEADER=0)**:
- ✅ All E2E tests passed
- ✅ No error signatures found in logs
- ✅ No crashes, hangs, or fatal errors

**BlogHeader v2 (EMBARCADERO_USE_BLOG_HEADER=1)**:
- ✅ All E2E tests passed
- ✅ No error signatures found in logs
- ✅ No crashes, hangs, or fatal errors

**Error Signatures Checked** (none found):
- `FATAL`
- `SIGSEGV`
- `Invalid paddedSize`
- `boundary mismatch`
- `payload size exceeds max`
- `would walk past batch end`
- `assert`

**Log Locations**: `build/test_output/<test_name>/broker_*.log`

### ⏳ Throughput Regression - Scripts Created

**Test Scripts Created**:
1. `scripts/run_throughput_regression.sh` - Full regression with CSV parsing
2. `scripts/run_throughput_regression_simple.sh` - Simplified direct iteration runner

**Approach**:
- Uses `scripts/measure_performance_baseline.sh` for statistical measurement
- Runs N iterations (default: 10, can be reduced for faster testing)
- Compares mean/p95 throughput and coefficient of variation (CV)
- Acceptance criteria:
  - Mean throughput: BlogHeader >= 98% of baseline
  - P95 throughput: BlogHeader >= 98% of baseline
  - CV < 10% for both variants

**Status**: Scripts validated, ready for execution. Full 10-iteration runs recommended for production validation.

**Note**: Throughput tests require longer execution time (10 iterations × ~1 minute per iteration = ~10+ minutes). For quick validation, use `NUM_ITERATIONS=3`.

### ⏳ Latency Regression - Script Created

**Test Script**: `scripts/run_latency_regression.sh`

**Test Matrix**:
- Message sizes: 256B, 1KB, 64KB
- Trials per size: 3 (configurable, plan recommends 5)
- Data volume: 256MB per test (sufficient for latency mode)

**Approach**:
- Starts 4-broker cluster
- Runs `throughput_test -t 2` (latency mode) for each message size
- Captures `cdf_latency_us.csv` and `latency_stats.csv`
- Stores results in `data/latency/order5/baseline/` and `data/latency/order5/blog_v2/`

**Acceptance Criteria**:
- p50 latency: within ±5% of baseline
- p99 latency: not worse than +10% of baseline

**Status**: Script created and validated. Ready for execution.

## Test Artifacts

### Correctness Tests
- **Logs**: `build/test_output/**/broker_*.log`
- **Test output**: `/tmp/e2e_regression_output.log`
- **Status**: ✅ All tests passed, no errors

### Performance Tests (When Run)
- **Throughput CSV**: `data/performance_baseline/baseline_<timestamp>.csv`
- **Throughput Summary**: `data/performance_baseline/summary_<timestamp>.txt` (if generated)
- **Latency CDF**: `data/latency/order5/{baseline,blog_v2}/cdf_<size>.csv`
- **Latency Stats**: `data/latency/order5/{baseline,blog_v2}/stats_<size>.csv`

## Quick Test Commands

### E2E Correctness (Quick - ~2 minutes)
```bash
cd /home/domin/Embarcadero
bash scripts/run_e2e_regression.sh
```

### Throughput Regression (Medium - ~10-20 minutes)
```bash
cd /home/domin/Embarcadero
NUM_ITERATIONS=5 bash scripts/run_throughput_regression.sh
```

### Latency Regression (Long - ~30-60 minutes)
```bash
cd /home/domin/Embarcadero
NUM_TRIALS=3 bash scripts/run_latency_regression.sh
```

## Production Validation Recommendations

For full production readiness, run:

1. **Extended Throughput Test**:
   ```bash
   NUM_ITERATIONS=10 ORDER=5 bash scripts/measure_performance_baseline.sh
   # Then with BlogHeader=1
   EMBARCADERO_USE_BLOG_HEADER=1 NUM_ITERATIONS=10 ORDER=5 bash scripts/measure_performance_baseline.sh
   ```

2. **Extended Latency Test**:
   ```bash
   NUM_TRIALS=5 bash scripts/run_latency_regression.sh
   ```

3. **Long-Running Stability** (30-60 minutes):
   - Use `scripts/run_throughput_optimized.sh` with thermal recovery
   - Monitor for memory leaks, hangs, error accumulation

## Exit Criteria Status

- ✅ **Correctness**: All E2E tests pass in both modes
- ⏳ **Throughput**: Scripts ready, execution pending (recommend 10 iterations)
- ⏳ **Latency**: Script ready, execution pending (recommend 5 trials per size)
- ✅ **Error Signatures**: None found in correctness tests

## Next Steps

1. **Immediate**: E2E correctness validated ✅
2. **Short-term**: Run throughput regression with 5-10 iterations
3. **Short-term**: Run latency regression with 3-5 trials
4. **Before Production**: Extended stability test (30-60 min)

## Notes

- All test scripts include proper cleanup (kill processes, remove temp files)
- Tests use existing infrastructure (`run_throughput.sh`, `measure_performance_baseline.sh`)
- Error signature checking is automated in E2E regression script
- Performance comparison logic handles edge cases (zero results, missing files)
