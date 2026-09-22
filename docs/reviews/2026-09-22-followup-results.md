# Remaining refactoring work: follow-up

Status: source 14 implementation and scoped validation are complete. The full
original plan remains partially open: one-broker primary nonregression is
unestablished, and broader experiment migration/measurement and hardware gates
remain in the [closure audit](2026-09-22-plan-closure-audit.md). Prior source09
evidence is retained separately and does not qualify the new extracted binaries.

Implemented: broker ordering/commit/scanning/recovery/export and client session/
retention/ACK compilation boundaries; owned experiment dispatch for 11 historical
launchers; three additional live fault cases; linked actual Topic publication and
ChainReplicationManager disk-failure fixtures; per-process TSan workaround;
private GitHub vulnerability reporting enabled; paper implementation pointer
claim corrected to distinguish the common-address prototype from offset-only design.

Validation below identifies each frozen source and executed binary. The [fixed measurement protocol](2026-09-22-followup-performance-protocol.md)
precedes the follow-up performance results.

## Implemented and checked

The frozen source 11 full build passed 51 CTest targets in 5.28s. The minimal-client
build and baseline-dependency exclusion checker passed. Ten production-linked
publisher tests passed ASan/UBSan with leak detection. Three small concurrency
fixtures and six larger linked fixtures passed ThreadSanitizer. The larger set
covers publisher recovery, configuration, network framing, epoch shutdown,
actual Topic publication and actual chain-replication disk I/O.

The first TSan run found a non-atomic GOI read in the new test oracle before
worker join. Moving that test-only read after join preserved the intermediate
production OPEN checks and passed both normal Release and TSan reruns. An initial
report in glog/libc timezone allocation disappeared with the process timezone
fixed to UTC. Initial failures and successful reruns are retained; no race
suppressions or host-wide ASLR changes were used. Fetched dependencies share the
selected sanitizer instrumentation; installed system libraries are not all
instrumented. This is not a full live-cluster TSan qualification.

Source 12 records the test-only oracle fix, the linked-TSan reproduction wrapper,
and documentation. Its production sources and CMake inputs are byte-identical
to 11. Broker/client runtime qualification remains bound to the frozen source 11
binaries, rather than silently relabeling old binaries as newly built ones.
The paper draft is git-ignored; its corrected implementation excerpt is retained
with the evidence rather than automatically added to public source control.

Private GitHub vulnerability reporting is enabled. A bounded pattern scan found
no credential matches in 1779 tracked/unignored text paths; it excludes ignored
content, build outputs, history and files larger than 2 MiB, and is not a security
certification. Apache-2.0 and bounded non-recycling retention remain selected.

## Final source 14 and a routing configuration regression

The final source 14 Release build passed **53/53 CTest targets in 28.78s**.
This includes actual differently sized `CXLManager` allocator instances in one
process, the production Topic publication fixture and the new owned workload
runner tests. The manager fixture uses a test-only constructor and small owned
anonymous mappings; it exercises the unchanged production allocator and
destructor, not full backend attachment or free/reuse behavior.

Seven linked TSan fixtures passed. The Topic/manager and publisher fixture
project objects were rebuilt consistently from source 14, using the recorded
instrumented dependency build. Four unchanged linked fixtures were rerun from
the retained source 11 TSan build. All 12 current publisher tests also passed
ASan/UBSan with leak detection using freshly compiled source 14 project objects
and consistently instrumented dependencies. The three earlier smaller TSan
fixtures remain separately recorded. These results do not constitute a fully
instrumented live-cluster qualification.

Live source-13 tooling exposed a routing configuration gap: a publisher whose
allowlist was only brokers 1–2 sent its workload but received no authoritative
ORDER5 ACK progress and exited on timeout. The simultaneous broker-0 publisher
completed. The failed run, original verdict and normal cleanup are retained at
`owned-workloads/publishers/embarcadero-dev-1002-bd25yxtp/`. ORDER5 ACK1/ACK2 and
fence notifications originate at the head, while this client's ACK connections
follow its publishing connections. No separate ACK-only head connection exists.

