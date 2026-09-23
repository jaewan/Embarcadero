# Embarcadero

**A distributed shared log over CXL disaggregated memory.**

Embarcadero is a totally ordered publish/subscribe shared log designed for CXL
disaggregated memory. Brokers append payloads to per-broker logs in shared memory;
a sequencer observes progress records and assigns global order without receiving
the payload. The supported ORDER5 profile is a bounded, single-host research
prototype: it retains primary-log data and stops admission at capacity. Brokers
require the same virtual mapping address because some shared records still
contain process pointers.

The paper's CXL performance results describe its evaluated snapshot and hardware.
The refactored code has passed finite local DRAM and NUMA-node-2 correctness checks,
but its one-broker throughput nonregression and physical-CXL performance remain
unqualified. See the [supported modes](docs/support-matrix.md),
[latest paired measurements](docs/reviews/2026-09-23-workload-followup.md), and
[evidence availability](docs/reviews/evidence/README.md). The manuscript in
`Paper/Text/` is managed separately and is not part of this checkout.

## Repository layout

| Path | Contents |
|------|----------|
| `src/` | Core system. Modules: `embarlet/` (broker), `cxl_manager/` (CXL coordination + sequencers), `disk_manager/` (replication), `network_manager/`, `client/` (pub/sub library), `common/`, `protobuf/`. |
| `benchmarks/` | `kv_store/` (end-to-end KV), `micro/` (Google Benchmark), `sequencer/` (algorithm micro-bench), `sequencer5_ablation/` (Order-5 ablation). |
| `test/` | Unit + end-to-end / integration tests (CTest). |
| `config/` | YAML deployment configs (broker/client, scaling variants). |
| `scripts/` | Experiment launchers, cluster setup, plotting — see [`scripts/README.md`](scripts/README.md). |
| `docs/` | Design, evaluation, and operational docs — see [`docs/README.md`](docs/README.md). |
| `results/` | Generated experiment output (git-ignored). |
| `Paper/` | Separately managed LaTeX manuscript; absent from public checkouts. |

## Build

Requires Linux/x86-64, C++17, CMake ≥ 3.20, Ninja, and the dependencies in the
[build guide](docs/development-build.md). Use its disposable Ubuntu 24.04
bootstrap for a clean dependency environment.

```bash
cmake --preset debug
cmake --build --preset debug -j 8
ctest --preset debug
```

## Run

For a local, owned DRAM smoke on a host with NUMA nodes 0 and 1, run:

```bash
python3 tools/dev_cluster.py --build-dir build/debug --dry-run
python3 tools/dev_cluster.py --build-dir build/debug
```

The [local development guide](docs/development-dram.md) gives prerequisites,
resource requirements, a three-broker run, and cleanup behavior. Research
launchers have different host assumptions; consult [their inventory](scripts/README.md)
before using them.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for branch conventions, the build/test loop, formatting,
and the "no generated data in git" rule.

## License

Apache-2.0; see [LICENSE](LICENSE) and [NOTICE](NOTICE). Third-party material
retains its own licenses and notices.
