**Embarcadero: research, implementation, and open-source readiness review — 2026-09-22**

Reviewed source baseline: `cd62a6bcbfd8618210641c5841dc4017845e84c8`, with the current working-tree documentation. Existing modifications to `README.md` and untracked artifact/session-skew files were preserved. This review adds documentation; it does not refactor the implementation or modify paper evidence.

**Assessment**

The repository contains a substantial research implementation and useful correctness machinery, but I would not release it as a supported durable logging service in its current state. The immediate problems include unsafe payload reclamation, unchecked metadata capacity, a reproduced data race, and unsafe test orchestration. These take priority over directory cleanup. An explicitly scoped research release is a reasonable next milestone once licensing, repeatable build/test instructions, and the critical implementation defects are addressed. Production multi-host support is a separate milestone.

The current paper is substantially more precise than the SOSP draft. Its main contribution is **per-session prefix FIFO for concurrently striped, ingest-first appends**, supported by session-local holding, retransmission, fencing, and retained-suffix replay. Passive CXL supports metadata coordination and payload accessibility. The ordering rule is not itself CXL-specific. ACK1 establishes speculative order in the active term; ACK2 additionally requires the configured persistence contract. The evaluated deployment co-locates CXL-accessing processes on one host. Replicated metadata, sequencer replacement, leadership/physical fencing, and crash-stable multi-host delivery remain design/model extensions.

I read the current main-paper sources and supplementary verification/baseline material under `Paper/Text/`. `Paper/SOSP_Text/` contains `Reviews.tex`; the older manuscript is under `Paper/Text-Old-SOSP/`, which was also used for comparison. The older reviews' concerns about failure semantics, realistic comparisons, and the public log API remain useful release criteria. They should not be used to attribute the older, broader claims to the revised paper.

**Review method and confidence**

Four review passes covered the paper/protocol, runtime/replication, build/tests/release, and architecture/configuration. Findings below distinguish executed reproductions, direct source traces, and design gaps. Fresh builds used temporary output directories and the existing gRPC source cache. They did not use the existing compiled broker binaries. No shared broker cluster, privileged setup, physical CXL failure, or paper performance campaign was run.

P1 means fix before recommending the affected supported path to external users. P2 means a significant correctness, assurance, or maintenance issue to address in the first hardening series. These are engineering priorities, not CVSS ratings. A source trace establishes the defective control flow; it does not imply that every possible end-to-end consequence was reproduced.

**Priority findings**

| ID | Priority | Finding | Evidence |
|---|---|---|---|
| F1 | P1 | GC returns payload segments while outstanding users can still reference them | Independently traced production call chain |
| F2 | P1 | GOI reservations can exceed the reserved region | Writer/layout source trace |
| F3 | P1 | Session-table exhaustion silently drops required session state | Admission/publication/GC source trace |
| F4 | P1 | Configuration memoization has a C++ data race | ThreadSanitizer reproduction |
| F5 | P1 | Ordinary topic names collide deterministically | Extracted production hash reproduction |
| F6 | P1 | Replication shutdown can wait forever on a missing predecessor | Fresh production-code reproduction |
| F7 | P1 | Session negotiation mistakes a fragmented TCP header for absent negotiation | Receive-path source trace |
| F8 | P1 | Default CTest can kill unrelated brokers | Registered-test/script source trace |
| F9 | P1 | Commit-time session guard cannot perform its first rejection | Exhaustive counter/call-site trace; harmful scheduling scenario not executed |
| F10 | P2 | CMake discards requested sanitizer flags | Fresh configure and generated compile-command inspection |
| F11 | P2 | E2E tests execute the wrong build directory | All three registered E2E scripts inspected |
| F12 | P2 | String configuration loading is a successful no-op; invalid geometry passes validation | Fresh configuration harness |
| F13 | P2 | Hold-depth accounting never returns to zero after ordinary release | All aggregate updates and both release paths inspected |
| F14 | P2 | Older heartbeat replies can regress membership | Version-check source trace |
| F15 | P1 | PBR admission mixes absolute and modulo positions and stalls at wrap | Independently checked source and bounded arithmetic trace |
| F16 | P1 | Ingest does not enforce a bounded, valid batch before publication | Allocation/validation/publication source trace |
| F17 | P2 | ACK-thread topic pointer can outlive the request stack | Thread argument/lifetime source trace |
| F18 | P1 | Network shutdown can block inserting sentinels into a full queue | Queue/worker shutdown source trace |
| F19 | P2 | LazyLog retries finish before transport reconnection | Fresh failing test; same case failed three further repetitions |
| F20 | P2 | Scalog memory-mode RPC queues tasks without consumers | Fresh failing test and sink/worker source trace |

**F1 — Reclamation does not establish that payloads are no longer needed.**

