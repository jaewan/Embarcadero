# Matched DRAM performance comparison

Historical first-tranche campaign: these results are retained unchanged. Later runtime changes require the separate final comparison recorded in the [completion evidence](2026-09-22-completion-results.md); results from the two candidates are not pooled. The coordinated mapping fix has since been implemented and tested.

The experiment compares baseline `ae9959dd2892af821878aa42d4e2c05fcb635466` with the first refactoring tranche. The candidate's source bytes, diff and hashes were archived before compilation. Raw artifacts are retained under `results/refactor-perf/2026-09-22/` (git-ignored); `protocol.json` records the choices before measured runs.

**Outcome:** the three-broker fixed-address profile stayed within the predefined ±5% throughput band on both endpoints. The one-broker audited endpoint narrowly met the 5% nonregression bound, while its publisher-completion result was inconclusive. No endpoint established an improvement. These finite DRAM results do not establish blanket server or whole-stack nonregression, and the full refactoring/release plan remains incomplete. The campaign also exposed an automatic mapping startup failure; the local runner now selects one explicit common address, while the generic startup policy remains release work.

## Protocol

Both versions use fresh, isolated Release/native builds with the same compiler, dependency sources, effective optimization/ISA and disabled latency instrumentation. The previous cached Release build had latency instrumentation enabled and is not the baseline. The earlier portable Debug smoke results are not performance comparison inputs.

| Setting | Value |
|---|---|
| Backend | Explicit DRAM emulation on every broker, fresh unique region per trial |
| Region / segments | 64 GiB / 4 GiB |
| Workload | 2 GiB total application payload, 4 KiB messages, 2 MiB batches |
| Semantics | ORDER5 / ACK1 / RF0, one topic and one publishing session |
| Topologies | One broker and three brokers, analyzed separately |
| Client | Same candidate binary and indexed ordered-delivery audit for both broker versions |
| Client placement | Physical CPUs 0–31, memory node 0 |
| Broker placement | CPUs 128–159, 160–191, 192–223 respectively; memory node 1 |
| Repetitions | One excluded qualification per version, then six pairs alternating AB/BA |
| RTO | Explicit 2,000 ms throughput default; any retransmission invalidates qualification |

CPU assignments are checked against discovered physical-core and SMT topology; actual thread masks and shared-memory placement must agree. Compilation finishes before measured trials. No host-wide tuning or unrelated-process cleanup is performed.

The host has two AMD EPYC 9754 sockets, NUMA nodes 0 and 1, and no node 2. Both builds use GCC 13.3, Release/native optimization, matching effective code-generation options and dependencies, disabled latency instrumentation, and the atomic128 PBR path. Baseline reservation mode is derived from its actual compile flags/macros because it lacks the runtime mode log. THP remains `madvise`, tmpfs THP `never`, governors `schedutil`, boost enabled, and NUMA balancing enabled. No remote clients or physical NIC traffic are part of this loopback experiment.

Even if one broker receives all traffic, the conservative payload-plus-header budget is about 2.0625 GiB, below one 4 GiB segment. This avoids baseline reclamation and rollover defects. Layout validation must establish enough initial segments for the selected broker count. A rollover or retry invalidates this premise and the trial.

The primary CSV field `publish_goodput_mbps` is application **MiB/s**, measured through publisher completion and ACK receipt, including final batch sealing and thread joins. The secondary `e2e_goodput_mbps` additionally includes waiting for remaining ordered delivery and the serial payload audit. That extra interval is not isolated audit CPU time. `ack_wait_ms=0` means the final ACK was present after publisher thread joins, not that ACK latency was zero. Neither throughput metric is a sustained measurement. Initialization is outside both timers; the client's nominal warmup does not guarantee every 4 KiB page was prefaulted. CPU and fault counter scopes are recorded separately.

Every qualified trial must acknowledge and audit all messages, with exact indexed payload order, zero duplicate/parser/export-gap counters, no retransmission or fencing, and participation by every expected broker. Failed trials are retained. Paired analysis cannot discard failures and report only favorable successful runs.

The practical throughput margin is 5%. Paired candidate/baseline ratios, raw values and uncertainty are reported for each topology and metric. A wide interval is inconclusive. Similar end-to-end rates can also reflect a common client bottleneck, so this experiment supports claims only about these broker versions under this client, placement and workload.

