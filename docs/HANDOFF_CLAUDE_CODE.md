# Handoff to Claude Code: ACK-Stall and Bandwidth Context

**Created:** 2026-01-28  
**Purpose:** Give Claude Code full context to continue work on the publisher ACK-stall / last-percent bandwidth issue. Use this when taking over from a previous session.

---

## 1. What We Did 

We implemented four latency/contention improvements and fixed one compile-breaking omission. **Batch header ring wrap-up was already ruled out as the cause of failure** by the user.

### 1.1 Implemented (Code Changes)

| Item | Location | Change |
|------|----------|--------|
| **AckThread configurable** | `src/network_manager/network_manager.cc` | Spin/drain/sleep are now env-configurable: `EMBARCADERO_ACK_SPIN_US` (default 500), `EMBARCADERO_ACK_DRAIN_US` (1000), `EMBARCADERO_ACK_SLEEP_LIGHT_US` (100), `EMBARCADERO_ACK_SLEEP_HEAVY_MS` (1), `EMBARCADERO_ACK_HEAVY_SLEEP_STALL_THRESHOLD` (200). |
| **BrokerScannerWorker5 backoff** | `src/embarlet/topic.cc` | No sleep until `idle_cycles >= 4096`, then 1µs sleep (reset idle_cycles). Below 4096 only `CXL::cpu_pause()`. Intended to improve tail latency; must be validated on 100MB/1GB so it does not reintroduce stalls. |
| **AssignOrder5 path: striped mutex** | `src/embarlet/topic.h`, `src/embarlet/topic.cc` | Replaced single `global_seq_batch_seq_mu_` with `global_seq_batch_seq_stripes_[kSeqStripeCount]` (32 stripes). Hot path locks `stripes_[client_id % kSeqStripeCount]`. ProcessSkipped5 locks all stripes in order 0..31 when scanning. All four call sites (BrokerScanner at 612, ProcessSkipped at 694, ProcessSkipped5 at 734+, BrokerScannerWorker5 at 1675) use stripes. |
| **Poll() ack-wait spin** | `src/client/publisher.cc` | Ack-wait spin shortened from 1ms to 500µs (`SPIN_DURATION = 500us`) when waiting for acks in `Poll()`. |
| **ProcessSkipped5 compile fix** | `src/embarlet/topic.cc` | After adding stripes, ProcessSkipped5 still referenced removed `global_seq_batch_seq_mu_`; updated to lock/unlock all stripes manually. |

### 1.2 Not Done

- No bandwidth test **completed**; the last run was **killed** (see below).
- No assessment document written for “fixes failure vs other cause” per change.
- No validation that BrokerScannerWorker5’s 4096/1µs backoff avoids tail stalls on 100MB/1GB.

---

## 2. Successes vs Failures

### Successes

- **Build:** `cd build && ninja -j$(nproc)` succeeds.
- **Code structure:** AckThread, BrokerScannerWorker5, AssignOrder5 path, and Poll() are updated as specified; no remaining references to `global_seq_batch_seq_mu_`.
- **Correctness:** Striped mutex preserves semantics (same logical critical sections, keyed by `client_id` or “all stripes” in ProcessSkipped5).

### Failures

- **Bandwidth test did not finish.** The last run (from `measure_bandwidth_proper.sh` or similar) was **killed with SIGTERM** while the client was in `Publisher::Poll()` waiting for acks. Logs showed “Waiting for acknowledgments, received 25064 out of 102400” (~24.5%) and no further progress until SIGTERM. So:
  - **Observed:** Client stalls at ~24.5% acks (e.g. 100k messages sent, ~25k acked), then process is killed (timeout or manual).
  - **Result:** `data/throughput/pub/result.csv` had only the header row—no bandwidth result row was written.
- **Root cause of “last percent” stall is still unknown.** User explicitly said batch header ring wrap is **not** the cause. Likely areas: ProcessSkipped5 / tail drain, one broker’s `ordered` not advancing, AckThread not seeing CXL updates, or contention/scheduling. The four improvements target latency and contention; they may help but might not fix the underlying stall.

---

## 3. Current State

- **Codebase:** Builds cleanly. All four improvements and the ProcessSkipped5 stripe fix are in place.
- **Tests:** No automated tests were run in this chat. Last bandwidth run: **killed** (SIGTERM) during ack wait.
- **Artifacts:**
  - `data/throughput/pub/result.csv` — header only (last run did not complete).
  - `/tmp/test_0_1.log` — last test log; shows publish threads finishing, then client stuck at ~24.5% acks until SIGTERM in `Publisher::Poll()`.
