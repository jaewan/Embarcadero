# Bounded workload and longer-run follow-up (2026-09-23)

The owned DRAM workload runner now accepts `--message-bytes` and exact
`--message-count` per client. It preserves the prior 4096-byte, 32 MiB default;
explicit counts are exclusive with `--payload-mib`. The runner limits each
client to 262144 messages and 32 MiB, requires exact byte accounting, and
checks segment reservation before startup. The native client rejects zero or
nonmultiple payload geometry, zero thread/client counts, latency messages below
16 bytes, and indexed ORDER5 audit messages below 8 bytes before using them.
Latency goodput now uses floating-point MiB so a valid sub-MiB workload does
not report zero throughput.

The release-native follow-up build passed all 54 CTests. One concurrent suite
attempt hit the preexisting `Phase2Test.ReplicaPollingNoSpin` timing threshold
(5654 spins versus a 5000 threshold); the test passed alone and the repeated
full 54-test suite passed. The owned one-broker emulation runs passed with
unique 64 GiB regions, node-0 client/node-1 broker placement, normal cleanup,
and zero process exit codes:

| Profile | Exact workload | Result |
|---|---:|---|
| Latency | 256 × 16 B | 256 delivered and recorded; strict order/UID counters pass; finite positive 0.073 MiB/s goodput |
| Indexed gap | 1024 × 8 B | 1024 ACKed and audited; 2 ms sender delay observed; zero duplicate/parse/export errors |

Artifacts: `results/refactor-next/2026-09-23/workload-live/`. The finite
latency sample includes pacing and batching; it is not fixed-offered-load
service latency.

The archived source14 and source5 builds were reconstructed from SHA-bound
source inventories in persistent directories and compared with a fixed 8 GiB,
one-broker, ORDER5/ACK1/RF0 protocol. Both full-size qualifications and all
eight measured trials passed exact 2,097,152-message indexed audits and owned
cleanup. Four balanced pairs yielded:

| Endpoint | Geometric source5/source14 ratio | Descriptive 95% interval | Paired log-ratio SD |
|---|---:|---:|---:|
| Audit-inclusive end to end | 1.008 | 0.789–1.287 | 0.154 |
| ACK completion | 0.848 | 0.611–1.176 | 0.206 |

These are exploratory intervals, not a 5% nonregression qualification. At a
true ratio of 1, the pilot SDs imply roughly 71 primary and 127 ACK pairs for
80% power against a 5% margin under a normal approximation; four-pair SDs are
unstable. The preregistered 32-pair gate therefore has low prospective power
and has not been launched. The source5 client reduced median peak RSS from
about 7.5 GiB to 0.59 GiB by streaming its exact audit; this changes concurrent
work relative to source14's serial audit. It does not establish throughput
equivalence. Raw manifests, analyzer, and source/build evidence are under
`results/refactor-next/2026-09-23/long8g-pilot-rebuilt-v3/`.

Three single-run causal probes all passed exact audit, identity, and cleanup
checks. Source5 with a serial audit and a bounded 16 GiB retention allowance
reported 1052 MiB/s ACK completion and 9.4 GiB client peak RSS. A source14
client on the source5 broker reported 1013 MiB/s; a source5 client on the
source14 broker reported 1259 MiB/s. These values point away from audit
overlap as the sole explanation and toward a possible broker contribution,
but the uncontrolled run-to-run spread prevents causal attribution. The only
changed broker files between these archives are session admission, session
publication, and ORDER5 commit; their changes reduce redundant flush or
locked-RMW work on the hot path. No code-path finding yet explains a 15% ACK
point-estimate loss, so changing broker logic on this evidence would be
speculative. Probe artifacts are under `results/refactor-next/2026-09-23/`
with `serial-audit-diagnostic-v1`, `cross-stack-diagnostic-v1`, and
`reverse-cross-stack-diagnostic-v1` names.

Later on 2026-09-23, the host exposed memory-only NUMA node 2 again (about
258 GiB free), and `/proc/iomem` showed `CXL Window 0`. The owned runner now
has an explicit `--physical-cxl` mode: it selects the real backend without
`--emul`, binds broker CPUs to node 1 with node 1/2 memory allowed, and requires
all sampled pages of the 64 GiB shared mapping to reside on node 2. Its
`cxl_evidence` flag becomes true only after that placement and the broker's
successful bind log are observed. The same mode is available to bounded
latency/gap/publisher workloads. On this host `/dev/dax0.0` is absent, so the
real backend uses the shared-memory fallback bound to node 2; the runner does
not establish a PCI device identity.

Audited one- and three-broker 32 MiB physical-mode smokes and the
1024 × 8 B indexed gap and 256 × 16 B latency workloads passed on node 2,
with exact delivery checks, zero exits, and owned cleanup. The three-broker
smoke used automatic address selection; all brokers mapped at
`0x600000000000`, and each sampled mapping had all 16,777,216 4 KiB pages
on node 2. Artifacts are under
`results/refactor-next/2026-09-23/cxl-live/`. This qualifies those finite
correctness cells and placement, not a physical CXL throughput comparison or
final paper performance claim. The public storage contract remains bounded
retention with fail-closed capacity behavior; no recycling is introduced.