## Results

The completed profiles are `n1-v2-run2` and `n3-v3`, each with two excluded qualifications and six measured pairs. All 28 trials in those profiles passed the exact 524,288-message / 2 GiB audit with no retries, fences, rollover, exhaustion or forced shutdown. Every expected broker received traffic, all child exit codes were zero, and owned regions were removed. The earlier `n3-v2` campaign failed during candidate startup in pair four and remains unqualified; none of its samples enter these statistics.

The profiles use different mapping policies: automatic selection for the completed one-broker v2 experiment, and an explicit common address for three-broker v3. Each comparison matches baseline and candidate within its own profile. There is no pooling or scaling comparison between profiles.

Ratios below are geometric means of paired candidate/baseline ratios, not ratios of the two medians. Intervals are exploratory paired Student-t 95% intervals on log ratios, with six pairs and no multiple-comparison correction.

| Brokers | Metric | Baseline median MiB/s | Candidate median MiB/s | Paired change | 95% interval for change | Interpretation at 5% margin |
|---|---|---:|---:|---:|---:|---|
| 1 | Publish through ACK completion | 982.4 | 1128.1 | +7.3% | −17.3% to +39.3% | Inconclusive |
| 1 | Delivery plus indexed audit | 926.2 | 954.2 | +2.9% | −4.6% to +11.1% | Narrowly meets pilot nonregression bound |
| 3 | Publish through ACK completion | 2723.0 | 2721.0 | −0.7% | −4.3% to +3.0% | Within ±5% pilot band |
| 3 | Delivery plus indexed audit | 2379.9 | 2380.1 | −0.5% | −3.6% to +2.8% | Within ±5% pilot band |

The audited one-broker result bounds degradation below 5% only under this small-sample protocol's statistical assumptions. It establishes neither a throughput improvement nor equivalence within ±5%. The primary ACK-completion interval still admits a material regression. Consequently the overall performance gate is not closed by the one-broker result.

One-broker timing illustrates why the endpoints differ. Baseline pair 1 took 2.189 s through publisher completion and 2.299 s through audited delivery; pair 3 took 1.324 s and 2.169 s respectively. A roughly 65% difference in the first rate became about 6% in the second. Delivery waits, retained payload allocation, publisher backpressure and thread joins overlap differently; these observations do not identify a causal broker hot path. The shared client can limit both versions, so even a narrow throughput interval cannot establish nonregression in every server component.

Startup and shutdown were outside the throughput timers. Median startup was approximately 47.8 s for both one-broker versions and 58.7–58.8 s for both three-broker versions. Median graceful shutdown was 11.6 s baseline versus 5.3 s candidate for one broker, and 15.3 s versus 6.9 s for three brokers. These are descriptive lifecycle observations, with no separate significance claim.

The full head mapping was observed as 16,777,216 resident 4 KiB pages on node 1. Startup thread masks and anonymous memory policies matched the requested nodes; the early client snapshot precedes its large allocations, so it does not directly sample every later workload page. The binding policy is inherited and the reviewed client path does not change it. CPU/fault counters in `resource-summary.json` cover the client lifetime and the brokers' client-launch-to-client-exit interval; they are not isolated ACK-interval costs. No cycles-per-message or latency-percentile claim is made.

## Retained failures and protocol amendments

The initial `n1` campaign stopped on a checker defect: it required the separately logged raw ACK counter to equal the authoritative ORDER5 frontier. The ACK thread publishes that frontier before retiring retained batches and incrementing the raw counter, so `Poll()` can legitimately observe a complete frontier and an older raw count. The rejected trial passed the exact indexed delivery audit. Independent source review confirmed the interleaving, and a regression test covers both valid raw-counter lag and an invalid frontier shortfall. The amended checker keeps the exact frontier, target, delivery audit and retry/fence gates; raw count and retention snapshots are diagnostic.

`protocol-amendment-ack-frontier.json` records revision `paired-dram-v2-authoritative-order5-ack`. The original campaign remains unqualified and contributes no samples to the restarted comparison. A subsequent `n1-v2` preflight stopped before starting brokers because the controller inherited a CPU-96-only mask; the restarted `n1-v2-run2` uses normal controller affinity and explicit child placement. No binaries, workload, timing or pair order changed. The analyzer checks protocol revision, executable/configuration identity and consecutive AB/BA ordering before producing qualified statistics.

