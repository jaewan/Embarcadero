# Experiment launchers

Use one dispatcher for supported local development:

```sh
python3 tools/experiment.py dev --build-dir build/debug --brokers 3 --dry-run
python3 tools/experiment.py workload publishers --build-dir build/debug --clients 2 --dry-run
python3 tools/experiment.py fault --build-dir build/debug-faults --case all
python3 tools/experiment.py perf --help
```

`bash scripts/run_experiment.sh PROFILE ...` is the equivalent shell entrypoint.
With no profile it prints help and starts nothing. The dispatcher replaces itself
with the selected existing runner; arguments, signals, and exit status pass
through directly. It adds no broker lifecycle, workload defaults, environment
translation, retry policy, or result qualification.

| Profile | Existing owner | Scope |
|---|---|---|
| `dev` | [dev_cluster.py](../tools/dev_cluster.py) | Audited 32 MiB local DRAM smoke |
| `workload` | [experiment_workload.py](../tools/experiment_workload.py), using the dev lifecycle | Finite latency telemetry and indexed sender gap; independent publisher acceptance pending |
| `fault` | [run_production_faults.py](../test/integration/run_production_faults.py) | Bounded production fault cases; fault-enabled build required |
| `perf` | [perf_compare.py](../tools/perf_compare.py) | Matched baseline/candidate DRAM pilot and immutable evidence |
| `legacy-startup` | [check_legacy_startup.py](../test/integration/check_legacy_startup.py) | Owned ORDER0/ACK1 startup check, without indexed payload audit |
| `analyze` | [analyze_perf_comparison.py](../tools/analyze_perf_comparison.py) | Read-only analysis of retained pilot runs |
| `legacy` | An inventory launcher below | Explicit access to unchanged historical research behavior |

Owned launch profiles select emulation explicitly, share the existing cluster
lock, use unique regions, and clean up owned process groups and shared memory.
Brokers/memory use NUMA node 1; clients/memory use node 0. They need no SSH client
or NUMA node 2. The 64 GiB mapping and each runner's resource/deadline checks remain
authoritative. See [supported commands](../docs/development-commands.md),
[DRAM requirements](../docs/development-dram.md), and the
[performance protocol](../docs/performance-pilot.md).

## Compatibility inventory

These eleven previously documented launchers now share an early dispatch guard.
Each accepts `--dev-dram`, `--workload-dram`, `--fault-dram`, `--perf-dram`, and
`--legacy-startup`, followed by that runner's options. Dispatch occurs before
historical locks, sourced lifecycle code, host checks, SSH, or cleanup.
`--help` explains the routes; unknown options fail before historical code.

| Launcher | Historical purpose | Name for `experiment.py legacy` |
|---|---|---|
| [singlenode_run_throughput.sh](singlenode_run_throughput.sh) | Local throughput | `singlenode_run_throughput` |
| [run_throughput.sh](run_throughput.sh) | Remote-client throughput | `run_throughput` |
| [run_multiclient.sh](run_multiclient.sh) | Multi-host throughput | `run_multiclient` |
| [run_latency.sh](run_latency.sh) | Latency matrix | `run_latency` |
| [run_throughput_latency_sweep.sh](run_throughput_latency_sweep.sh) | Offered-load sweep | `run_throughput_latency_sweep` |
| [run_latency_vs_load.sh](run_latency_vs_load.sh) | Latency versus load | `run_latency_vs_load` |
| [run_e2e_throughput_benchmark.sh](run_e2e_throughput_benchmark.sh) | Research E2E throughput | `run_e2e_throughput_benchmark` |
| [run_failures.sh](run_failures.sh) | Broker-failure trace | `run_failures` |
| [run_ordering_durability_ladder.sh](run_ordering_durability_ladder.sh) | ORDER/ACK sweep | `run_ordering_durability_ladder` |
| [run_slow_replica_heterogeneity.sh](run_slow_replica_heterogeneity.sh) | Slow-replica experiment | `run_slow_replica_heterogeneity` |
| [publication/run_throughput_matrix.sh](publication/run_throughput_matrix.sh) | Publication matrix | `run_throughput_matrix` |

For example:

```sh
bash scripts/run_latency.sh --dev-dram --build-dir build/debug --dry-run
bash scripts/run_latency.sh --workload-dram latency --build-dir build/debug --target-mibps 128 --dry-run
bash scripts/run_multiclient.sh --workload-dram publishers --build-dir build/debug --brokers 3 --clients 2 --client-brokers '0;0,1,2' --dry-run
bash scripts/run_failures.sh --fault-dram --build-dir build/debug-faults --case control --dry-run
bash scripts/publication/run_throughput_matrix.sh --perf-dram --help
```

These select the named owned profile. A latency launcher's `--dev-dram` route
runs the smoke profile; it does not reproduce its historical latency matrix.
Historical environment variables are not translated into owned runner arguments.
Use explicit options and the resulting manifest to establish what ran.

The workload adapter selects actual existing latency, sender-delay or independent
publisher behavior while reusing owned orchestration. It has separate completion
and telemetry gates; these do not establish equivalence with historical remote
experiments, fixed offered-load studies, or publication matrices. The supported
subset, resource bounds and remaining gaps are listed in
[the command guide](../docs/development-commands.md). Latency and gap passed their
bounded live acceptance. The follower-only publisher attempt failed and remains
in the evidence; publisher acceptance with supported destinations is pending.
Every acknowledged client allowlist must include broker 0 because its current
head-owned ACK/fence channel shares the publish connection. The runner rejects
follower-only lists before any cluster launch and does not add a destination.
Earlier smoke/fault/performance artifacts do not qualify these new modes.

## Historical workflows

Historical bodies, paper scripts, and retained data remain in place. List the
historical inventory without launching anything:

```sh
python3 tools/experiment.py legacy --help
```

To execute the old workflow use `python3 tools/experiment.py legacy run_latency`
or `bash scripts/run_latency.sh --legacy` with its historical environment.
Arguments after the launcher name are passed to its historical body; those
scripts may not implement help or argument validation.
The original environment-only invocation with no arguments also remains
compatible; it is historical behavior and has no owned-runner cleanup guarantee.

Historical workflows may use SSH, machine-specific hardware, host tuning,
shared readiness files, or broad process cleanup. Inspect them before running
them in a dedicated research environment. Existing notes are preserved in
[HISTORICAL_LAUNCHERS.md](HISTORICAL_LAUNCHERS.md) for interpreting older artifacts.
Additional publication, paper, setup, network, plotting, and diagnostic scripts
have not been converted or newly qualified by this dispatch change.

The [support matrix](../docs/support-matrix.md) determines supported contracts.
Local DRAM tests cannot qualify real CXL, persistent-media durability, independent
host failure, or the historical latency/publication workflows.
