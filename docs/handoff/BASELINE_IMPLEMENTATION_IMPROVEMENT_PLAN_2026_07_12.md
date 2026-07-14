# Baseline implementation improvement plan

**Date:** 2026-07-12  
**Audience:** implementation agent with limited context  
**Scope:** baseline control-transport ablation, faithful LazyLog append
completion, media-durable ACK2, explicit serialized Corfu replication, honest
Scalog labeling, and batching/RF instrumentation.

### Implementation status — 2026-07-13

- F1–F3 implementation, focused unit tests, launcher manifest, endpoint
  propagation, and readiness preflight are complete.
- **Required before treating F3 as experimentally validated:** run a real
  multi-host/CXL smoke with one metadata-replica service per configured failure
  domain. Verify broker-to-replica reachability, a successful faithful ACK1
  append, a transient replica restart/redrive, and that no acknowledged batch
  is lost. Record the resulting `run_contract.csv`. This is an external-cluster
  gate and remains pending; do not infer it from local unit tests.

### Validation note — 2026-07-15 (local four-process CXL emulation)

- The direct Embarcadero four-broker smoke passed on this CXL-memory machine.
  The mapping was POSIX shared memory (`dax_backed=0`), so this is process-level
  CXL/control-path emulation only, not a multi-host failure-domain result.
- The initial faithful LazyLog RF2 reproducer exposed an ACK shortfall
  (1,924/2,048). Investigation showed that the metadata sidecars had accepted
  every immutable descriptor, while the data-replica frontier had not been
  published with the contiguous-range release protocol. LazyLog now uses
  `PublishValidatedWrittenRange`, as Scalog does, rather than an unfenced raw
  frontier store. This prevents an ACK from observing metadata durability ahead
  of payload visibility/durability.
- A fresh faithful RF2 smoke subsequently passed: four local broker processes,
  two independent metadata-replica processes, and the standalone sequencer
  acknowledged 2,048/2,048 messages. The trace showed the payload frontier,
  primary/replica durable frontiers, and metadata callback all advancing before
  the ACK verification. This validates the local process-emulation path only.
  It does not close the external multi-host/failure-restart gate above.
- The launcher now fails before broker startup if a selected local sequencer
  executable is absent, and `lazylog_metadata_replica` is emitted under
  `<build>/bin` with the other deployable roles. These artifact fixes do not
  close the protocol-validation blocker above.
- M1 is complete for Corfu RF2 only. Corfu ACK2 rejects RF>2 until C2 installs
  the required ordered replica chain.
- **C1a implementation is present and its focused proxy checks have passed;
  the live publisher smoke remains required before it is complete:**
  - [x] In-process direct-sequencer versus broker-proxy trace parity: identical
    requests produce identical grants and ordering.
  - [x] Retry/idempotence: repeat the same logical proxy request after a
    successful grant and verify the cache returns the identical grant without
    issuing a second global token.
  - [x] Failed/expired token phase: the proxy deadline is bounded at 30 seconds
    (publisher payload suppression remains part of the live smoke).
  - [ ] Two-broker local smoke: membership-derived proxy endpoint selection
    reaches each selected ingress broker and bounded publish terminates.
- **C1b implementation and focused local validation are complete; its real
  multi-process CXL deployment test remains open.** The broker proxy uses an
  MPSC handoff, one sole up-ring dispatcher, one sole down-ring receiver, and
  a transport-only correlation ID. Its focused smoke verifies mailbox token
  assignment, retry idempotence, ingress ownership, status mapping, and
  concurrent proxy callers. It has not yet passed a real multi-process CXL run.
- **Corfu RF3 implementation status — 2026-07-15:** C2/C3 now implement the
  serialized RF-includes-primary chain, payload+CRC `WriteOnce`, durable
  sidecar recovery, and a broker-indexed replica membership map. RF>1 fails
  closed without an explicitly configured durable replica directory, and the
  launcher passes disk mode to each Corfu broker. Focused chain/proxy/mailbox
  tests pass. A four-broker CXL process smoke reached all four durable replica
  listeners, but the c4 publisher had only 3/4 ACK connections (B1 missing)
  and therefore sent no complete batch. This is a multi-broker publisher
  readiness failure, not RF3 success. C4 remains open; retain no RF3 result
  until that ACK-connect defect is fixed and the remote/client process smoke
  completes with chain-sidecar evidence.
- **Current packet: M2.** Media-sync wiring is implemented and locally built;
  do not mark M2 complete until these tests are added and pass:
  - [ ] Scalog replication-RPC test: a successful response is impossible before
    the write worker has completed `fdatasync`; a forced write/sync failure
    returns failure and advances neither local cut nor ACK frontier.
  - [ ] Scalog CXL primary/remote-replica test: `replication_done` remains
    unchanged until `fdatasync` completes, including a deliberately stalled or
    failed replica.
  - [ ] Production-path crash/restart smoke: kill the Scalog/LazyLog replica
    immediately after an acknowledged batch, restart from its data file, then
    verify recovery/redrive and no ACK frontier passes an unsynced item.
  - [ ] RF2 integration smoke: a lagging media replica holds ACK2; it advances
    only after that replica's durable frontier catches up.

**Publication rule:** do not use an intermediate ablation result as a final
baseline result. The control-transport work may be measured before the
durability work to debug the transport delta, but every number retained for the
paper must be produced only after the LazyLog and media-sync gates in Section 8
are green.

