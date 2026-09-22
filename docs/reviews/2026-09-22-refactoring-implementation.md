# Initial refactoring implementation and cross-review

This records the first implementation tranche of the [refactoring plan](2026-09-22-refactoring-plan.md), against `ae9959dd2892af821878aa42d4e2c05fcb635466`. Changes are in the working tree, not committed. The pre-existing README, artifact, and experiment-plan edits were preserved. The full twelve-stage release plan is not complete.

## Delivered behavior

| Area | Implementation | Limits |
|---|---|---|
| Development loop, PR01 | Owned local DRAM runner, explicit emulation on every broker, unique 64 GiB region, 256 MiB segments, node-1 brokers/memory and node-0 client/memory, effective configuration and run manifest | One or three local brokers; 32 MiB correctness workload; fixed ports serialized per user |
| Build/test integrity, PR02 | Target-scoped flags, portable/native and sanitizer presets, mandatory test dependencies, generated-header dependency fixes, explicit test inventory, historical destructive E2E tests excluded by default | Dependencies were already installed; no clean-machine bootstrap qualification |
| Configuration/layout, PR03 | Thread-safe resolution followed by freezing, shared file/string parser, strict environment values, CLI precedence, checked geometry, per-region bitmap allocation, versioned attachment descriptor | Layout v5 requires a fresh region and matching executables; inherited shared pointers require matching mapping base |
| Lifetime/capacity, PR04–05 | Unsafe payload reuse disabled; concurrent rollover reservation coordinated; PBR absolute/modulo accounting repaired; ORDER5 single-writer capacity check before advancing either GOI or message-order frontier | Finite storage only; exhaustion stops progress safely; sustainable reclamation remains unimplemented; no crash-atomic transaction across frontiers |
| Session state, PR06 | Explicit capacity admission, bounded non-reused authoritative entries, commit/fence gate, monotonic fenced snapshots/frontiers, hold-depth release accounting, membership version checks | Process-local sequencer authority; no physical fencing or production host failover |
| Transport/lifecycle, PR07 | Fragment-aware session parsing, bounded envelope and body checks before publication, owned ACK topic strings, cancellable queues, replication-token waits cancellable on shutdown, and interruptible shutdown waits | Blocking kernel disk operations are still outside cancellation guarantees; normal replication waits have no deadline |
| Topic ownership, PR08 | Hash ordinary names correctly, validate identity on attachment, enforce one distinct topic per region lifetime | Multi-topic GOI/CV isolation is deliberately unsupported |
| Baselines, PR09 | LazyLog transport readiness/retry policy; Scalog rejects unsupported memory-mode durable RPC and tests actual disk mode | This does not qualify every baseline deployment or failure mode |
| Extraction/organization, PR10–11 | Narrow production headers for layout, reservation, session/topic admission, framing, validation and queues; shared configuration target; development docs and runner under `tools/` | Large runtime classes and historical experiment scripts still need staged extraction/consolidation |

The broker still receives directly into reserved BLog memory on the normal ingest path and reserves the ordered PBR position after the payload is received. Body validation is independent of cache-flush policy. Batch identity must agree with the connection handshake before allocation or duplicate draining.

`--print-layout` uses the production layout calculator without mapping memory. The deprecated `cxl.emulation_size` produces a diagnostic; `cxl.size` remains authoritative. Real-memory fallback rejects a nonexistent or CPU-bearing CXL NUMA node. This prevents silently treating an ordinary DRAM node as real CXL.

## Independent cross-review

Three parallel reviewers covered architecture/builds, allocation/session correctness, and development/transport integration. Authors did not serve as the sole reviewers of their own changes. The lead reviewed the integrated paths and ran the combined suite. Cross-review changed the implementation, including:

- Preserving Corfu's actual initial segment offset rather than assuming all paths start immediately after the segment header.
- Assigning PBR identity from the same successful reservation operation as the slot, removing a separate counter race.
- Releasing the commit/fence gate before taking retirement-shard locks and terminalizing classification after capacity failure.
- Retrying bitmap CAS contention and masking invalid tail bits so races do not manufacture exhaustion or allocate outside the region.
- Checking follower topic identity before claiming a local slot, and checking same-topic lookup before the single-topic capacity guard.
- Withholding head registration until initialization is complete; followers validate the region descriptor before interpreting derived metadata.
- Replacing a legacy E2E checker that had accepted zero parsed messages with an audit of the actual ordered-delivery path. Indexed payloads detect reordered/duplicated messages in addition to corruption and count errors.
- Bypassing session-prefix peeking for non-session topics. Legacy publishers wait for an ACK connection during initialization and cannot send the first batch until that connection exists.
- Rejecting a batch client ID that differs from its connection identity; admission, duplicate detection, and publication must address the same session key.
- Closing queued socket ownership exactly once, fixing worker accounting on queue cancellation, and preventing cancelled partial disk writes from publishing durability progress.
- Making heartbeat sleep and ACK connection retries observe shutdown; review also covered control-RPC shutdown and startup-failure callback lifetime.

This is source review plus bounded implementation testing, not proof that all harmful concurrent schedules have been eliminated.

## Validation record

The complete out-of-tree Debug build passed for broker, client, baselines, benchmarks, and test binaries. The supported CTest suite passed **38/38** after integration-review fixes. After the performance campaign, the measurement harness and analyzer tests were registered with CTest; the final suite passed **40/40**, including 28 Python tooling cases across its three Python test entries. Running the earlier suite concurrently with a live cluster first exposed use of the production lock in fake-process tests; each test now uses a private lock inode while still exercising real lock semantics. That suite then passed while the live cluster held the production lock.

