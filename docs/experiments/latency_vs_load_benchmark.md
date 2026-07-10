# Latency-vs-load benchmark

## Contract

- Entrypoint: `scripts/run_latency_vs_load.sh`
- Controlled variable: publisher offered load, in MB/s, passed to `throughput_test --target_mbps`
- Default pacing semantics: `PACING_MODE=open_loop`, which maps to the existing latency harness `burst` mode and uses only the target-load scheduler in `LatencyTest`
- Alternate pacing semantics: `PACING_MODE=steady`, which adds the legacy `--steady_rate` pause behavior and must be labeled separately from open-loop runs
- Primary measured latency: `publish_to_deliver_latency` from `delivery_latency_stats.csv`
- Secondary receive-side latency: `publish_to_receive_latency` from `latency_stats.csv`
- Additional measured latency: publisher batch latencies from `pub_latency_stats.csv`
- Measured throughput outputs: achieved offered load, publish goodput, end-to-end goodput, all emitted in `latency_benchmark_summary.csv`

## Controlled variables

- `LOAD_POINTS_MBPS`: whitespace-separated offered-load targets for the x-axis
- `ORDER`, `ACK_LEVEL`, `REPLICATION_FACTOR`, `SEQUENCER`
- `MSG_SIZE`, `TOTAL_MESSAGE_SIZE`, `NUM_BROKERS`, `NUM_TRIALS`
- `PACING_MODE`

## Outputs

- Raw per-trial artifacts remain under:
  `data/latency_vs_load/<tag>/<system>/run_<run_id>/points/<idx>_<target>mbps/raw/...`
- Each trial preserves:
  - `latency_stats.csv`
  - `delivery_latency_stats.csv`
  - `delivery_ordering_assertion.csv`
  - `delivery_stage_breakdown.csv`
  - `cdf_latency_us.csv`
  - `pub_latency_stats.csv`
  - `pub_cdf_latency_us.csv`
  - `stage_latency_summary.csv`
  - `latency_benchmark_summary.csv`
  - `run.log`
- Aggregated outputs:
  - `trial_results.csv`: one row per target/trial
  - `summary.csv`: one row per offered-load point, averaged over trials

## Semantics

- Offered load is open-loop publisher pacing over wire bytes, not only payload bytes. The scheduler counts message payload plus protocol/header padding used by the latency path.
- Goodput is reported separately from offered load so saturation is visible.
- End-to-end latency is publish timestamp to subscriber delivery hand-off through `ConsumeOrdered`, in microseconds.
- Publisher-side latency metrics are batch-granularity metrics and are not interchangeable with end-to-end message-granularity latency.

## Limitations

- If `EMBARCADERO_BATCH_SIZE` is large, low-load latency will be dominated by batching delay. Use a smaller runtime batch size for per-record latency figures and record that choice.
- `PACING_MODE=steady` changes semantics and should not be mixed with `open_loop` in the same figure.
- Overload still needs interpretation by the operator: use achieved offered load and goodput together with latency percentiles to identify saturation.

## End-to-end throughput follow-on

- The matching throughput harness is `scripts/run_e2e_throughput_benchmark.sh`.
- It uses the same deterministic run-directory model and emits `throughput_benchmark_summary.csv`, `trial_results.csv`, and `summary.csv`.
- Keep it separate from latency-vs-load figures and from the existing multiclient publish-throughput publication matrix.