In [topic.cc](../../src/embarlet/topic.cc), lines 1604–1618, `MaybeGCRetiredSegments()` frees the oldest segment whenever more than two retired segments exist. Successful allocation calls it at line 2819. The callback is actually installed in [topic_manager.cc](../../src/embarlet/topic_manager.cc), lines 216–217 and 479–480, and [cxl_manager.cc](../../src/cxl_manager/cxl_manager.cc), lines 941–950, makes the segment available by clearing its allocation bit. The comment suggesting that freeing is not wired is stale.

When a broker enters its fourth segment, an old segment can still contain an in-flight receive, held descriptor, unread subscriber payload, or unpersisted replica payload. No such reference or frontier is checked. Reuse changes the bytes behind existing GOI/PBR offsets; the GOI does not validate a segment generation. A delayed subscriber can receive different data under an old identity, and a delayed replica can persist it. This is a current single-host defect. It contradicts the paper's stated reclamation minimum over replicas and subscriber cursors.

Repair: establish segment ownership/pins for receives and unpublished descriptors, plus explicit replica/subscriber retention watermarks. Until those conditions are implemented, fail admission on exhaustion instead of reclaiming live data. Generation validation should detect stale references, but does not replace retention. Regression: tiny segments; pause a reader, replica, receive, and held batch in separate cases; roll and reuse repeatedly; require unchanged payload identity or producer backpressure.

**F2 — GOI capacity is not checked before publication.**

[topic.cc](../../src/embarlet/topic.cc), line 5207, reserves an epoch with an unbounded `global_batch_seq_.fetch_add`; lines 5244–5245 directly index `goi[batch_index]`. [baseline_cxl_layout.h](../../src/cxl_manager/baseline_cxl_layout.h), lines 19 and 42–48, reserve 268,435,456 entries and place the mailbox after that region. The live writer has no capacity check or safe generation rotation. If cumulative reservations reach that limit, publication writes beyond its region; other admission defects such as F15 may halt a particular configuration earlier. A startup/recovery bounds check does not protect a running writer.

Repair: reserve only a complete fitting epoch with overflow-safe arithmetic, and return explicit capacity/backpressure before publication. Do not substitute modulo indexing without a safe reuse protocol. Regression: injectable small capacity, adjacent canary region, exact-full and epoch-straddling cases. No giant allocation or long benchmark is necessary.

**F3 — Session capacity failure is silently accepted.**

The session table contains 4,096 entries ([cxl_datastructure.h](../../src/cxl_manager/cxl_datastructure.h), line 225). `FindOrCreateSessionEntry()` returns null when full; `PublishSessionEntry()` silently returns while ordering continues ([topic.cc](../../src/embarlet/topic.cc), lines 391–445). SessionOpen can acknowledge a missing entry as OK ([network_manager.cc](../../src/network_manager/network_manager.cc), lines 1133–1148). Reopening consumes new `(client_id, session_epoch)` entries; there is no safe table reclamation path.

This definitely loses required shared session state for later generations. After volatile session/dedup state is also evicted, the implementation can no longer reconstruct that generation's expected sequence or terminal fence from the session table. The duplicate trace requires an overflow generation created after startup recovery, eventual volatile GC, and replay through a broker without the original ingress-dedup entry. Physical batch-ID dedup does not cover it: the new ingress assigns a new physical batch ID. This trace was not demonstrated in a live broker run. The reconnect code also bases `has_committed_prefix` on the missing session-table `expected_seq`, even when a GOI scan finds history; returning a wire hint does not restore the sequencer's missing state.

Repair: reserve session capacity before accepting a generation, propagate resource exhaustion, and define retention for generation tombstones. Session publication failure must not be invisible to the commit path. Regression: tiny table; exceed capacity through session churn; reconnect, evict volatile state, and replay an old generation; require explicit rejection or preserved HWM/dedup/fence semantics.

**F4 — The configuration cache's “benign race” is undefined behavior.**

[configuration.h](../../src/common/configuration.h), lines 42–52, lets two cold readers both write non-atomic `cached_`. Release/acquire on `resolved_` does not provide exclusive initialization. Identical values do not make concurrent C++ writes safe; string-valued configurations are especially problematic.

A two-thread harness using the actual header, with a synchronized environment-resolution test seam, produced `ThreadSanitizer: data race ... configuration.h:49`. It used GCC with `-fsanitize=thread -no-pie`. The initial PIE executable failed in the sanitizer runtime with an unexpected mapping; the non-PIE run reported the actual race.

Repair: parse and resolve configuration once before starting workers, validate it, and publish an immutable configuration object. If lazy resolution remains, use proper once/mutex synchronization with an explicit reset contract. Regression: simultaneous cold reads under TSan, including a string value; separately test pre-start configuration updates.

**F5 — Padded topic names all select slot zero.**

[cxl_manager.cc](../../src/cxl_manager/cxl_manager.cc), lines 715–721, repeatedly multiplies an `unsigned int` hash by `TOPIC_NAME_SIZE`, currently 256, over all 256 name bytes. Only the final four bytes survive 32-bit overflow. Normal zero-padded names end in zero bytes, so they hash to zero. The head RPC creates exactly such padded names ([heartbeat.cc](../../src/embarlet/heartbeat.cc), lines 405–407).

