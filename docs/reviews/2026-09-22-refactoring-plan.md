# Embarcadero refactoring and development plan

This is the original ordered plan. Current implementation and acceptance status are tracked in the [completion ledger](2026-09-22-refactoring-completion-plan.md) and [completion evidence](2026-09-22-completion-results.md). The [initial implementation record](2026-09-22-refactoring-implementation.md) preserves the earlier tranche. Public-release and hardware gates remain distinct from software completion.
Planning baseline: `ae9959dd2892af821878aa42d4e2c05fcb635466`, with existing working-tree changes preserved.

Revised after the [systems expert review and thought experiments](2026-09-22-systems-plan-review.md). The expected reliability gains are substantial; performance improvement remains a hypothesis. Behavior-preserving extraction must meet a nonregression gate, while necessary correctness costs must be measured and explained separately.

This plan follows the [engineering and research review](2026-09-22-open-source-readiness.md), the current paper in `Paper/Text/`, and the earlier SOSP material. Finding identifiers F1–F20 refer to that review, which used the preceding `cd62a6bc` baseline. Revalidate each finding against the implementation before changing it. Historical experiment instructions are evidence of earlier workflows, not acceptance criteria for the new release.

## 1. Development contract

Proceed with DRAM now. Restoring CXL is not a prerequisite for fixing software correctness, isolating tests, or extracting maintainable components. Real CXL remains a prerequisite for final CXL performance and hardware-publication claims.

The host currently exposes NUMA nodes **0 and 1**, approximately **1.5 TiB DRAM**, and **756 GiB available in `/dev/shm`**. Node 2 is absent. These were checked during planning; runners must discover them again at execution time.

| Profile | Broker placement and memory | Clients | Purpose |
|---|---|---|---|
| `dev-dram-local` | CPU node 1, memory node 1; every broker uses `--emul` | CPU node 0, memory node 0 | Default integration, debugging, local performance regression |
| `dev-dram-remote` | Same DRAM broker configuration | Selected hosts via `ssh c1`, `ssh c3`, or `ssh c4`; discover their CPU/NIC topology | Network-path and remote-client checks |
| `research-cxl` | Explicit real backend and verified CXL device/topology | Recorded local or remote topology | Deferred hardware validation and final CXL measurements |

The local profile is implemented by `tools/dev_cluster.py`; remote and research profiles remain proposed. Local clients are a useful high-throughput development profile, but are not guaranteed to be the highest-performance configuration: loopback, CPU contention, cross-socket traffic, and NIC offloads change the comparison. Keep local and remote results separate.

Required settings for the initial DRAM profile:

```text
broker arguments:             --emul, on head, followers, and restarted brokers
EMBARCADERO_CXL_SHM_NAME:      /embarcadero-dev-<uid>-<unique-run-id>
embarcadero.cxl.size:         68719476736       # 64 GiB
embarcadero.storage.segment_size: 268435456    # 256 MiB
broker CPU / memory nodes:   1 / 1
local client CPU / memory nodes: 0 / 0
```

`cxl.size` is authoritative in today's emulation implementation. Setting only `cxl.emulation_size` does not resize the mapping. The checked-in main config already sets `cxl.size` to 64 GiB, but has 8 GiB segments and a misleading 32 GiB `emulation_size`. Generate a complete effective configuration; do not assume the current loader supports YAML overlays.

The GOI reserves **32 GiB by itself**, before other metadata and payload segments. A 4–32 GiB whole-region mapping is insufficient. Preflight must calculate the actual layout, aligned offsets, segment availability, and payload budget. Available tmpfs space alone is insufficient: check node-1 memory, process/cgroup limits, existing mappings, and concurrent reservations. Default to one full integration cluster at a time.

Bind the head before it initializes the shared mapping, then start followers after readiness. CPU affinity alone does not establish page placement; use a node-1 memory policy and inspect actual placement after first touch. Preserve initialization until a separate change proves a safe alternative; do not use an uninitialized mapping merely to shorten startup. Verify thread affinity as well as parent-process affinity, and ensure cgroups do not override the profile.

