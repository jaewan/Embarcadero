# Latency-vs-load contract

- Controlled variable: publisher offered load in MB/s via `throughput_test --target_mbps`.
- Pacing semantics: `PACING_MODE=open_loop` maps to the existing latency harness `burst` mode, meaning no extra pause injection beyond the target-load scheduler. `PACING_MODE=steady` maps to `--steady_rate` and must not be mixed with open-loop results in one figure.
- Measured latency: historical runs in this directory sourced `publish_to_deliver_latency` from `latency_stats.csv`; that file now represents `publish_to_receive_latency`. Current delivery-stamped runs use `publish_to_deliver_latency` in `delivery_latency_stats.csv`.
- Measured throughput: achieved offered load, publish goodput, and end-to-end goodput from `latency_benchmark_summary.csv`.
- Raw artifacts: every target/trial keeps its original CSV files and run log under a deterministic point directory.
- Failure policy: the script stops on the first failed target to avoid silently publishing partial or incompatible sweeps.