### Execution precondition: isolate the work

The shared repository is currently dirty. The coding agent must not create the
stage commits below in that working tree or stage unrelated user changes. Before
handoff, the owner must choose and record an immutable `BASE_REV` containing all
changes that should be retained, then create a dedicated branch/worktree, for
example:

```bash
git worktree add ../Embarcadero-baseline-ablation -b baseline-ablation BASE_REV
```

All implementation, tests, and commits happen in that worktree. If the owner
cannot provide a clean `BASE_REV`, the coding agent must stop; it must not guess
which dirty changes belong in the baseline work.

## 1. Goal and non-negotiable rules

The finished implementation must support controlled comparisons between the
existing gRPC control path and a CXL-mailbox control path without changing the
baseline protocol. It must restore LazyLog's pre-binding 1-RTT append contract,
tie every ACK2 frontier to real media synchronization, restore Corfu's ordered
replica chain, and make the paper accurately describe the Scalog harness.

Non-negotiable invariants:

1. A transport switch may change only how an existing logical message moves.
   Message fields, participants, causal order, decision function, and ACK
   dependency must remain unchanged.
2. Corfu remains token-before-write. The client must complete a token phase
   before sending the payload; mailbox mode must not collapse those into one
   client-to-broker round trip.
3. Corfu replication is an explicit RF-length ordered chain. Replica `i+1`
   may start only after replica `i` successfully completes its required write.
4. Scalog global cuts remain the element-wise minimum across every expected
   storage-server/replica stream.
5. The current Scalog implementation must be labeled a **non-FT Scalog
   data-plane harness**. It omits the original Paxos ordering layer and
   reconfiguration. This is favorable to Scalog—it removes real work—so its
   steady-state result is optimistic and favorable to Scalog, not a strawman
   weakened in Embarcadero's favor. It is not claimed as a strict mathematical
   upper bound because the topology and implementation also differ.
6. RF always includes the primary: RF1 is primary-only; RF2 is primary plus
   one replica; RF3 is primary plus two replicas.
7. Every benchmark artifact must record the effective transport, RF contract,
   ACK contract, configured batch caps, and observed batch distribution.
8. A system may be called LazyLog only if append completion is independent of
   background global binding. If that implementation is not ready, exclude
   LazyLog from comparative figures rather than presenting the current eager
   coordinator harness as LazyLog.
9. A system may claim `ack2_media_durable` only when every required replica has
   completed its required data and metadata synchronization for that item; a
   periodic best-effort `fsync` is not an ACK2 dependency.

Do not begin the final publication campaign until Gates 1--6 in Section 8 are
green. The control-transport ablation may run earlier only as a development
experiment and must be marked `nonpublication_pre_durability_fix` in its
manifest.

## 2. Work order

Implement the stages in this order and keep each task packet in a separate
commit:

1. Apply the immediate paper truthfulness edits in Section 7; do not insert new
   numbers yet.
2. Shared configuration, CXL mailbox region, and manifest fields.
3. Scalog/LazyLog selectable gRPC versus mailbox transport.
4. Corfu token proxy and selectable transport.
5. Corfu RF-length ordered replication chain and hole completion.
6. Batch/RF telemetry and staged ablation matrix.
7. Apply post-measurement paper figures and causal explanations.

Do not combine protocol changes with performance tuning. First make the two
transport modes behaviorally equivalent, then measure them.

For a low-context agent, each labeled subsection (`A1`, `A2`, ..., `E4`) is a
separate task packet and turn. Never assign all of Stage B or Stage C in one
prompt. After each packet, require its focused tests and handoff note, then have
a higher-context reviewer approve the diff before starting the next packet.
The agent must stop rather than proceed when a prerequisite API or test from the
previous packet is absent.

Use this exact packet sequence:

| Packet | Bounded deliverable |
|---|---|
| P0 | Immediate paper truthfulness edits only |
| F1 | LazyLog protocol boundary and append/ACK trace tests |
| F2 | LazyLog pre-binding append implementation and recovery/read-scope disclosure |
| F3 | LazyLog integration/failure tests and manifest contract |
| M1 | Shared durable-frontier primitive plus Corfu media-sync wiring |
| M2 | Scalog/LazyLog media-sync wiring and production-path crash tests |
| A1 | Typed transport parser and unit tests |
| A2 | Manifest schema/dirty-revision enforcement; no mailbox code |
| A3a | Centralized CXL layout plus non-overlap tests |
| A3b | In-place mailbox getters and non-destructive backing attach helper |
| A3c | Three production mailbox sequencer binaries and startup tests |
| A4 | Launcher selection, ready marker, preflight, and dry-run tests |
| B1 | Scalog transport abstraction/dispatcher and focused equivalence tests |
| B2 | LazyLog transport abstraction and focused equivalence tests |
| B3 | Cross-transport trace tests and two-broker smokes |
| C1a | Corfu broker proxy with gRPC backend; prove parity with old direct path |
| C1b | Corfu mailbox backend, correlation, cache, and equivalence tests |
| C2 | Ordered RF1/RF2/RF3 chain without hole recovery |
| C3a | Replica Probe/WriteOnce/Junk service and durable sidecar recovery |
| C3b | Hole-prefix validation and suffix completion driver |
| C4 | Corfu failure/concurrency/process smoke suite |
| D1 | Common message-count batch cap and seal-reason tests |
| D2 | Hot-path-safe telemetry plus manifest export |
| D3 | RF/ACK machine-readable validation and tests |
| D4 | Staged matrix runner and plot-from-manifest pipeline |
| E1 | Post-measurement paper figures and evidence-based explanations |

