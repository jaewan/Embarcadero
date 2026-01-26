# Bandwidth Test Results - BlogMessageHeader V2

**Date**: January 26, 2026  
**Test Configuration**: ORDER=5, ACK=1, Message Size=1024 bytes

## Test Results Summary

### Individual Test Runs

| Timestamp | BlogHeader Setting | Bandwidth (MB/s) | Status |
|-----------|-------------------|------------------|--------|
| 20260126_103637 | BlogHeader=1 | 39.72 | Success |
| 20260126_103304 | BlogHeader=1 | 6.81 | Success |
| 20260126_102952 | BlogHeader=0 | 54.03 | Success |
| 20260126_102402 | BlogHeader=0 | 4.76 | Success |

### Observations

1. **High Variance**: Both baseline and BlogHeader v2 show significant variance:
   - Baseline range: 4.76 - 54.03 MB/s
   - BlogHeader v2 range: 6.81 - 39.72 MB/s

2. **Most Recent Comparison**:
   - Baseline (20260126_102952): **54.03 MB/s**
   - BlogHeader v2 (20260126_103637): **39.72 MB/s**
   - Ratio: **73.5%** of baseline

3. **Earlier Comparison**:
   - Baseline (20260126_102952): **54.03 MB/s**
   - BlogHeader v2 (20260126_103304): **6.81 MB/s**
   - Ratio: **12.6%** of baseline

## Analysis

### Possible Causes of Variance

1. **System Load**: Tests run sequentially, system load may vary between runs
2. **Thermal Throttling**: CPU may throttle during extended runs
3. **Network Conditions**: Local network stack behavior
4. **Single Iteration**: Each test only ran 1 iteration (not statistically significant)

### Recommendations

1. **Run Extended Tests**: Execute 10+ iterations for statistical significance
2. **Use Thermal Recovery**: Enable `scripts/run_throughput_optimized.sh` for consistent results
3. **Control Environment**: Ensure consistent system load between baseline and BlogHeader tests
4. **Multiple Runs**: Run the comparison multiple times to account for variance

## Next Steps

To get reliable bandwidth comparison:

```bash
# Baseline - 10 iterations
export EMBARCADERO_USE_BLOG_HEADER=0
NUM_ITERATIONS=10 ORDER=5 ACK=1 MESSAGE_SIZE=1024 TOTAL_MESSAGE_SIZE=10737418240 \
  bash scripts/measure_performance_baseline.sh

# BlogHeader v2 - 10 iterations  
export EMBARCADERO_USE_BLOG_HEADER=1
NUM_ITERATIONS=10 ORDER=5 ACK=1 MESSAGE_SIZE=1024 TOTAL_MESSAGE_SIZE=10737418240 \
  bash scripts/measure_performance_baseline.sh
```

Then compare the mean values from the CSV files.

## Current Status

⚠️ **Inconclusive**: High variance in single-iteration tests makes it difficult to determine if there's a performance regression. The most recent BlogHeader test (39.72 MB/s) is within reasonable range of baseline (54.03 MB/s), suggesting the earlier low result (6.81 MB/s) may have been due to system load or other transient factors.

**Action Required**: Run extended multi-iteration tests for statistical significance.
