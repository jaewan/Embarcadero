# Hold Timeout Experiment: Validate 1.5 ms Claim in Paper

## Background and the Problem

The paper (`Paper/Text/Design.tex`) claims the strong-ordering (Order 5) hold buffer uses a
**"configurable wall-clock duration (default 1.5 ms)"** in two places:

1. Prose: *"The gap timeout is a configurable wall-clock duration (default 1.5 ms)."*
2. Algorithm 2 pseudocode header: *"Timeout (wall-clock, default 1.5 ms)"*

The implementation disagrees. In `src/embarlet/topic.cc`, line 89:

```cpp
// GetOrder5HoldTimeoutNs()
const uint64_t default_ms = 100ULL;   // ← 100 ms, NOT 1.5 ms
return default_ms * 1000ULL * 1000ULL;
```

The runtime default can be overridden via the env var `EMBARCADERO_ORDER5_HOLD_TIMEOUT_MS`.

**Why this matters for the paper:** The central claim of the paper is that CXL makes a
millisecond-scale hold buffer *practical* for the first time, because CXL coordination rounds
are ~200–500 ns vs. Scalog's >10 ms network rounds. A 100 ms default undermines this claim
entirely—it is larger than Scalog's coordination round, not smaller. Reviewers who probe the
implementation will immediately notice. The default must be 1.5 ms (or similarly tight) and
must work correctly for the claim to be credible.

---

## What to Experiment

### Goal
Determine the smallest hold timeout that achieves:
1. **Correctness**: All strong-ordering clients' batches are committed in FIFO order with no
   spurious out-of-order commits (no false gap-expiry events that reorder batches that did
   arrive in order).
2. **Performance**: Throughput and tail latency at that timeout are within the numbers reported
   in the paper (11.2 GB/s, P99 ≤ 1.68 ms).

### Experiment Matrix

Run the standard throughput/latency benchmark (`throughput_test --record_results`) with Order 5
enabled across the following timeout values, using `EMBARCADERO_ORDER5_HOLD_TIMEOUT_MS`:

| Timeout | Env var setting            | Expected outcome                     |
|---------|----------------------------|--------------------------------------|
| 100 ms  | (current default, no env)  | Baseline — correct but slow ACK tail |
| 10 ms   | `=10`                      | Should be correct; latency improves  |
| 3 ms    | `=3`                       | Near paper claim; target range       |
| 1.5 ms  | `=2` (round up for safety) | **Paper's stated default**           |
| 1 ms    | `=1`                       | Aggressive; check for false expiries |
| 0.5 ms  | `=0` will be rejected — use min viable | Stress test |

For each point collect:
- **Throughput** (GB/s) — from `--record_results` output
- **P99 end-to-end latency** (ms)
- **Gap-expiry count** — number of batches forced through timeout rather than arriving in order.
  Add a counter in `ProcessLevel5BatchesShard()` in `topic.cc` where `force_expire_all_frontiers`
  or `age_ns >= normal_timeout_ns` fires. Log this count per epoch or export via a stats struct.
- **Ordering violation count** — after the run, verify that all committed batches for each
  client appear in strict `client_seq` order in the GOI. A non-zero count at any timeout means
  the protocol has a bug independent of the timeout value.

### Specific Test Configuration

Use the same RF=2, 4-server, 4 GB message-size configuration as the published results on the
`scalog-cxl-baseline` branch (`config/embarcadero.yaml`, `segment_size >= 8 GB`). Order 5
(strong total order) must be enabled for the target topic (set `order: 5` in the topic config).

Run at least **3 trials per timeout value** to account for run-to-run variance on the CXL
testbed. Discard first trial if it shows anomalous cold-start behavior.

---

## How to Interpret Results and Update the Paper

### Case A: 1.5 ms works (no false expiries, throughput ≥ 10 GB/s, P99 ≤ 2 ms)

This confirms the paper's claim. **Code change required:**

In `src/embarlet/topic.cc`, `GetOrder5HoldTimeoutNs()`, change:
```cpp
const uint64_t default_ms = 100ULL;
```
to:
```cpp
const uint64_t default_ms = 2ULL;   // or 1 or 2, whichever is the validated minimum
```

**No paper change needed.** The text already says "default 1.5 ms" in both prose and Algorithm 2.

### Case B: 1.5 ms causes false expiries but 3–5 ms works cleanly

Change the code default to the validated value (e.g., 3 ms) and update the paper in two places:

1. **`Paper/Text/Design.tex` prose** (~line 215): Change
   `(default 1.5\,ms)` → `(default X\,ms)` where X is the validated value.

2. **`Paper/Text/Design.tex` Algorithm 2** (~line 248): Change
   `\textbf{Timeout} (wall-clock, default 1.5\,ms)` → `(wall-clock, default X\,ms)`.

3. **`Paper/Text/Introduction3.tex`** (~line 48): The introduction says
   *"holds out-of-order batches in a bounded reorder buffer with a 1.5\,ms timeout"*. Update
   this to match.

4. Add one sentence in the Design.tex prose explaining the choice: *"The 1.5 ms default is
   chosen to exceed the 99th-percentile inter-server ingestion skew observed on our testbed
   (X µs) while remaining below the target P99 latency budget."* Fill in X from the experiment.

### Case C: No timeout below ~50 ms works without false expiries

This would indicate a correctness or clock-skew problem in the hold buffer logic, not just a
tuning issue. **Do not change the paper default claim until the bug is fixed.** Instead:

1. Profile where the ingestion skew comes from: add per-batch timestamps at `BrokerScannerWorker5`
   push time and at `ProcessLevel5BatchesShard` arrival time. Compute the gap between when a
   client's batch $k$ arrives and when the missing predecessor $k-1$ arrives (or times out).
2. If the skew is consistently >10 ms, investigate whether the PBR polling interval in
   `BrokerScannerWorker5` is the bottleneck (too infrequent), or whether cross-broker clock
   drift is affecting `SteadyNowNs()` comparisons.
3. Fix the root cause, re-run the experiment matrix, then apply Case A or B above.

---

## Key Code Locations

| What                          | File                          | Location               |
|-------------------------------|-------------------------------|------------------------|
| Default timeout constant      | `src/embarlet/topic.cc`       | Line 89 (`default_ms`) |
| Env var override              | `src/embarlet/topic.cc`       | Lines 77–85            |
| Timeout applied per shard     | `src/embarlet/topic.cc`       | Line 5248 (`normal_timeout_ns`) |
| Expiry loop (where to count)  | `src/embarlet/topic.cc`       | Lines 5243–5253        |
| Paper prose claim             | `Paper/Text/Design.tex`       | ~Line 215              |
| Paper Algorithm 2 claim       | `Paper/Text/Design.tex`       | ~Line 248              |
| Introduction claim            | `Paper/Text/Introduction3.tex`| Line 48                |

---

## Why This Is the Highest-Priority Pre-Submission Task

The paper's novelty argument rests on the claim that CXL makes a sub-2 ms hold buffer
practical. If the default in the shipped code is 100 ms, any reviewer who clones the repo
will immediately flag it as inconsistent. A system paper's code and prose must agree on
configuration defaults. This experiment closes that gap.
