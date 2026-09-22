# Supported local commands

Start with the [disposable build bootstrap and presets](development-build.md).
These commands use local clients and explicit DRAM emulation; they do not need
SSH clients or the absent NUMA node 2. Brokers use node 1 and clients use node 0.

| Purpose | Command | Evidence |
|---|---|---|
| Inspect a smoke run | `python3 tools/experiment.py dev --build-dir build/debug --dry-run` | Configuration, layout and command manifest; no cluster allocation |
| Run an audited smoke | `python3 tools/experiment.py dev --build-dir build/debug --brokers 3 --automatic-mapping` | 32 MiB indexed delivery, ACK1 completion and owned cleanup |
| Check legacy startup | `python3 tools/experiment.py legacy-startup --build-dir build/debug` | ORDER0/ACK1 startup and completion; no indexed payload audit |
| Exercise production faults | `python3 tools/experiment.py fault --build-dir build/debug-faults --case all` | All 20 cases, including real client recovery and withheld ACK; separate fault build required |
| Run a paired broker pilot | `python3 tools/experiment.py perf --help` | Requires separately built baseline/candidate, common audited client and complete paired protocol |
| Analyze retained pilot runs | `python3 tools/experiment.py analyze OUTPUT/index.json --output OUTPUT/analysis.json` | Rejects incomplete or mismatched protocols before qualified comparisons |

`bash scripts/run_experiment.sh PROFILE ...` is an equivalent shell entrypoint.
With no profile, the dispatcher prints help and starts nothing. It uses `exec`
to preserve the selected runner's arguments, signals and exit status. The existing
direct Python entrypoints remain valid and own all lifecycle and validation code.

The [eleven compatibility launchers](../scripts/README.md#compatibility-inventory)
share the same early routes:

```sh
bash scripts/run_multiclient.sh --dev-dram --build-dir build/debug --brokers 3 --dry-run
bash scripts/run_failures.sh --fault-dram --build-dir build/debug-faults --case control --dry-run
bash scripts/publication/run_throughput_matrix.sh --perf-dram --help
```

`--dev-dram`, `--fault-dram`, `--perf-dram`, and `--legacy-startup` delegate before
research backend checks, locks, SSH, or cleanup. The selected runner validates
forwarded options. Historical environment settings are not translated into its
workload. These aliases select the owned profile, not the historical experiment
suggested by a shell script's name; a latency launcher with `--dev-dram` runs the
same audited smoke.

For the historical inventory use `python3 tools/experiment.py legacy --help`.
`legacy NAME` or a shell launcher's `--legacy` explicitly enters its unchanged
research body. Original environment-only invocations with no arguments are also
retained and remain historical. Those routes retain machine-specific topology
and cleanup, and have no owned-runner safety or new qualification claim. See the
[historical notes](../scripts/HISTORICAL_LAUNCHERS.md) when interpreting old commands
and artifacts.

See [DRAM development](development-dram.md) for resource limits and fault-build
commands, [the performance protocol](performance-pilot.md) before comparing
throughput, and [the support matrix](support-matrix.md) for qualified contracts.
An emulated run cannot establish real-CXL behavior or persistent-media durability.
