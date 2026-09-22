# Matched DRAM broker pilot

`tools/perf_compare.py` compares broker binaries using the **same candidate client binary** for both versions. This is a finite-transfer development experiment on DRAM. It does not establish sustained throughput, whole-stack improvement, remote-client performance, or real-CXL behavior.

Build both brokers in Release with equivalent compiler, optimization, native ISA, definitions, and instrumentation settings. The baseline must be a clean checkout of `ae9959dd2892af821878aa42d4e2c05fcb635466`. The candidate requires `--print-layout` and the indexed ordered-delivery audit. Capture build/source/dependency evidence alongside the binaries; the harness can check and copy the matched build attestation JSON with `--build-evidence`.

```sh
python3 tools/perf_compare.py \
  --baseline-build /path/to/baseline-build \
  --candidate-build /path/to/candidate-build \
  --build-evidence /path/to/matched-build-evidence.json \
  --brokers 1 --output results/refactor-perf/n1-dry --dry-run

python3 tools/perf_compare.py \
  --baseline-build /path/to/baseline-build \
  --candidate-build /path/to/candidate-build \
  --build-evidence /path/to/matched-build-evidence.json \
  --brokers 1 --output results/refactor-perf/n1
```

Repeat with `--brokers 3` and a new output directory. Existing output paths are never overwritten. Runs use the isolated developer runner's lock and owned process-group cleanup, so only one cluster runs at a time. No live integration experiment belongs in ordinary unit-test execution.

The default topology experiment first runs one excluded qualification for each broker version, then six adjacent pairs in alternating AB/BA order. Each member gets a fresh, owner-only 64 GiB shared-memory region, 4 GiB segments, and explicit `--emul` on every broker. Both versions receive identical configuration and environment. The baseline lacks the v5 descriptor and layout CLI; the harness records that its geometry is checked with the candidate layout calculation, supported by source review of the unchanged offsets. A cluster never mixes broker versions.

Current protocol `paired-dram-v3-fixed-mapping-base` sets `EMBARCADERO_CXL_BASE_ADDR=0x400000000000` for every broker through the existing override supported by both binaries. Before starting the client, every broker's mapping log and saved `/proc/<pid>/numa_maps` must identify that base for the owned region. An address collision fails safely instead of selecting a different fallback. This changes orchestration only; broker/client binaries remain unchanged. Automatic base negotiation for direct broker launches remains a production follow-up.

The workload is **2 GiB total application payload**, 4 KiB messages, ORDER5, ACK1, RF0, and one sender per broker. It stays below one 4 GiB payload segment per broker on a successful run. `--payload-mib 32` can make a smaller compatibility experiment, but such artifacts cannot be pooled with the default workload. `--qualification-only` stops after the two excluded runs; use a separate directory. Qualification uses the declared workload size, not a hidden smaller warmup.

CPU assignments are fixed physical cores: client CPUs 0–31 on NUMA node 0; broker 0 CPUs 128–159, broker 1 CPUs 160–191, and broker 2 CPUs 192–223 on node 1. Preflight checks actual socket/core identities, allowed affinity, NUMA membership, and absence of selected SMT siblings. Memory binds to the respective node. The audit retains payload copies, so the client has at least 6 GiB of node-0 memory headroom in addition to the broker region and per-broker budgets. Thread affinity and resident anonymous/shared mapping placement are observed through `/proc`.

RTO is explicitly 2000 ms for both versions. ACK timeout is 60 seconds, the current indexed audit has a 20-second deadline, and the outer client limit is 90 seconds. Each broker has a bounded startup allowance; both versions get 60 seconds for graceful shutdown. Startup and shutdown are recorded separately from throughput. A failure preserves its artifacts and stops the experiment; failed samples are not silently retried or discarded.

Every successful run must have full global ACK completion, exact indexed payload/order audit, zero duplicates/parser errors/export gaps, zero retransmissions or fencing, no segment rollover/exhaustion, and traffic through every selected broker. Per-broker sent counts must sum to the workload. ORDER5's ACK frontier is global, so per-broker ACK-socket deltas are not equated to that broker's sent count. The candidate's observed PBR mode must match the compile-derived native atomic128 path; the baseline mode is derived from its actual Topic flags/macros because it lacks the runtime log. Process exit codes, graceful cleanup, and removal of the owned region are qualification gates too.

The ACK rule introduced in `paired-dram-v2-authoritative-order5-ack` remains in v3: check `ACK_VERIFY.normalized_received` and `target` against the exact workload. Its separately sampled `raw_received` counter is diagnostic: the ACK thread publishes the authoritative ORDER5 frontier before incrementing that counter, so a valid completed run can print a lower raw count. The one-shot unacked-ledger snapshot can similarly precede retirement. Exact indexed delivery and the zero-retry/fence gates remain mandatory. The original campaign stopped by the incorrect raw-counter predicate remains unqualified.

The three-broker v2 campaign stopped during pair 4 candidate startup: PIE address randomization occupied part of `[0x600000000000, 0x601000000000)` in the head, which fell back to `0x500000000000`, while a follower independently mapped at `0x600000000000`. The region descriptor correctly rejected the disagreement before shared metadata use. Those failed/incomplete v2 artifacts remain unchanged. The entire three-broker experiment restarts, including both qualifications and all pairs, in `n3-v3`; there is no selective replacement of the failed run. The completed `n1-v2-run2` profile remains separate and must not be pooled with v3. The explicit base and protocol revision participate in every comparison fingerprint.

The primary metric, `ack_completion_mib_s`, is application MiB/s from the publish loop through `Publisher::Poll` ACK completion. `audit_inclusive_e2e_mib_s` additionally includes the serial indexed payload audit after publication. Both use the same audit-enabled client, whose retained parsing and payload copies can affect the publish interval as well. The CSV's historical `*_mbps` names actually represent MiB/s here.

Manifests retain binary/configuration hashes, compiler flags and macros, source patches and untracked source files, build evidence, commands/environment, placement, page policies and governors, pool/credit observations, ACK/audit counters, failures, and execution chronology. Broker CPU time and faults are differences from immediately before client launch to client exit, excluding the 64 GiB startup prefault. That interval includes client initialization and auditing. Exact client `wait4` resource usage covers its whole lifetime. These counters are not cycles per message or measurements limited to the ACK interval.

Analyze preserved artifacts without running a cluster:

```sh
python3 tools/analyze_perf_comparison.py results/refactor-perf/n1/index.json \
  --output results/refactor-perf/n1/analysis.json
```

The analyzer checks paired identity and chronology before reporting paired ratios and exploratory confidence intervals. Failed qualification or incomplete pairs remain unqualified. Six pairs provide limited evidence; inspect all raw results and failure artifacts alongside the intervals.

Exercise the harness using fake executables, without a real shared mapping:

```sh
python3 -m unittest discover -s tools -p test_perf_compare.py
```
