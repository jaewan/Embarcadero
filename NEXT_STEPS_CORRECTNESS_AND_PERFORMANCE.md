# Next Steps: Correctness, Observability, and Performance

**Date:** 2026-01-29
**Status:** Immediate stall resolved (100% completion on 10GB test @ 3 GB/s)
**Commit:** 7f0044a - Fix critical ring gating bug and enable non-blocking architecture

---

## Current State Summary

### ✅ RESOLVED
- **Ring gating bug:** Fixed circular buffer logic (was: `consumed >= offset`, now: proper `in_flight < ring_size`)
- **Client buffer capacity:** Increased from 256MB to 768MB per thread (12GB total for 10GB tests)
- **Non-blocking architecture:** Enabled by default with staging pool and CXL allocation workers
- **Test results:**
  - 1GB test: 100% ACKs, ~446 MB/s, 0 ring full, 0 drops
  - 10GB test: 100% completion, ~3078 MB/s, 1 transient ring full, 0 drops

### 🔴 KNOWN ISSUES (Correctness)
1. Per-client ordering not enforced in BrokerScannerWorker5 (ORDER=5)
2. BlogMessageHeader `received=1` flush missing after NetworkManager sets fields
3. Throughput at 3 GB/s vs 10 GB/s target
4. Per-broker ACK diagnostics missing

---

## Prioritized Work Plan

### Phase 1: Correctness (CRITICAL - Must Fix)

#### 1.1 Restore Per-Client Ordering in BrokerScannerWorker5 [HIGH]

**Reference:** `docs/SENIOR_CODE_REVIEW_PUBLISHER_TO_ACK.md` §2.1

**Issue:**
ORDER=5 is specified to be globally ordered per client. The current "process all batches in ring order" path dropped:
- `next_expected_batch_seq_` tracking per client
- `skip_batch` logic for out-of-order batches
- `ProcessSkipped5()` deferred processing

This works fine for single-client, in-order, round-robin delivery (current 10GB test), but is **wrong** if:
- Batches arrive out-of-order on a broker
- Multiple clients send to the same broker
- Network reordering occurs

**Impact:**
- **Immediate:** None (single client test works)
- **Production:** Violates ordering guarantees, potential data corruption

**Action Required:**
```cpp
// File: src/embarlet/topic.cc BrokerScannerWorker5()

// Add per-client state tracking
std::unordered_map<uint32_t, size_t> next_expected_batch_seq;  // client_id -> next_batch_seq
std::vector<BatchHeader*> skipped_batches;

// In main loop, after reading batch_header:
uint32_t client_id = current_batch_header->client_id;
size_t batch_seq = current_batch_header->batch_seq;
size_t expected_seq = next_expected_batch_seq[client_id];

if (batch_seq < expected_seq) {
    // Duplicate, skip
    LOG(WARNING) << "Duplicate batch: client=" << client_id
                 << " seq=" << batch_seq << " expected=" << expected_seq;
    current_batch_header = next_batch_header;
    continue;
} else if (batch_seq > expected_seq) {
    // Out-of-order, defer
    skipped_batches.push_back(current_batch_header);
    current_batch_header = next_batch_header;
    continue;
}

// In-order batch, process normally
AssignOrder5(header_to_process, start_total_order, header_for_sub);
next_expected_batch_seq[client_id] = batch_seq + 1;

// After processing, check skipped batches
ProcessSkipped5(skipped_batches, ring_start_default, ring_end, ...);
```

**Testing:**
- Multi-client test with intentional reordering
- Verify ordering is preserved per client
- Check that `ProcessSkipped5` successfully processes deferred batches

**Estimated Effort:** 2-3 hours

---

#### 1.2 Add CXL Flush After BlogMessageHeader `received=1` [MEDIUM]

**Reference:** `docs/SENIOR_CODE_REVIEW_PUBLISHER_TO_ACK.md` §2.2

**Issue:**
After NetworkManager sets `current_msg->received = 1` and `ts` fields, there is no CXL flush or store_fence:

```cpp
// File: src/network_manager/network_manager.cc:2150-2165
for (size_t msg_idx = 0; msg_idx < num_msg; msg_idx++) {
    current_msg->received = 1;
    current_msg->ts = rdtsc();
    current_msg = reinterpret_cast<Embarcadero::BlogMessageHeader*>(
        reinterpret_cast<uint8_t*>(current_msg) + stride);
}
// NO FLUSH HERE! ⚠️
```

Any reader polling `received` or using `ts` (replication, subscriber) may see stale cached values.

**Impact:**
- **Current ACK path:** Not required (sequencer only uses BatchHeader)
- **Replication/Subscriber:** Reads may see `received=0` or stale `ts`