The `n3-v2` failure is a real startup-availability problem under the automatic mapping policy, not a bad throughput sample. Its saved `broker-0.numa_maps` places the executable at `0x600c73ebf000`, inside the first-choice 64 GiB interval `[0x600000000000, 0x601000000000)`. `MAP_FIXED_NOREPLACE` therefore makes the head fall back to `0x500000000000`; the follower independently succeeds at `0x600000000000`. The candidate's descriptor check correctly rejects unequal mapping bases. The fallback loop also exists in the baseline; the compatibility check is new. This does not prove that the baseline would corrupt this particular offset-based ORDER5 path, but the layout contract still requires equal bases while legacy absolute pointers remain.

The revised local runner passes `EMBARCADERO_CXL_BASE_ADDR=0x400000000000` to every broker and verifies each actual mapping against the log and saved `/proc` data before launching the client. An occupied requested address must still fail safely; no `MAP_FIXED` overwrite or disabled compatibility check is an acceptable workaround. All three-broker qualifications and pairs restart under `paired-dram-v3-fixed-mapping-base`, recorded in `protocol-amendment-mapping-base.json`. The complete one-broker v2 campaign remains evidence for its original profile. Results across these profiles must not be pooled or interpreted as a scaling experiment. Coordinated automatic address selection and actionable startup errors remain release work.

## Evidence and reproduction

- [Separate profile results](../../results/refactor-perf/2026-09-22/profile-results.json), [all qualified-profile trials](../../results/refactor-perf/2026-09-22/trials.csv), and [resource summaries](../../results/refactor-perf/2026-09-22/resource-summary.json).
- [One-broker index](../../results/refactor-perf/2026-09-22/n1-v2-run2/index.json) and [three-broker index](../../results/refactor-perf/2026-09-22/n3-v3/index.json) link to every manifest and raw log. [Campaign inventory](../../results/refactor-perf/2026-09-22/campaign-inventory.json) also identifies retained failed attempts.
- `build-evidence/` contains the compiled candidate source archive, source hashes, patch, matched configure/build logs, dependency/ISA evidence, and exact harness revisions/amendments. Broker/client binaries stayed unchanged throughout the comparisons. Files under `/tmp` are supplementary and may be removed by host cleanup; repository-local evidence is git-ignored and must be packaged deliberately for release.

After the campaigns, the two new tooling test suites were registered with CTest. The final suite passed **40/40**, including 28 Python tooling cases across three entries. [Final verification](../../results/refactor-perf/2026-09-22/final-verification.json) records no production source drift, the post-measurement test-registration-only build-input change, and cleanup of all 45 owned regions from the attempted campaigns, with no recorded broker/client process remaining.

Recompute each profile independently:

```sh
python3 tools/analyze_perf_comparison.py results/refactor-perf/2026-09-22/n1-v2-run2/index.json \
  --output results/refactor-perf/2026-09-22/n1-v2-run2/analysis.json
python3 tools/analyze_perf_comparison.py results/refactor-perf/2026-09-22/n3-v3/index.json \
  --output results/refactor-perf/2026-09-22/n3-v3/analysis.json
python3 results/refactor-perf/2026-09-22/assemble_results.py
```

Each analyzer output marks the absent topology unqualified; use the topology actually present in its index. The assembler exports the independently qualified results without pooling their protocols. Independent review recomputed both profiles’ paired intervals from raw CSVs, logs and manifests, verified qualification exclusions and AB/BA ordering, and checked exact delivery, routing, cleanup and the three-broker fixed mapping addresses.

## Remaining performance and release gates

Resolve one-broker publisher-completion variability with separately specified phase measurements and a workload that provides enough observation time while respecting the baseline's finite-storage limits. Measure latency at matched offered load, small messages/partial batches, multiple clients, and load-generator saturation before making a general server nonregression claim. Do not remove validation or reenable unsafe reuse to improve these numbers.

The [production fault-test plan](2026-09-22-production-fault-plan.md) covers the next correctness work, including deterministic mapping collisions. Safe reclamation, coordinated generic startup, remaining component extraction, clean-machine CI, licensing/provenance, whole-stack/client comparisons, and real CXL validation remain separate gates.