## 3. Stage A — shared configurable control transport

### A1. Define one typed configuration

Add this shared helper as the prescribed location:

- `src/common/baseline_control_transport.h`
- `src/common/baseline_control_transport.cc`

Define:

```cpp
enum class BaselineControlTransport { kGrpc, kCxlMailbox };

BaselineControlTransport ParseBaselineControlTransport();
const char* BaselineControlTransportName(BaselineControlTransport);
```

Read `EMBARCADERO_BASELINE_CONTROL_TRANSPORT`, accepting only `grpc` and
`cxl_mailbox`. Default to `grpc` for compatibility. Invalid values must fail
fast with a clear error; never silently fall back.

The selected value must be parsed once per process and used consistently by
the launcher, clients, brokers/local sequencers, and global sequencer. Avoid
separate static environment checks such as `SCALOG_CXL_MODE` deciding control
transport. Existing `SCALOG_CXL_MODE`/`LAZYLOG_CXL_MODE` may continue to select
the payload-replication path until a later cleanup.

### A2. Add manifest/output fields

Extend every throughput/latency summary row with:

- `control_transport`: `grpc` or `cxl_mailbox`
- `control_topology`: `broker_proxy` for the matched Corfu ablation;
  `direct_global` is legacy-only and cannot share its curve
- `data_transport`: currently `cxl_shared_log`
- `mailbox_backing`: `dax`, `cxl_numa_shm`, `posix_smoke`, or `not_applicable`
- `cxl_layout_version`
- `sequencer_ft`: `none`, `paxos`, or another explicit value
- `rf_includes_primary`: always `true`
- `remote_replica_count`: `max(rf - 1, 0)`
- `ack_contract`: e.g. `ack1_ordered_visible` or `ack2_media_durable`
- `git_commit` and `git_dirty`

Publication scripts must reject `git_dirty=true` unless an explicit
`ALLOW_DIRTY_ARTIFACT=1` is supplied for developer smoke runs. Curators must
always exclude dirty rows even when that override produced them. Publication
curation must reject mixed `control_transport`, control topology, mailbox
backing, ACK contracts, or commits within one plotted curve.

Touchpoints are not optional: update `src/client/result_writer.{h,cc}`, the
throughput/latency summary generators under `scripts/publication/`, the curated
CSV exporter, and plotting loaders. Delete hard-coded `CANONICAL` measurement
arrays; plots must read rows carrying the fields above.

### A3. Reserve and own a real CXL mailbox region

The production `cxl_mailbox` mode must use `MailboxSegment::CreateInPlace` /
`AttachInPlace` over the existing CXL mapping. POSIX `CreateShm` is allowed only
for unit and process smoke tests; data from POSIX DRAM must never be labeled a
CXL-mailbox result.

Centralize the CXL layout constants first. The current hard-coded
`kPhase2MetadataEnd` must be replaced by a checked calculation based on
`kGOIOffset + kMaxGOIEntries * sizeof(GOIEntry)`. Reserve one cache-line-aligned
baseline mailbox region immediately after the GOI and move `base_for_regions_`
after that region. Add static/runtime overlap checks proving that ControlBlock,
CV, GOI, mailbox, TInode, bitmap, batch headers, session table, and segment pool
do not overlap. This is important because the current GOI-size comments and
hard-coded end constant disagree.

Reserve the mailbox using `MailboxSegment::BytesNeeded` with
`NUM_MAX_BROKERS_CONFIG`, record size 512, and both capacities 1024, rounded up
to 4 KiB. Active runs may use fewer brokers inside that fixed maximum region;
they must not change subsequent CXL offsets. Increment and record a
`kCxlLayoutVersion` whenever this layout changes.

Expose these APIs from `CXLManager` or a small shared layout helper:

```cpp
void* GetBaselineMailboxBase();
size_t GetBaselineMailboxBytes();
```

Use one mailbox region per active baseline run; only one sequencer type is
active in the harness. Parameters are fixed for publication runs:

```text
num_brokers = EMBARCADERO_NUM_BROKERS
record_size = 512
up_capacity = 1024
down_capacity = 1024
```

Broker 0 is the sole initializer and calls `CreateInPlace` once after CXL layout
initialization. Followers call `AttachInPlace`. Add a shared, non-destructive
`CxlBackingAttach` helper so a standalone mailbox sequencer process can map the
same DAX/shm backing and attach to only the reserved mailbox region without
`ftruncate`, zeroing, or unlinking it. It must use the same backing selection and
size configuration as the brokers.

Add three production binaries—not benchmark binaries:

- `corfu_mailbox_global_sequencer`
- `scalog_mailbox_global_sequencer`
- `lazylog_mailbox_global_sequencer`

Each waits with a bounded timeout for the mailbox magic/version and exact
parameters, attaches, runs the existing mailbox sequencer/core, and exits
nonzero on mismatch. It never creates or clears the CXL region.

### A4. Launcher behavior

