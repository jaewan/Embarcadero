# Isolated DRAM development

The [supported command index](development-commands.md) links smoke, legacy startup,
fault tests and matched performance pilots. `scripts/run_multiclient.sh --dev-dram`
delegates directly to this owned smoke lifecycle before any historical launcher actions.

Build this revision's `embarlet` and `throughput_test`, then inspect a run:

```sh
python3 tools/dev_cluster.py --build-dir build/debug --dry-run
python3 tools/dev_cluster.py --build-dir build/debug
python3 tools/dev_cluster.py --build-dir build/debug --brokers 3
```

The runner supports Linux with cgroup v2, Python 3, `numactl`, and NUMA nodes 0 and 1. `--build-dir` accepts an out-of-tree build. It uses the broker's side-effect-free `--print-layout` command for production geometry; an older binary fails preflight. A dry run writes its configuration and manifest and runs that layout query, but creates no broker process or shared region.

Every broker explicitly receives `--emul`. Broker CPUs and shared-memory first touch are bound to node 1; the local client is bound to node 0. The complete [profile](../config/dev-dram-local.json) is JSON-formatted YAML, so the Python runner needs no YAML package. It sets **`cxl.size` to 64 GiB**, **segments to 256 MiB**, and preserves the production 32 GiB GOI. The runner updates the broker count and data-broker IDs together. It sanitizes inherited experiment overrides and records the selected environment and ignored variable names.

New shared-memory objects use owner-only permissions (`0600`); an open failure stops startup instead of silently switching to a `/tmp` file.

The owned runner normally sets `EMBARCADERO_CXL_BASE_ADDR=0x400000000000` for every broker, overriding inherited values. Shared raw pointers require identical virtual bases, and the explicit base fails safely on a collision. The production mapping path now lets the head choose an available address and requires followers to use the published descriptor's base. Add `--automatic-mapping` to exercise that path in a smoke: the runner removes inherited fixed overrides and checks every broker's logged and observed shared-memory base for agreement. The performance pilot retains its fixed-base protocol.

The workload is intentionally fixed: one topic, one client process, one sender thread per broker, ORDER5, ACK1, RF0, and 32 MiB of 4 KiB application messages. The client deadline is 30 seconds; its 10-second RTO floor is inside the default broker lease so a missing predecessor can be retried before fencing. Unexpected retransmissions invalidate the smoke result. Disconnect-driven retries still depend on runtime capacity handling; this runner is not a fault-injection or segment-rollover test. ACK1 and RF0 make no disk-durability claim.

The current shared GOI and completion vector support **one topic identity per region**. Topic creation rejects a second distinct identity before allocating or writing shared metadata, including after deleting the first topic or a failed creation attempt. Use a fresh region for a different topic. This contains shared-state ownership until true multi-topic isolation is implemented; the metadata capacity is not a promise of supported topic count.

Each run gets a private artifact directory and a unique, exclusively created shared-memory inode. Preflight checks ports, node memory, tmpfs space, address-space limits, allowed memory nodes, and cgroup ancestor limits. The fixed control port is 12140 and data ports are 1214 through 1214 + broker count - 1. Runs take a per-user lock through cleanup and reject occupied ports. Other users and old launchers do not take that lock, so concurrent use with those tools is unsupported. No global process kill, host tuning, privilege change, hugepage reservation, or SSH is performed.

The head must finish initialization before followers start. The runner captures thread affinity and `/proc/<pid>/numa_maps`, and checks that the head's shared pages are on node 1. Brokers share all CPUs available on node 1, including available SMT siblings; this is explicitly recorded and is **not** a fixed-core performance experiment. Client buffers request anonymous pages with THP advice instead of HugeTLB. The manifest records the observed host page policies; the NUMA snapshots preserve actual mappings.

The artifact directory keeps commands, binary hashes, revision and dirty-file status, effective configuration, placement observations, logs, and the client CSV. It also records relevant CMake cache settings, the compile-commands hash, and observed PBR reservation modes. Match compiler, build type, native ISA, sanitizer, and reservation mode for performance comparisons: changing native ISA can switch the 128-bit PBR reservation between atomic and mutex implementations. Dirty runs explicitly have incomplete source provenance; use a clean commit for comparison artifacts. If the client finishes before the placement sample, the manifest reports that observation as unavailable.

