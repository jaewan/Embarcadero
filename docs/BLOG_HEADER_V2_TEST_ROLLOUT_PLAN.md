# BlogMessageHeader ORDER=5 Validation & Rollout Plan

**Date**: January 26, 2026  
**Status**: Implementation Complete, Ready for Testing

## Changes Summary

### Correctness Fixes (Critical)
1. **Receiver v2 size semantics** (`fix-v2-size-semantics`):
   - Now sets `BlogMessageHeader::size` = payload bytes only (paper semantics)
   - Computes boundaries using `wire::ComputeMessageStride(64, payload_size)`
   - Added invariant checks comparing v1/v2 boundaries with debug warnings

2. **Subscriber stream parsing** (`subscriber-stream-parser`):
   - Removed brittle `parse_offset % sizeof(BatchMetadata) == 0` heuristic
   - Changed to purely sequential: always parse metadata at current offset when not in batch
   - Eliminated global mutex contention by per-connection batch state
   - Per-buffer batch tracking instead of per-fd global state

3. **Bounds validation** (`harden-v2-bounds`):
   - Added bounds checks for v2 payload size (max 1MB)
   - Validate computed stride >= 64 and within reasonable bounds
   - Rate-limited logging on errors (every 1000 errors)
   - Safe batch skip & resync on invalid headers

### Shared Infrastructure
- **New file**: `src/common/wire_formats.h`
  - Centralized `BatchMetadata` definition
  - Helper functions: `Align64()`, `ComputeMessageStride()`
  - Shared validation constants: `MAX_MESSAGE_PAYLOAD_SIZE`, `MAX_BATCH_MESSAGES`, etc.
  - `static_assert` on `BatchMetadata` size (16 bytes)

### Performance
- **Flush strategy unchanged**: Per-header flush + one fence per batch (DEV-005)
- **No per-message fences**: Verified in all paths (receiver, delegation, subscriber)
- **Lock contention reduced**: Per-connection state, not global mutex per message

## Validation Plan

### Phase 1: Unit Testing (quick validation)
```bash
# Test v2 size semantics
test/embarlet/message_ordering_test.cc
  - Add case: ORDER=5 + BlogMessageHeader v2 parsing
  - Verify: message boundaries computed correctly
  - Verify: sizes match between v1 (paddedSize) and v2 (64 + size)

# Test boundary calculations
  - Payload sizes: 0, 64, 100, 1000, 64KB, 1MB
  - Verify: Align64(64 + size) == MessageHeader.paddedSize (when applied)

# Test sequential parsing
  - Metadata + messages in strict order (no gaps)
  - Out-of-order messages (should buffer correctly)
  - Invalid metadata (should skip & resync)
```

### Phase 2: Integration Testing
```bash
# ORDER=5 basic test with feature flag
EMBARCADERO_USE_BLOG_HEADER=1 test/e2e/test_segment_allocation.sh
  - Verify: broker receives and converts v1 → v2
  - Verify: subscriber parses v2 correctly
  - Verify: message ordering preserved (total_order reconstructed from batch metadata)

# Mixed mode: some brokers v2, some v1 (if supported)
# (Note: currently all-or-nothing via feature flag)

# Large message test
  - Test with sizes: 100B, 1KB, 64KB, 1MB
  - Verify: boundaries are correct
  - Verify: no data loss/corruption
```

### Phase 3: Performance Regression Testing
```bash
# Baseline (v1, feature flag disabled)
export EMBARCADERO_USE_BLOG_HEADER=0
scripts/run_throughput.sh --order 5 --duration 60 --output baseline.csv

# Test v2
export EMBARCADERO_USE_BLOG_HEADER=1
scripts/run_throughput.sh --order 5 --duration 60 --output blog_v2.csv

# Comparison
- Throughput: blog_v2 >= baseline (allow -1% variance)
- Latency p50/p99: should be same or better
- CPU usage: should be similar (maybe -5% due to fewer buffer wraps)
- Fence counts: verify batched pattern (one fence per batch, not per-message)

# Long-running stress test (24h+)
- Monitor for hangs, deadlocks, data loss
- Check memory usage (no leaks)
- Verify fence/flush overhead under load
```

