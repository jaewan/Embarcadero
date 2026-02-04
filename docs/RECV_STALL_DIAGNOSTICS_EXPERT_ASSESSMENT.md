# Recv Stall Diagnostics: Expert Panel Brutal Assessment

**Scope:** What we did (broker recv/GetCXLBuffer stall diagnostics), the code we added, the logs we captured, and the conclusions we drew. Experts scrutinize implementation, logs, and claims.

**Artifacts:** `src/network_manager/network_manager.cc` (blocking path), `build/test_output/order0_ack1/broker_*.log`, client.log.

---

## 1. What We Did (Summary)

- **Hypothesis:** Broker blocking in `recv()` was the cause of ACK timeouts; "Ring full" was seen only once per broker in prior runs, so GetCXLBuffer was not the repeated stall.
- **Change:** Added stall diagnostics to the **blocking** receive path (`HandlePublishRequest`): time around first batch-header `recv()`, time around each payload `recv()`. If wall time ≥ 50 ms, log `[STALL] recv batch header blocked Xms` or `[STALL] recv payload blocked Xms ...`.
- **Test:** Ran e2e `test_order0_ack1.sh` (10k messages, 4 brokers, ORDER=0, ACK=1). Test script failed (client log contained "error" from hugepage warning); publish completed; brokers were then killed by cleanup.
- **Conclusion drawn:** "Blocking recv() is the problem"; stall logs showed 51–57 ms and ~10 s blocks on `recv` batch header, then "Connection closed by publisher."

---

## 2. Expert 1 (Correctness & Code) — "Show me the code"

### What’s in the hot path

- **Batch header:** Before/after the **first** `recv(client_socket, &batch_header, sizeof(BatchHeader), 0)` we take `steady_clock::now()` twice, compute microseconds, compare to 50 ms, optionally LOG(WARNING). So every batch pays: two clock calls and a branch. On a fast path that’s negligible; on a path that already blocks for seconds it’s irrelevant. **Verdict:** Acceptable.

- **Partial batch-header loop:** We did **not** add any timing or `[STALL]` logging for the loop that finishes a partial header read (`while (bytes_read < sizeof(BatchHeader)) { recv(...); }`). If the first recv returns 1 byte and the rest blocks for 5 s, we’ll see a 5 s stall on the **next** iteration’s first recv (next batch), not attributed to "partial". So we **under-count or mis-attribute** stalls that happen in the partial-read loop. **Verdict:** Gap. Either add the same timing in the partial loop or document that "[STALL] recv batch header" means "first recv of a header (or first recv after a partial) blocked", not "the partial recv blocked".

- **Payload recv:** Same pattern: time before/after each `recv()` in the payload loop, log if ≥ 50 ms. Correct. We also introduced `static std::atomic<size_t> recv_payload_stall_count{0}`. It’s incremented every time we log a payload stall. **Verdict:** Fine; total_stall_count is useful.

- **Constant in loop:** `static constexpr int64_t kRecvStallThresholdMs = 50` is defined **inside** the outer `while (running && !stop_threads_)` loop. So it’s not "in the loop" in the sense of being re-evaluated; it’s still one definition per invocation of the function. Style nit: move it to file scope or the top of the function so the hot loop doesn’t visually suggest a per-iteration constant. **Verdict:** Cosmetic.

### Misleading log text

- Message says: `"[STALL] recv batch header blocked Xms (batch_header will be partial if bytes_read<sizeof)"`. When we log this, we haven’t yet checked `bytes_read`. So "batch_header will be partial" is a conditional fact about what **could** be true if `bytes_read < sizeof(BatchHeader)`, not "we observed partial". The 10 s blocks we saw were followed by `bytes_read == 0` (connection closed). So for those, the header was not partial — the connection was dead. **Verdict:** Wording is confusing. Prefer: "recv batch header blocked Xms (client_id=...); bytes_read=<value>" or drop the parenthetical.

### Summary (Expert 1)

- Diagnostics are **correct** for where they’re placed: they do show when the **first** header recv or a payload recv blocks ≥ 50 ms.
- **Gaps:** (1) No stall diagnostic in the partial batch-header read loop. (2) Log message suggests "partial" when we didn’t actually observe partial. (3) Threshold constant inside loop is cosmetic.

---

## 3. Expert 2 (Logs & Causality) — "What do the logs actually prove?"

### What the logs show

- **Broker 0:** `[STALL] recv batch header blocked` at 51, 53, 55, 57 ms (four threads), then "Ring full, waiting for space" once (total_ring_full=1), then batch_data_complete 1–4, then ~10 s later `[STALL] recv batch header blocked` 10280–10286 ms, then "Connection closed by publisher ... No batch data received."
- **Brokers 1–3:** Similar: initial 61–62 ms stalls (broker 1), then Ring full once (broker 1), then ~10 s stalls, then "Connection closed by publisher."

### What we can infer

1. **Blocking recv() is where the broker spent time.** The 10 s delays are wall time in the first `recv()` call for the **next** batch header. So the broker was blocked in `recv()` waiting for data. **Verdict:** True.

