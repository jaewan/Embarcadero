# Supported local commands

Start with the [disposable build bootstrap and presets](development-build.md).
These commands use local clients and explicit DRAM emulation; they do not need
SSH clients or the absent NUMA node 2. Brokers use node 1 and clients use node 0.

| Purpose | Command | Evidence |
|---|---|---|
| Inspect a smoke run | `python3 tools/dev_cluster.py --build-dir build/debug --dry-run` | Configuration, layout and command manifest; no cluster allocation |
| Run an audited smoke | `python3 tools/dev_cluster.py --build-dir build/debug --brokers 3` | 32 MiB indexed delivery, ACK1 completion and owned cleanup |
| Check legacy startup | `python3 test/integration/check_legacy_startup.py --build-dir build/debug` | ORDER0/ACK1 startup and completion; no indexed payload audit |
| Exercise production faults | `python3 test/integration/run_production_faults.py --build-dir build/debug-faults --case all` | Explicitly enabled barriers and native prefix/cleanup oracles; separate fault build required |
| Run a paired broker pilot | `python3 tools/perf_compare.py --help` | Requires separately built baseline/candidate, common audited client and complete paired protocol |
| Analyze retained pilot runs | `python3 tools/analyze_perf_comparison.py OUTPUT/index.json --output OUTPUT/analysis.json` | Rejects incomplete or mismatched protocols before qualified comparisons |

The historical entrypoint also provides the same owned smoke route:

```sh
scripts/run_multiclient.sh --dev-dram --build-dir build/debug --brokers 3 --dry-run
```

`--dev-dram` delegates before any research backend checks, lock, SSH, or cleanup.
Forwarded options are validated by `dev_cluster.py`. Running the shell script
without that option retains the environment-driven research workflow and its
machine-specific topology/cleanup. It is not an additional supported local
development lifecycle. Scripts are kept in place so historical experiments
remain inspectable; documentation does not imply every historical mode is a
release-supported configuration.

See [DRAM development](development-dram.md) for resource limits and fault-build
commands, [the performance protocol](performance-pilot.md) before comparing
throughput, and [the support matrix](support-matrix.md) for qualified contracts.
An emulated run cannot establish real-CXL behavior or persistent-media durability.