Update `scripts/run_multiclient.sh` and publication cell/matrix scripts:

- `grpc`: launch the current gRPC global sequencer binaries.
- `cxl_mailbox`: start broker 0 first so it initializes the CXL region; wait for
  the exact broker log marker `BASELINE_MAILBOX_READY`, emitted only after
  `CreateInPlace` succeeds; start the matching production mailbox
  sequencer; then start follower brokers. Do not launch the corresponding gRPC
  control server.
- Print one startup line containing system, control transport, RF, remote
  replica count, ACK level, and batch caps.
- Add a `DRY_RUN=1` mode that prints commands and environment without starting
  processes. This is required for cheap script tests.
- Fail preflight if client, broker, and sequencer transport selections differ,
  if mailbox parameters differ, or if a mailbox benchmark binary appears in a
  publication command.

### A5. Unit tests

Add parser tests for default, both valid values, invalid value, and stable
round-trip name conversion. Add a script test that checks `DRY_RUN=1` launches
the correct binaries for all three baselines and both transports. Add a memory
layout test covering minimum and configured CXL sizes and a negative
magic/version/parameter mismatch test.

## 4. Stage B — end-to-end mailbox control-path ablation

### B1. Scalog

Refactor `ScalogLocalSequencer` so transport is behind an interface with two
operations:

```cpp
SendLocalCut(const LocalCutRecord&);
ReceiveGlobalCut(GlobalCutRecord*);
```

Implement:

- `GrpcScalogCutTransport`: current bidi-stream behavior.
- `MailboxScalogCutTransport`: writes the same fields to the per-broker
  mailbox up-ring and reads the down-ring.

Both paths must call the existing `ScalogGlobalOrderingCore`; do not duplicate
the minimum calculation. There is no implementation choice about cadence:

1. Both modes accumulate each accepted report immediately in the shared core.
2. Both modes call `ComputeGlobalCut(..., use_epoch_barrier=false)` on the
   existing `SCALOG_SEQ_LOCAL_CUT_INTERVAL` periodic timer.
3. Once readiness is satisfied, both modes broadcast the current cut on every
   timer tick, including an unchanged cut, matching current gRPC behavior.
4. The mailbox `epoch` field is transport correlation only: populate it with a
   monotonically increasing broadcast sequence and never feed it into the cut
   decision.

Delete the mailbox-only minimum-fresh-epoch barrier. It changes straggler
semantics and is not a transport substitution.

Scalog has multiple producer threads per broker—the primary local report plus
remote-replica reports—but `up(b)` is SPSC. Add one broker-local
`ScalogMailboxCutDispatcher`: report producers enqueue POD records into an MPSC
host-DRAM queue; one dispatcher thread is the sole writer of `up(b)`. One
receiver thread is the sole reader of `down(b)` and forwards global cuts to the
existing application path. Do not allow multiple report threads to write the
mailbox ring directly.

Preserve separate streams for the primary component and every remote replica.
An RF2 test must demonstrate that the global cut does not pass a lagging
replica, even though the primary's own stream is ahead.

### B2. LazyLog transport only

Apply the same interface pattern to `LazyLogLocalSequencer`:

```cpp
SendLocalProgress(const LocalProgressRecord&);
ReceiveGlobalBinding(GlobalBindingRecord*);
```

Both modes must call the existing `LazyLogBindingCore` and produce identical
binding deltas for identical inputs. Do not treat this work as fixing the
separate LazyLog fidelity blocker: client-visible append currently waits for
binding and must be corrected in a separate task.

Use the exact cadence rule below; do not choose another one:

1. Both modes call `ComputeGlobalBinding(..., use_epoch_barrier=false)` on the
   existing `LAZYLOG_SEQ_LOCAL_CUT_INTERVAL` timer.
2. Broadcast only when the returned binding map is non-empty, matching current
   gRPC behavior.
3. Delete the mailbox-only fresh-epoch barrier. Use a monotonically increasing
   transport broadcast sequence only for correlation.

The publication harness remains single-topic. Encode that topic as `topic_id=0`
in mailbox mode and fail if a second topic is registered; do not invent a
cluster-wide topic-ID service in this task.

### B3. Transport equivalence tests

For Scalog and LazyLog, build a deterministic trace test:

1. Generate registration and progress/cut messages, including duplicate,
   delayed, and regressing reports.
2. Feed the trace through the gRPC-facing core adapter and mailbox-facing core
   adapter.
3. Decode both outputs into a canonical logical record and assert equal
   cuts/bindings plus identical emission count/order. Do not compare protobuf
   bytes with padded POD bytes.
4. Repeat for RF1, RF2, and RF3 and for one deliberately lagging replica.

Add a local two-broker end-to-end smoke test for each transport. It must
publish a small bounded workload, verify ordered count, and terminate without
timeouts. Performance is not an acceptance condition for smoke tests.

## 5. Stage C — Corfu token path and faithful replication

### C1. Preserve two client-visible phases in mailbox mode

The current gRPC mode is:

1. client asks the Corfu sequencer for a token;
2. client receives the token;
3. client sends the payload to a broker.

For a clean ablation, **both** modes use the same external client-to-ingress-
broker token proxy. Only the broker-to-global-sequencer leg changes. Do not
multiplex token frames onto the raw payload socket, which currently begins each
batch with an untagged `BatchHeader`.