Source 14 therefore rejects follower-only acknowledged ORDER5 allowlists early
in `Publisher::Init`. It also conservatively rejects implicit nonzero home-broker
selection without an explicit head-containing list, since that policy can omit
the head. Default all-broker routing and explicit lists containing broker 0 remain
supported. ACK0 and unrelated modes are unchanged. Neither client nor runner
silently changes the requested destinations. Linked tests exercise real Init
rejection and unchanged Publisher-owned worker/pool state. The constructor's
preexisting lazy gRPC channel is outside that Init-specific guarantee.

The runner now reports failed/missing Git provenance as unavailable instead of
calling an extracted archive a clean revision. Source archives and build evidence
establish the association separately. It also checks each broker after readiness
and each client after exec through `/proc/PID/exe`, retaining the observed path
and digest or failing the run. Earlier source-13 manifests are preserved with
their original inaccurate runner-Git label; their external native source/binary
attestation is recorded in the independent audit.

The source 14 archive SHA-256 is
`811fd23dd548c2b310576c514d7b5642263c201c2ae493d9be9d951cb14383ec`.
Its only native changes from source 13 are the publisher startup-routing
predicate, Init check and declaration. The final broker's executable and
relocation sections match measured source 11 exactly, including addresses and
sizes. Its other differences are debug/build metadata and 32 source-path bytes
in `.rodata`. This is not whole-file identity. Performance remains attributed
to the executed source-11 broker/common-client binaries; the source-14 client
has not been benchmarked.

## Frozen live fault campaign

All 23 cases passed against the source 11 fault build. There were no forced
terminations; every owned broker exited normally, with the withheld-ACK client's
nonzero exit required by that negative test. Cases include all 20 preceding
schedules plus partial-control shutdown, partial-body shutdown and 128 rejected
connections with no net descriptor-count growth. Exact worker-TID syscall
snapshots establish the real post-hook waits for the new shutdown schedules and
the full queue. Per-case oracles, payload bounds, binary digests and cleanup
outcomes are retained under `results/refactor-followup/2026-09-22/faults/`.

The independent fault audit passed 652 checks, including 48 identities observed
through `/proc/PID/exe`, the exact source archive, native hook/wait ordering and
owned cleanup. Maximum active duration was 10.768s against the 60s budget.
Maximum post-active cleanup was 16.320s, including shared-memory reclamation;
the 15s child shutdown grace is not a promise that total cleanup takes 15s.
Ordinary Release smokes passed with one and three brokers using automatic
common-address selection, plus legacy ORDER0 startup. None required forced exits.

## Fixed before/after performance campaign

All 52 trials passed runtime qualification: 12 balanced AB/BA measurement pairs
and two excluded qualification runs for each of one and three brokers. The
baseline is `ae9959dd2892af821878aa42d4e2c05fcb635466`; the candidate broker and
**common client used on both versions** come from frozen source 11. Each fresh
64 GiB DRAM region uses 4 GiB segments, a fixed common mapping base and a 2 GiB
application transfer of 4 KiB indexed messages, ORDER5/ACK1/RF0. Client physical
cores 0–31 are on NUMA0; broker core ranges 128–159, 160–191 and 192–223 are on
NUMA1. Initialization is outside the client timing boundary. Every trial audits
all 524288 messages, completes its ACK frontier without RTO/fencing, and cleans
up normally. This is a finite common-client broker comparison, not a whole-client
stack, server-capacity, steady-state, remote-network or CXL result.

| Brokers | Measurement | Paired geometric candidate/baseline ratio | 95% interval | Predeclared classification |
|---|---|---:|---:|---|
| 1 | ACK completion (primary) | 0.9809 | 0.8114–1.1859 | Inconclusive |
| 1 | Audit-inclusive E2E | 1.0085 | 0.9526–1.0676 | Bounded nonregression signal |
| 3 | ACK completion (primary) | 1.0642 | 0.9809–1.1547 | Bounded nonregression signal |
| 3 | Audit-inclusive E2E | 1.0361 | 0.9993–1.0743 | Bounded nonregression signal |