Executing the extracted function with `orders`, `payments`, `audit`, `topic1`, and `topic2` returned zero for every name. The collision check in [topic_manager.cc](../../src/embarlet/topic_manager.cc), lines 356–359, rejects the next distinct topic, despite the configured multi-topic capacity. Merely choosing a better hash is insufficient: legitimate collisions still require consistent collision resolution.

Repair: use bounded names and a persistent topic directory with collision handling and identity validation at every lookup. Regression: create/read multiple ordinary names and deliberately colliding names, and attach through followers. Before promising multiple ORDER5 topics, also audit ownership of the globally shared GOI/CV; fixing this hash alone is not proof of multi-topic isolation.

**F6 — Shutdown depends on failed replicas making progress.**

[chain_replication.cc](../../src/disk_manager/chain_replication.cc), lines 618–655, waits for the predecessor token without a stop condition. Lines 795–805 require pending work to drain before workers receive termination. With a missing predecessor, that cannot happen.

A fresh harness linked the production replication implementation, started an RF=2 tail with one published GOI entry and no head completion, and called `Stop()`. `timeout 3s` exited 124; `STOP_RETURNED` was never reached. This used the explicit in-memory accounting sink and private fixture state, not a running cluster. Persistence-error paths need the same bounded shutdown treatment.

Repair: separate admission stop, bounded drain, cancellation, worker termination, and join. A stop/error must not fabricate replication tokens or durable ACKs. Regression: actual manager with missing predecessor and injected persistence failure; bound shutdown and assert no false durability advancement. The existing chain-pipeline model test does not exercise this production lifecycle.

**F7 — A valid fragmented SessionOpen can be misclassified.**

[network_manager.cc](../../src/network_manager/network_manager.cc), lines 289–315, performs one `recv(..., MSG_PEEK)` for an eight-byte header. Any short read becomes “no SessionOpen.” TCP can deliver a valid header in fragments; after a short peek the control bytes remain queued, and the caller proceeds toward batch parsing while the session-aware client waits for the negotiation ACK.

Repair: implement incremental, deadline-bounded protocol framing; distinguish incomplete, absent/legacy, malformed, closed, and complete input. Prefer explicit protocol/version negotiation over timeout-based detection. Regression: every header split boundary, fragmented payload, slow valid handshake, and close midway through negotiation. This finding is a source-level protocol defect, not a claim that a full client/broker reproduction was run here.

**F8 — The documented test command is unsafe around other experiments.**

[test/CMakeLists.txt](../../test/CMakeLists.txt), lines 21–25, registers `e2e_explicit_replication_order5_ack2` in ordinary CTest. Its [script](../../test/e2e/test_explicit_replication_order5_ack2.sh), lines 60, 181, and 561, uses broad `pkill -f embarlet`/`pkill -9 -f embarlet`. That can terminate unrelated services or experiments owned by the same user. The surrounding CMake comment explicitly retires another test for this exact behavior.

Repair: own exact child PIDs/process groups; allocate unique ports, temporary paths, and shared-memory names; opt into hardware/cluster tests; use CTest resource locks where isolation is impossible. Regression: keep an unrelated sentinel process alive throughout startup failure, test failure, timeout, and cleanup. These E2E scripts were not executed during this review.

**F9 — The commit-time fence guard is unreachable.**

[topic.cc](../../src/embarlet/topic.cc), lines 5153–5156, calls the rejection predicate only if `order5_spatial_guard_rejects_ > 0`. The counter starts at zero, and its only increment is inside that predicate at line 5128. Therefore the first rejection can never occur. The code equates “no rejection yet” with “no fencing has occurred,” which is invalid.

The unexecuted harmful scheduling case is classification of a ready batch followed by a same-session fence before commit; a regular publication snapshot can then clear the persistent fence flag. The exact application-visible trace needs a production fixture with an injected clock/pause. It should not be presented as an observed end-to-end failure.

Repair: gate on authoritative fencing state or always check; make a generation's terminal fence monotonic; define publication and HWM visibility at the fence boundary. Regression: first-ever fence between classification and publication, followed by volatile-state eviction and replay. A test of the helper predicate alone cannot catch the broken call-site gate.

**F10 — Sanitizer builds can silently be ordinary optimized builds.**

[CMakeLists.txt](../../CMakeLists.txt), line 16, replaces `CMAKE_CXX_FLAGS` with `-Wall -O3`. A fresh configure with `-DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_FLAGS='-fsanitize=address -fno-omit-frame-pointer'` succeeded, but generated compilation commands contained neither requested instrumentation flag and retained `-O3`.

Repair: use target-scoped warning settings, normal build-type optimization, and explicit sanitizer compile/link options or interface targets. Check actual compile/link commands in CI. Make native-ISA benchmark builds explicit; `src/CMakeLists.txt` currently also adds `-march=native`.