Add `src/protobuf/corfu_token_proxy.proto` with a unary
`CorfuTokenProxy.GetTotalOrder`. Its request and response mirror the existing
Corfu sequencer protobuf fields exactly. Run one proxy service in every broker
on `EMBARCADERO_CORFU_PROXY_PORT_BASE + broker_id` (default base `50100`; add
port collision/preflight checks). Change the Corfu client to contact its chosen
ingress broker's proxy, not the global sequencer directly.

Inside the broker, define one backend interface:

```cpp
class CorfuTokenBackend {
 public:
  virtual TokenResult GetToken(const TokenRequest&) = 0;
};
```

- `GrpcCorfuTokenBackend` forwards to the existing global gRPC sequencer.
- `MailboxCorfuTokenBackend` uses the CXL mailbox.

Mailbox rings are SPSC while gRPC proxy handlers are concurrent. The mailbox
backend therefore owns a broker-local MPSC request queue, one dispatcher thread
as the sole `up(b)` writer, and one receiver thread as the sole `down(b)` reader.
Add a transport-only `uint64_t correlation_id` to `CorfuTokenRequest` and
`CorfuTokenGrant`; the receiver uses it to fulfill the correct waiting promise.
`client_id` and the original pre-sequencer `batch_seq` remain the logical
idempotency key and must not be rewritten by the proxy.

The resulting mailbox sequence is:

1. client calls the ingress broker's unary token proxy;
2. proxy backend posts the same logical request to the Corfu mailbox up-ring;
3. mailbox sequencer calls the shared `CorfuSequencerImpl::AssignToken`;
4. broker relays the grant to the client;
5. only then does the client send the payload.

Do not let the broker acquire a token after receiving the payload. Do not merge
token grant and payload ACK. Reuse one assignment implementation for gRPC and
mailbox modes.

Use a 30-second proxy request deadline. If the token phase fails or times out,
the publisher must abort that batch attempt and must not send its payload.
Transport retries reuse the same logical `(client_id, batch_seq)`; add a small
bounded broker-proxy grant cache so a retry returns the prior successful grant
rather than requesting a second token. Use an LRU limit of 65,536 grants and a
60-second TTL from grant creation; never evict an entry with a waiter currently
attached. The cache is transport plumbing; `CorfuSequencerImpl::AssignToken`
remains the sole token decision function.

Add client-side phase counters/timers:

- token requests and grants
- token RPC/proxy latency
- payload sends
- violations where payload send begins before grant (must remain zero)

### C2. Remove Corfu parallelization

Remove all paper and code comments claiming Corfu replicas fetch in parallel.
Implement an explicit ordered chain whose length equals RF.

Use the project's includes-primary RF convention. Chain position 0 is the
already-completed CXL primary publication. The explicit remote target list has
exactly `RF-1` entries and represents chain positions `1..RF-1`. This is the
only permitted model for this task:

- RF1: CXL primary only; ACK1 ordered/visible, no redundant durable copy.
- RF2: CXL primary, then one remote disk target.
- RF3: CXL primary, then remote disk target 1, then remote disk target 2.

This differs from original Corfu's all-flash replica set and must be disclosed
as the project's common RF convention; what is preserved is Corfu's strict
ordered-chain causal dependency. Do not call RF1 durable.

Recommended representation:

```cpp
struct CorfuReplicaTarget {
  int chain_index;
  int broker_id;
  std::string endpoint;
};

class CorfuOrderedChain {
 public:
  Result Append(const CorfuAppendDescriptor&, std::span<const CorfuReplicaTarget>);
  Result CompleteHole(const CorfuAppendDescriptor&, std::span<const CorfuReplicaTarget>);
};
```

Place `CorfuOrderedChain` in `src/disk_manager/` and invoke it from the existing
Corfu completion callback after the ingress broker has published the payload in
CXL. The ingress broker is the sole chain driver in this harness: it calls each
remote `WriteOnce` endpoint synchronously in list order. Do not spawn one thread
per target and do not let replicas independently poll the same new item. This
choice matches the existing broker-owned replication integration and removes
the current ambiguity; the paper must disclose that the CXL harness preserves
Corfu's serialized chain dependency but uses a broker-side descriptor driver
rather than original Corfu's client library driving flash-unit writes.

Read remote targets from `EMBARCADERO_CORFU_CHAIN_ENDPOINTS`, a comma-separated
ordered list. Build and validate the vector at topic creation. It must contain
exactly `max(RF-1, 0)` distinct endpoints, with `chain_index` values `1..RF-1`.
RF1 requires an empty list. Do not keep the current single endpoint and then
mark all RF slots complete.

The chain rule is strict:

- the primary-publication event for position 0 completes before position 1
  starts;
- position `i` completes before position `i+1` starts;
- append succeeds only after the tail completes;
- failure at position `i` leaves a valid written prefix and an unwritten
  suffix; later positions must not be marked complete;
- RF1 ACK1 completes at position 0 under the ordered-visible contract;
- ACK2 for RF>=2 advances only after the last configured remote target returns
  its required media-durable success. Until the separate media-durability task
  is merged, ACK2 publication measurements remain blocked.

Always emit a position-0 trace event so RF1/RF2/RF3 call order is unambiguous.

### C3. Hole completion

