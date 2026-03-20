# Scripts

This directory contains the experiment launchers for throughput, latency, and related broker lifecycle workflows.

## Quick Start

Run any script as an environment-driven command:

```bash
VAR1=value VAR2=value scripts/run_throughput.sh
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

### `scripts/run_throughput.sh`

Optimized for large-message throughput tests.

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
NUM_BROKERS=4 TEST_TYPE=5 ORDER=0 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 SEQUENCER=EMBARCADERO scripts/run_throughput.sh
```

```bash
REMOTE_BROKER_HOST=broker \
REMOTE_PROJECT_ROOT=/home/domin/Embarcadero \
EMBARCADERO_HEAD_ADDR=192.168.50.12 \
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

### `scripts/run_ordering_durability_ladder.sh`

Sweeps `ORDER` and `ACK` combinations by calling `run_throughput.sh`.

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
