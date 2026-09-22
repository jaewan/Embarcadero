# Embarcadero tests

The supported CTest suite contains production component tests, protocol/property fixtures, baseline tests, and Python tooling tests. Optional live tests use an owned local DRAM cluster. See [build instructions](../docs/development-build.md) for dependencies and [supported commands](../docs/development-commands.md) for orchestration.

```sh
cmake --preset debug
cmake --build --preset debug --parallel 8
ctest --preset debug

# Inspect first, then run a bounded audited cluster.
python3 tools/dev_cluster.py --build-dir build/debug --brokers 3 --dry-run
python3 tools/dev_cluster.py --build-dir build/debug --brokers 3 --automatic-mapping
```

Run these commands from the repository root. Ordinary CTest does not start a 64 GiB broker cluster or run historical cleanup scripts. Target paths refer to the configured build tree; test dependencies and the expected inventory are checked during configuration.

## Production fault campaign

Use a separate [fault-enabled build](../docs/development-dram.md), then run:

```sh
python3 test/integration/run_production_faults.py --build-dir build/debug-faults --case all
```

All 20 cases include fragmented/malformed ingress, blocked shutdown, fence/commit ordering, authoritative ACK publication, real-client suffix recovery, session/GOI/BLog exhaustion, retained payload across rollover, and a missing replication token. Native drivers use production protocol declarations and read-only region observers; client cases execute the real publisher. A successful helper/model test does not replace a live case. The [fault specification](../docs/reviews/2026-09-22-production-fault-plan.md) states each schedule and its limits.

The runner explicitly selects `--emul` for every broker, creates a unique region, binds brokers/memory to NUMA node 1 and clients/memory to node 0, checks executable provenance, and records owned cleanup. Node 2 and SSH clients are unnecessary. The supported live profile requires **64 GiB**, because the GOI alone reserves 32 GiB; do not shrink the whole mapping to 4–32 GiB. `cxl.size` is authoritative, not deprecated `cxl.emulation_size`. Small-capacity unit fixtures do not allocate the production region.

## Organization and evidence

- Top-level `*_test.cc`: linked production components and narrowly scoped protocol/property fixtures, registered by `CMakeLists.txt`.
- `integration/`: optional native production fault drivers and owned Python runners.
- `../tools/test_*.py`: orchestration, cleanup, provenance, and performance-analysis regressions.
- `e2e/`: historical research scripts and hardware-specific scenarios. They are excluded from ordinary CTest by default; their older launch/cleanup assumptions do not define the supported local workflow.

Add regressions beside the affected production component and register generated executable paths. Prefer a deterministic reached/release barrier to sleeps for concurrency faults. Keep live clusters serialized, bound resources and deadlines, and retain failed artifacts. Never use global process killing or unlink an unowned region in a new test.

ASan/UBSan and TSan have separate build presets. A configured sanitizer or CI workflow is not evidence of an executed successful run. DRAM tests do not establish CXL cache visibility, media durability, safe reclamation, sequencer replacement, or independent-host failure tolerance. See the [support matrix](../docs/support-matrix.md) and [completion ledger](../docs/reviews/2026-09-22-refactoring-completion-plan.md) for the recorded qualification scope.