Start smoke coverage with one topic, one broker, one client, ORDER=5 and ACK1; then expand to three brokers and failure scenarios. Use a small aggregate workload, initially 32 MiB of application payload, with a checked upper bound including headers, padding, retries, and all bytes that could land on one broker. Stay below one segment per broker for smoke tests. Segment rollover, retention, and exhaustion belong in separate deliberate boundary tests. Do not assume striping divides every workload evenly.

Memory backend and persistence sink are independent settings. DRAM emulation can exercise real disk writes, but a memory-copy sink does not validate media durability. ACK2 results must identify the actual sink. Neither configuration on this host validates independent host failures or production sequencer replacement.

## 2. Ordered implementation work

Keep fixes, behavior-preserving extraction, file moves, and performance tuning in separate reviewable commits. Do not begin with a wholesale rewrite of `topic.cc` or a repository-wide move. The first milestone is a safe, repeatable development loop.

| PR | Change | Dependencies | Acceptance evidence |
|---|---|---|---|
| 01 | Isolated DRAM runner and explicit profiles | None | Dry-run manifest; small local smoke; timeout/interruption cleanup leaves unrelated processes and mappings intact |
| 02 | Trustworthy builds and test registration | None; integration uses 01 | Out-of-tree tests execute their own binaries; Debug and sanitizer flags verified in compile commands; safe test labels |
| 03 | Immutable configuration and checked memory layout | 02 | F4/F12 regressions; invalid configurations rejected before mapping/listening; head/follower configuration agreement |
| 04 | Segment lifetime containment and safe capacity failure | 01–03 | F1 regression with delayed consumers; exhausted allocation neither overwrites retained bytes nor falsely acknowledges |
| 05 | PBR and GOI sequence/capacity fixes | 02–04 | F15 repeated wrap and F2 boundary tests through production code; explicit backpressure/exhaustion behavior |
| 06 | Session admission, commit fencing, and state accounting | 02–05 | F3/F9/F13/F14 regressions; production ready→fence→commit interleavings; fail-closed admission |
| 07 | Wire parsing and cancellable process lifecycle | 01–03 | F6/F7/F16/F17/F18 regressions, fragmented input and shutdown under load |
| 08 | Topic identity and shared-state ownership | 03, 05–06 | F5 collision tests plus concurrent multi-topic isolation, or enforced single-topic support until isolation is complete |
| 09 | Repair optional baseline implementations | 02, 07 | F19/F20 tests pass with explicit sink/retry contracts; no silent removal of failing coverage |
| 10 | Extract protocol, storage, transport, and client components | Relevant correctness PRs above | Differential traces, focused tests, explicit ownership, unchanged supported protocol behavior |
| 11 | Consolidate experiment tooling and repository layout | 01–03, 10 | Existing documented experiments resolve through shared orchestration; compatibility entry points and reproducible manifests |
| 12 | Open-source release gate | 04–11 | Clean-machine build, supported tests, release documentation and provenance, scoped claims; CXL-dependent evidence marked deferred |

Split PR03 into **03a**, immutable configuration plus checked existing-layout geometry, and **03b**, region-header/ABI migration. PR04 depends on 03a, not 03b; do not delay reclamation containment behind an ABI change. Collect correctness-qualified baseline traces and define performance workloads during PRs 01–02. PR03b must land before releasing a changed shared layout. All other early PR03 dependencies refer to 03a unless they change that layout.

PRs identify review units; split a large unit into smaller commits with the same gates. Independent branches can progress separately, but performance baselines and integrated results must name the exact combined revision.

### PR 01: make development runs safe and explicit

Implement one small cluster runner used by the new development entry point. Reuse it from legacy launchers incrementally. It owns:

- A unique run directory, effective configuration, shared-memory name, disk-output directory, logs, and run manifest.
- Explicit backend selection propagated to every spawn/restart. Reject a mixed backend or mismatched shared region within a cluster. Real mode fails clearly when its required hardware is unavailable; no automatic relabeling of DRAM as CXL.
- Startup dependency ordering, readiness checks, bounded startup/run/shutdown deadlines, and useful diagnostics on failure.
- Exact owned child PIDs/process groups and shared-memory ownership. Cleanup never uses broad `pkill`, never unlinks another run's region, and waits for child exit before removing owned resources.
- All broker/control/replication ports. Until every required port is configurable, serialize colliding profiles and fail on occupied ports; do not claim concurrent cluster isolation prematurely.
- A dry-run mode listing commands, environment, resource estimates, topology, and cleanup targets before starting processes.

The existing `run_multiclient.sh` NUMA fallback does not add `--emul`; its head/follower spawn paths need explicit propagation. Its per-trial suffixes help, but a UID-based name alone is not a unique run identity. Do not change host-wide tuning, hugepage reservations, NIC settings, or privileges as a hidden runner side effect.

Remote mode is optional for the initial milestone. When added, use the same manifest and owned-process discipline on c1/c3/c4, validate artifact/configuration versions, and discover placement rather than copying local node numbers. Local smoke and regression testing must not depend on SSH availability.

### PR 02–03: reliable build, configuration, and layout

Replace global compiler-flag assignment with target-scoped options. Provide Debug, Release, ASan/UBSan, and TSan build presets; do not combine TSan with ASan. Make host-specific ISA tuning opt-in and record it in performance artifacts. Register tests with generated target paths instead of a hardcoded source-tree `build` directory. Separate unit, DRAM integration, hardware, and performance test labels; ordinary CTest must not kill existing workloads (F8/F10/F11).

The supported test preset must require its test dependencies and verify its expected test inventory: current GTest discovery can silently omit most unit tests. Distinguish tests that call production transitions from tests that simulate them. Preserve identical optimization/ISA settings for before/after performance comparisons; portable-build policy is a separate experiment.

Resolve defaults, files, environment, and CLI once using documented precedence; validate the final result before starting threads or allocating resources. Publish an immutable configuration snapshot rather than lazily mutating shared cached values. Implement string loading through the same parser as file loading. Deprecate `cxl.emulation_size` with a clear diagnostic and compatibility policy; do not silently change effective region size.

Introduce a checked layout calculator for GOI, session tables, rings, metadata, alignment, and segments. Use checked arithmetic and explicit units. Validate capacity, broker/topic bounds, ring geometry, and maximum batch size. Add a versioned region header containing layout identity and initialization state; mismatched followers must reject attachment. A layout change requires an explicitly new region. Preserve production geometry initially; small injected capacities are a separate test facility using the same layout/access implementation.

Store geometry per region rather than in `GetNewSegment()` process-static initialization. Acceptance includes two different-capacity production allocator fixtures in one process. Cache immutable primitive configuration in hot objects; replacing a racy getter with a locked getter or runtime dictionary lookup is not an acceptable extraction strategy.

### PR 04–08: restore safety before extraction

**Payload lifetime:** initially disable unsafe reclamation and return bounded backpressure or explicit exhaustion. This is a containment measure, not the completed storage design. Implement safe reuse only after identifying all owners: active receives, held descriptors, readers, replica writes, and retained replay references. Release requires all relevant references/frontiers to permit it; a segment generation alone does not prevent reading overwritten bytes. Audit every caller of `CheckSegmentBoundary()` for failed reservation and rollover handling.

Remove the unconditional reclamation mutex from successful batch reservation when reclamation is disabled. Later safe GC should be triggered by rollover/frontier progress or amortized work, with a proved synchronization scheme; do not replace this lock with global per-message reference counting. Measure lock contention and reservation cycles before claiming a gain.

Audit race-free publication of segment identity and cursor together: `current_segment_` is currently read outside the rollover mutex and written during rollover. A reclamation fix alone does not establish correct concurrent reservation. Include concurrent reservations at segment boundaries in PR04's tests.