2. **Why did recv unblock?** Immediately after the 10 s stall we see "Connection closed by publisher". So `recv()` returned 0 (peer closed). The client had already finished sending (10000 messages, 6 batches), declared "Publisher finished sending", and the test cleanup then killed the brokers. So the **client** (or test harness) closed the connections; the broker didn’t "time out" — it blocked until the close. **Verdict:** The 10 s block is "broker waiting for next batch; client had already finished and connections were closed later". So we’re not seeing a TCP timeout; we’re seeing normal blocking recv until EOF/close.

3. **Is "blocking recv is the problem" the right takeaway?** For **this** run: the "problem" was that the broker blocked in recv for ~10 s **after** the client had effectively finished, until the connection was closed. So the "problem" in this test is **protocol/lifecycle**: client finishes and doesn’t close; broker blocks waiting for more data. For the **original** ACK timeout (10 GB, 5435 batches, client sent 516): the problem could still be "broker blocked in recv() because client didn’t send" (e.g. client stuck on queue full or EAGAIN). The diagnostics don’t **prove** that — they only show that when the broker blocks, it’s in recv. **Verdict:** Logs support "broker stalls in recv()"; they don’t by themselves prove that the 10 GB ACK timeout was due to client not sending. We’d need a run with the 10 GB scenario and these diagnostics to see if we get many "[STALL] recv payload" or "[STALL] recv batch header" **before** connection close.

4. **Ring full once per broker.** So GetCXLBuffer returned null once per broker at the start, then we got a buffer. That’s consistent with "ring not the repeated stall". **Verdict:** Conclusion that ring is not the main stall is supported.

### What we didn’t see

- No `[STALL] recv payload blocked` in the logs. So we didn’t observe payload recv blocking ≥ 50 ms in this run. That’s expected for a 10k-message run that completes in a few hundred ms of actual send; the long blocks were all "waiting for next batch header" after the client had stopped sending.

### Summary (Expert 2)

- Logs correctly show **where** the broker blocked (recv batch header) and for how long.
- The 10 s blocks are **post** client completion, until connection close — so this run doesn’t reproduce the original 10 GB ACK timeout; it only confirms that blocking recv is the call that blocks.
- To blame the **original** timeout on "client didn’t send, broker blocked in recv", we need the same diagnostics on a **10 GB / 5435-batch** run (or similar) and see stalls **during** the run, not only at shutdown.

---

## 4. Expert 3 (Test & Environment) — "Did we even run the right experiment?"

### Test parameters

- **test_order0_ack1.sh:** 10k messages, 1024 B msg, total 10 MB, 4 brokers, ORDER=0, ACK=1, timeout 5 s for the client run.
- Client log: "Publisher::Init() waiting for cluster connection" for **50 s** (5, 10, … 45 s). So the client didn’t start sending until ~49 s after start. Cause: client was initializing (e.g. buffer allocation). We see "MAP_HUGETLB failed ... 137451536384 bytes ... Falling back to THP" and "Hugepages: 6144 free × 2048 kB = 12288 MB free; need 131082 MB". So the client tried to allocate ~128 GB (hugepage), failed, fell back to THP — and that (or other init) took ~49 s. **Verdict:** The run is dominated by client init delay, not by broker recv behavior during load.

### What the test actually exercised

- Once the client connected, it sent 6 batches (broker 0 got 4, broker 1 got 1 partial, etc.), completed in ~0.1 s, then "Publisher finished sending". So the **throughput** phase was short; the long broker-side stalls (10 s) happened when brokers were waiting for the **next** batch that never came (client already done). **Verdict:** We validated "broker blocks in recv() when no data arrives" and "when client closes, we see Connection closed". We did **not** stress the path under sustained load (e.g. 10 GB) with these diagnostics.

### Test script failure

- The script fails because it greps client.log for "error|failed|timeout" and the client log contains "MAP_HUGETLB failed" and "Error: Cannot allocate memory". So the test is marked failed even though publish completed and ACKs were received. **Verdict:** Test logic is brittle. Either exclude known warnings from the grep or require a more specific failure condition. Unrelated to the stall diagnostics but worth fixing so we don’t confuse "diagnostic run" with "test failed".

### Summary (Expert 3)

- We ran a **light** test (10 MB, 6 batches) after a 50 s client init delay. The stall diagnostics did their job (they showed where the broker blocked), but we did **not** run the scenario that originally timed out (10 GB, 5435 batches). So we have not yet validated that the **same** diagnostics would show recv stalls **during** the original failure. Recommendation: run a 10 GB (or similar) test with these diagnostics and capture broker logs; then re-assess.

---

## 5. Expert 4 (Overclaim & Next Steps) — "What did we really learn?"

### What we can safely claim

1. **Broker blocking path:** With `use_nonblocking=false`, the broker uses blocking `recv()` for batch header and payload. When no data arrives, it blocks in that call. **True.**

2. **Diagnostics:** The added timers correctly attribute long delays to "recv batch header" or "recv payload" when those calls block ≥ 50 ms. **True.**

