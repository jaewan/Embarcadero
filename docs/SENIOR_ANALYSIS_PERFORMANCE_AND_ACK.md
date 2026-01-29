# Senior Engineer Analysis: Performance Investigation & ACK Path

**Date:** 2026-01-27  
**Context:** Post-revert state; simplified BrokerScannerWorker5 (no per-client ordering); ACK path empty-topic hypothesis.

---

## 1. Agreement / Disagreement with Your Summary

### Agree

- **ACK path as bottleneck when topic is empty:** If `AckThread` is started with an empty or wrong `topic`, then `GetTInode(topic)` can return the wrong TInode (e.g. `hashTopic("")` or uninitialized slot). Then `GetOffsetToAck()` reads the wrong `ordered` (or garbage), so ACKs are wrong or never advance. The client expects ACKs per broker (`acked_messages_per_broker_[broker_id]`); if broker 3’s AckThread uses the wrong topic, broker 3’s ACKs never reach the client correctly → timeouts and low effective throughput. **Fixing ACK setup so every broker’s AckThread gets the correct topic is necessary.**

- **Scanner processing rate:** The current scanner (simplified, no per-client ordering) processes batches in ring order with `global_seq_.fetch_add()` and updates `consumed_through` after each batch. That is consistent with “scanner processes all batches correctly” and a high processing rate (e.g. 544 batches in ~180 ms). The bottleneck is not “scanner too slow” in normal conditions.

- **Test overhead on small data:** For 1 GB, startup, connection setup, and ACK timeout behavior can dominate wall time, so 450 MB/s can be a measurement artifact rather than a pure code regression.

- **CXL invalidation pattern:** Current code invalidates before reading `batch_complete` and uses correct flush/fence. No disagreement there.

- **Ring gating in GetCXLBuffer:** The current `EmbarcaderoGetCXLBuffer` uses `consumed_through` with correct circular-buffer semantics (in-flight, slot_free). No issue found there.

### Disagree or Clarify

- **“Scanner processes batches correctly without per-client ordering”:**  
  Functionally it does not crash and throughput can be good, but **ORDER=5 semantics are not preserved**. The current logic is “process in ring order, assign `total_order` via `global_seq_.fetch_add()`”. That can reorder batches from the same client (e.g. client A’s batch A1 on broker 0 and A2 on broker 1 can get A2’s `total_order` < A1’s if broker 1’s scanner runs first). So:
  - If ORDER=5 is required to mean “per-client batch order preserved in `total_order`”, the current code **does not** guarantee that.
  - If the product accepts “best-effort batch order, no per-client guarantee”, then the simplified scanner is consistent with that.

---

## 2. Root Cause: Why Could Brokers 1, 2, 3 Have Empty Topic?

- **Blocking path:** Each broker’s `HandlePublishRequest` runs when a connection is accepted on that broker. `ack_connections_` is per-NetworkManager (per broker). So when broker 1 gets its first publish connection, it creates its own `ack_fd` and starts `AckThread(this, handshake.topic, ...)`. So the topic comes from the handshake read on **that** connection. In principle the client sends the same handshake (with topic) to every broker (see `connect_to_server` and `memcpy(shake.topic, topic_, ...)`), so all brokers should receive the same non-empty topic unless:
  - The handshake is read before the full message has arrived (partial read).
  - The topic field is not null-terminated and `strlen(handshake.topic)` is 0 or wrong.
  - Some code path overwrites or reuses the handshake before `AckThread` is started.

- **Non-blocking path:** `SetupPublishConnection` is called with `conn.handshake`; `AckThread` is started with `conn.handshake.topic`. So again, topic should match what was enqueued in `NewPublishConnection(..., handshake, ...)`. If you observe empty topic only on brokers 1,2,3, likely causes are:
  - Order of connection setup (e.g. broker 0’s connection processed first with full handshake; others processed or copied incorrectly).
  - Shared or stale handshake copy (e.g. one shared buffer overwritten by another connection).
  - Handshake struct layout/padding and copy semantics (e.g. topic not copied or not null-terminated in some path).

**Recommendation:** Add defensive checks and diagnostics:

