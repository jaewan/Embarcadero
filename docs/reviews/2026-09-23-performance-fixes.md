# Performance fixes and validation — September 23, 2026

This implements the measured remedies from the [root-cause investigation](2026-09-23-performance-root-causes.md). It preserves authoritative ACK completion, session fencing, validation before publication, and the bounded primary log contract. The log still retains data and rejects admission at capacity; no recycling was introduced.

## Implementation

- **Concurrent exact audit:** the ORDER5 E2E client verifies indexed payload and order while publishing proceeds. Its task owns an immutable seed and cancels/joins before subscriber destruction on every exit. A final public counter check runs after both successful publisher completion and payload auditing. Serial audit remains an explicitly selected diagnostic.
- **Bounded ordered retention:** receive chunks and carry/fallback allocations share a capacity budget, default 256 MiB. Allocation capacity and overlapping old/new carry allocations count until their storage is freed. Descriptors, including pooled descriptors, and sparse reorder slots have separate default limits of 262144. Exhaustion latches an explicit failure rather than blocking a receiver behind a missing message. These limits exclude fixed connection buffers and optional latency telemetry; they are not a total process RSS cap.
- **Adaptive ACK waiting:** `Poll` and `WaitUntilAcked` spin only at entry, then wait for progress notifications with a bounded fallback. Notifications follow authoritative state publication; the state predicate, timeout and recovery workers remain authoritative. The event registration/generation protocol closes the check-to-sleep race. No per-message notification was added.
- **Broker metadata work:** successful session lookup no longer repeats its cache invalidation in the commit guard. Existing ACTIVE session entries avoid redundant claim flushing; newly claimed or still-inactive entries retain the flush/helping path. Unchanged state avoids a CAS while retaining the prefix and state visibility sequences and the commit/fence publication gate.
- **Terminal error propagation:** empty consumer reads inspect delivery status, stop on retention failure, and latch an explicit application error. Key-value sync/apply waits and replica convergence observe the latch; benchmark catches return failure after unwinding the store. Latency results distinguish a terminal error from a genuine timeout, including a final status observation after ACK/drain completion; ACK-primary reporting cannot soften a terminal failure. Successful per-message processing adds no status snapshot.
- **Alignment-safe consumer API:** sanitizer coverage exposed existing casts of arbitrary receive-buffer addresses to 64-byte-aligned shared-memory headers. Ordered parsing now reads/writes scalar fields with `memcpy`; public borrowed views expose safe field accessors. Auditing, latency delivery extraction, and the key-value benchmark use them. Legacy subscriber paths inspect aligned local header copies and preserve the original returned byte addresses. The wire format and payload storage remain unchanged.

The byte budget is charged per chunk/carry allocation, not per delivered message. Header accessors copy scalar fields, not payloads. Receive chunks still require one retained copy, and descriptor-pool synchronization remains; this is not a zero-copy claim. PBR cache-line layout and speculative refresh/inlining changes were deliberately left for separate evidence.

## Reproducible build

The tested optimization source is frozen as `/tmp/embarcadero-optimization-source-4.tar.gz`, SHA256 `d4c68cc3ba250fd9df19e100a9aa1a64823275b6a62a96f8d803e0d746f55098`, with 1798 inventoried files. The comparison baseline is the previous refactored source14 release, not the original pre-refactor commit. Its archive SHA256 is `811fd23dd548c2b310576c514d7b5642263c201c2ae493d9be9d951cb14383ec`.

Both release executables use native Release builds with fault injection, sanitizers and latency instrumentation disabled. All broker/client translation-unit compiler/ISA contracts, link options, external static dependency bytes and resolved shared dependency bytes match. BoringSSL assembly initially differed only in build-directory debug information; the exact baseline crypto archive was explicitly reused and all four release binaries were relinked. The original archive and before/after evidence are preserved. No baseline executable or archived source member was changed.

Evidence lives under `results/refactor-optimization/2026-09-23/`, including `matched-build-evidence.json`, `dependency-reuse/`, source inventories, commands, logs and failed attempts. Both tested source archives and their inventories are also retained in `source-archives/`, independent of the temporary build directories.

## Review and correctness

Three agents implemented/reviewed independent broker, publisher and subscriber paths; authors cross-checked one another's changes. Tests cover event notification races, authoritative versus diagnostic ACK counts, repeated ACTIVE prefix publication, competing claims/fences, real ordered-parser exhaustion, sparse gaps, cancellation, late audit errors and V1/V2 wire alignment.

The full native release build, both key-value benchmarks and the fault-enabled production targets build successfully. All 53 CTests pass on source4.

Subscriber: 23 cases pass under ASan/UBSan with leak detection and under TSan on source4. Publisher: 15 cases pass under each sanitizer on source3; all 11 linked translation units and 90 existing core headers are byte-identical in source4. Linked broker publication and manager-geometry TSan cases pass; all 132 recorded broker inputs match source4. No sanitizer suppression or host-policy change was used.

Eight live fault scenarios pass on source3: both fence/commit gate winners, empty-prefix fencing, ACK-publication lag, withheld authoritative ACK, session reopen/resubmission, session capacity and retained rollover capacity. The broker/publisher/subscriber core inputs are unchanged in source4; its additional changes propagate terminal errors through latency and key-value callers. Source4 serial audit compatibility and a 32 MiB latency workload pass. A live 2 GiB serial transfer with a 32 MiB ordered-retention limit completes all publisher ACKs but rejects subscriber delivery with `ORDERED_RETENTION_EXHAUSTED` and a failing client exit; owned cleanup remains normal. The initial 32 MiB probe reached only about 8.2 MiB of retained data and correctly succeeded, so it is retained as an ineffective exhaustion probe rather than counted as a negative-control pass.