A successful run requires a positive E2E completion result and the actual ordered-delivery audit: exactly 8192 messages and 32 MiB, contiguous global order, indexed payload sequence 0 through 8191 across broker striping, full remaining-payload comparison, and zero duplicate, parser-error, and export-gap counters. This covers one publisher session; arbitrary multiple sessions, recovery, and fencing require separate protocol and fault tests. A legacy empty-buffer header check cannot satisfy this gate. The small transfer's throughput includes retained parsing and payload-comparison work and is diagnostic; it is not a performance baseline or final CXL evidence.

Cleanup signals only process groups started by this runner, waits for them, and unlinks only its own unchanged shared-memory inode. The manifest records every child exit and whether the shared-memory path was removed; nonzero exits, forced termination, or a remaining path fail the lifecycle gate even after a successful payload audit. SIGINT/SIGTERM use the same cleanup. SIGKILL or host loss cannot execute cleanup; the manifest identifies the run's remaining resources for manual inspection. It does not automatically reclaim another run's resources.

Test orchestration without allocating a real region:

```sh
python3 tools/test_dev_cluster.py
```

The fake-executable tests cover explicit emulation propagation, environment isolation, occupied-port rejection, serialization, dry run, successful cleanup, startup failure, and stubborn-child termination while an unrelated process survives. Optional remote clients remain separate; local development does not depend on `ssh c1`, `ssh c3`, or `ssh c4`. The runner also has a `--physical-cxl` NUMA-node-2 placement profile. It requires the real backend's shared-memory fallback and verifies node-2 residency; it does not identify a PCI CXL device or qualify physical-CXL throughput. See the [recorded finite checks](reviews/2026-09-23-workload-followup.md).

An optional live regression checks legacy ORDER0/ACK1 startup, which must establish its ACK connection without waiting for session negotiation:

```sh
python3 test/integration/check_legacy_startup.py --build-dir build/debug --dry-run
python3 test/integration/check_legacy_startup.py --build-dir build/debug
```

It reuses the isolated runner's 64 GiB region, NUMA placement, lock, deadlines, and owned cleanup. It requires full ACK and E2E completion with no session negotiation or retransmission, and labels its manifest as a legacy startup regression with the indexed audit disabled. It does not prove payload correctness or performance and is intentionally excluded from ordinary unit tests.

The legacy entrypoint selects an immutable `SmokeProfile` in the shared runner. Recorded and executed commands use the same profile; it no longer patches lifecycle functions at runtime.

The legacy `scripts/run_multiclient.sh` now accepts `EMBARCADERO_MEMORY_BACKEND=emul` and passes `--emul` to all broker launches and retries, retaining node-1 binding for Corfu too. Its historical default is `real`; a live real run rejects missing DAX/zero-core-node hardware. Custom real hardware must set `EMBARCADERO_CXL_DEVICE` or `EMBARCADERO_CXL_NUMA_NODE` explicitly. The selected backend is recorded in its run-contract CSV. Legacy dry runs show the selected backend and do not invoke cleanup. These changes do not give that older launcher the isolated runner's resource-ownership guarantees; use `tools/dev_cluster.py` for development smoke.

For a matched broker performance comparison with a common audited client, use the separate [DRAM pilot harness](performance-pilot.md). Its larger segments, fixed physical-core assignments, paired protocol, and resource accounting leave this smoke profile unchanged.

Optional production fault tests use a separate build with compile-time hooks:

```sh
cmake -S . -B build/debug-faults -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DEMBARCADERO_ENABLE_FAULT_INJECTION=ON
cmake --build build/debug-faults --target embarlet throughput_test production_fault_driver
python3 test/integration/run_production_faults.py --build-dir build/debug-faults --case control --dry-run
python3 test/integration/run_production_faults.py --build-dir build/debug-faults --case all
```

These optional cases are not ordinary CTest cluster tests. `--case` selects one case; `--help` lists the current matrix. The default `--case all` matrix includes 23 cases, including an unarmed success control, and stops on the first failure. Each attempt owns a new 64 GiB region and retains a manifest, exact binary/configuration hashes, control-event timeline, native prefix observations, placement snapshots and cleanup result. At controlled startup barriers, each child's actual `/proc/PID/exe` hash must match its preflight binary hash; a binary replaced during startup fails the run. The native C++ driver uses production wire structures and generated protobufs; Python sends only local test-controller commands. The three client cases instead use the real audited publisher/subscriber client.