Do not invent the hole state during coding. Extend the Corfu replication proto
with these explicit operations:

```text
ProbeSlot(SlotKey) -> {UNWRITTEN | VALUE | JUNK, ValueId}
WriteOnce(SlotKey, ValueId, source_offset, size) ->
    {WRITTEN | ALREADY_SAME | CONFLICT | IO_ERROR}
WriteJunkOnce(SlotKey) -> {WRITTEN | ALREADY_JUNK | CONFLICT | IO_ERROR}
```

Use:

```text
SlotKey = {topic, broker_id, broker_batch_seq}
ValueId = {client_id, original_client_batch_seq, total_order, num_msg, total_size}
```

`ValueId` is identity, not a pointer. The service must never compare or persist
virtual addresses.

Each replica service maintains a durable sidecar append log. A sidecar record
contains `SlotKey`, state (`VALUE` or `JUNK`), `ValueId` when applicable, data
offset/size, record version, and CRC32C. Define a fixed little-endian record with
magic, version, record length, body, and trailing CRC32C; add known-vector and
round-trip tests rather than serializing an in-memory C++ struct with implicit
padding. On startup, replay the sidecar into
an in-memory slot map; a truncated or checksum-invalid final record is ignored,
while corruption before the final record fails startup.

For `WriteOnce`, serialize transitions per `SlotKey`:

1. If state is the same `ValueId`, return `ALREADY_SAME`.
2. If state is another value or junk, return `CONFLICT`.
3. Write the payload from CXL to the replica data file and complete the required
   data sync.
4. Append and sync the `VALUE` sidecar record.
5. Only then return `WRITTEN`.

`WriteJunkOnce` performs the analogous durable metadata transition without
payload. Never overwrite an existing state.

`CompleteHole` probes remote chain positions in order. All written positions
must form one prefix with the same `ValueId`. Resume at the first `UNWRITTEN`
position and issue ordered `WriteOnce` calls for the suffix. A conflict or a
written entry after an unwritten entry fails the operation. Completion is
reported only when the tail is `VALUE` with the expected identity. An explicit
fill-junk request follows the same ordered rule with `WriteJunkOnce`.

Position 0 is the CXL-primary event and has no durable sidecar under the common
RF convention. Hole tests therefore validate remote chain suffix completion;
the paper must not claim full Corfu projection/reconfiguration or survival of
the sole RF1 CXL copy.

This need not implement full Corfu projection reconfiguration in this stage,
but the limitation must be documented.

### C4. Corfu tests

Use fake/in-process replica services with an append-only event log. Required
tests:

1. RF1: exactly one ordered position completes.
2. RF2: events are `[0 start, 0 done, 1 start, 1 done]`.
3. RF3: no position starts before its predecessor finishes.
4. RF3 failure at position 1: position 2 never starts and no tail ACK occurs.
5. Hole completion after failure at each possible chain position resumes only
   the missing suffix.
6. Conflicting retry is rejected without overwriting any position.
7. Duplicate retry after full completion is idempotent.
8. Token-before-payload invariant passes in gRPC and mailbox modes.
9. Configured `RF-1` differs from remote endpoint count: topic creation fails.
10. Replica service restart replays VALUE/JUNK sidecar states correctly.
11. A truncated final sidecar record is ignored; earlier corruption fails.
12. A written entry after an unwritten entry is rejected as a non-prefix chain.

Add one local process-level RF3 smoke test. RF3 is essential: RF2 cannot expose
accidental parallelism beyond a single remote replica.

## 6. Stage D — batching, RF semantics, and causal instrumentation

### D1. Add a message-count batch cap

Keep the existing byte cap, and add:

`EMBARCADERO_CLIENT_PUB_BATCH_MESSAGES`

Seal a batch when the first of these occurs:

- message-count cap reached;
- byte cap reached;
- linger timer expires;
- shutdown/drain requires a partial batch.

Use the same client batching implementation and both caps for every system in
this harness. No system-specific override is permitted in a comparison cell.
Corfu receives one token range per resulting client batch; Scalog/LazyLog
progress remains cumulative over the messages in those same batches. Record
this adaptation explicitly, and always include batch=1 and batch=4 sensitivity
so large-batch amortization cannot hide the token/cut cost.

### D2. Record observed batching

Per trial and per system, report:

- total messages and batches
- mean, P50, P95, P99, min, and max messages/batch
- mean/P50/P99 bytes/batch
- percentage sealed by message cap, byte cap, linger, and drain
- Corfu tokens/s and messages/token
- Scalog local cuts/s and messages/new global cut
- LazyLog progress reports/s, bindings/s, and messages/binding
- sequencer service time, client-observed control latency, in-flight requests,
  and mailbox ring occupancy/high-water mark where applicable

Implement batch telemetry with per-publisher-thread counters and fixed
histogram buckets, merged only at trial shutdown; do not add a mutex or CSV
write to the publish hot path. Put the accumulator in
`src/client/batch_telemetry.{h,cc}` and export through `ResultWriter`. Add
monotonic atomic counters around the shared sequencer cores for request/report
count, decision count, active handlers, and service nanoseconds. Add read-only
approximate-depth and high-water accessors to `MailboxRing`; instrumentation
must not change producer/consumer ownership.