- **Goal:** Reach reliable 9–10 GB/s (or at least complete runs) for 10GB, ORDER=5, ACK=1, 1KB messages; and/or stabilize 100MB/1GB runs so bandwidth is measurable and “last percent” behavior is understood.

---

## 4. Files Claude Code Must Use

Read these **before** changing behavior or debugging the stall:

| File | Use |
|------|-----|
| `docs/memory-bank/activeContext.md` | Current focus, spec governance, deviations, “last percent” and 10GB status. |
| `docs/memory-bank/LOCKFREE_BATCH_HEADER_RING_DESIGN.md` | Batch header ring, consumed_through protocol; confirms ring wrap is not the identified failure cause. |
| `docs/memory-bank/spec_deviation.md` | Approved deviations (DEV-001 …). |
| `docs/BANDWIDTH_MEASUREMENT_APPROACH.md` | How bandwidth is defined and how CSV/logs are used. |
| `src/embarlet/topic.cc` | BrokerScannerWorker5 (idle_cycles, backoff), ProcessSkipped5 (all-stripe lock), AssignOrder5 path (striped lock), and older BrokerScanner/ProcessSkipped paths. |
| `src/embarlet/topic.h` | `kSeqStripeCount`, `global_seq_batch_seq_stripes_`, guards. |
| `src/network_manager/network_manager.cc` | AckThread loop: spin/drain/sleep and env vars. |
| `src/client/publisher.cc` | `Poll()` ack-wait loop and `SPIN_DURATION`. |

When touching CXL/sequencer/ack logic, also check:

- `src/cxl_manager/cxl_datastructure.h` — TInode/offset_entry layout.
- `src/cxl_manager/README.md` — “Four Laws” and cache-line rules.

---

## 5. How to Build, Test, and Measure Bandwidth

### Build

```bash
cd /home/domin/Embarcadero/build
ninja -j$(nproc)
```

Uses existing CMake/Ninja setup; no extra configure step.

### Quick Sanity (100MB, 1 iter)

```bash
cd /home/domin/Embarcadero
TOTAL_MESSAGE_SIZE=104857600 NUM_ITERATIONS=1 bash scripts/measure_bandwidth_proper.sh
```

- 100MB = 104857600 bytes.
- Completes in a couple of minutes if healthy.
- Result: `data/throughput/pub/result.csv` gets a new row; column 12 is `pub_bandwidth_mbps` (MB/s).
- Historical reference: ~49.7 MB/s for 100MB, 1KB, ORDER=5, ACK=1.

### Full Bandwidth (10GB, 3 iters)

```bash
cd /home/domin/Embarcadero
bash scripts/measure_bandwidth_proper.sh
```

- Default: 10GB, 1KB, ORDER=5, ACK=1, test type 5 (publish-only).
- Reads env: `ORDER`, `ACK`, `MESSAGE_SIZE`, `TOTAL_MESSAGE_SIZE`, `TEST_TYPE` (or `TEST_NUMBER` where used).
- Calls `scripts/run_throughput.sh`, which starts brokers then `./throughput_test ... --record_results ...`.
- Authoritative result: `data/throughput/pub/result.csv` — column 12 = pub bandwidth in MB/s.
- Script timeout is ACK_TIMEOUT + 60s (default 90+60). If the client stalls in Poll(), the run is killed and CSV may stay header-only.

### 1GB Test (Middle Ground)

```bash
cd /home/domin/Embarcadero
TOTAL_MESSAGE_SIZE=1073741824 NUM_ITERATIONS=1 bash scripts/measure_bandwidth_proper.sh
```

Useful to see whether stalls appear only at 10GB or also at 1GB.

### Where Results Live

- **CSV:** `data/throughput/pub/result.csv`. Header:  
  `message_size,total_message_size,...,sequencer_type,pub_bandwidth_mbps,sub_bandwidth_mbps,e2e_bandwidth_mbps`  
  Use **column 12** for publish bandwidth (MB/s).
- **Logs:** Script writes to `/tmp/test_{0|1}_{iteration}.log`. Client-side “Waiting for acknowledgments, received X out of Y” is in those logs.

### Interpret “Killed” or Timeout

If the run is killed or times out:

1. Inspect `/tmp/test_0_1.log` (or latest) for the last “Waiting for acknowledgments, received X out of Y”.
2. If X stays well below Y (e.g. 24.5%) and then SIGTERM appears, the **ack stall** is still present; the four changes did not by themselves fix it.
3. Consider: EMBARCADERO_ACK_TIMEOUT_SEC, broker logs under `build/bin/`, and per-broker ack counts in client logs.

---

## 6. Instructions Optimized for Claude Code

Use the following as the main **instruction block** when handing off to Claude Code. It follows current best practices: explicit scope, ordered steps, clear success criteria, and “plan before code.”
