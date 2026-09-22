# Performance fixes and validation — September 23, 2026

This implements the measured remedies from the [root-cause investigation](2026-09-23-performance-root-causes.md). It preserves authoritative ACK completion, session fencing, validation before publication, and the bounded primary log contract. The log still retains data and rejects admission at capacity; no recycling was introduced.

## Implementation

- **Concurrent exact audit:** the ORDER5 E2E client verifies indexed payload and order while publishing proceeds. Its task owns an immutable seed and cancels/joins before subscriber destruction on every exit. A final public counter check runs after both successful publisher completion and payload auditing. Serial audit remains an explicitly selected diagnostic.
- **Bounded ordered retention:** receive chunks and carry/fallback allocations share a capacity budget, default 256 MiB. Allocation capacity and overlapping old/new carry allocations count until their storage is freed. Descriptors, including pooled descriptors, and sparse reorder slots have separate default limits of 262144. Exhaustion latches an explicit failure rather than blocking a receiver behind a missing message. These limits exclude fixed connection buffers and optional latency telemetry; they are not a total process RSS cap.
- **Adaptive ACK waiting:** `Poll` and `WaitUntilAcked` spin only at entry, then wait for progress notifications with a bounded fallback. Notifications follow authoritative state publication; the state predicate, timeout and recovery workers remain authoritative. The event registration/generation protocol closes the check-to-sleep race. No per-message notification was added.
- **Broker metadata work:** successful session lookup no longer repeats its cache invalidation in the commit guard. Existing ACTIVE session entries avoid redundant claim flushing; newly claimed or still-inactive entries retain the flush/helping path. Unchanged state avoids a CAS while retaining the prefix and state visibility sequences and the commit/fence publication gate.
- **Alignment-safe consumer API:** sanitizer coverage exposed existing casts of arbitrary receive-buffer addresses to 64-byte-aligned shared-memory headers. Ordered parsing now reads/writes scalar fields with `memcpy`; public borrowed views expose safe field accessors. Auditing, latency delivery extraction, and the key-value benchmark use them. Legacy subscriber paths inspect aligned local header copies and preserve the original returned byte addresses. The wire format and payload storage remain unchanged.

The byte budget is charged per chunk/carry allocation, not per delivered message. Header accessors copy scalar fields, not payloads. Receive chunks still require one retained copy, and descriptor-pool synchronization remains; this is not a zero-copy claim. PBR cache-line layout and speculative refresh/inlining changes were deliberately left for separate evidence.

## Reproducible build

The tested optimization source is frozen as `/tmp/embarcadero-optimization-source-3.tar.gz`, SHA256 `35f1717875b94732adf52ca1104739a1b639798141464ec7da954c899cae3c1d`, with 1796 inventoried files. The comparison baseline is the previous refactored source14 release, not the original pre-refactor commit. Its archive SHA256 is `811fd23dd548c2b310576c514d7b5642263c201c2ae493d9be9d951cb14383ec`.

Both release executables use native Release builds with fault injection, sanitizers and latency instrumentation disabled. All broker/client translation-unit compiler/ISA contracts, link options, external static dependency bytes and resolved shared dependency bytes match. BoringSSL assembly initially differed only in build-directory debug information; the exact baseline crypto archive was explicitly reused and all four release binaries were relinked. The original archive and before/after evidence are preserved. No baseline executable or archived source member was changed.

Evidence lives under `results/refactor-optimization/2026-09-23/`, including `matched-build-evidence.json`, `dependency-reuse/`, source inventories, commands, logs and failed attempts.

## Review and correctness

Three agents implemented/reviewed independent broker, publisher and subscriber paths; authors cross-checked one another's changes. Tests cover event notification races, authoritative versus diagnostic ACK counts, repeated ACTIVE prefix publication, competing claims/fences, real ordered-parser exhaustion, sparse gaps, cancellation, late audit errors and V1/V2 wire alignment.

Validation is still in progress; final sanitizer, live-fault and performance results will be recorded here before completion.

Retained intermediate failures are part of the evidence:

1. Source1 failed compilation because an old ACK-loop diagnostic referenced a removed counter. Source2 counts actual event waits instead.
2. Source2 passed 53 CTests, but expanded subscriber UBSan coverage exposed unaligned wire-header dereferences. Source3 fixes the actual parser and consumer paths and adds alignment regressions.
3. Source2's withheld-ACK live test rejected a legitimate concurrent delivery-only audit. The publisher had correctly timed out after two seconds with zero authoritative/raw ACKs and exited with failure. The corrected oracle still requires that exact timeout and failing exit, rejects ACK/final completion, and permits only an exact indexed delivery-only audit. The failed attempt remains unchanged and final live tests use the new oracle.

## Measurement protocol

The fixed whole-stack DRAM pilot compares each version's own broker and client, including the intended audit scheduling change. It uses a 2 GiB indexed workload, 4 KiB messages, ORDER5/ACK1/RF0, 64 GiB fresh DRAM mappings and 4 GiB log segments. Clients use NUMA node 0 CPUs 0–31; brokers use node 1 CPUs 128–159, 160–191 and 192–223. Each topology has two excluded qualification trials and four balanced AB/BA pairs: 20 trials total across one and three brokers. Counts and endpoints were fixed before execution.

The primary endpoint includes both authoritative ACK completion and exact payload/order audit. ACK-only throughput is secondary. Client CPU, minor faults and peak RSS cover the entire process lifetime, including initialization. Paired log-ratio Student-t intervals use four pairs and are exploratory; the preregistered 5% nonregression margin is not a power guarantee. Binary paths and hashes are checked through `/proc` while children are alive, alongside placement, audit limits/counters, normal exits and owned cleanup.

This is a local finite-transfer DRAM comparison. It does not establish physical-CXL performance, persistence, sustained capacity, multi-host behavior, or an isolated broker improvement. The older common-client broker campaign and its inconclusive one-broker result remain separate.
