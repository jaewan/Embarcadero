## KV Shared-Log Benchmark

This directory contains the KV benchmark used for end-to-end evaluation of shared-log systems in this repo.

The main binary is `build/bin/kv_ycsb_bench`.

Despite the name, this is **not yet a full canonical YCSB implementation**. It is currently a **single-client KV benchmark** that:

- preloads a keyspace,
- issues writes or mixed read/write operations through the shared log,
- waits for local application to the subscriber-side KV state machine,
- reports throughput and latency summaries,
- can target `EMBARCADERO`, `CORFU`, `SCALOG`, or `LAZYLOG` through the same client path.

`bench/kv_store/ycsb_workload.h` exists, but it is not wired into `kv_ycsb_bench` today. So this benchmark should be described as a **YCSB-like KV benchmark**, not a paper-accurate YCSB A-F runner.

## What It Measures

The benchmark has three phases:

1. `Load`
   Preload `record_count` KV pairs into the log and wait until they are applied locally.
2. `Warmup`
   Issue unmeasured writes to warm caches, connections, and broker state.
3. `Run`
   Execute the measured workload and emit a `summary.csv`.

Current workload model:

- `write_ratio=1.0`: write-only
- `write_ratio<1.0`: mixed reads and writes
- `batch_size=1` with `sync_interval=1`: low-latency mode
- `batch_size>1` with `sync_interval=0`: pipelined throughput mode

Reported metrics:

- `throughput_ops_sec`
- `write_throughput_ops_sec`
- `pub_*_us`: client-side publish/enqueue cost
- `apply_*_us`: publish-to-local-apply latency for this client's writes
- `read_*_us`: synchronous local read latency for mixed workloads

For end-to-end comparison across systems, **prefer `apply_*_us` over `pub_*_us`**. `pub_*_us` is only the client-side submission cost.

## Current Correctness Status

What is correct today:

- The benchmark uses one KV state machine and one client path across all sequencer selections.
- The Embarcadero `ORDER=5` path has been fixed and validated for both latency-style and throughput-style runs.
- The read barrier now waits for local application instead of returning early.
- Publisher teardown now exits cleanly on benchmark paths.

What is still a limitation:

- This is **single-client**, not a multi-client benchmark.
- This is **not** the full YCSB A/B/C/D/E/F workload model yet.
- The benchmark currently hardcodes topic creation and cluster teardown behavior for convenience.
- Existing historical baseline numbers in `data/` were mostly produced by the older `throughput_test` / latency harness, not by `kv_ycsb_bench`, so they are useful context but not perfectly apples-to-apples with the new KV benchmark.

Recent latency-harness finding:

- A strict remote publication-mode failure on `throughput_test` was traced to an Embarcadero `ORDER=0` export bug, not to benchmark accounting. The broker could mislabel the actual message header format in `BatchMetadata`, which let bytes arrive while latency parsing lost framing.
- That `ORDER=0` provenance bug is now fixed in Embarcadero and strict single-cell validation reaches `100%` parse coverage with zero parser fallback counters.
- A separate Embarcadero `ORDER=5` remote issue still remains in the bounded strict matrix: only broker 0 delivers subscriber bytes while brokers 1-3 stay idle. That remaining failure is outside `kv_ycsb_bench` itself.

Bottom line:

- It is a reasonable **single-client e2e KV benchmark** for comparing shared-log backends through a common client/state-machine path.
- It is **not yet sufficient by itself** to claim a full paper-grade YCSB comparison suite across Embarcadero, Corfu, Scalog, and LazyLog.

## Build

```bash
cmake -S . -B build
cmake --build build --target kv_ycsb_bench -j
```

Binary:

```bash
build/bin/kv_ycsb_bench
```

## Bring-Up

The benchmark assumes brokers are already reachable through the standard client/broker control plane.

At minimum set:

```bash
export NUM_BROKERS=4
export EMBARCADERO_NUM_BROKERS=4
export EMBARCADERO_REPLICATION_FACTOR=1
export EMBARCADERO_HEAD_ADDR=127.0.0.1
```

For local Embarcadero validation, bring up four brokers first, for example:

```bash
env NUM_BROKERS=4 EMBARCADERO_NUM_BROKERS=4 EMBARCADERO_REPLICATION_FACTOR=1 \
EMBARCADERO_HEAD_ADDR=127.0.0.1 EMBARCADERO_RUNTIME_MODE=latency \
build/bin/embarlet --config config/embarcadero.yaml --head --EMBARCADERO
```

