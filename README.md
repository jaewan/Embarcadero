# Embarcadero

**A distributed shared log over CXL disaggregated memory.**

Embarcadero is a totally-ordered publish/subscribe shared log that exploits CXL disaggregated
memory to break the classic ordering-vs-throughput tradeoff. Log servers append payloads to
per-server logs in shared memory and publish 64-byte metadata records; a sequencer polls those
records over memory loads, assigns global order, and uses a bounded hold buffer to preserve
per-client FIFO and ack ordering — without moving the sequencer onto the write path. It remains
correct on non-coherent CXL 2.0 hardware via single-writer ownership, monotonic updates, and
poll-based state transitions.

On a CXL 2.0 testbed, Embarcadero reaches **18.2 GB/s** append throughput under strong total
ordering (2.2× Scalog, 2.3× LazyLog, 2.8× Corfu) with **1.6 ms P99** append latency.

> Research prototype accompanying the paper in [`Paper/Text/`](Paper/Text/). See
> [`docs/design/EMBARCADERO_DEFINITIVE_DESIGN.md`](docs/design/EMBARCADERO_DEFINITIVE_DESIGN.md)
> for the full design.

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
| `Paper/` | LaTeX manuscript (managed separately, git-ignored). |

## Build

Requires Linux/x86-64 with a C++17 compiler and CMake ≥ 3.20. Dependencies: gRPC/Protobuf
(fetched by CMake), plus Folly, glog, gflags, mimalloc, yaml-cpp, cxxopts, Abseil (installed by
the setup script).

```bash
# 1. Install dependencies (run once, from the repo root)
scripts/setup/setup_dependencies.sh

# 2. Configure & build
cmake -S . -B build
cmake --build build -j

# Binaries are written to build/bin/ (embarlet, throughput_test, kv_ycsb_bench,
# *_global_sequencer, config_test, ...).

# 3. Grant capabilities needed for cgroups / CXL (once per binary)
sudo setcap cap_sys_admin,cap_dac_override,cap_dac_read_search=eip build/bin/embarlet
```

## Run

The first broker started is the **head node** (a rendezvous point for broker discovery):

```bash
# Head broker
build/bin/embarlet --head

# Additional brokers
build/bin/embarlet --follower 'ADDR:PORT'   # or just --follower to auto-discover
```

Cluster experiments (throughput, latency, failures, multi-client) are driven by the scripts in
`scripts/` — most are environment-variable configured. See [`scripts/README.md`](scripts/README.md).

## Test

```bash
ctest --test-dir build --output-on-failure
```

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) for branch conventions, the build/test loop, formatting,
and the "no generated data in git" rule.