**F11 — E2E tests ignore the configured binary directory.**

All three registered scripts set their binary directory to the source tree's `build`: [explicit replication](../../test/e2e/test_explicit_replication_order5_ack2.sh), line 10; [session fence](../../test/e2e/test_order5_live_session_fence_fire.sh), line 6; [unacked release](../../test/e2e/test_order5_multibroker_unacked_release.sh), line 6. Setting CTest's working directory does not change these assignments. An out-of-tree or sanitizer test run can therefore execute stale binaries.

Repair: pass exact CMake target paths/configured build paths to fixtures and honor overrides. Regression: configure outside the source tree with no source-tree `build`, and verify the executable identity recorded by the test.

**F12 — Configuration APIs accept settings that they do not apply or cannot safely use.**

[configuration.cc](../../src/common/configuration.cc), lines 325–330, parses a YAML string but never maps it into `config_`. A fresh linked harness called it with broker port 2222: it returned true and left the port at 1214. The same harness set `max_brokers=33`, `segment_size=0`, `batch_size=0`, and `batch_headers_size=0`; `validate()` still returned true. Allocation code divides by segment size, while fixed broker arrays use a compile-time maximum of 32. CLI overrides are applied after file validation without final validation ([embarlet.cc](../../src/embarlet/embarlet.cc), lines 201–217).

Repair: one parser for file/string input, one precedence rule, checked sizes/alignment/products, bounded broker IDs, nonzero capacities, layout-fit validation, and one final validation before side effects. Regression: valid override equivalence plus zero, negative, overflow, unknown-key, and incompatible-layout cases. Distinguish parsing errors from unsupported configurations.

**F13 — Repaired gaps leave the fast-seal state disabled.**

[topic.cc](../../src/embarlet/topic.cc), lines 7418–7419 and 7530–7531, decrement shard hold size during normal release but omit the aggregate decrement corresponding to line 7125's increment. Lines 6259–6261 derive steady state from that aggregate. One ordinary repaired gap can leave it positive even when the hold maps are empty, disabling early sealing and distorting instrumentation.

Repair: centralize all hold removal bookkeeping. Regression: hold a successor, release its predecessor, and verify both counts return to zero and fast sealing becomes eligible; repeat across shards and duplicate/fence removal paths.

**F14 — Membership versions are allowed to go backward.**

[heartbeat.cc](../../src/embarlet/heartbeat.cc), lines 769–800, returns immediately for empty replies, then checks `new_version <= cluster_version_ && reply.cluster_info_size() == 0`. That condition is impossible for the remaining nonempty replies. An older delayed response can replace a newer membership snapshot and lower the local version.

Repair: reject old versions independently of snapshot size, and define equal-version initialization behavior explicitly. Regression: apply version 10, delayed version 9, and duplicate version 10; membership must remain at the accepted newest snapshot.

**F15 — The PBR consumer loses its generation at wrap.**

[topic.cc](../../src/embarlet/topic.cc), lines 2838–2844, converts the consumer's modulo byte position to a slot number, mapping the end-of-ring sentinel to zero. Lines 2987–3001 compare that value with `next_slot_seq`, which only increases. The consumer publishes modulo positions at lines 8599–8606. The lock-free path is selected on the supported x86 build when CMPXCHG16B is available (line 644).

After a completely drained ring wraps, producer sequence `N` is compared with consumer sequence zero, yielding `N` supposedly in-flight slots. Admission refuses further work despite an empty ring. An independently executed four-slot arithmetic trace accepted four individually drained batches, then permanently rejected the fifth. This was a bounded reproduction of the production expressions, not a live broker wrap test. It is also a practical earlier capacity boundary than the very large GOI limit in many configurations.

Repair: publish an absolute consumer sequence/generation and compare compatible sequence domains. Preserve release/publication ordering and define the initial sentinel independently of later wraps. Regression: small real production ring; interleave admission and consumption through several wraps, with a held slot and delayed consumer variants; verify bounded occupancy, progress, and no ABA reuse.

**F16 — Batch validation is neither bounded nor a prerequisite for publication.**

There are two related ingest-boundary defects in [network_manager.cc](../../src/network_manager/network_manager.cc). Before allocation, lines 1374–1378 only reject zero `total_size`; several drain/staging paths resize to that unrestricted size. [topic.cc](../../src/embarlet/topic.cc), lines 2735–2739 and 2807–2820, uses unchecked alignment and address-plus-size arithmetic. An invalid length can exceed capacity, wrap reservation arithmetic, or cause an uncaught allocation failure.

Separately, body-validation loops at network lines 1854–1868 and 1879–1895 merely log and break their inner loop on an invalid message. Execution continues to set `kBatchHeaderFlagValid` and publish at lines 1902–1915. Validation is also tied to the cache-flush branch and can be skipped on coherent mappings. Therefore receiving the declared byte count does not establish that the batch contains the declared valid messages.

