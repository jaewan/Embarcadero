# Known Limitations

**Document Purpose:** Document unsupported features, known bugs, and incomplete implementations
**Last Updated:** 2026-01-27
**Status:** Current limitations that affect correctness or performance

---

## ORDER=1 Not Implemented

**Status:** ❌ **NOT IMPLEMENTED** - ORDER=1 sequencer is not ported from cxl_manager

**Scope:** Embarcadero sequencer with `ORDER=1` will hang indefinitely due to missing sequencer thread

**Root Cause:**
- Location: `src/embarlet/topic.cc:149-151`
- Issue: Sequencer1 is not implemented - code only logs "Sequencer 1 is not ported yet" and does not start a sequencer thread
- Impact: Without sequencer thread, batches are never ordered, ACKs never advance, publisher hangs waiting for acknowledgments

**Symptoms:**
- Test hangs waiting for acknowledgments (e.g., "received 4 out of 102400")
- Publisher blocks indefinitely in `Publisher::Poll()` waiting for ACKs
- No sequencer thread running to process batches

**Workarounds:**
- Use ORDER=0, ORDER=3, or ORDER=5 instead
- For ORDER=1 testing, disable ACK (`ACK=0`) to avoid hang (but ordering still won't work)

**Future Work:**
- Port Sequencer1 implementation from cxl_manager
- Or document ORDER=1 as permanently unsupported if not needed

---

## ORDER=4 Not Supported / May Hang

**Status:** ⚠️ **NOT VALIDATED** - ORDER=4 is not part of the supported correctness/perf matrix

**Scope:** Embarcadero sequencer with `ORDER=4` and `ACK=1` may hang indefinitely

**Affected Order Levels:**
- ✅ ORDER=0: Supported and validated
- ❌ ORDER=1: **Not implemented** (sequencer not ported)
- ✅ ORDER=3: Supported and validated
- ⚠️ ORDER=4: **Not validated, may hang**
- ✅ ORDER=5: Supported and validated (primary focus)

**Symptoms:**
- Test hangs waiting for acknowledgments (e.g., "received 1932 out of 102400")
- Publisher blocks indefinitely in `Publisher::Poll()` waiting for ACKs
- Sequencer4 thread may stall processing batches

**Root Causes:**

1. **Publisher waits forever for ACKs** (no timeout/fail-fast):
   - Location: `src/client/publisher.cc:245-271`
   - Issue: `while (ack_received_ < client_order_)` loop has no timeout
   - Impact: If any broker stops advancing ACKs, test hangs forever

2. **Sequencer4 waits forever on per-message completion**:
   - Location: `src/embarlet/topic.cc:759-773`
   - Issue: `while (msg_header->paddedSize == 0)` blocks indefinitely
   - Impact: If message header never becomes visible (non-coherent CXL, corruption, missed invalidation), sequencer stalls and ACKs stop

3. **Strict batch sequence ordering**:
   - Location: `src/embarlet/topic.cc:BrokerScannerWorker`
   - Issue: Requires strictly sequential batch sequences per client
   - Impact: If batches are lost or out-of-order, sequencer may wait forever

**Workarounds:**
- Use ORDER=0, ORDER=3, or ORDER=5 instead
- For ORDER=4 testing, disable ACK (`ACK=0`) to avoid hang
- Monitor broker logs for stalled sequencer threads

**Future Work:**
- Add bounded timeouts to `Publisher::Poll()` and `AssignOrder()` per-message wait
- Implement fail-fast logic when batches are missing
- Add diagnostics to identify which batch/offset is stuck

**BlogMessageHeader Compatibility:**
- BlogMessageHeader is **only intended for ORDER=5**
- ORDER=4 behavior with `EMBARCADERO_USE_BLOG_HEADER=1` is best-effort and not validated
- Fail-fast mechanism logs error and disables BlogHeader for `ORDER != 5`

---

## Other Limitations

### CXL Hardware Support

**Status:** ⚠️ **Emulated** - Real CXL hardware not yet validated

**Current State:**
- Code supports `/dev/dax0.0` DAX devices
- Falls back to `shm_open()` for emulation
- NUMA binding used for CXL emulation

**Limitations:**
- Performance characteristics may differ on real CXL hardware
- Non-coherent memory semantics validated via emulation only
- Cache flush overhead measured on emulated setup

**Future Work:**
- Validate on real CXL 1.1/2.0 hardware
- Measure cache flush latency on real hardware
- Test multi-node non-coherent CXL scenarios

---

### Sequencer Recovery

**Status:** ⚠️ **Not Implemented** - Sequencer state is in-memory only

**Current State:**
- `next_expected_batch_seq_` map is in-memory
- Sequencer crash requires manual recovery
- No checkpointing to CXL

**Impact:**
- Sequencer crash loses client FIFO tracking state
- Requires manual intervention to resume sequencing

**Future Work:**
- Persist sequencer checkpoint to CXL (Phase 3.1)
- Implement sequencer failover protocol
- Add client-side batch retry logic

---

### Multi-Node CXL

**Status:** ⚠️ **Single-Node Only** - Multi-node non-coherent CXL not implemented

**Current State:**
- Segment allocation uses cache-coherent atomics (single-node)
- Thread-local hints assume shared cache domain
- No network coordination for segment allocation

**Limitations:**
- Cannot run across multiple physical nodes with non-coherent CXL
- Segment allocation assumes cache-coherent domain

**Future Work:**
- Implement partitioned bitmap for multi-node (Option A)
- Or leader-based allocation via network RPC (Option B)
- Or hardware-assisted CXL 3.0 atomics (Option C)

---

## Deprecated Features

### BrokerMetadata Region

**Status:** ❌ **REMOVED** (DEV-004)

**Reason:** Redundant with `TInode.offset_entry`

**Migration:** All code uses `TInode.offset_entry` directly

---

### ReceiverThreadPool Class

**Status:** ❌ **DISCARDED** (DEV-003)

**Reason:** NetworkManager receiver logic is more efficient (zero-copy)

**Migration:** Receiver stage is in `NetworkManager::ReqReceiveThread()`

---

### Cache Prefetching

**Status:** ❌ **REVERTED** (DEV-007)

**Reason:** Violates non-coherent CXL semantics, caused infinite loops

**Migration:** Direct volatile reads only, no prefetching of remote-writer data

---

**Last Updated:** 2026-01-27
**Maintainer:** Engineering Team
**Review Required:** When adding new limitations or resolving existing ones
