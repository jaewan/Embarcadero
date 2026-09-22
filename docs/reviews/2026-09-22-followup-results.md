# Remaining refactoring work: follow-up

Status: implementation and qualification in progress. Prior source09 evidence is
retained separately and does not qualify the new extracted binaries.

Implemented: broker ordering/commit/scanning/recovery/export and client session/
retention/ACK compilation boundaries; owned experiment dispatch for 11 historical
launchers; three additional live fault cases; linked actual Topic publication and
ChainReplicationManager disk-failure fixtures; per-process TSan workaround;
private GitHub vulnerability reporting enabled; paper implementation pointer
claim corrected to distinguish the common-address prototype from offset-only design.

Validation and final measurements will be recorded here after source freeze.
The [fixed measurement protocol](2026-09-22-followup-performance-protocol.md)
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