Repair: define bounded fixed-width wire lengths, use checked arithmetic and fixed-size discard buffers, and produce a validated descriptor before publication. Reject the whole batch on any count/stride/header inconsistency; validation must not depend on coherence or performance mode. Regression: production codec/allocator helpers at size/alignment limits and malformed/truncated bodies, both header versions and coherence modes; no admission cursor, GOI, or ACK advance for rejected input. These findings are source traces; no oversized allocation or malformed-input broker campaign was run.

**F17 — The ACK thread receives a borrowed stack pointer.**

[network_manager.cc](../../src/network_manager/network_manager.cc), lines 1274–1276, passes `handshake.topic` through the thread forwarding helper in [network_manager.h](../../src/network_manager/network_manager.h), lines 98–108. The array decays to a pointer into the receive worker's stack. Copying it into a string inside `AckThread()` at lines 2846–2857 is too late if the child starts after the request exits or its stack is reused.

Repair: construct an owned `std::string` at the spawn site and accept it by value. Regression: delay child execution until after request-scope destruction, then check the retained topic under ASan. The lifetime defect is statically established; its scheduling-dependent failure was not dynamically reproduced here.

**F18 — Full-queue shutdown has no guaranteed consumer.**

[network_manager.cc](../../src/network_manager/network_manager.cc), line 861, sets the stop flag, then lines 873–876 perform blocking sentinel writes into the bounded request queue. Workers finishing their handlers exit on the stop flag (lines 1028–1030), so they need not drain queued requests. If all workers have long-lived connections and the queue is full, shutdown itself blocks inserting a sentinel. The acceptor can independently block in `EnqueueRequest()` at line 903.

Repair: cancellable admission and an explicit close/drain-or-cancel queue protocol, with owned descriptors and listener shutdown before joins. Regression: tiny queue, occupy workers, fill pending connections, and request shutdown; require bounded completion and no leaked descriptors. This is distinct from the reproduced replication hang in F6; its full NetworkManager schedule was not executed.

**F19 — LazyLog's application retry loop does not wait for channel recovery.**

Fresh `unit_lazylog_metadata_replica` failed `RetriesTransportFailureUntilReplicaStarts` ([test](../../test/lazylog_metadata_replica_test.cc), line 226); the specific case then failed three more repetitions. The replica starts after 25 ms, but the client exhausts four fail-fast attempts in roughly 302 ms.

[lazylog_metadata_replica.cc](../../src/cxl_manager/lazylog_metadata_replica.cc), lines 286–296 and 324–339, caches a channel and retries without wait-for-ready. The checked gRPC v1.55.1 source uses a default initial reconnection backoff of one second; these application attempts can finish before transport reconnects. Repair: one bounded overall deadline with a deliberate channel-readiness/reconnect policy. Preserve the delayed-start regression and add an unavailable-through-deadline case.

**F20 — Scalog's memory-mode RPC has no writer, and its durability test selects that mode.**

Fresh `integration_scalog_replication_rpc_durability` failed both cases on RPC deadlines. [scalog_replication_manager.cc](../../src/disk_manager/scalog_replication_manager.cc), lines 334–354, starts writer threads only for disk mode, but `Replicate()` at lines 448–484 unconditionally queues a write and waits for a completion that only the writer fulfills. The CXL polling memory-copy path is separate and is not implicated by this particular defect.

The [durability tests](../../test/scalog_replication_rpc_durability_test.cc), lines 39 and 59, explicitly select `log_to_memory=true`, so they also cannot validate `fdatasync`. Repair: implement the supported memory RPC sink or reject that combination immediately; run the disk-durability checks with isolated disk-mode files, and add a separate memory-mode progress check. Do not repair this by acknowledging queue admission as media persistence.

**Paper-to-code boundary and research assessment**

| Contract / claim | Assessment | Required follow-through |
|---|---|---|
| Session-local hold, repair, fence, replay | Implemented substantial path, with finite-capacity and publication-boundary defects above | Drive production state machines with deterministic histories; preserve operation identities through replay |
| Order separate from persistence | Clear in the revised paper; explicit DRAM sinks are labeled non-durable | Keep ACK tier and sink/failure domain explicit in API, metrics, configs, and artifacts |
| Safe payload reclamation | Current code contradicts the paper's frontier rule | F1 is a correctness prerequisite for sustained operation |
| Single writer per metadata cache line | Not fully implemented for a multi-host backend | Split ownership domains and validate field offsets, not just struct size |
| Stable durable subscriber | Full-design behavior; evaluated subscriber follows order visibility | Publish this distinction in public API docs and examples |
| Sequencer replacement / module loss / physical fencing | Explicitly unimplemented in evaluated prototype | Do not treat their absence as a surprising defect; reject unsupported modes and preserve the limitation |
| TLA+ safety | Useful bounded specification evidence | Add implementation correspondence and targeted negative controls; do not infer liveness or C++ memory safety |
| Performance comparisons | Revised paper labels topology, sink, ACK contract, and port limitations | Preserve those labels in every reproduction command and result |

