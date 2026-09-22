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

The owned runner explicitly sets `EMBARCADERO_CXL_BASE_ADDR=0x400000000000` for every broker, overriding inherited values. Shared raw pointers require identical virtual bases; independent automatic fallback can choose different bases when PIE address randomization occupies one candidate. The explicit base fails safely on a collision. Coordinating automatic base selection in direct broker launches remains a release follow-up.

The workload is intentionally fixed: one topic, one client process, one sender thread per broker, ORDER5, ACK1, RF0, and 32 MiB of 4 KiB application messages. The client deadline is 30 seconds; its 60-second RTO floor avoids timer retransmissions during a successful smoke. Unexpected retransmissions invalidate the result. Disconnect-driven retries still depend on runtime capacity handling; this runner is not a fault-injection or segment-rollover test. ACK1 and RF0 make no disk-durability claim.

The current shared GOI and completion vector support **one topic identity per region**. Topic creation rejects a second distinct identity before allocating or writing shared metadata, including after deleting the first topic or a failed creation attempt. Use a fresh region for a different topic. This contains shared-state ownership until true multi-topic isolation is implemented; the metadata capacity is not a promise of supported topic count.

Each run gets a private artifact directory and a unique, exclusively created shared-memory inode. Preflight checks ports, node memory, tmpfs space, address-space limits, allowed memory nodes, and cgroup ancestor limits. The fixed control port is 12140 and data ports are 1214 through 1214 + broker count - 1. Runs take a per-user lock through cleanup and reject occupied ports. Other users and old launchers do not take that lock, so concurrent use with those tools is unsupported. No global process kill, host tuning, privilege change, hugepage reservation, or SSH is performed.

The head must finish initialization before followers start. The runner captures thread affinity and `/proc/<pid>/numa_maps`, and checks that the head's shared pages are on node 1. Brokers share all CPUs available on node 1, including available SMT siblings; this is explicitly recorded and is **not** a fixed-core performance experiment. Client buffers request anonymous pages with THP advice instead of HugeTLB. The manifest records the observed host page policies; the NUMA snapshots preserve actual mappings.

The artifact directory keeps commands, binary hashes, revision and dirty-file status, effective configuration, placement observations, logs, and the client CSV. It also records relevant CMake cache settings, the compile-commands hash, and observed PBR reservation modes. Match compiler, build type, native ISA, sanitizer, and reservation mode for performance comparisons: changing native ISA can switch the 128-bit PBR reservation between atomic and mutex implementations. Dirty runs explicitly have incomplete source provenance; use a clean commit for comparison artifacts. If the client finishes before the placement sample, the manifest reports that observation as unavailable.

A successful run requires a positive E2E completion result and the actual ordered-delivery audit: exactly 8192 messages and 32 MiB, contiguous global order, indexed payload sequence 0 through 8191 across broker striping, full remaining-payload comparison, and zero duplicate, parser-error, and export-gap counters. This covers one publisher session; arbitrary multiple sessions, recovery, and fencing require separate protocol and fault tests. A legacy empty-buffer header check cannot satisfy this gate. The small transfer's throughput includes retained parsing and payload-comparison work and is diagnostic; it is not a performance baseline or final CXL evidence.

Cleanup signals only process groups started by this runner, waits for them, and unlinks only its own unchanged shared-memory inode. It preserves logs on failure and reports forced termination as a failed lifecycle gate. SIGINT/SIGTERM use the same cleanup. SIGKILL or host loss cannot execute cleanup; the manifest identifies the run's remaining resources for manual inspection. It does not automatically reclaim another run's resources.

Test orchestration without allocating a real region:

```sh
python3 tools/test_dev_cluster.py
```

The fake-executable tests cover explicit emulation propagation, environment isolation, occupied-port rejection, serialization, dry run, successful cleanup, startup failure, and stubborn-child termination while an unrelated process survives. Optional remote clients and the real-CXL profile are deferred; local development does not depend on `ssh c1`, `ssh c3`, or `ssh c4`.

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

These optional cases are not ordinary CTest cluster tests. `--case` selects one case; `--help` lists the current matrix. The default matrix begins with an unarmed success control and stops on the first failure. Each attempt owns a new 64 GiB region and retains a manifest, exact binary/configuration hashes, control-event timeline, native prefix observations, placement snapshots and cleanup result. The native C++ driver uses production wire structures and generated protobufs; Python sends only local test-controller commands. The T4 case instead uses the real audited publisher/subscriber client.

Cases cover fragmented or rejected ingress, shutdown while handshake/ACK-connect/queue admission is blocked, reachable session-fencing schedules, the legitimate gap between authoritative ACK publication and diagnostic counters, and a missing predecessor token during three-broker memory-copy replication shutdown. A reached barrier is mandatory: a timeout cannot pass a fault case. A successful native marker alone is insufficient; every child must exit normally, no forced termination may occur, and the owned region must be removed. These are bounded DRAM correctness tests, with no throughput or media-durability claim. The [fault specification](reviews/2026-09-22-production-fault-plan.md) explains the invariants and remaining coverage; implementation and a runnable command do not themselves establish a live pass.

The fault profile requires 72 GiB free in `/dev/shm`, broker-node memory checked by the shared preflight, and 6 GiB free on the client node. It keeps 256 MiB segments and the full production GOI. Hooks use inherited local `SOCK_SEQPACKET` endpoints with per-run tokens, never public control listeners, and compile out of default builds. Fake-process harness tests run with `python3 -m unittest discover -s tools -p test_production_faults.py`; the native `--self-test` exercises wire transport, malformed boundaries and truncated frames without a cluster.
