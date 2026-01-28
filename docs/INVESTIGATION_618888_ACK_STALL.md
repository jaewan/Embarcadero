# Investigation: ACK Stall at 618,888 / 10,485,760 (~5.9%)

**Observed:** Publisher log shows acks stuck at 618,888 out of 10,485,760 for 159s+ (elapsed 159s, 162s, 165s with no change). Timeout is 300s.

**Test:** `TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 ORDER=5 ACK=1 TEST_TYPE=5 EMBARCADERO_ACK_TIMEOUT_SEC=300 ./scripts/run_throughput.sh` (10GB, 1KB messages, 4 brokers).

---

## 1. What the Numbers Imply

- **10,485,760** = total messages (`client_order_`) = 10GB / 1024.
- **618,888** = `ack_received_` (sum of acks from all brokers).
- **618,888 / 4 = 154,722** if acks are evenly distributed across 4 brokers.

Two plausible distributions:

| Scenario | Interpretation |
|----------|----------------|
| **A: Even** | Each broker sent ~154,722 acks then stopped → all four `ordered` counters stopped advancing at ~154,722. |
| **B: One broker** | One broker sent 618,888 acks, others 0 → only one broker’s connection/acks active; others never advanced. |

Without per-broker counts in the log we can’t distinguish A vs B. The code path is the same: **no new acks after 618,888** ⇒ either no new `ordered` updates, or acks not reaching the publisher.

---

## 2. Root Cause Candidates (Code-Based)

### 2.1 **Ring full → connection close (most plausible)**

**Mechanism:**

1. **Producer:** `EmbarcaderoGetCXLBuffer` returns `nullptr` when `slot_free` is false (`consumed_through < next_slot_offset`).
2. **NetworkManager** (e.g. `network_manager.cc` 636–674): On `buf == nullptr` it retries up to **20 times** with exponential backoff (1ms, 2ms, … up to 128ms). After 20 failures it **breaks and closes the connection**.
3. After connection close, no more batches are received ⇒ no new `batch_complete=1` ⇒ sequencer has nothing new to process ⇒ `ordered` stops ⇒ no new acks ⇒ **ack_received_** stuck at 618,888.

So the stall can be **downstream of ring full**: once the producer hits “ring full” 20 times in a row and the connection is closed, the system will stall at whatever ack count was already reached (here 618,888).

**Why would the ring be “full” if the ring gating fix is in place?**

- **Fix not in binary:** Build/run might not include the fix `consumed_through >= next_slot_offset` (and the old `>= next_slot_offset + sizeof(BatchHeader)` would cause false “ring full”).
- **Config:** If `batch_headers_size` is small (e.g. 64KB = 512 slots for 128B headers), the ring can fill if the sequencer doesn’t keep up; with 10MB (e.g. `config/embarcardero.yaml`: 10MB) there are many more slots, so less likely unless throughput is very high.
- **Throughput vs sequencer speed:** Producer can lap the sequencer (e.g. 50µs sleep every 1024 idle cycles in BrokerScannerWorker5). Then `next_slot_offset` advances past `consumed_through` ⇒ “ring full” ⇒ retries ⇒ eventually connection close.

So even with the **correct** gating condition, “ring full” can still occur if the **sequencer is slower than the producer**; the consequence (connection close after 20 retries) is what turns that into a hard stall at 618,888.

### 2.2 **Same as previous 6.7% stall (ring gating bug)**

If the deployed binary still had the **old** condition `consumed_through >= next_slot_offset + sizeof(BatchHeader)`:

- Slot at `next_slot_offset` would be considered “not free” when it actually is ⇒ false “ring full” ⇒ same chain as above (retries → connection close → stall).

So 618,888 could be the same class of bug (off-by-one gating) if the fix wasn’t rebuilt/deployed.

### 2.3 **Sequencer not seeing `batch_complete=1`**

If the sequencer (head) reads stale `batch_complete=0` (e.g. missing or wrong cache invalidation before reading the batch header), it would skip slots and never advance `ordered` for those batches. Current code does invalidate before every read (`topic.cc` ~1499–1516), so this is less likely unless that path wasn’t built or there’s another reader path.