Source3 live-fault path equivalence is recorded separately: broker inputs, publisher/subscriber core inputs, the complete E2E function, fault driver and owned runner are unchanged in source4. The source4 latency and key-value failure-policy tests use actual bounded parser exhaustion, empty consume results, cross-thread failure latching, shutdown and late error snapshots. They are not a live key-value network fault test.

An independent final correctness audit passed 370 checks with no blocker (`independent-correctness-review.json`). All 12 completed live manifests have the expected executable identities and exit statuses, normal cleanup, and no surviving owned PID or shared-memory name. The performance campaign is checked separately. A final inventory comparison verifies all 1798 frozen paths; only this review and the historical root-cause document differ in the working tree (`source4-working-tree-equivalence.json`).

Retained intermediate failures are part of the evidence:

1. Source1 failed compilation because an old ACK-loop diagnostic referenced a removed counter. Source2 counts actual event waits instead.
2. Source2 passed 53 CTests, but expanded subscriber UBSan coverage exposed unaligned wire-header dereferences. Source3 fixes the actual parser and consumer paths and adds alignment regressions.
3. Source2's withheld-ACK live test rejected a legitimate concurrent delivery-only audit. The publisher had correctly timed out after two seconds with zero authoritative/raw ACKs and exited with failure. The corrected oracle still requires that exact timeout and failing exit, rejects ACK/final completion, and permits only an exact indexed delivery-only audit. The failed attempt remains unchanged and final live tests use the new oracle.

## Measurement protocol

The fixed whole-stack DRAM pilot compares each version's own broker and client, including the intended audit scheduling change. It uses a 2 GiB indexed workload, 4 KiB messages, ORDER5/ACK1/RF0, 64 GiB fresh DRAM mappings and 4 GiB log segments. Clients use NUMA node 0 CPUs 0–31; brokers use node 1 CPUs 128–159, 160–191 and 192–223. Each topology has two excluded qualification trials and four balanced AB/BA pairs: 20 trials total across one and three brokers. Counts and endpoints were fixed before execution.

The primary endpoint includes both authoritative ACK completion and exact payload/order audit. ACK-only throughput is secondary. Client CPU, minor faults and peak RSS cover the entire process lifetime, including initialization. Paired log-ratio Student-t intervals use four pairs and are exploratory; the preregistered 5% nonregression margin is not a power guarantee. Binary paths and hashes are checked through `/proc` while children are alive, alongside placement, audit limits/counters, normal exits and owned cleanup.

This is a local finite-transfer DRAM comparison. It does not establish physical-CXL performance, persistence, sustained capacity, multi-host behavior, or an isolated broker improvement. The older common-client broker campaign and its inconclusive one-broker result remain separate.

## Measured results

All 20 preregistered runs passed; no samples were replaced or added. The independent reviewer checked the raw delivery and ACK populations, observed executable identities, placement, mapping, and cleanup, and recomputed the paired intervals. All 60 owned process IDs and 20 shared-memory paths were absent afterward. See `independent-performance-review.{md,json}` and each `stack-n{1,3}-source4/{analysis,resources,independent-integrity-audit}.json` in the evidence directory.

Ratios are candidate / previous refactored version. Each interval uses four paired measurements; qualification runs are excluded.

| Brokers | Endpoint | Geometric ratio | Exploratory 95% CI | Assessment |
|---:|---|---:|---|---|
| 1 | Audited end-to-end throughput, primary | 1.0382 | 0.7544–1.4287 | Inconclusive |
| 3 | Audited end-to-end throughput, primary | 1.0599 | 0.9752–1.1519 | Meets exploratory 5% nonregression threshold; improvement unproven |
| 1 | ACK-completion throughput, secondary | 1.0949 | 0.5396–2.2215 | Inconclusive |
| 3 | ACK-completion throughput, secondary | 0.9527 | 0.8694–1.0440 | Inconclusive |

Whole-client resource medians include initialization, transfer, audit and teardown. These are not transfer-phase-only counters.

| Brokers | Resource | Previous median | Candidate median | Paired geometric ratio (95% CI) |
|---:|---|---:|---:|---|
| 1 | Peak RSS, MiB | 2614 | 562 | 0.2254 (0.1783–0.2849) |
| 3 | Peak RSS, MiB | 2838 | 648 | 0.2298 (0.2201–0.2400) |
| 1 | Minor page faults | 671118 | 147595 | 0.2295 (0.1801–0.2924) |
| 3 | Minor page faults | 726310 | 167586 | 0.2336 (0.2202–0.2479) |
| 1 | Total CPU, seconds | 5.138 | 4.543 | 0.8990 (0.5896–1.3709) |
| 3 | Total CPU, seconds | 6.963 | 6.560 | 0.9558 (0.8517–1.0726) |

Memory and page-fault reductions are consistent across both topologies, approximately 77% by paired geometric ratios. Total CPU reduction is inconclusive. User CPU increases: median 1.248→1.829 seconds for one broker and 2.320→3.285 seconds for three. System CPU decreases: 3.956→2.679 and 4.630→3.308 seconds respectively. The whole-stack comparison does not isolate whether the user-CPU increase comes from concurrent scheduling, bounded bookkeeping, alignment-safe parsing, or another changed path; it is a measured tradeoff, not evidence that every hot path became cheaper.

The implemented fixes materially reduce client memory pressure and preserve tested correctness. This pilot does **not** establish overall throughput improvement or nonregression across both topologies. The three-broker ACK-only point estimate is lower, with an interval spanning both regression and improvement; the one-broker intervals are particularly wide. More precise throughput attribution requires a separately specified campaign with more pairs and isolated treatments, rather than adding samples opportunistically to this completed pilot. Physical CXL, multi-host clients, replication, and sustained workloads remain outside these measurements.