3. **This run:** The long stalls (10 s) were the broker blocked in recv **after** the client had finished sending, until the connection was closed. So we saw "blocking recv() is where the thread spent time", not "blocking recv() caused the 10 GB ACK timeout in production." **True.**

### What we should not claim

1. **"Blocking recv() is the root cause of the 10 GB ACK timeout."** We didn’t run that scenario with these diagnostics. Root cause could be: client queue full, client EAGAIN, client not calling send, broker recv blocking, or something else. We only know that **when** the broker blocks, it’s in recv. **Verdict:** Overclaim to say we proved root cause for the original failure.

2. **"We’ve fixed the problem."** We didn’t change the blocking behavior; we only added logging. So we didn’t fix anything — we only observed. **Verdict:** Correct; no fix was applied.

### Recommended next steps (done)

1. **Reproduce with diagnostics:** Run the **10 GB** scenario with the current broker build; collect broker logs; grep for `[STALL]` and "Ring full". Use `scripts/run_10gb_stall_diagnosis.sh` (ORDER=0, ACK=1, 10 GB, `EMBARCADERO_ACK_TIMEOUT_SEC=120`). It starts 4 brokers, runs `throughput_test -t 5 -s 10737418240 -o 0 -a 1`, then greps broker logs for `[STALL]` and "Ring full". Logs go to `build/test_output/10gb_stall_diagnosis/`. If you see many "[STALL] recv payload" or "[STALL] recv batch header" **during** the run (before client finishes), that’s evidence broker recv blocking is part of the story.

2. **Tighten the diagnostic code:** **Done.** (a) Added 50 ms stall log in the **partial** batch-header read loop. (b) Log message now includes `bytes_read=` (or `recv_ret=` for partial). (c) `kRecvStallThresholdUs` moved to file scope.

3. **Test script:** **Done.** E2E `test_order0_ack1.sh` no longer fails solely on known hugepage/THP messages; we exclude "MAP_HUGETLB failed", "Falling back to THP", "Hugepages:", "Cannot allocate memory", "need.*MB" from the error grep.

---

## 6. Summary Table

| Item | Verdict | Note |
|------|--------|------|
| Diagnostic placement (first header recv, payload recv) | OK | Correctly times blocking recv. |
| Partial header recv loop | **Gap** | No stall diagnostic; can mis-attribute or under-count. |
| Log message "batch_header will be partial" | **Misleading** | We don’t know bytes_read at log time; often it’s 0 (closed). |
| Conclusion "blocking recv is the problem" | **Overclaim for original bug** | True for "where broker blocks"; not proven for 10 GB timeout cause. |
| Ring full once per broker | **Supported** | GetCXLBuffer not the repeated stall in this run. |
| Test run (10k, 50 s init delay) | **Wrong scenario** | Need 10 GB run with same diagnostics to validate original failure. |
| Test script failure (grep error) | **Brittle** | Fails on hugepage warning; publish actually completed. |

---

## 7. 10 GB Stall Diagnosis Run (2026-02-02)

**Script:** `scripts/run_10gb_stall_diagnosis.sh` (ORDER=0, ACK=1, 10 GB, timeout=120 s).

**Results:**

| Metric | Value |
|--------|--------|
| [STALL] recv batch header | **704** (during run; 51–270 ms blocks) |
| [STALL] recv payload | 0 |
| Ring full | 1 per broker (4 total) |
| Client | ACK timeout at 120 s; received **3,243,136** / 10,485,760; sent_all=yes; total_batches_sent=688 |
| Per-broker ACKs | B0=965,425 B1=331,444 B2=973,134 B3=973,133 |

**Interpretation:** Brokers blocked in `recv()` **during** the run (704 header stalls), not only at shutdown. Client reported sent_all=yes and 688 batches sent but only ~31% of ACKs received before timeout. Ring full occurred once per broker at the start. This run supports the conclusion that broker blocking recv() is where time is spent when the client is not delivering data fast enough; the client-side bottleneck (why only 688 batches / ~3.2M ACKs) remains to be investigated (queue capacity, EAGAIN, or other backpressure).

**Are batch headers never arriving or arriving at low speed?** **Low speed (or bursty), not “never”.** If headers never arrived, we would see one long block in recv() until connection close (e.g. 10+ s), not 704 shorter blocks of 51–270 ms. Each stall is: broker calls recv() → blocks 50–270 ms → recv() returns (with data) → broker processes → next recv() → blocks again. So data is arriving, but with large gaps between deliveries. That points to (a) client sending slowly (queue/backpressure, EAGAIN), (b) TCP flow control (broker not reading fast enough → client’s send buffer fills → client blocks), or (c) network/kernel buffering. The 0 “[STALL] recv payload” suggests payload delivery is not the main stall; the repeated header stalls are the dominant symptom.

**Bottom line:** The diagnostics are useful and correctly show that the broker blocks in `recv()`. The code has small gaps and one misleading log string. The **conclusion** that blocking recv is "the problem" is true in the sense of "that’s where the broker blocks" but is **not** proven as the root cause of the original 10 GB ACK timeout until we run that scenario with these diagnostics and inspect the logs.
