# Supported local commands

Start with the [disposable build bootstrap and presets](development-build.md).
These commands use local clients and explicit DRAM emulation; they do not need
SSH clients or the absent NUMA node 2. Brokers use node 1 and clients use node 0.

| Purpose | Command | Evidence |
|---|---|---|
| Inspect a smoke run | `python3 tools/experiment.py dev --build-dir build/debug --dry-run` | Configuration, layout and command manifest; no cluster allocation |
| Run an audited smoke | `python3 tools/experiment.py dev --build-dir build/debug --brokers 3 --automatic-mapping` | 32 MiB indexed delivery, ACK1 completion and owned cleanup |
| Inspect finite latency telemetry | `python3 tools/experiment.py workload latency --build-dir build/debug --target-mibps 128 --dry-run` | Plans the existing latency client; strict delivered counts/UID population/global positions when executed |
| Inspect a sender gap | `python3 tools/experiment.py workload gap --build-dir build/debug --gap-ms 1 --dry-run` | Plans a logged one-shot sender delay plus indexed delivery audit |
| Inspect independent publishers | `python3 tools/experiment.py workload publishers --build-dir build/debug --brokers 3 --clients 2 --target-mibps 16,32 --client-brokers '0;0,1,2' --dry-run` | Plans separate processes with a shared ready/go barrier, rates and destination allowlists |
| Check legacy startup | `python3 tools/experiment.py legacy-startup --build-dir build/debug` | ORDER0/ACK1 startup and completion; no indexed payload audit |
| Exercise production faults | `python3 tools/experiment.py fault --build-dir build/debug-faults --case all` | All 23 cases, including real client recovery, withheld ACK and repeated rejected connections; separate fault build required |
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
bash scripts/run_latency.sh --workload-dram latency --build-dir build/debug --target-mibps 128 --dry-run
bash scripts/run_multiclient.sh --workload-dram publishers --build-dir build/debug --clients 2 --dry-run
bash scripts/run_failures.sh --fault-dram --build-dir build/debug-faults --case control --dry-run
bash scripts/publication/run_throughput_matrix.sh --perf-dram --help
```

`--dev-dram`, `--workload-dram`, `--fault-dram`, `--perf-dram`, and `--legacy-startup` delegate before
research backend checks, locks, SSH, or cleanup. The selected runner validates
forwarded options. Historical environment settings are not translated into its
workload. These aliases select the owned profile, not the historical experiment
suggested by a shell script's name; a latency launcher with `--dev-dram` runs the
same audited smoke.

All three profiles passed bounded live acceptance on source 14, including
publishers with the supported `0;0,1,2` destination sets. The
`../results/refactor-followup/2026-09-22/independent-source14-workload-audit.json`
checks actual executable identities, completion and owned cleanup. The earlier
follower-only publisher failure remains in the
`../results/refactor-followup/2026-09-22/independent-owned-workload-audit.json`.
These profiles reuse the smoke runner's broker startup, lock, 64 GiB region,
NUMA placement and owned cleanup.
Each uses ORDER5/ACK1/RF0, 4 KiB messages and 1–32 MiB per process, with at most
four processes and 128 MiB total. The whole client phase is bounded at 60 seconds;
node 0 must have 3 GiB free per process. Log output is capped at 64 MiB, including
shutdown logs. Remove `--dry-run` to execute once; failures are retained without
automatic retry.

`latency` runs the existing `-t 2` client and forces strict delivery validation
(`EMBARCADERO_LATENCY_ACK_PRIMARY=0`). It checks every planned delivery sample,
UID population and contiguous global position, and records achieved offered load
and delivery percentiles. It does not compare all payload bytes or prove UID order
within a session; `FifoCheckedMessages` is recorded explicitly. Publisher ACK
latency instrumentation is not requested. The native target option is named
`--target_mbps`, but its arithmetic uses MiB/s over padded payload plus native
message-header bytes. The payload timestamp precedes pacing, so reported delivery
latency includes that scheduler wait. `--steady-rate` additionally applies the
historical four-batch flush and 1.5 ms pause policy. This finite telemetry is not a
fixed-offered-load latency comparison or sustained-load qualification.

`gap` runs the indexed E2E client with two sending threads and one 1–10 ms delay at
batch 0. Both delay markers and exact indexed delivery must pass with no fence or
retransmission. A marker proves the delay happened; it does not prove overtaking
or emulate a statistical key-skew distribution. This mode does not accept pacing
options because the indexed E2E path currently ignores them.

`publishers` runs separate publish-only processes on one topic. Each has its own
output directory, payload size, rate and broker allowlist; use comma-separated
`--payload-mib` and `--target-mibps` values, and semicolon-separated
`--client-brokers` lists. A scalar payload/rate applies to every process. The real
client's ready files gate a shared future GO timestamp after setup; validation
matches that timestamp, records actual start spread, checks distinct client IDs,
and requires exact per-client authoritative ACK completion and allowed routing.
No subscriber payload audit is claimed. Destination restriction can represent a
hot-broker placement; it does not guarantee a statistical distribution among all
allowed brokers. Rate 0 is unpaced; positive rates must fit a bounded 15-second
planned send, and permit at most three decimal digits.

Every client allowlist must include broker 0. The current ORDER5 acknowledged
session protocol obtains the head-owned ACK/fence channel through a publish
connection, so follower-only routing cannot complete. The retained `0;1,2`
attempt reached the common GO barrier, but the second client timed out with
zero ACK progress; it is a failed run, not workload qualification. The runner
rejects that configuration before starting a cluster and never silently adds
head-directed payload. Use `0;0,1,2` for different supported destination sets.
Direct ORDER5 acknowledged clients also reject a nonzero implicit
`EMBARCADERO_ORDER5_HOME_BROKERS` policy unless an explicit allowlist containing
broker 0 overrides it: hashing a subset can omit the head. Owned profiles clear
inherited experiment settings and always supply an explicit supported allowlist.

| Historical operation | Owned local subset | Still historical or unqualified |
|---|---|---|
| Single-node, remote-client, E2E and publication throughput launchers | `dev`, `workload publishers`, and the separate paired `perf` protocol | Remote hosts, full publication matrices, different sequencers and arbitrary ORDER/ACK combinations |
| Latency, throughput/latency sweep and latency-versus-load launchers | One explicit `workload latency` point with original pacing semantics | Matched offered-load studies, repeated sweeps, ACK-stage instrumentation and published percentiles |
| Multi-client launcher | Independent local `workload publishers`, per-client rates/bytes/destinations; `workload gap` for one indexed sender gap | Remote failure domains, aggregate subscriber FIFO/payload audit, statistical skew and arbitrary session-gap campaigns |
| Failure, durability-ladder and slow-replica launchers | Enumerated `fault` cases for their stated bounded invariants | Arbitrary broker-failure traces, disk latency/heterogeneity and durability qualification |

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