**Action Required:**
```cpp
// After the loop that sets received/ts:
for (size_t msg_idx = 0; msg_idx < num_msg; msg_idx++) {
    current_msg->received = 1;
    current_msg->ts = rdtsc();

    // Flush each touched cache line
    CXL::flush_cacheline(current_msg);

    current_msg = reinterpret_cast<Embarcadero::BlogMessageHeader*>(
        reinterpret_cast<uint8_t*>(current_msg) + stride);
}
CXL::store_fence();  // Ensure all flushes complete
```

**Alternative (Batched Flush):**
Flush every N messages or at end of batch to reduce overhead.

**Testing:**
- Replication test with BlogMessageHeader polling
- Subscriber test verifying `received=1` visibility
- Performance test to measure flush overhead

**Estimated Effort:** 1 hour

---

### Phase 2: Observability (IMPORTANT - Debugging Aid)

#### 2.1 Add Per-Broker ACK Diagnostics [HIGH]

**Reference:** `docs/INVESTIGATION_618888_ACK_STALL.md` §4 and Senior Review §5

**Issue:**
When `Poll()` is waiting for ACKs, logs show:
```
Waiting for acknowledgments, received 618888 out of 10485760
```

But we don't know which broker(s) are lagging. This makes debugging future stalls very difficult.

**Action Required:**
```cpp
// File: src/client/publisher.cc Poll()

// When waiting, log per-broker progress every N seconds
if (elapsed > 5 && elapsed % 5 == 0) {
    std::stringstream ss;
    ss << "Waiting for ACKs: [";
    for (size_t i = 0; i < num_brokers; i++) {
        size_t acked = acked_messages_per_broker_[i].load(std::memory_order_relaxed);
        if (i > 0) ss << ", ";
        ss << "B" << i << "=" << acked;
    }
    ss << "] total=" << client_order_.load() << "/" << n;
    LOG(INFO) << ss.str();
}
```

**Expected Output:**
```
I Waiting for ACKs: [B0=986828, B1=986828, B2=986828, B3=0] total=2960484/10485760
```

This immediately shows broker 3 is stuck.

**Testing:**
- Run 10GB test and verify per-broker logging
- Simulate broker failure to verify diagnostic value

**Estimated Effort:** 30 minutes

---

#### 2.2 Verify Config and Ring Size [MEDIUM]

**Action Required:**

1. **Confirm config file used:**
```bash
# Verify run_throughput.sh uses config/embarcadero.yaml
grep -E "CONFIG|YAML" scripts/run_throughput.sh

# Check batch_headers_size in config
grep batch_headers_size config/embarcadero.yaml
```

2. **Verify ring size is adequate:**
```yaml
# config/embarcadero.yaml
storage:
  batch_headers_size: 10485760  # 10MB ring = 163,840 slots (64B each)
```

For 5000 batches across 4 brokers = 1250 batches per broker, ring must hold at least:
- 1250 batches × 64B = 80KB per broker ✓ (10MB >> 80KB)

3. **Check for ring full events:**
```bash
# Should be 0 or very low (<10)
grep -c "Ring full" build/bin/broker_*_trial1.log
```

**Expected:** 0-1 transient ring full events. If higher, consider:
- Increase `batch_headers_size` to 20MB
- Tune sequencer backoff (reduce idle spinning)

**Estimated Effort:** 20 minutes

---

### Phase 3: Performance (OPTIONAL - 10 GB/s Target)

**Current:** 3 GB/s
**Target:** 10 GB/s (from `docs/memory-bank/activeContext.md`)

#### 3.1 Profile Sequencer Bottleneck [HIGH PRIORITY IF TARGET MATTERS]

**Hypothesis:** BrokerScannerWorker5 is the bottleneck.

**Action Required:**
1. **Add sequencer throughput metrics:**
```cpp
// In BrokerScannerWorker5, log processing rate
auto now = std::chrono::steady_clock::now();
if (std::chrono::duration_cast<std::chrono::seconds>(now - last_log_time).count() >= 1) {
    double batches_per_sec = total_batches_processed / elapsed_seconds;
    double msgs_per_sec = batches_per_sec * avg_batch_size;
    LOG(INFO) << "BrokerScannerWorker5 [B" << broker_id << "]: "
              << batches_per_sec << " batches/s, "
              << (msgs_per_sec * msg_size / 1e9) << " GB/s";
}
```

2. **Profile with perf:**
```bash
# Profile sequencer CPU time
perf record -g -p $(pgrep -f "embarlet.*broker_0") -- sleep 10
perf report
```