**Sequence spaces:** keep absolute producer/consumer sequences distinct from modulo ring indices. Define reserve, publish, retire, and wrap invariants. Guard GOI capacity before allocating an externally visible sequence; initially fail safely at capacity if reusable GOI storage cannot yet be proved. Do not add modulo indexing to GOI without a retention/generation protocol.

Capacity admission must precede **both** message-order and GOI-range reservation and leave no orphaned order range on failure. Coordinate concurrent committers explicitly: a racy check followed by `fetch_add` is insufficient. Exercise full-epoch admission at the boundary and verify subscribers encounter no permanent gap. Preserve payload receive before ordered PBR reservation; if adding earlier backpressure, use bounded admission credits that do not claim scanner-visible positions.

**Sessions:** make session-table admission failure visible before accepting an unrepresentable session. Define when authoritative session state can be reclaimed. Enforce epoch/fence checks at actual commit publication, not merely during classification. Correct hold-depth release accounting and monotonic membership updates. Regression coverage must exercise the duplicate/reconnect preconditions described in the review rather than assuming every reconnect reproduces F3.

Define fencing/commit ownership and a linearization point; normal session snapshots must not reactivate a terminal fenced generation or regress frontiers. Test fences after classification, before GOI publication, and before snapshot publication. An unconditional per-batch session probe/cache invalidation has a potential cost; optimize only with a proved visibility rule, then measure healthy traffic, high table occupancy, and fence churn separately. Capacity failure must also preserve or explicitly terminalize classification state advanced before commit.

**Transport and shutdown:** incrementally parse negotiation and batches with explicit framing and bounds independent of cache-flush mode. Reject malformed input before publication. Own strings and descriptors across asynchronous work. Give queues, blocking I/O, and replication waits a cancellation path. Cancellation must not manufacture durability progress to unblock a waiter.

Preserve direct `recv()` into the reserved BLog for the normal receive path. Pass lifetime-safe reservation handles/views across component boundaries, not mandatory copied payload vectors. Validate once before publication, including coherent and ORDER0 paths; measure the newly required validation cost separately from extraction. No new per-message allocation, payload copy, virtual call, global lock, or fence solely to satisfy an abstraction boundary without evidence and review.

**Topics:** fix hashing together with collision handling and shared GOI/CV ownership. A better hash does not establish multi-topic isolation. If ownership remains unresolved, enforce and document a single-topic supported mode rather than silently accepting unsafe configurations.

These are intentional behavior fixes and should have failing-before/passing-after tests. F9's harmful scheduling scenario and F15's live ring-wrap failure were not exercised end-to-end in the original review; turn those source-level findings into production-path regressions before claiming reproduction.

### PR 09–11: component boundaries and repository organization

Repair LazyLog reconnection with a bounded overall deadline and explicit transport readiness policy. Resolve Scalog memory-mode RPC task ownership so every accepted task has a consumer or receives an explicit unsupported-mode error. Keep baselines optional build targets, with their tests required whenever enabled.

Target boundaries, introduced incrementally:

| Component | Responsibility | Boundary rule |
|---|---|---|
| Configuration/layout | Effective immutable settings, region geometry and compatibility | No runtime worker mutation |
| Shared-region backend | Mapping, attachment, publication/observation primitives | Explicit DRAM/CXL semantics; no duplicated ordering algorithm |
| Storage | Segment allocation, reservation, lifetime, reclamation | No raw reuse without established ownership |
| Ordering/session core | Admission, classification, holding, fencing, commit decisions | Testable state transitions; publication remains an explicit operation |
| Replication/persistence | Replica progress and sink completion | ACK2 tied to the documented sink contract |
| Transport | Framing, sockets, connection lifecycle, bounded queues | Validated owned messages/handles cross the boundary |
| Client | Session state, retransmission, buffers, cumulative ACK handling | Explicit buffer ownership and bounded retention |
| Runtime/benchmarks | Lifecycle wiring and experiment orchestration | No protocol decisions hidden in shell scripts |