### 2.4 **AckThread not seeing updated `ordered`**

If AckThread doesn’t see updated `ordered` (e.g. no invalidation in `GetOffsetToAck`), it wouldn’t send new acks. `GetOffsetToAck` does invalidate before reading `ordered`; again, less likely unless something changed or a different code path is used.

---

## 3. Relation to What We Already Know

| Item | Relation to this stall |
|------|------------------------|
| **Ring gating fix** (`consumed_through >= next_slot_offset`) | If **not** in the running binary ⇒ same class of bug as before (false ring full → connection close → stall). If **in** the binary ⇒ ring can still fill when producer outpaces sequencer ⇒ same **consequence** (connection close after 20 retries). |
| **“Tail batches not marked batch_complete”** (review §5) | Exactly what happens when the connection is closed: no more batches received ⇒ no more `batch_complete=1` ⇒ sequencer stops advancing `ordered` for new data. |
| **“One broker’s ordered not advancing”** (review §5) | Either one broker’s connection closed (scenario B) or all brokers’ connections closed / all rings “full” (scenario A). |

So this 618,888 stall is **consistent** with the same causal chain we had before (ring full → connection close → no more batches → no more acks), with the only open question: **current** ring full is due to (i) old gating bug, (ii) correct gating but sequencer too slow, or (iii) small ring size.

---

## 4. Recommended Next Steps

1. **Confirm binary and config**
   - Rebuild and ensure the binary contains the fix: `slot_free = ... (consumed_through >= next_slot_offset)` in `EmbarcaderoGetCXLBuffer` (`topic.cc` ~1183–1185).
   - Confirm which config is used by `run_throughput.sh` (e.g. `config/embarcardero.yaml`) and the value of `batch_headers_size` (10MB vs 64KB, etc.).

2. **Add per-broker ack count to the 3s log**
   - In `publisher.cc` where the “Waiting for acknowledgments, received X out of Y” log is printed (~302), also log `acked_messages_per_broker_[i]` for each broker (same as the timeout path ~285–288). That will show whether the stall is “all brokers at ~154k” (A) or “one broker at 618k, others 0” (B).

3. **Inspect broker/NetworkManager logs**
   - Search for:
     - `"Ring full"` / `"EmbarcaderoGetCXLBuffer: Ring full"` (ring full hits).
     - `"Failed to get CXL buffer after 20 retries"` / `"Closing connection to client_id"` (connection close due to ring full).
     - `"Batch incomplete"` / connection closed during receive (tail batches not marked complete).
   - If you see “Ring full” and then “Closing connection” around the time acks stop, that supports 2.1 (and possibly 2.2).

4. **If ring is filling due to sequencer speed**
   - Consider increasing BrokerScannerWorker5 idle threshold (e.g. sleep only after 4096 idle cycles) and/or shortening sleep (e.g. 1µs instead of 50µs) so the sequencer keeps up; re-run 10GB to confirm the stall point moves or disappears.

5. **If config has small `batch_headers_size`**
   - For 10GB / 1KB messages and 4 brokers, ensure the batch header ring is large enough (e.g. 10MB per broker) so that under normal throughput the producer doesn’t lap the sequencer and hit “ring full” after ~618k messages.

---

## 5. Summary

- **Observed:** Acks stuck at 618,888 / 10,485,760; no progress for 159s+.
- **Most plausible cause:** Producer sees “ring full” (either due to old gating bug or sequencer slower than producer) → GetCXLBuffer returns nullptr 20 times → NetworkManager closes the connection → no more batches → no more `batch_complete=1` → sequencer stops advancing `ordered` for new data → no new acks → stall at 618,888.
- **Same chain as before:** “Tail batches not marked batch_complete” and “ordered not advancing” are the downstream effects of that connection close.
- **Next:** Verify binary (ring gating fix), config (`batch_headers_size`), add per-broker ack logging, and check broker logs for “Ring full” and “Closing connection.”