For publication configurations, reject a message cap above 512. Use exact
per-thread message-count buckets `0..512` plus one overflow bucket, exact sums
for means, and logarithmic byte-size/control-latency buckets with documented
boundaries. Percentiles are computed from merged bucket counts by the result
writer. Unit tests must cover empty input, partial batches, every seal reason,
bucket boundaries, overflow, and percentile selection.

Write one machine-readable `trial_manifest.json` plus the existing summary CSV.
The manifest is the authoritative source; summary/plot scripts derive their
columns from it. Add a schema version and reject unknown newer versions.

Do not infer a sequencer bottleneck from throughput shape alone. Attribute a
knee to the sequencer only if control latency/queue occupancy rises and the
sequencer approaches measured service capacity.

### D3. Make RF semantics machine-readable

At topic creation and in the artifact manifest, emit:

```text
rf=1 primary_copies=1 remote_replicas=0 ack=1 redundancy=none
rf=2 primary_copies=1 remote_replicas=1 ack=2 redundancy=one_host_loss_if_media_durable
rf=3 primary_copies=1 remote_replicas=2 ack=2 redundancy=contract-specific
```

Reject ACK2 with RF less than 2. Reject any claim of `media_durable` unless the
production durable frontier is tied to successful media synchronization.

### D4. Ablation matrix

Do not launch the full Cartesian product initially. Use this staged down-select:

**D4a — control/batch characterization:** Corfu, Scalog, and LazyLog;
`grpc` and `cxl_mailbox`; batch messages `{1,4,64,512}`; RF1; one all-remote
publisher. Use three throughput trials and five latency trials. This stage
identifies whether the control path saturates and selects at most two batch
sizes for the main comparison: batch=1 or 4 plus one amortized size supported
by observed batching.

**D4b — main matched comparison:** only the selected batch sizes; both
transports; RF `{1,2}`; publisher count `{1,2}`, all remote. Use identical
message size, offered loads, client hosts/NUMA nodes, disks, ACK semantics,
warmup, duration, and trial count. Collect at least three throughput and five
latency trials per retained cell.

**D4c — Corfu chain correctness:** RF3, batch `{1,4}`, one publisher, bounded
correctness/smoke load only. Do not add RF3 to the full performance matrix
unless the paper makes an RF-scaling claim.

For the retained cells, plot:

1. throughput versus offered load;
2. P50/P99 append latency versus offered load;
3. control requests/s and queue/ring occupancy versus offered load;
4. observed messages/batch, not only configured cap;
5. gRPC-to-mailbox delta at matched protocol settings;
6. RF1-to-RF2 delta with the RF contract printed in the caption.

The main paper may use a representative batch size, but the batch sensitivity
must appear in the appendix/artifact. If Corfu's request rate remains far below
measured sequencer capacity, remove the claim that the sequencer endpoint caused
the knee and report the measured limiter instead.

## 7. Stage E — paper and documentation changes

Split paper work into two commits:

1. **Immediate truthfulness edit, before coding:** remove the Corfu parallel
   chain claim, remove the “second replica shares sequencer duty” explanation,
   label Scalog as the non-FT data-plane harness below, correct RF terminology,
   and mark current comparative figures/tables as pending refresh. Do not alter
   or invent numbers.
2. **Post-measurement edit:** regenerate figures/tables from clean manifests and
   write causal explanations only from the new telemetry.

Update `Paper/Text/Sec7_Evaluation.tex` and any included appendix. Do not rely on
an uncompiled Markdown document to carry a necessary qualification.

### E0. Exact immediate wording before implementation

Until C2/C4 pass, do not write as if the ordered RF-length chain already exists.
Replace the current Corfu replication description with a temporary disclosure:

> The current Corfu harness preserves token-before-write but uses one
> replication-service endpoint and therefore does not yet validate an RF-length
> ordered chain. We withhold comparative Corfu replication conclusions pending
> the protocol-fidelity refresh described in the artifact methodology.

Mark affected Corfu RF2 comparative values as pending refresh. After C2/C4 pass
and the cells are rerun, replace this temporary text with E1 below.

### E1. Corfu wording

Replace the statement that CXL-Corfu changes chain replication to parallel
shared-memory reads. Use wording equivalent to:

> CXL-Corfu preserves token-before-write and Corfu's strict ordered replica
> chain dependency. After the CXL primary publication, the harness's ingress broker
> drives replica descriptors in strict chain order: replica `i+1` does not begin
> until replica `i` completes. This preserves the serialized replication cost,
> although the descriptor driver is broker-side rather than original Corfu's
> client-side flash writer.

Remove the statement that a second replica shares sequencer duty. Corfu has one
sequencer endpoint in this harness.

Describe RF1/RF2/RF3 using the includes-primary convention. State whether each
reported ACK is ordered-visible or media-durable.

### E2. Scalog wording

Replace any claim that Paxos is not on Scalog's data path. Add a prominent
methodology statement equivalent to:

> Our Scalog result uses a non-fault-tolerant Scalog data-plane harness. It
> preserves FIFO in-shard replication reports and the element-wise minimum
> global-cut decision, but omits Scalog's Paxos-replicated ordering service and
> failure/reconfiguration protocols. This omission is favorable to Scalog by
> removing real work; the reported steady-state result is therefore an
> optimistic, favorable-to-Scalog harness result, not a measurement of full
> Scalog. We do not claim it is a strict mathematical upper bound because the
> harness topology and implementation also differ from the original system.

