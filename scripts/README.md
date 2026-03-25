# Scripts

This directory contains the experiment launchers for throughput, latency, and related broker lifecycle workflows.

## Quick Start

Run any script as an environment-driven command:

```bash
VAR1=value VAR2=value scripts/singlenode_run_throughput.sh
```

Most scripts read their options from environment variables instead of flags. That keeps them easy to compose in shell loops and CI jobs.

## Common Options

### Cluster / broker control

- `NUM_BROKERS`: broker count to start or reuse.
- `SEQUENCER`: `EMBARCADERO`, `CORFU`, or `SCALOG` depending on the script.
- `BROKER_CONFIG`: broker config path, default `config/embarcadero.yaml`.
- `CLIENT_CONFIG`: client config path, default `config/client.yaml`.
- `FORCE_RESTART_BROKERS=1`: stop and relaunch brokers instead of reusing healthy ones.
- `BROKER_READY_TIMEOUT_SEC`: remote/local startup timeout for broker readiness.
- `BROKER_POLL_INTERVAL_SEC`: polling interval while waiting for broker readiness.

### Remote mode

Set these to run scripts from a client node while brokers live on a broker node:

- `REMOTE_BROKER_HOST`: SSH alias or host name for the broker node. Set this to enable remote orchestration.
- `REMOTE_PROJECT_ROOT`: repository path on the broker host.
- `REMOTE_BUILD_BIN`: optional override for the broker host binary directory.
- `EMBARCADERO_HEAD_ADDR`: broker-node IP or host name that clients should connect to.

Remote mode reuses healthy brokers when possible. Use `FORCE_RESTART_BROKERS=1` when you want a clean cluster start.

## Throughput Script

### `scripts/singlenode_run_throughput.sh`

Single-node throughput launcher. This is the original local workflow where the script starts local brokers and the client runs on the same machine.

Common options:

- `NUM_TRIALS`: number of benchmark trials.
- `TOTAL_MESSAGE_SIZE`: total bytes to publish.
- `MESSAGE_SIZE`: message size in bytes.
- `TEST_TYPE`: throughput-test mode.
- `ORDER`: ordering mode.
- `ACK`: ack level.
- `REPLICATION_FACTOR`: replication factor for ack/replication experiments.
- `THREADS_PER_BROKER`: overrides the default thread selection.
- `SEQUENCER`: broker sequencer mode.

Examples:

```bash
NUM_BROKERS=4 TEST_TYPE=5 ORDER=0 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 SEQUENCER=EMBARCADERO scripts/singlenode_run_throughput.sh
```

### `scripts/run_throughput.sh`

Remote-client throughput launcher. This is the multi-node workflow where the client runs here and brokers are managed on a remote broker node over SSH.

Remote-only requirements:

- `REMOTE_BROKER_HOST`
- `EMBARCADERO_HEAD_ADDR`

```bash
REMOTE_BROKER_HOST=broker \
REMOTE_PROJECT_ROOT=/home/domin/Embarcadero \
EMBARCADERO_HEAD_ADDR=10.10.10.10 \
NUM_BROKERS=4 TEST_TYPE=5 ORDER=0 ACK=1 \
TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 SEQUENCER=EMBARCADERO \
scripts/run_throughput.sh
```

## Latency Scripts

### `scripts/run_latency.sh`

Runs the latency benchmark matrix across the configured modes and sequencers.

Common options:

- `NUM_BROKERS`
- `TEST_CASE`
- `MSG_SIZE`
- `ACK_LEVEL`
- `TOTAL_MESSAGE_SIZE`
- `NUM_TRIALS`
- `RUN_ID`
- `PLOT_RESULTS=1` to generate plots after the run
- `TARGET_MBPS`
- `MODES`: whitespace-separated list such as `steady burst`

### `scripts/run_throughput_latency_sweep.sh`

Runs a throughput-vs-latency sweep over a list of offered-load targets.

Common options:

- `NUM_BROKERS`
- `MESSAGE_SIZE`
- `TOTAL_BYTES`
- `ORDER`
- `ACK`
- `REPLICATION_FACTOR`
- `SEQUENCER`
- `THREADS_PER_BROKER`
- `SWEEP_TARGETS`: whitespace-separated MB/s targets
- `POINT_MAX_ATTEMPTS`
- `STRICT_BROKER_COUNT=1` to reject runs that do not use all brokers

## Experiment Wrappers

### `scripts/run_failures.sh`

Runs the broker-failure benchmark locally: starts a 4-broker cluster, kills one broker mid-run, records real-time throughput, writes failure events, and generates a plot.

Common options:

- `NUM_BROKERS`
- `NUM_BROKERS_TO_KILL`
- `FAILURE_PERCENTAGE`
- `NUM_TRIALS`
- `TOTAL_MESSAGE_SIZE`
- `MESSAGE_SIZE`
- `ORDER`
- `ACK`
- `SEQUENCER`
- `THREADS_PER_BROKER`
- `CLIENT_TIMEOUT`
- `EMBARCADERO_RUNTIME_MODE`
- `EMBARCADERO_ACK_TIMEOUT_SEC`
- `EMBARCADERO_FAILURE_QUEUE_SIZE_MB` or `EMBARCADERO_FAILURE_QUEUE_SIZE_BYTES`

Outputs:

- `data/failure/real_time_acked_throughput.csv`
- `data/failure/failure_events.csv`
- `data/failure/failure.png`
- `data/failure/failure.pdf`
- per-trial archives such as `data/failure/real_time_acked_throughput_trial1.csv`

The script now prints both the benchmark's reported end-to-end `Bandwidth` and a trimmed active-window throughput summary, which is usually the more representative metric for the failure/re-route phase.

`plot_failure.py` also supports `--keep-full-tail` when you want to inspect the full post-failure drain/tail instead of the trimmed active window.

Example:

```bash
ORDER=5 ACK=1 NUM_BROKERS=4 NUM_BROKERS_TO_KILL=1 FAILURE_PERCENTAGE=0.5 \
TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 NUM_TRIALS=3 \
scripts/run_failures.sh
```

### `scripts/run_multiclient.sh`

Multi-client throughput orchestration script. Starts brokers locally, then launches up to five physical client machines in parallel using a NTP-synchronized future-timestamp barrier so all clients begin sending simultaneously.

**Client roster** (cumulative; each level adds one machine):

| `NUM_CLIENTS` | Active clients | NUMA binding |
|:---:|---|---|
| 1 | c4 | node 1 |
| 2 | c4, local (broker node) | node 1, node 0 |
| 3 | c4, local, c3 | node 1, node 0, node 1 |
| 4 | c4, local, c3, c2 | node 1, node 0, node 1, node 1 |
| 5 | c4, local, c3, c2, c1 | node 1, node 0, node 1, node 1, node 1 |

`TOTAL_MESSAGE_SIZE` is divided equally across all active clients; each client sends `TOTAL_MESSAGE_SIZE / NUM_CLIENTS` bytes.

Configuration options:

| Variable | Default | Description |
|---|---|---|
| `NUM_CLIENTS` | `1` | Number of clients to activate (1–5) |
| `NUM_BROKERS` | `4` | Brokers to start locally |
| `NUM_TRIALS` | `3` | Number of benchmark trials |
| `TRIAL_MAX_ATTEMPTS` | `3` | Retry attempts per trial on failure |
| `TOTAL_MESSAGE_SIZE` | `8589934592` | Combined bytes across all clients (8 GiB) |
| `MESSAGE_SIZE` | `1024` | Per-message size in bytes |
| `THREADS_PER_BROKER` | `4` | Publisher threads per broker connection |
| `TEST_TYPE` | `5` | Throughput-test mode (5 = publish-only) |
| `ORDER` | `0` | Ordering mode |
| `ACK` | `1` | Ack level |
| `REPLICATION_FACTOR` | `0` | Replication factor |
| `SEQUENCER` | `EMBARCADERO` | Sequencer type |
| `EMBARCADERO_HEAD_ADDR` | `10.10.10.10` | Broker node IP |
| `START_DELAY_SEC` | `8` | Lead time (seconds) for SSH launch + clock settling |
| `EMBARCADERO_ORDER0_FAST_PATH` | `1` | Enable Order-0 fast path |
| `EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES` | `524288` | Client send chunk size (bytes) |
| `EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE` | `1` | Enable `MSG_MORE` on payload sends |
| `EMBARCADERO_BATCH_SIZE` | `524288` | Client batch size (bytes) |
| `EMBARCADERO_CLIENT_PUB_BATCH_KB` | `512` | Client publish batch (KiB) |
| `EMBARCADERO_NETWORK_IO_THREADS` | `4` | Client network I/O threads |

