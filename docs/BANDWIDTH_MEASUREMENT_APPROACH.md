# Bandwidth Measurement Approach - Fixed Implementation

## Problem Statement

Previous bandwidth measurements showed inconsistent results (4-60 MB/s) when the system is expected to achieve **9-10 GB/s** (9000-10000 MB/s) with:
- 10GB total message size
- 1KB payload per message  
- 4 brokers
- ORDER=5, ACK=1

## Root Causes Identified

### 1. **Log Scraping Instead of CSV Parsing**
- `measure_performance_baseline.sh` was using fragile `grep` on log output
- Multiple "Bandwidth:" lines could cause wrong extraction
- Timeouts and partial runs weren't handled properly

### 2. **Test Configuration Mismatch**
- Need to verify we're running the same test type (publish-only vs E2E)
- Default `run_throughput.sh` uses `test_number=5` (publish-only)
- Historical 9-10 GB/s might have been E2E (`test_number=1`)

### 3. **Environment Not Verified**
- Hugepages availability not checked
- NUMA binding effectiveness not verified
- CPU governor not checked (should be "performance")

### 4. **Insufficient Iterations**
- Single-iteration tests have high variance
- Need 5-10 iterations for statistical significance

## Solution Implemented

### New Script: `scripts/measure_bandwidth_proper.sh`

**Key Features:**

1. **CSV-Based Measurement**
   - Uses `--record_results` flag to generate CSV
   - Parses `data/throughput/pub/result.csv` (authoritative source)
   - Falls back to log parsing only if CSV unavailable

2. **Environment Verification**
   - Checks hugepages availability and size
   - Verifies NUMA node configuration
   - Checks CPU governor (warns if not "performance")

3. **Proper Configuration**
   - Default: 10GB total, 1KB payload, ORDER=5, ACK=1
   - Uses `test_number=5` (publish-only) by default
   - Configurable via environment variables

4. **Statistical Analysis**
   - Runs multiple iterations (default: 3, configurable)
   - Calculates mean, stddev, CV, P95
   - Compares baseline vs BlogHeader v2
   - Writes summary to file

5. **Error Handling**
   - Proper cleanup between iterations
   - Timeout handling (300s per iteration)
   - Clear failure reporting

## Usage

```bash
# Basic usage (3 iterations each, 10GB total)
bash scripts/measure_bandwidth_proper.sh

# Custom iterations and message size
NUM_ITERATIONS=5 TOTAL_MESSAGE_SIZE=10737418240 bash scripts/measure_bandwidth_proper.sh

# For E2E test (if historical baseline was E2E)
TEST_NUMBER=1 NUM_ITERATIONS=5 bash scripts/measure_bandwidth_proper.sh
```

## Expected Results

### Baseline (BlogHeader=0)
- **Expected**: 9000-10000 MB/s (9-10 GB/s)
- **If lower**: Check environment (hugepages, NUMA, CPU governor)

### BlogHeader v2 (BlogHeader=1)
- **Acceptable**: ≥98% of baseline (≥8820 MB/s)
- **Regression**: <98% of baseline

## Output Files

- **CSV Results**: `data/throughput/pub/result.csv` (per iteration)
- **Summary**: `data/bandwidth_comparison/summary_<timestamp>.txt`
- **Logs**: `/tmp/test_<variant>_<iteration>.log`

## Next Steps

1. **Run Extended Tests**: Once basic validation passes, run 10 iterations
2. **Profile if Regression**: If BlogHeader shows regression, use `perf` to identify hotspots
3. **Check Flush/Fence Behavior**: Verify BlogHeader isn't adding excessive flushes
4. **Compare Test Types**: If publish-only differs from E2E, investigate pipeline stages

## Notes

- Each iteration takes ~1-2 seconds for 10GB at 9-10 GB/s (plus broker startup/teardown)
- Total test time: ~5-10 minutes for 3 iterations of each variant
- System load should be minimal during testing for consistent results
- Consider setting CPU governor to "performance" for production measurements
