# Release checkpoint against the original PR01–12 plan — September 24, 2026

This checkpoint supersedes the hardware-status statements in the dated
[September 22–23 closure audit](2026-09-22-plan-closure-audit.md). The selected
contract remains a **bounded, single-topic, single-host research prototype**:
retain primary-log data and stop admission safely at capacity. The qualified
measurement profile is four brokers, ORDER5/ACK1/RF0, finite 4 KiB messages.
Other research modes remain governed by the [support matrix](../support-matrix.md).

| Original plan unit | Present state | Release interpretation |
| --- | --- | --- |
| PR01–02: owned development and trustworthy builds/tests | Delivered for local DRAM; current Release build and 55/55 supported CTests pass. | Remote CXL now has an owned finite profile, but no automated remote native-client build or hosted CI execution is claimed. |
| PR03–09: geometry, retention, capacity, sessions, transport, topic containment, baselines | Delivered to the bounded contract and tested as described in the closure audit. The epoch-seal scanner race was fixed at `f754c906` with a failing-before/passing-after concurrent regression. | No log recycling, multi-topic admission, physical writer fencing, or general crash-stable recovery. |
| PR10: production extraction and performance nonregression | Extraction delivered. Ten paired four-broker local real-CXL runs measured fixed/old geometric throughput ratio 0.99991 (95% interval 0.9816–1.0186). | Supports maintained throughput only for the tested local profile. One-broker primary ACK throughput remains statistically inconclusive. |
| PR11: shared experiment tooling | Supported local routes and one parameterized, owned remote CXL transfer route delivered. Historical full matrices and remote machine provisioning are not migrated. | The remote route records client/configuration hashes and executed identities, but binary hashes alone do not attest the native client source revision. |
| PR12: open-source release | Apache-2.0 foundation, documentation, CI definition, local clean bootstrap, finite CXL correctness/performance evidence, and scoped claims exist. | Branch integration, hosted CI evidence, remote-source provenance, broader reliability and research-mode qualification remain open. |

Real CXL returned as memory-only NUMA node 2. The earlier statement that CXL
hardware was unavailable no longer applies. The [paired local campaign]
(2026-09-23-cxl-before-after.md) passed 30/30 fixed-broker 10 GiB zero-stall
trials across its two campaigns, but zero in thirty gives only an approximate
9.5% one-sided 95% upper failure-rate bound under independent identical trials.
The demonstrated epoch-seal race was pre-existing in the retained baseline;
the first trigger of the earlier natural missing-batch stalls lacks per-sequence
proof. A fresh fixed-version stall should first be traced through client send,
broker ingress/PBR, scanner, epoch extraction, and classification.

The [remote CXL study](2026-09-23-real-cxl-performance.md) used six-thread
publishers on c1/c3 and later c1/c2/c3. The c1/c3 old/fixed broker geometric
ratio was 0.9969 over three pairs (95% interval 0.951–1.045): encouraging,
but too imprecise for a tight remote nonregression bound. The three-client
11.303 GB/s median is a capacity experiment, not a reproduction of the
paper's two-client 11.130 GB/s cell. During one qualifying three-client run,
the host NIC received 11.95 GB/s over its busiest 0.5-second interval versus
12.47 GB/s in a same-counter raw-TCP control. This is 95.8% of the observed
steady receive ceiling on that setup, not proof of global implementation
optimality.

The new `tools/remote_cxl.py` profile was run from clean revision `0476e2ed`
with two remote native Intel clients (c1/c3), both on the verified 9000-byte
test path and HugeTLB pools. Its 10 GiB ORDER5/ACK1/RF0 run passed exact
1,310,720 ACKs per client, all four routes, zero retry/fence, matching
`/proc` executable digests, real node-2 placement, normal process exit, and
owned cleanup. Approximate client-clock completion was **9.851 decimal GB/s**.
The local manifest is at
`/home/domin/Embarcadero/results/cxl-release-checkpoint/embarcadero-dev-1002-uc3lpdvn/manifest.json`;
generated results are intentionally not committed. This is one finite
qualification observation, not a new old/new estimate or paper reproduction.

The next release actions are concrete:

1. Integrate the clean review branch without overwriting the dirty historical
   `main` worktree, then run the published CI workflow on the integrated commit.
2. Preserve native-client source/build provenance with any new remote
   comparison; rerun matched pairs only if claiming a tighter remote bound.
3. Keep the rare-stall rate, one-broker primary ACK result, broader workload
   matrix, and unsupported failure/durability modes explicitly open. Use a
   predeclared reliability target before launching a larger campaign.

Broad broker throughput optimization is not the next gate. For this saturated
four-broker ingress profile, investigate only a repeatable release-workload
regression or a separately scoped latency, subscriber, or replication target.