1. Before starting `AckThread`: if `strlen(topic) == 0`, log ERROR with broker_id and client_id and either skip starting the thread or use a fallback only if the product allows it.
2. In `AckThread` at start: log broker_id and topic (and length); if topic is empty, log and exit (or handle explicitly).
3. Ensure handshake is copied by value when enqueueing (e.g. `NewPublishConnection(..., handshake, ...)`) and that the topic field is null-terminated after read (you already have `handshake.topic[sizeof(handshake.topic)-1] = '\0'` in ReqReceiveThread; verify it runs for the path that leads to AckThread).

---

## 3. Other Issues That Must Be Addressed

### 3.1 ORDER=5 semantics (per-client order)

- **Current behavior:** Batches are processed in ring order per broker; `total_order` is assigned by a single `global_seq_.fetch_add()`. Order between batches from the same client on different brokers is not guaranteed.
- **If you need strict per-client order:** You must reintroduce per-client state (e.g. expected_seq, deferred_batches) and process batches in client batch_seq order (including across brokers). That implies either:
  - Bringing back the per-client ordering design (with correct consumed_through advancement and no data races), or
  - A different design that still preserves per-client order.
- **If you do not need it:** Document that ORDER=5 is “batch-level total_order, no per-client ordering guarantee” and keep the simplified scanner.

### 3.2 consumed_through when “not ready” (skip path)

- **Current code:** When the current slot is not ready (`!batch_ready`), the scanner advances `current_batch_header` to the next slot and continues. It does **not** update `consumed_through` in that path.
- **Implication:** Slots that are never “ready” (e.g. abandoned or stuck batches) are never marked consumed, so the producer will not reuse them. That is safe but can leak ring capacity. If you later add a “skip N slots” or “advance past freed slots” path, you must advance `consumed_through` only through a **contiguous prefix of freed slots** in ring order (as in the previous correct fix), not to “last freed in window”.

### 3.3 Hot-path logging in BrokerScannerWorker5

- **Current code:** `LOG(INFO)` is used for:
  - `ready_seen <= 10 || ready_seen % 200 == 0` (batch_ready_seen)
  - `processed_logs <= 10 || processed_logs % 100 == 0` (Found valid batch)
- **Impact:** At high throughput (e.g. thousands of batches per second), logging every 100–200 batches can add noticeable I/O and CPU. Prefer `VLOG(3)` or similar for high-frequency logs, and keep `LOG(INFO)` for rare events or periodic summaries (e.g. every 5 s).

### 3.4 GetTInode("") behavior

- **Current:** `GetTInode(topic)` uses `hashTopic(topic)` and returns a TInode*; it never returns nullptr. So for an empty or wrong topic you get some TInode slot (possibly uninitialized or for another topic).
- **Recommendation:** In `GetOffsetToAck`, if `topic == nullptr` or `strlen(topic) == 0`, return `(size_t)-1` immediately and optionally log. That avoids using a wrong TInode when ACK setup passes an empty topic.

---

## 4. Summary Table

| Item | Status | Action |
|------|--------|--------|
| ACK path / empty topic | Agree it can cause broker ACKs to never reach client | Fix ACK setup so every broker gets correct topic; add validation and logging |
| Scanner processing rate | Agree scanner is not the main bottleneck | No change required for throughput |
| 1 GB test / 450 MB/s | Agree small data + ACK timeout can dominate | Fix ACK; re-measure with 8 GB+ and stable ACKs |
| ORDER=5 per-client order | Disagree that current code preserves it | Decide requirement; if needed, restore per-client ordering correctly |
| consumed_through on skip | Current code doesn’t advance on skip; safe but can leak slots | If you add skip path, advance only contiguous freed prefix |
| Hot-path LOG(INFO) in scanner | Can hurt performance at high throughput | Move high-frequency logs to VLOG |
| GetTInode(empty) | Wrong tinode → wrong ACKs | Guard in GetOffsetToAck: empty topic → return -1 |

---

## 5. Recommended Order of Work

1. **ACK path:** Ensure topic is never empty for any broker’s AckThread; add validation and logging; optionally guard `GetOffsetToAck` on empty topic.
2. **Performance:** Reduce hot-path logging; then consider written_addr guard or other scanner optimizations to approach 10 GB/s.
3. **Semantics:** ORDER=5 must preserve per-client batch order; re-implement per-client ordering (with correct consumed_through and no data races).