Use narrow C++ targets and ownership types; avoid a universal abstraction layer or virtual dispatch on every hot-path access. Keep DRAM and CXL on the same ordering logic. Abstract cache operations at the existing publication boundary, with injectable observation/fault hooks for tests. Such hooks test protocol behavior, not physical CXL cache coherence.

Extract one subsystem at a time from `topic.cc`, network management, and CXL management. First characterize behavior, then extract, then optimize in a separate change. Replay identical inputs and compare commit identities, order, ACK frontiers, and reclamation events. Differential equivalence must not preserve known unsafe behavior: correct that behavior first with an explicit contract test.

Inventory existing attempted abstractions before reuse: `SegmentManager` and `BufferManager` are not the linked production allocation path. Extract the tested path rather than assuming similarly named code is authoritative. Verify a minimal client build before declaring baseline dependencies optional; generated baseline headers/libraries currently cross component boundaries.

After boundaries stabilize, consolidate `src/` by component, tests by unit/integration/hardware, shared launch code under `tools/`, thin compatibility wrappers under `scripts/`, and supported configuration profiles under `config/`. Preserve current paper sources and historical evaluation inputs. Archive obsolete experiments with provenance rather than deleting evidence. Update README/ARTIFACT links against the user's current documentation changes instead of overwriting them.

## 3. Validation and performance gates

| Layer | Required checks | Resource policy |
|---|---|---|
| Unit/component | Configuration race, geometry overflow, capacity, ring wrap, framing, ownership, cancellation | Small capacities through production implementations; no 64 GiB allocation in ordinary CI |
| DRAM integration | One/three brokers, FIFO and duplicate audit, fragmented connections, client reconnect, stalled reader/replica, exhaustion, orderly and interrupted shutdown | Isolated 64 GiB region; short bounded runs; explicit emulation |
| Persistence | ACK2 versus actual disk completion, disk errors, cancellation; restart claims only where implemented | Separate disk and memory-sink results; unique output directories |
| Formal/fault injection | Selected bounded models plus implementation tests for modeled interleavings | Record model scope and assumptions; no claim that model success proves implementation correctness |
| Local regression | Fixed CPU budgets, node placement, message/batch sizes, broker counts, ordering, ACK, sink, duration | Release build; exclusive measured run; no sanitizers |
| Real hardware | Real CXL mapping, flush/publication behavior, reconnect reads, layout/cache-line ownership, matched performance matrix | Deferred until hardware returns; distinct artifact namespace |

The prior review ran 31 non-E2E CTest targets: 29 passed and two failed (F19/F20). Registered E2E tests were withheld because of unsafe cleanup. Start from that evidence, rerun after build/runner repair, and do not reinterpret the original results as a clean release gate.

For initial performance characterization, take at least five measured repetitions after warmup and record both median and variability, plus latency percentiles where measured. Define the exact measurement boundary: startup/mapping initialization is separate from steady publication. Record offered load, completed and acknowledged bytes, subscriber audit results, retransmissions, CPU utilization, and whether the working set crossed a segment or ring boundary. A correctness failure invalidates a throughput result.

Until safe reclamation exists, use **bounded finite-transfer** measurements; count warmup, wire overhead, pending receives, and retries against the same allocation budget. The 64 GiB region has less than 32 GiB available for payload after GOI and other metadata. At a hypothetical 12 GiB/s this is under 2.67 seconds of storage, so conventional long warmup plus steady-state runs cannot fit. Never silently enable unsafe reuse to extend a benchmark. The 32 MiB smoke workload is not a performance test.

Default PBR geometry is 81,920 slots; at nominal 2 MiB batches a wrap represents about 160 GiB per broker. Normal short DRAM tests cannot reach this boundary after reclamation containment. Use reduced-capacity production-path fixtures for ring/GOI wrap and exhaustion, and keep production geometry in throughput comparisons.