Then start three followers with the same environment but without `--head`.

For `CORFU`, `SCALOG`, and `LAZYLOG`, the benchmark client path already supports those sequencers, but you must ensure their sequencer/coordinator side is running and that the broker cluster is started in the matching mode.

Native order defaults used by the benchmark:

- `EMBARCADERO` -> `order=5`
- `SCALOG` -> `order=1`
- `CORFU` -> `order=2`
- `LAZYLOG` -> `order=2`

You can override with `--order`, but for cross-system comparison the native order above is the intended default.

## How To Run

### Latency-style run

This mode minimizes batching and forces a barrier after each operation.

```bash
build/bin/kv_ycsb_bench \
  --sequencer=EMBARCADERO \
  --record_count=1000 \
  --operation_count=1000 \
  --batch_size=1 \
  --sync_interval=1 \
  --warmup_ops=1000 \
  --latency \
  --log_level=0
```

### Throughput-style run

This mode pipelines writes and only synchronizes at the end.

```bash
build/bin/kv_ycsb_bench \
  --sequencer=EMBARCADERO \
  --record_count=1000 \
  --operation_count=1000 \
  --batch_size=10 \
  --sync_interval=0 \
  --warmup_ops=1000 \
  --log_level=0
```

### Run the same benchmark against a baseline

```bash
build/bin/kv_ycsb_bench \
  --sequencer=CORFU \
  --record_count=1000 \
  --operation_count=1000 \
  --batch_size=10 \
  --sync_interval=0 \
  --warmup_ops=1000 \
  --log_level=0
```

Replace `CORFU` with `SCALOG` or `LAZYLOG` once the corresponding sequencer/broker environment is up.

## Output

Each run writes a directory under `build/results/`:

```text
build/results/<SEQUENCER>_wr<ratio>_rf<rf>_b<batch>_<run_id>/
```

Files:

- `metadata.txt`
- `summary.csv`
- `apply_latency_us.csv` when apply latency tracking is enabled

`summary.csv` columns include:

- workload shape: `sequencer`, `order`, `ack`, `rf`, `record_count`, `operation_count`, `batch_size`, `write_ratio`, `sync_interval`
- throughput: `runtime_sec`, `throughput_ops_sec`, `write_throughput_ops_sec`
- latency: `pub_*_us`, `apply_*_us`, `read_*_us`

## How To Compare Systems Fairly

To make an apples-to-apples comparison, keep all of these fixed:

- same binary: `kv_ycsb_bench`
- same `record_count`
- same `operation_count`
- same `value_size`
- same `batch_size`
- same `sync_interval`
- same `write_ratio`
- same `warmup_ops`
- same `ack`
- same `rf`
- same broker count
- same client placement and network topology

Recommended comparisons:

- Throughput comparison:
  compare `throughput_ops_sec`
- End-to-end write latency comparison:
  compare `apply_p50_us`, `apply_p95_us`, `apply_p99_us`
- Mixed workload comparison:
  compare both `apply_*_us` and `read_*_us`

Do **not** directly compare:

- `kv_ycsb_bench` ops/s numbers to older `throughput_test` MB/s numbers
- `pub_*_us` to old publish-to-deliver latency logs
- single-client results to multi-client matrix results

## Recommended Comparison Workflow

1. Start the cluster for one sequencer mode.
2. Run one latency-style `kv_ycsb_bench` case.
3. Run one throughput-style `kv_ycsb_bench` case.
4. Save the two `summary.csv` files.
5. Repeat for the other sequencers with the same parameters.
6. Build a comparison table from those four pairs of `summary.csv` files.

## Current Evidence In This Repo

What is already validated with `kv_ycsb_bench`:

- `EMBARCADERO`, `order=5`, `ack=1`, `rf=1`
- latency-style run with `batch_size=1`, `sync_interval=1`
- throughput-style run with `batch_size=10`, `sync_interval=0`

What is already present historically in `data/`:

- older throughput/latency harness results for `CORFU`, `SCALOG`, `LAZYLOG`
- older benchmark matrix logs for `EMBARCADERO` and `CORFU`

Those historical logs are useful for context, but they should not be treated as the final answer for the new KV benchmark comparison until the same `kv_ycsb_bench` runs are produced for all four systems.