The final one-broker ORDER5/ACK1/RF0 smoke passed: 8,192 messages, 32 MiB payload, complete ACK count, exact indexed payload order, zero duplicates/parser errors/export gaps, and no forced process termination. Its manifest is `/tmp/embarcadero-dev-1002-p5zd_xku/manifest.json`; all 16,777,216 shared 4 KiB pages were observed on node 1. Client thread masks were confined to node 0 and broker masks to node 1. The executed portable Debug build reported the mutex PBR reservation path.

The final three-broker run also passed the same exact 8,192-message audit and graceful cleanup, with no retransmission. Routing counters showed 3,024 messages through broker 0, 2,648 through broker 1, and 2,520 through broker 2; this exercised actual multi-broker ingest/export. Its manifest is `/tmp/embarcadero-dev-1002-ukzyexgj/manifest.json`. Both runs removed their owned shared-memory objects, and all recorded child processes exited. Neither result establishes multi-host behavior or failure recovery.

An isolated ORDER0/ACK1 legacy startup regression passed with all 8,192 messages acknowledged, positive E2E completion, no session negotiation and graceful cleanup (`/tmp/embarcadero-dev-1002-zolwoq72/manifest.json`). The adapter used for that run is preserved as `test/integration/check_legacy_startup.py`, with the checker additionally verified against those artifacts. This check deliberately makes no indexed-payload or ordering-audit claim. It is optional and is not run by ordinary CTest because it allocates a real 64 GiB region.

Configuration and transport safety tests passed ASan/UBSan. Reservation and session-admission tests also passed standalone ASan/UBSan runs. These are focused instrumented tests, not a fully instrumented cluster run. ThreadSanitizer binaries were built, but the host runtime exited before `main` with `unexpected memory mapping`; no TSan-clean claim is made.

The reservation tests call the extracted production functions, including repeated PBR wrap, concurrent bounded allocation, segment freeze/rollover with delayed writes, and joint GOI/message capacity boundaries. Layout tests include overflow, descriptor mismatch, a non-word-aligned bitmap tail under concurrency, and multiple region geometries in one process. Session tests exercise the same admission, monotonic-update, gate and rejection primitives with synthetic entries; they do not invoke real `CommitEpoch`/OPEN/fence-notification/ACK interleavings. These tests do not replace full live Topic/NetworkManager fault tests.

The first live one-broker attempt correctly failed qualification despite completing the transfer: the old subscriber checker parsed zero messages, and broker shutdown needed forced termination. Those failures drove the actual ordered-delivery audit and interruptible heartbeat shutdown fixes. Failed artifacts were retained and owned processes/region cleaned up. A failed run is not counted as a pass.

Initial build and smoke artifacts live under `/tmp` and may be removed by host cleanup. Performance evidence is additionally retained under the git-ignored `results/refactor-perf/2026-09-22/`, including the exact compiled candidate source archive, hashes and patch. Performance manifests associate the frozen binaries with that archive; the ordinary smoke runner's dirty-tree source identity remains explicitly incomplete.

## Performance assessment

The [matched Release/native DRAM comparison](2026-09-22-performance-comparison.md) completed six baseline/candidate pairs per profile with one common audited client. Under the pilot's statistical assumptions, both three-broker endpoints stayed within ±5%; one-broker audited delivery narrowly met the 5% nonregression bound, while one-broker publisher completion remained inconclusive. No throughput improvement or blanket server/whole-stack nonregression is established. The one- and three-broker profiles use separately recorded mapping policies and are not pooled. The earlier Debug, audited, 32 MiB smoke remains correctness evidence only.

Removing the per-reservation unsafe-GC lock and retaining direct receive should help or preserve the common allocation path. Immutable configuration uses an atomic fast path after resolution, avoiding a mutex on each read. These are code-path expectations, not benchmark results. Always-on message validation and commit/fence serialization have necessary correctness costs that require measurement.

The campaign found two additional issues. The first checker incorrectly required a diagnostic raw ACK count to equal the authoritative frontier; source review demonstrated legal lag, and a regression test now distinguishes that lag from a true completion shortfall. A later three-broker candidate startup failed because PIE ASLR forced the head to a fallback mapping address different from its follower's. The descriptor correctly rejected it. Both failed campaigns remain retained and unqualified. The owned local runner now sets one explicit base, and performance qualification verifies every broker's actual mapping. The entire three-broker experiment was restarted with both frozen broker versions. Generic automatic address coordination and clear startup rejection remain release work.

Portable and native builds can select different PBR reservation implementations. Compare identical compiler, optimization, ISA, ACK/sink, CPU/core budget, page policy and observed reservation mode. Local NUMA0 clients are the requested primary development profile; loopback and cross-socket traffic mean they are not a guaranteed upper bound on remote-client performance.

## Remaining gates

- The [production-path fault campaign](2026-09-22-production-fault-plan.md), including deterministic mapping collisions, session/fence/reconnect, stalled-reader/replica, exhaustion and interrupted-shutdown cases beyond the bounded component tests and normal smoke runs.
- Safe retention/reclamation before any sustained or long-duration throughput claim.
- Resolve the one-broker publisher-completion uncertainty and extend the completed finite-transfer comparison to matched-load latency, small messages, partial batches, multiple clients and load-generator saturation. No sustained or general server nonregression claim is established.
- Remaining component extraction, baseline-free minimal client builds, shared experiment orchestration and safe replacements for historical scripts.
- Clean-machine bootstrap/CI, explicit supported-mode matrix, license and dependency provenance decisions, and public release preparation. No project license was selected on the owner's behalf.
- Real CXL publication/observation and performance validation when hardware returns; independent-host failure and physical writer-fencing validation remain separate work.

See [development instructions](../development-dram.md) and [component contracts](../architecture/refactoring-boundaries.md) for the implemented interface.