Cases cover fragmented or rejected ingress, shutdown while handshake/ACK-connect/queue admission is blocked, reachable session-fencing schedules, ACK publication, actual session-table exhaustion, segment rollover with delayed writers/readers, finite BLog/GOI capacity, and a missing predecessor token during three-broker memory-copy replication shutdown. A reached barrier is mandatory: a timeout cannot pass a fault case. A successful native marker alone is insufficient; children must meet their declared exit expectations, no forced termination may occur, and the owned region must be removed. These are bounded DRAM correctness tests, with no throughput or media-durability claim. The [fault specification](reviews/2026-09-22-production-fault-plan.md) explains the invariants and remaining coverage; implementation and a runnable command do not themselves establish a live pass.

The `ack_hwm_withheld` client case expects exit code 1 only after the real publisher reports its exact two-message ACK timeout, with no successful delivery audit. Its failure path returns through publisher/subscriber destructors before process-global controls are destroyed. The `session_reopen_resubmit` case requires one real epoch-1 fence, an epoch-2 suffix resubmission, and exactly four original indexed messages. Both are included in `--case all`, alongside `ack_publication_lag`, and require a current fault build containing their named hooks and publisher lifecycle fixes.

`fence_before_commit` deliberately places two client identities in the held epoch to reach the general classification path. The single-client optimization can commit ready work before its later expiry sweep, which is a different legal schedule. The controller checks the selected scanner's epoch and the target client's ready count before releasing expiry; the successful case must ultimately deliver eight approved messages, including the independent control session's two batches.

The fault profile requires 72 GiB free in `/dev/shm`, broker-node memory checked by the shared preflight, and 6 GiB free on the client node. It keeps 256 MiB segments and the full production GOI; storage cases use a 64-slot PBR derived from the native driver's side-effect-free geometry query and stay below 768 MiB of publisher ingress payload. Subscriber replay is additional traffic. After broker startup, an owned 60-second timer covers the entire active case, including blocked controller and native-stage waits; it is canceled before the separate 15-second child shutdown grace period. Removing the large tmpfs region can take additional time. Hooks use inherited local `SOCK_SEQPACKET` endpoints with per-run tokens, never public control listeners, and compile out of default builds. Fake-process harness tests run with `python3 -m unittest discover -s tools -p test_production_faults.py`; the native `--self-test` exercises wire transport, malformed boundaries and truncated frames without a cluster.

The development runner explicitly selects a concurrent indexed audit
(`EMBARCADERO_E2E_AUDIT_MODE=stream`). It verifies payloads and order while
publishing proceeds, then checks final delivery counters again after publisher
completion. Its audit deadline is 20 seconds from audit startup, before the
publish loop. This avoids retaining the whole workload until ACK completion.

The ordered consumer has separate limits: `EMBARCADERO_SUBSCRIBER_RETAINED_BYTES`
(default 256 MiB, environment minimum 32 MiB) bounds retained receive chunks
and carry/fallback allocation capacities;
`EMBARCADERO_SUBSCRIBER_MAX_MESSAGES` (default 262144) bounds allocated message
descriptors, including pooled descriptors, and sparse reorder slots. Exceeding
either produces an explicit terminal delivery error instead of waiting behind
a missing message. These are ordered-retention limits, not a universal process
memory cap: fixed receive buffers and optional latency telemetry are separate.
Library users can supply `OrderedRetentionLimits` at construction.

For a deliberate serial audit, select `EMBARCADERO_E2E_AUDIT_MODE=serial` and
size both limits for the full retained workload. For example, the matched
2 GiB performance protocol uses 3 GiB and 1048576 slots. A slow consumer must
stay within its declared capacity; successful publisher ACKs alone do not
prove successful subscriber delivery.

Ordered consumer views borrow serialized bytes until the next consume call.
Use one consumer per subscriber and read header fields through
`Subscriber::OrderedMessageView` (`PayloadSize()`, `TotalOrder()`, and
`Payload()`), rather than casting the bytes to the shared-memory header types.
TCP fragmentation can place a frame at any byte alignment; those in-memory
header types require 64-byte alignment. The accessors preserve the wire format
and read fields without copying the payload.