Intervals use paired log ratios and Student-t with 11 degrees of freedom. They
are exploratory, unadjusted across the four endpoints, and assume independent,
approximately normal pair differences. None establishes equivalence within
±5% or improvement above 5%. **Overall primary-throughput nonregression remains
unestablished because the one-broker interval is inconclusive.** Its unpaired
baseline/candidate medians are 1247.3165/1147.0030 MiB/s, an 8.0% decrease that
crosses the original investigation trigger; this descriptive median difference
must not be substituted for the paired estimate or declared a proven regression.
No sample was discarded and no extra pairs were added after inspecting results.
The earlier source09 campaign is separate and was not pooled.

The independent audit verified all 52 manifests and recomputed the confidence
intervals. Project compiler/ISA settings and shared-library identities match.
Fetched dependency flags differ, but the complete inventory of 1727 matching
objects has identical executable and relocation sections; 1707 files are byte
identical and 20 differ only in debug strings. Do not describe all dependency
flags as identical.

The first combined-index script rejected four topology-local artifact paths.
The retained correction permits only those location differences and verifies
source archive/inventory contents and baseline patch identities separately;
all other identity fields remain equal. The original failure and corrected
analysis are retained. No run verdict or measurement changed.

Evidence: `results/refactor-followup/2026-09-22/performance-index.json`,
`performance-analysis.json`, `independent-performance-audit.json`,
`cross-topology-identity-check.json`, `independent-static-dependency-audit.json`,
and the original `perf-n1/` and `perf-n3/` manifests/logs. Exact executed broker,
client and fault-driver files are retained in `runtime-binaries/` with hashes.

The retained-log investigation found variation in both producer time before
`Poll` (baseline/candidate medians 1244/1338 ms) and logged ACK waiting (358/442
ms). Worker joining was only 0.20–0.38 ms, so a former 100 ms join delay does not
explain this result. Time after ACK through the completed audit also varied
widely (78–825 ms), with medians 426/201 ms. Work can move across that timing
boundary, so the ACK and audit-inclusive estimates differ. Existing logs do not
separate producer backpressure, client scheduling, transport and broker
sequencing causally; no stage CPU/counter measurement covers the exact ACK
window. The trigger was investigated, its cause remains unresolved, and the
one-broker classification remains inconclusive. The supporting per-run values
are in `n1-retained-phase-diagnostics.json`.

## Final owned profiles, minimal build and artifact boundaries

Source 14's fresh minimal-client build passed the check for absence of baseline
generated sources, headers and RPC symbols. The full Release test inventory,
compile commands, package/dependency provenance and binary digests accompany
both final builds. The earlier clean Ubuntu-rootfs validation remains historical;
53 current CTests on this host are not relabeled as a newly executed clean-rootfs
or hosted-CI campaign.

All four final owned runs passed, with an independent 310-check audit:

- Latency: all 8192 delivery samples, UID population and global-position checks;
  finite telemetry, not a full-payload or per-session FIFO proof.
- Sender gap: 8192 indexed messages delivered exactly, no retries/fences, both
  delay markers present (logged 2 ms for the requested 1 ms); no overtaking claim.
- Two publishers across three brokers: distinct sessions completed 4096 and
  8192 messages. Destinations were `0` and `0,1,2`; the latter sent
  3024/2648/2520 messages respectively. Both obeyed the same GO timestamp, with
  a recorded start spread of 16960 ns. This is an ACK/routing check, not a
  combined subscriber payload audit or performance comparison.
- Ordinary automatic-mapping smoke: 8192 indexed messages passed with the head's
  selected base `0x600000000000`.

All 11 running executable identities (six broker processes and five clients)
matched the final source-14 binaries. Git-unavailable labels were accurate;
all children exited zero, no forced termination occurred, and all owned regions
and PIDs were absent afterward. Results are under `owned-workloads/source14/`
and `independent-source14-workload-audit.json`. The original failed follower-only
run remains separately retained and is never counted as a successful profile.

Independent source/fixture review passed 48 checks, including consistent fault
definitions across all 30 linked Topic-fixture translation units and absence
of the test constructor/fault symbols from the production broker. See
`independent-source14-fixture-audit.json` and
`independent-source-13-to-14-audit.json`. Reproduction commands and all failure,
build, test and sanitizer logs are retained under the same campaign directory.
The campaign started September 22 and final software validation finished
September 23; keeping one artifact directory does not change execution dates.