The Completion Vector is a specific unresolved mismatch: [cxl_datastructure.h](../../src/cxl_manager/cxl_datastructure.h), lines 204–220, places the sequencer-owned and tail-owned counters at byte offsets 8 and 16 in the same cache line. The paper says they are cache-line-disjoint. Padding the entire struct to 128 bytes does not separate its internal writers. Independent atomic updates plus flushes cannot establish the paper's single-writer argument on a non-coherent multi-host mapping. Co-located hardware coherence masks this issue. Treat it as a blocker for that future backend, not evidence that the measured single-host execution necessarily fails.

Likewise, the reconnect GOI scan in [network_manager.cc](../../src/network_manager/network_manager.cc), lines 179–185, refreshes only the first line before reading generation/sequence fields on the second. Consolidate GOI reading behind the existing fresh-entry accessor and test stale second-line observations before extending the deployment claim.

The production replica lifecycle also needs explicit public documentation: [chain_replication.cc](../../src/disk_manager/chain_replication.cc), lines 355–366, opens its benchmark replica files with `O_TRUNC`. Its manager does not implement a production checkpoint/replay path. The passing media-restart test uses test-side persistence/replay functions, so it does not establish that restarting this manager preserves existing replica files. This is consistent with the paper's narrow prototype recovery boundary, but a reusable release should refuse accidental reuse of an existing replica directory until its restart contract is implemented.

The model's publication transition combines changes that the C++ implementation spreads across classification, fencing, GOI writes, session publication, and frontiers. The model also abstracts generation/replay details and does not model these C++ cache-line operations. More states in the same abstraction will not detect F1, F4, or a disabled F9 call site. Add a traceability table from each invariant to model actions, production functions, and regression histories.

The most valuable additional experiments are sustained small-capacity reuse with stalled consumers; durable sticky-versus-striped single-session scaling; sequencer CPU/decision-rate and metadata bandwidth measurements; and end-to-end latency including batch formation and apply. The revised paper correctly acknowledges that its headline transfers are finite, its durable configuration does not demonstrate an ingress placement bottleneck, and the Corfu comparison measures a particular gRPC port. Preserve those qualifications. The reported 1.366 versus 1.295 GB/s durable results support near parity under that configuration; the stronger contribution is the ordering contract and evidence about where its costs occur.

**Architecture and maintainability**

The tracked `src/` C++ inventory is 56,205 physical lines across 138 files, including comments, baselines, and tools. This is not directly comparable to the paper's 18,900-line claim without defining its counting scope. `topic.cc` has 8,707 lines; `publisher.cc` 4,788; `network_manager.cc` 3,413; `subscriber.cc` 3,243. There are 216 source lines containing environment lookups. These counts are review-surface indicators, not quality scores.

`Topic` owns allocation, ordering policies, session state, publication, export, recovery, and extensive instrumentation. Public client headers expose benchmark failure injection and result-writing concerns; `Poll()` joins publishing workers, while `WaitUntilAcked()` provides a different lifecycle. Client sources are compiled repeatedly into applications instead of exported through a supported library. Experimental extracted managers remain beside the mainline implementation; some include a nonexistent `common/common.h` and are not compiled into the broker. Building a new abstraction on these dormant classes without auditing them would preserve confusion.

The documentation currently has competing authorities. The README points at `EMBARCADERO_DEFINITIVE_DESIGN.md`, whose line 467 calls `committed_seq` durable and whose older atomic/coordination descriptions differ from the revised paper. `test/README.md` says integration/property tests do not exist, despite the configured inventory. Archive historical design notes explicitly and make the supported contract and architecture guide the entry points.

Recommended boundaries, extracted incrementally:

| Component | Responsibility | Verification seam |
|---|---|---|
| Configuration | Parse, resolve, validate, freeze effective settings | Pure parser/schema tests; print effective configuration |
| Session ordering core | Expected sequence, hold, dedup, fence, replay decisions | Injected clock and deterministic/generated operation histories |
| Publication | GOI reservation, readiness, session/frontier publication | Small capacities, controlled interleavings, canary boundaries |
| Segmented payload store | Allocation, pinning, retention, generation, reclamation | Delayed readers/replicas/receives and wrap/reuse tests |
| CXL backend | Mapping, offsets, cache operations, hardware capabilities | Coherent emulation and separately qualified CXL implementation |
| Replication | Persistence outcomes, tokens, durable frontier, cancellation | Actual production manager with failing/slow sink |
| Transport | Versioned framing, bounded parsing, connection lifecycle | Fragmentation, disconnect, framing fuzz tests |
| Client SDK | Stable append/completion/subscription API and ownership | Consumer example built against installed/exported library |
| Experiments | Baselines, fault campaigns, plotting, benchmark knobs | Separate optional targets and immutable manifests |