Use matched before/after runs on a validated path. As an initial investigation trigger, flag a repeated median throughput decrease above 5% or p99 increase above 10%; calibrate these thresholds against observed host noise before enforcing them automatically. Fixed payload, batch size, CPU count, affinity, sink, and runtime parameters are prerequisites. Compare local DRAM only with local DRAM. Keep sanitizer timing out of performance results.

Interleave paired baseline/candidate runs and include phase-level evidence: cycles per batch, reservation contention, scanner progress, page faults, retransmission rate, and achieved load. Exercise both large-batch bandwidth and small-message/partial-batch cases, at fixed client pool/credit/RTO settings. Sample timing outside peak runs when instrumentation changes the path. A client- or loopback-limited throughput plateau cannot establish server nonregression. Hold load generation fixed and report its own saturation.

Compare latency at the same offered load, separately from maximum-throughput comparisons. Preserve batched order/GOI reservations rather than replacing them with per-message atomics. Record whether optional non-temporal ingest staging is enabled. Distinguish memory-accounting, memory-copy, and disk sinks, and preserve ACK-specific client retention policies: disk ACK2 retransmission copies and ACK1 pool pinning have different resource behavior. Include actual client pool allocation and replication buffers in memory preflight; the nominal 32 MiB workload can still allocate approximately 512 MiB of client pool with current default slot geometry.

On this dual EPYC 9754 host, node binding alone leaves 256 logical CPUs per socket eligible. Record physical core sets, SMT siblings, and shared L3 groups; use stable physical-core budgets and a separate SMT sensitivity run. Preserve the requested node-0 clients/node-1 brokers as the main profile. Local same-socket clients on disjoint cores and remote clients over the node-1 100 Gb/s NIC are diagnostic controls, not replacements or pooled results. Record the actual route/interface and receiver/softirq placement when diagnosing differences.

Record actual client HugeTLB/THP/base-page outcomes, shared-region page sizes, zero/prefault mode, and post-touch placement. At review time both nodes had zero free 2 MiB HugeTLB pages, THP was `madvise`, and tmpfs THP was `never`; allocator intent is not proof of hugepage use. Freeze these conditions across paired runs without hidden global tuning.

Short runs establish smoke/regression coverage, not sustainable throughput. Add long-duration rollover/reclamation and bounded-memory soak tests after PRs 04–06 establish safe behavior. A repeated flaky pass rate such as “7/8 succeeds” is not a correctness acceptance criterion, even if older experiment briefs used it.

Every run manifest should capture commit and dirty-tree identity, binary/build flags, dependency versions, effective configuration, profile/backend, region/layout identity, sink, ports, CPU and memory policy, observed placement, kernel/CPU details, workload/ACK/replication parameters, timestamps, exit status, and audit results. Mark all current performance artifacts `DRAM_EMULATION` and local or remote explicitly. Do not merge them into historical CXL figure inputs.

## 4. Release scope and completion criteria

The first release is a documented, bounded **single-host research prototype** with a reliable DRAM developer path and a separately qualified CXL path. Supported order/ACK/sink combinations must be enumerated and tested. Explain session lifetime, retained-client-suffix assumptions, resource exhaustion, persistence limitations, and failure behavior in the public API documentation.

Release requires resolved or explicitly disabled unsafe supported paths, deterministic cleanup, green supported tests, a clean-machine dependency/bootstrap check, license and third-party provenance review, contribution/security guidance, and a reproducible small example. Do not present production sequencer failover, physical fencing, arbitrary multi-host cache coherence, or crash-stable delivery as completed by this refactor.

When CXL returns, rerun hardware-specific correctness checks before collecting final performance numbers. DRAM validation cannot close the paper/implementation gaps around noncoherent observations, cache-line ownership, or hardware failure. Multi-host failover remains a separate research and engineering milestone.

**First implementation tranche:** PRs 01–03, followed immediately by unsafe reclamation containment and the PBR/GOI fixes. Deliver an isolated local DRAM smoke test, trustworthy instrumented builds, and validated immutable configuration before undertaking broad file organization.
