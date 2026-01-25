# Performance Validation Plan

**Created:** 2026-01-26  
**Status:** Scripts Ready - Manual Execution Required  
**Purpose:** Establish performance baseline and identify optimization opportunities

---

## Overview

This plan implements the recommended performance validation approach from the senior expert evaluation. Instead of completing Task 4.3 refactoring (low impact), we focus on data-driven optimization decisions.

---

## Scripts Created

### 1. `scripts/measure_performance_simple.sh`
**Purpose:** Run multiple performance test iterations and calculate statistics

**Usage:**
```bash
cd /home/domin/Embarcadero
NUM_ITERATIONS=10 bash scripts/measure_performance_simple.sh
```

**Output:**
- `data/performance_baseline/baseline_TIMESTAMP.csv` - Raw results
- `data/performance_baseline/summary_TIMESTAMP.txt` - Statistics summary

**Metrics Calculated:**
- Mean, Median, StdDev
- Min, Max, P95, P99
- Coefficient of Variation (variance %)
- Assessment (low/moderate/high variance)

**Configuration:**
- `NUM_ITERATIONS` - Number of test runs (default: 10)
- `ORDER`, `ACK`, `MESSAGE_SIZE`, `TOTAL_MESSAGE_SIZE` - Test parameters

---

### 2. `scripts/profile_hot_paths.sh`
**Purpose:** Profile CPU bottlenecks using `perf`

**Usage:**
```bash
cd /home/domin/Embarcadero
PROFILE_DURATION=30 bash scripts/profile_hot_paths.sh
```

**Requirements:**
- `perf` installed: `sudo apt-get install linux-perf`
- Optional: `flamegraph.pl` for flamegraph generation

**Output:**
- `data/performance_baseline/perf_data_TIMESTAMP.data` - Raw perf data
- `data/performance_baseline/perf_report_TIMESTAMP.txt` - Flat report
- `data/performance_baseline/perf_flamegraph_TIMESTAMP.svg` - Flamegraph (if available)

**Metrics:**
- Top 20 functions by CPU time
- Cache statistics (LLC loads/stores, misses)
- Branch prediction statistics
- Call graph for hot paths

---

### 3. `scripts/measure_mutex_contention.sh`
**Purpose:** Measure mutex contention for `global_seq_batch_seq_mu_`

**Usage:**
```bash
cd /home/domin/Embarcadero
MEASURE_DURATION=60 bash scripts/measure_mutex_contention.sh
```

**Requirements:**
- `perf` installed
- Kernel support for lock contention events (may fall back to context switch metrics)

**Output:**
- `data/performance_baseline/mutex_contention_TIMESTAMP.txt` - Contention report

**Metrics:**
- Lock contentions per second
- Wait time (cycles)
- Context switches
- Assessment: Low/Moderate/High contention

**Decision Criteria:**
- <100 contentions/sec → Lock-free CAS not needed
- 100-1000 contentions/sec → Consider lock-free CAS
- >1000 contentions/sec → Lock-free CAS recommended

---

## Execution Plan

### Phase 1: Baseline Measurement (1-2 hours)

**Step 1: Run Performance Baseline**
```bash
cd /home/domin/Embarcadero
NUM_ITERATIONS=10 bash scripts/measure_performance_simple.sh
```

**Expected Results:**
- 10 successful iterations
- Mean bandwidth: ~9-11 GB/s
- Variance: <15% (low to moderate)

**Action Items:**
- If variance >20%: Investigate system load
- If mean <8 GB/s: Investigate performance regression
- If mean >12 GB/s: Document as improvement

---

### Phase 2: Profiling (30-60 minutes)

**Step 2: Profile Hot Paths**
```bash
cd /home/domin/Embarcadero
PROFILE_DURATION=60 bash scripts/profile_hot_paths.sh
```

**Analysis:**
1. Review top 20 functions
2. Identify CPU hotspots (>5% of total time)
3. Check cache miss rates
4. Review branch misprediction rates

**Action Items:**
- If cache misses >10%: Consider prefetching or data layout optimization
- If branch mispredictions >5%: Consider branchless code paths
- If single function >20% CPU: Target for optimization

---

### Phase 3: Mutex Contention (30 minutes)

**Step 3: Measure Mutex Contention**
```bash
cd /home/domin/Embarcadero
MEASURE_DURATION=60 bash scripts/measure_mutex_contention.sh
```

**Decision Tree:**
```
Contention Rate?
├─ <100/sec → ✓ Task 4.3 lock-free CAS NOT needed
├─ 100-1000/sec → ⚠ Consider Task 4.3 completion
└─ >1000/sec → ✗ Task 4.3 lock-free CAS RECOMMENDED
```

---

## Expected Outcomes

### Scenario A: Low Contention, Stable Performance
**Findings:**
- Mutex contention <100/sec
- Performance variance <15%
- Bandwidth 9-11 GB/s

**Decision:**
- ✅ Mark Task 4.3 as complete (lock-free CAS not needed)
- ✅ Focus on Phase 3/4 work (BlogMessageHeader, Sequencer recovery)
- ✅ Document performance as stable

---

### Scenario B: Moderate Contention, Some Variance
**Findings:**
- Mutex contention 100-1000/sec
- Performance variance 15-20%
- Bandwidth 8-10 GB/s

**Decision:**
- ⚠ Complete Task 4.3 lock-free CAS (if profiling shows it's a bottleneck)
- ⚠ Investigate variance sources (system load, NUMA effects)
- ⚠ Consider additional optimizations from profiling

---

### Scenario C: High Contention, High Variance
**Findings:**
- Mutex contention >1000/sec
- Performance variance >20%
- Bandwidth <8 GB/s or >12 GB/s

**Decision:**
- ✗ Complete Task 4.3 lock-free CAS (high priority)
- ✗ Investigate root causes (system load, hardware issues)
- ✗ Profile deeper to identify bottlenecks

---

## Documentation Updates

After completing measurements, update:

1. **`docs/memory-bank/activeContext.md`**
   - Add performance baseline section
   - Document variance statistics
   - Update Task 4.3 status based on contention findings

2. **`docs/memory-bank/spec_deviation.md`**
   - Add DEV-007 if lock-free CAS is implemented
   - Document performance impact

3. **`SESSION_2026_01_26_SUMMARY.md`**
   - Add performance validation results
   - Document optimization decisions

---

## Troubleshooting

### Issue: Tests Timeout
**Solution:** Increase timeout in scripts or run tests manually
```bash
# Manual test run
export ORDER=5 ACK=1 MESSAGE_SIZE=1024 TOTAL_MESSAGE_SIZE=10737418240
bash scripts/run_throughput.sh
```

### Issue: perf Not Available
**Solution:** Install perf
```bash
sudo apt-get install linux-perf
# Or on some systems:
sudo apt-get install perf
```

### Issue: Lock Events Not Available
**Solution:** Script falls back to context switch metrics (still useful)

---

## Next Steps After Validation

1. **If Task 4.3 completion needed:**
   - Implement lock-free CAS for `next_expected_batch_seq_`
   - Test and measure improvement
   - Document in spec_deviation.md

2. **If performance is stable:**
   - Move to Phase 3 work (Sequencer recovery)
   - Begin BlogMessageHeader migration planning
   - Consider hardware validation on real CXL

3. **If bottlenecks identified:**
   - Prioritize optimizations based on profiling data
   - Create optimization plan
   - Measure impact of each optimization

---

**Last Updated:** 2026-01-26  
**Status:** Scripts Ready - Awaiting Manual Execution