Do not use this harness for head-to-head failure recovery claims. Either add
Paxos/reconfiguration in a future full-system baseline or retain failure
comparison as explicitly absent.

### E3. Mailbox ablation wording

Describe two separate baseline legs:

- original control transport (`grpc`)
- protocol-equivalent CXL mailbox control transport (`cxl_mailbox`)

Only call the second leg “CXL-control” after end-to-end manifests prove mailbox
mode was selected. Report the delta between the two; do not mix their points in
one curve.

### E4. Batching/RF wording

Every relevant caption must state configured batch cap, observed median/P95
messages per batch, whether clients were paced, RF-includes-primary semantics,
and ACK contract. Replace causal explanations with measurements from the new
queue/occupancy telemetry.

Ensure the baseline-fidelity appendix is actually included in `Main.tex` or
move its essential methodology into Section 7. A file that is not included in
the compiled submission cannot carry a necessary qualification.

## 8. Acceptance gates

### Gate 1 — build and unit correctness

- All new and existing unit tests compile and pass.
- Transport parser and manifest tests pass.
- CXL layout non-overlap and mailbox attach/mismatch tests pass.
- No duplicated Corfu token assignment or Scalog/LazyLog decision function.
- Thread-sanitizer or explicit concurrency tests show exactly one writer and
  one reader per mailbox ring.

### Gate 2 — protocol equivalence

- Scalog gRPC and mailbox deterministic traces produce identical global cuts.
- LazyLog gRPC and mailbox deterministic traces produce identical bindings.
- Corfu gRPC and mailbox modes assign identical tokens for the same request
  trace.
- Corfu mailbox mode shows token grant before payload send in every trace.
- Corfu's two modes use the same external broker proxy; only the backend leg
  differs.

### Gate 3 — replication correctness

- RF1/RF2/RF3 ordered-chain tests pass.
- Failure prevents later replica start and tail ACK.
- Hole completion, conflict, and idempotence tests pass.
- Sidecar restart and torn-final-record tests pass.
- No code path marks every RF slot after one RPC.

### Gate 4 — local end-to-end smoke

- Every baseline completes a bounded workload in `grpc` and `cxl_mailbox` mode.
- The mailbox manifest proves `CreateInPlace/AttachInPlace` used the CXL
  backing; POSIX-shm smoke results cannot pass this gate.
- Scalog RF2/RF3 global cut is held by a deliberately lagging replica.
- Artifacts contain all transport, RF, ACK, commit, and batch fields.
- `DRY_RUN=1` demonstrates the intended process topology for every cell.

### Gate 5 — ready for the control-transport ablation campaign

- Clean immutable commit.
- No hard-coded result arrays in plotting scripts.
- All plots regenerate from raw manifests.
- No mixed commits/topologies/transports within curves.
- Paper contains the non-FT Scalog qualification and disclosed serialized Corfu chain
  description.
- Batch sensitivity and observed batch distributions are available.

### Gate 6 — separate final-publication blockers

This plan does not close Gate 6. Before final paper measurements, separately
require:

- LazyLog append completion restored to its real 1-RTT pre-binding contract.
- Corfu, Scalog, and LazyLog ACK2 tied to the production media-sync frontier,
  with the same media/sync policy used for Embarcadero.
- All retained cells rerun after those fixes at one clean immutable revision.

## 9. Required verification commands

Wire the CMake/CTest targets using the names below so the agent does not invent
names or skip a layer:

```bash
cmake -S . -B build-ablation -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-ablation -j2
ctest --test-dir build-ablation --output-on-failure \
  -R 'baseline_control_transport|cxl_layout_mailbox|scalog_transport_equivalence|lazylog_transport_equivalence|corfu_token_proxy_equivalence|corfu_ordered_chain|corfu_hole_completion|corfu_sidecar_recovery'

DRY_RUN=1 SEQUENCER=CORFU \
  EMBARCADERO_BASELINE_CONTROL_TRANSPORT=grpc \
  REPLICATION_FACTOR=3 bash scripts/run_multiclient.sh

DRY_RUN=1 SEQUENCER=CORFU \
  EMBARCADERO_BASELINE_CONTROL_TRANSPORT=cxl_mailbox \
  REPLICATION_FACTOR=3 bash scripts/run_multiclient.sh

# POSIX-shm is smoke-only and must identify itself as non-CXL.
ctest --test-dir build-ablation --output-on-failure -R mailbox_process_smoke

# Paper truthfulness lint: these unsupported phrases must be absent.
! rg -n 'replaces chain replication with parallel|second replica shares sequencer duty|none of the systems exercise.*Paxos' Paper/Text
```

Do not run the full publication matrix until the small deterministic and local
smoke tests pass.

## 10. Required handoff from each coding stage

Each stage must leave a short note containing:

1. `BASE_REV`, stage commit, and confirmation that the worktree was clean before
   the stage;
2. files changed;
3. protocol invariant preserved;
4. tests added and exact commands run;
5. remaining failures or skipped tests;
6. example manifest row, including proof of CXL in-place versus POSIX-shm
   backing;
7. process/port topology for both transport modes;
8. whether any paper statement became stale.

If an implementation choice would change a participant, message, causal edge,
decision function, or ACK frontier, stop and request author review rather than
silently choosing the simpler implementation.