Prefer value types for session generations, message order, batch/GOI positions, and CXL offsets. Do not interchange them as anonymous `size_t` values. Keep host-memory synchronization distinct from CXL publication operations. Use RAII for socket/mapping/thread lifetimes, bounded cancellation, and explicit errors for capacity and persistence failures. Avoid imposing a virtual call on every record merely to obtain testability; pure decision logic and injectable coarse-grained backends provide useful seams without that cost.

**Assessment against current engineering practice**

This is an evidence-based local assessment, not a formal certification or a remotely executed Scorecard scan. The reference criteria are the [OpenSSF Best Practices passing requirements](https://www.bestpractices.dev/en/criteria/0), [OpenSSF Scorecard checks](https://github.com/ossf/scorecard/blob/main/docs/checks.md), and [C++ Core Guidelines](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines).

| Area | Current state | Release target |
|---|---|---|
| License and contribution ownership | CONTRIBUTING exists; no tracked project LICENSE/COPYING/NOTICE | Owner-approved license, dependency notices, contribution policy |
| Build reproducibility | Fresh configure works on this provisioned host; dependencies mix tags and host installations | Supported OS/toolchain matrix, immutable dependency identities, clean-environment build |
| Test automation | Useful CTest/GTest coverage and formal scenarios; no tracked CI workflow | Required portable CI; explicit isolated integration/hardware tiers |
| Analysis | Warnings enabled; requested instrumentation can be discarded | Verified ASan/UBSan, targeted TSan, static analysis of changed code |
| Dependencies and delivery | Some version pins; setup skips installs on header existence | Version/ABI checks, dependency inventory, advisory scanning, release checksums/provenance |
| Security process | No tracked SECURITY policy | Reporting contact, supported versions, network trust/deployment assumptions |
| Public API | Research client API mixed with benchmark controls | Documented lifecycle, ownership, errors, ACK/failure semantics, installed example |
| Documentation | Extensive notes and artifact work; conflicting current/historical statements | One supported contract, quickstart, operational limits, versioned layout/API docs |
| Reproducible research | Claim manifests, curated data packaging, bounded model and negative control | Immutable evidence release with exact source/config/tool identities and validation commands |

Build/test presets should encode developer, sanitizer, and benchmark modes consistently; CMake documents [configure/build/test presets](https://cmake.org/cmake/help/latest/manual/cmake-presets.7.html). Sanitizer jobs need separate configurations and must inspect actual instrumentation; [ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html) checks host C++ races and does not certify cross-host CXL visibility.

GoogleTest discovery is optional in `test/CMakeLists.txt`; its absence disables most unit tests while configuration succeeds with warnings. Require it whenever testing is requested, allow an explicit build-without-tests mode, and assert expected test inventory in CI. The non-coherence fault harness exists but is not added to the root CMake graph. Some tests labeled integration/production reimplement simplified logic instead of calling production recovery/replication code. Keep those as model tests and add shared-code fixtures. No blanket assertion that Release tests are disabled by `NDEBUG` is warranted: inspected registered tests use GTest or explicit failure returns.

The documented dependency installer requests 26 GB of hugepages and changes global network settings by default (`scripts/setup/setup_dependencies.sh`, lines 32–54). Separate dependency installation from opt-in benchmark host tuning. Local pre-commit hooks mentioned by CONTRIBUTING are not distributed in the tracked tree; put intended checks in versioned scripts and CI.

The tracked tree has 1,686 files, including 946 under `docs/` and 271 under `data/`. Local working data and Git history are much larger than the shipped tree; do not confuse those sizes. Preserve valuable paper evidence in immutable artifact releases with hashes and small tracked manifests. Define explicit exceptions for curated evidence instead of simultaneously forbidding all data and tracking it. Do not rewrite history or delete experiments as part of the first correctness refactor. Repository-host branch protection, remote CI services, and release access controls were not inspected.

**Recommended implementation sequence and acceptance gates**

1. **Freeze the evidence and make checks trustworthy.** Preserve the evaluated commit/data, add a supported-capability matrix, resolve licensing ownership, fix F8/F10/F11, make test dependencies explicit, and establish a clean portable CI job. Gate: an outside contributor can build and run bounded tests without tuning the host or touching unrelated processes.
2. **Repair integrity and resource boundaries in small PRs.** Address the P1 runtime findings, including F15/F16/F18, with focused production regressions; repair the failing baseline tests and remaining P2 issues. Keep behavior fixes separate from mechanical moves. Gate: capacity, replay, reclamation, shutdown, fragmentation, and concurrent initialization failures have deterministic reproductions that fail before the fix and pass after it.
3. **Extract the tested implementation.** Start with immutable configuration, framing, session decisions, GOI publication, and segmented storage. Introduce reusable client/core libraries, then move optional baselines and instrumentation behind build options. Gate: old and extracted paths produce equivalent operation traces for supported modes; shared-memory/wire layout changes have explicit versions and compatibility rules.
4. **Qualify a research release.** Add install/export support, a small non-CXL example, SECURITY/contribution/release documentation, locked dependency identities, and audited artifact manifests. Gate: reproduce the source package and functional checks from a clean environment; documentation says exactly which guarantees and failure domains are supported.
5. **Qualify sustained/hardware operation separately.** Run repeated segment/PBR/GOI reuse, session churn, slow consumers, disk-full/I/O faults, broker restarts, and failure campaigns using controlled resources. Multi-host CXL adds writer-line ownership, stale/torn visibility, physical fencing, and certified metadata recovery tests. Gate: observed operation histories satisfy the intended contract under each explicitly claimed failure mode.

The first refactoring PR should establish trustworthy build/test isolation and preserve the evidence baseline. The first runtime fix should stop unsafe segment reuse. Renaming directories or rewriting the whole broker before these controls exist would make regressions harder to attribute.

**Verification record**

Fresh review build: `/tmp/embarcadero-review-build`. Configuration used the cached gRPC source at tag `v1.55.1`, commit `12161ee3aa7c216741cd7c406573abc0df1d0926`; gRPC and its bundled dependency binaries were rebuilt in the temporary directory. Installed Folly, glog, gflags, mimalloc, yaml-cpp, cxxopts, and GTest were reused. This validates this host's installed prerequisites, not an untouched operating system.

The selected TLC runs used the repository-pinned 2.19 JAR, verified by `spec/fetch_tlc.sh`, with two workers and a 1 GB heap. They completed as follows:

| Scenario | Result | Distinct states |
|---|---|---:|
| `stale_cv_ack_relay` | No error; exhaustive within configured scope | 180,496 |
| `exactly_once_fence` | No error; exhaustive within configured scope | 53,820 |
| `reader_agreement` | No error; exhaustive within configured scope | 565,954 |
| `stale_cv_bug_demo` | Expected Safety violation with epoch check disabled | Counterexample found |

The entire long-running TLC campaign was not repeated. These results do not establish liveness, arbitrary scale, a refinement proof, or physical hardware behavior. Reproduction harnesses and transient logs remain under `/tmp/embarcadero-root-review`, `/tmp/emb-chain-review`, and `/tmp/embarcadero-review-*` in the review workspace.

The full fresh build **passed** without product-source changes, including broker, client, baseline, KV, and registered test binaries. Across bounded groups, **all 31 registered non-E2E CTest targets ran: 29 passed, 2 failed**. The failures are F19 and F20. The three registered broker-cluster E2E tests were intentionally excluded because of their shared-resource/cleanup behavior. The build was configured as Debug, but F10 means its actual compiler options still included `-O3`; it must not be called an ASan build.

The 29 passing targets were: `corfu_sequencer_fifo_smoke`, `corfu_token_proxy_smoke`, `corfu_mailbox_bench`, `corfu_ordered_chain_smoke`, `corfu_ordered_token_gate_test`, `scalog_mailbox_bench`, `lazylog_mailbox_bench`, `cxl_mailbox_smoke`, `unit_blog_header_validation`, `unit_phase2_integration`, `unit_phase3_recovery`, `unit_replication_topology`, `unit_chain_pipeline`, `unit_durable_frontier`, `unit_scalog_ack_invariant`, `unit_lazylog_latency_invariant`, `unit_session_entry_torn_read`, `unit_session_proto_roundtrip`, `unit_durable_ack2_power_loss_model`, `integration_media_durability_process_restart`, `unit_order5_recovery_property`, `unit_ack_rf_policy`, `unit_baseline_control_transport`, `unit_baseline_cxl_layout`, `unit_nt_memcpy`, `unit_order5_session_fencing`, `unit_order5_publisher_rollover`, `unit_order5_subscriber_reorder`, and `unit_baseline_transport_equivalence`.

The standalone non-coherence injection harness passed 50,000 iterations, including its intended broken-protocol negative controls. `bash -n` passed for 134 tracked shell scripts, and AST parsing passed for 34 tracked Python files. Syntax checks do not establish experiment correctness. The added configuration and shutdown harnesses reproduced the failures described above; they are independent of the passing repository tests.

Representative commands used:

```bash
cmake -S . -B /tmp/embarcadero-review-build \
  -DFETCHCONTENT_SOURCE_DIR_GRPC="$PWD/build/_deps/grpc-src" \
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build /tmp/embarcadero-review-build -j16
# Tests were run in reviewed bounded groups, with isolated temporary resources.
# After repairing F8/F11, consolidate that selection into a supported preset.
ctest --test-dir /tmp/embarcadero-review-build --show-only=json-v1
bash test/faultinj/smoke_build.sh 50000
```

Detailed transient test logs: `/tmp/embarcadero-review-unit-results.log`, `-process-results.log`, `-mailbox-results.log`, `-additional-results.log`, `-client-results.log`, and `-lazylog-retry.log`, all with the common `/tmp/embarcadero-review` prefix. The build log is `/tmp/embarcadero-review-build.log`. These paths document this review run; durable project regression tests are proposed above rather than silently added to the product test suite.