3. **Check for contention:**
- Cache line invalidation overhead (CXL flush in tight loop)
- Mutex contention on `tinode_->offsets[broker_id].ordered`
- Idle backoff too aggressive (sleeping too long)

**Estimated Effort:** 2-4 hours for profiling + tuning

---

#### 3.2 Optimize AckThread [MEDIUM PRIORITY]

**Reference:** `docs/SENIOR_CODE_REVIEW_PUBLISHER_TO_ACK.md` §3.1

**Issue:** AckThread may be CPU-bound or network-bound.

**Action Required:**
1. **Batch ACK reads:**
   - Instead of `recv()` per message, use `recv()` with larger buffer
   - Parse multiple ACKs from single syscall

2. **Reduce spin/sleep:**
   - Profile AckThread CPU usage
   - Tune epoll timeout vs spin

**Estimated Effort:** 2-3 hours

---

#### 3.3 Reduce Ring Full Events [LOW PRIORITY]

**Current:** 1 transient ring full event on broker 3

**Options:**
1. **Increase ring size:** 10MB → 20MB
2. **Tune sequencer backoff:** Reduce idle sleep duration
3. **Optimize CXL allocation:** Reduce GetCXLBuffer latency

**Estimated Effort:** 1-2 hours

---

## Recommended Execution Order

### Immediate (Next Session)
1. ✅ **Git commit** (DONE: 7f0044a)
2. 🔴 **1.1: Per-client ordering** (CRITICAL CORRECTNESS)
3. 🔴 **1.2: BlogMessageHeader flush** (CORRECTNESS)
4. 🟡 **2.1: Per-broker ACK diagnostics** (OBSERVABILITY)

### Follow-up (After Correctness Verified)
5. 🟡 **2.2: Verify config and ring size** (VALIDATION)
6. 🟢 **3.1: Profile sequencer** (PERFORMANCE - if 10 GB/s target matters)
7. 🟢 **3.2: Optimize AckThread** (PERFORMANCE)
8. 🟢 **3.3: Reduce ring full** (POLISH)

---

## Testing Checklist

After each fix, run:

```bash
# Quick validation (1GB)
TOTAL_MESSAGE_SIZE=1073741824 MESSAGE_SIZE=1024 ORDER=5 ACK=1 TEST_TYPE=5 ./scripts/run_throughput.sh

# Full validation (10GB)
TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 ORDER=5 ACK=1 TEST_TYPE=5 EMBARCADERO_ACK_TIMEOUT_SEC=300 ./scripts/run_throughput.sh

# Check metrics
grep -c "Ring full" build/bin/broker_*_trial1.log
grep -c "Dropping batch" build/bin/broker_*_trial1.log
grep "total_processed" build/bin/broker_0_trial1.log | tail -4
```

**Expected (after all fixes):**
- 100% ACK completion
- 0 ring full errors (or 1-2 transient)
- 0 dropped batches
- Per-client ordering verified in logs
- Throughput improved (3 GB/s → target)

---

## Risk Assessment

| Fix | Risk Level | Impact if Skipped |
|-----|-----------|------------------|
| 1.1 Per-client ordering | **HIGH** | Silent data corruption in multi-client or out-of-order scenarios |
| 1.2 BlogMessageHeader flush | **MEDIUM** | Replication/subscriber may read stale data |
| 2.1 Per-broker diagnostics | **LOW** | Harder to debug future stalls |
| 2.2 Config verification | **LOW** | Potential config mismatch |
| 3.x Performance tuning | **LOW** | Lower throughput, but system is functional |

---

## Success Criteria

**Minimum (Production-Ready):**
- ✅ 100% ACK completion on 10GB test
- ✅ 0 dropped batches
- ⬜ Per-client ordering enforced and tested
- ⬜ BlogMessageHeader flush added
- ⬜ Per-broker ACK diagnostics available

**Stretch (Performance Target):**
- ⬜ Throughput: 10 GB/s (from 3 GB/s)
- ⬜ Ring full events: 0 (from 1)
- ⬜ Sequencer profiled and optimized

---

## References

- `docs/SENIOR_CODE_REVIEW_PUBLISHER_TO_ACK.md` - Detailed code review with specific line numbers
- `ROOT_CAUSE_RING_GATING_BUG.md` - Root cause analysis of off-by-one bug
- `SENIOR_REVIEW_FIXES_APPLIED.md` - Previous fixes (SIGPIPE, retry logic)
- `docs/memory-bank/activeContext.md` - System goals and targets
- `docs/PUBLISH_PIPELINE_EXPLAINED.md` - Pipeline architecture

---

**Next Action:** Start with **1.1 Per-Client Ordering** as it's the highest-risk correctness issue.