Logs for each trial are written to `multiclient_logs/trial<N>_<host>.log`.

Examples:

```bash
# 1 client — c4 alone
NUM_CLIENTS=1 scripts/run_multiclient.sh

# 2 clients — c4 + local, larger messages
NUM_CLIENTS=2 MESSAGE_SIZE=8192 scripts/run_multiclient.sh

# Full 5-client sweep, 5 trials, 16 GiB total load
NUM_CLIENTS=5 NUM_TRIALS=5 TOTAL_MESSAGE_SIZE=17179869184 \
    scripts/run_multiclient.sh

# Compare without MSG_MORE
NUM_CLIENTS=3 EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE=0 \
    scripts/run_multiclient.sh
```

**Prerequisite:** all cluster machines must have NTP-synchronized clocks before running.
Use `scripts/setup/sync_clocks.sh` if they are not already synced.

**Aggregation note:** the per-trial totals printed at the end are the naive sum of
each client's self-reported average bandwidth. For peer-reviewable results, collect
the per-client time-series CSVs and calculate throughput within the overlapping
steady-state send window instead.

**TODO (pending test runs):**
- [ ] Test with 1 client (c4 alone)
- [ ] Test with 2 clients (c4 + local)
- [ ] Test with 3 clients (c4 + local + c3)
- [ ] Test with 4 clients (c4 + local + c3 + c2)
- [ ] Test with 5 clients (c4 + local + c3 + c2 + c1)
- [ ] Verify aggregate throughput using time-series overlapping window analysis

### `scripts/run_ordering_durability_ladder.sh`

Sweeps `ORDER` and `ACK` combinations by calling `singlenode_run_throughput.sh`.

Common options:

- `ORDERS`
- `ACK_LEVELS`
- `NUM_BROKERS`
- `TEST_TYPE`
- `TOTAL_MESSAGE_SIZE`
- `MESSAGE_SIZE`
- `THREADS_PER_BROKER`
- `TRIAL_MAX_ATTEMPTS`
- `EMBARCADERO_ACK_TIMEOUT_SEC`
- `ACK2_REPLICATION_FACTOR`
- `NON_REPLICATED_RF`

### `scripts/run_slow_replica_heterogeneity.sh`

Injects a temporary slowdown into one broker and compares the baseline versus slowed run.

Common options:

- `NUM_BROKERS`
- `SEQUENCER`
- `ORDER`
- `ACK`
- `MESSAGE_SIZE`
- `TOTAL_MESSAGE_SIZE`
- `THREADS_PER_BROKER`
- `TEST_TYPE`
- `SLOW_BROKER_INDEX`
- `INJECT_AFTER_SEC`
- `PAUSE_SEC`
- `POINT_MAX_ATTEMPTS`

## Tips

- Keep local mode and remote mode separate in your shell history so it is obvious when a broker node is being managed over SSH.
- If a remote cluster is left in a bad state, rerun with `FORCE_RESTART_BROKERS=1`.
- For remote runs, use the broker node IP in `EMBARCADERO_HEAD_ADDR`, not `127.0.0.1`.
- On the current host, `192.168.50.12` and `192.168.60.8` are on `1GbE` NICs. The `100GbE` interface currently has no usable IPv4 address assigned, so the scripts cannot target it until that interface is addressed and routed.