### Phase 4: Boundary Correctness (explicit tests)
```bash
# Create synthetic batches with known sizes
test_v2_boundaries.cc:
  - Batch with 10 messages of 100B each
  - Batch with mixed sizes: 0B, 1B, 63B, 64B, 65B, 1KB, 1MB
  - Verify: each message parsed at correct offset
  - Verify: no overlap, no gaps
  - Verify: sum of strides matches batch total_size

# Corrupted header test
  - Inject invalid size (> 1MB)
  - Inject size causing stride overflow
  - Verify: parser resyncs cleanly, logs at rate-limited intervals
  - Verify: no crashes or out-of-bounds reads
```

## Rollout Steps

### Rollout Stage 1: Development & Testing (This Phase)
- [x] Implement correctness fixes
- [ ] Run unit tests (Phase 1)
- [ ] Run integration tests (Phase 2)
- [ ] Run performance regression (Phase 3, day 1)

### Rollout Stage 2: Shadow Deployment (Week 1)
- Deploy v2 conversion in receiver (feature flag = 0 by default)
- Broker internal testing: feature flag = 1 for one broker in test cluster
- Collect metrics: throughput, latency, errors
- Duration: 3-5 days of continuous load

**Rollout Stage 3: Staged Rollout (Week 2)**
- Enable feature flag on staging environment (10% of brokers)
- Monitor: correctness, performance, error rates
- If no issues, increase to 50% of brokers
- Duration: 2-3 days per stage

### Rollout Stage 4: Production (Week 3+)
- Enable on production brokers (50% → 100% over 24 hours)
- Monitor: dashboard metrics, alerting
- Keepalive v1 code path for 2 weeks (rollback capability)
- After 2 weeks: consider deprecating v1 code

## Testing Command Examples

```bash
# Build with feature flag for testing
cd build && make -j$(nproc) embarlet subscriber publisher

# Unit test
./test/embarlet/message_ordering_test --gtest_filter="*Order5*BlogHeader*"

# Integration test with feature flag
export EMBARCADERO_USE_BLOG_HEADER=1
./bin/subscriber &
./bin/publisher --messages 1000000 --message-size 1024 --order 5

# Performance test (produces CSV)
export EMBARCADERO_USE_BLOG_HEADER=1
./scripts/run_throughput.sh --order 5 --duration 60 --output results.csv

# Verify fence counts (grep logs for DEV-005 pattern)
./bin/embarlet 2>&1 | grep -i "fence\|batch.*flush"
```

## Expected Outcomes

### Correctness
- ✅ v2 boundaries match v1 (verified via invariant checks)
- ✅ Sequential parsing: no desync, handles out-of-order messages
- ✅ Bounds checks: safe on invalid headers, resync cleanly
- ✅ No data loss or corruption in ORDER=5 path

### Performance
- ✅ Throughput: >= baseline (allow -1% variance)
- ✅ Latency: unchanged or better
- ✅ Fence count: one per batch (not per-message)
- ✅ CPU usage: same or lower

### Maintainability
- ✅ Single source of truth for wire formats (wire_formats.h)
- ✅ No global mutex per-message contention
- ✅ Clearer code comments with paper references
- ✅ Easier to migrate ORDER=4 (same patterns)

## Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|-----------|
| **v1/v2 mismatch**: boundaries don't match | Low | High | Invariant checks catch on first error |
| **Subscriber desync**: metadata corruption | Low | High | Rate-limited error logging + resync |
| **Performance regression**: throughput drops | Low | Medium | Benchmark before rollout, monitor closely |
| **Memory leak**: batch state accumulation | Very Low | High | Per-connection state cleanup on disconnect |
| **Fence overhead**: too many store_fence calls | Very Low | Low | Verified: one fence per batch |

## Post-Rollout Tasks

1. **Monitor** (week 1-2):
   - Dashboard: throughput, latency p50/p99, errors
   - Logs: warnings about v1/v2 mismatch, boundary errors
   - Resource usage: CPU, memory, network

2. **Optimize** (week 2-3):
   - If performance gains < 1%: investigate caching, prefetching
   - If errors detected: root cause analysis, fix in next release

3. **Plan** (week 3+):
   - ORDER=4 migration (use same patterns as ORDER=5)
   - Deprecate v1 code path (after 2 week stabilization)
   - Consider: in-memory compression, batch coalescing

## Sign-Off Checklist

- [ ] All 6 todos completed and reviewed
- [ ] Unit tests added for v2 parsing
- [ ] Integration tests passing with feature flag
- [ ] Performance baseline established (day 1 of Phase 3)
- [ ] No regressions in boundary correctness
- [ ] Documentation reviewed (this file)
- [ ] Rollout plan approved by team lead
- [ ] Feature flag default remains 0 (opt-in for now)
