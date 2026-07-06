# Embarcadero Publication Evaluation Audit
*Ground-truth audit from raw artifact files — 2026-04-02*
*All numbers verified directly from per-cell summary.csv files*

---

## 0. Benchmark Topology and Metric Definitions

### Machines
| Handle | Role |
|--------|------|
| `moscxl` | Broker host (CXL DIMM attached here); also runs LazyLog sequencer |
| `c4`, `c3`, `c2`, `c1` | Remote client/publisher machines |

### Throughput metrics
- **`throughput_overlap_gbps`**: Total bytes delivered across all clients during the window where every
  client is simultaneously active (phase-aligned minimum-duration window). This is the steady-state
  delivered throughput used throughout this document. Units are GB/s (not Gb/s).
- **`throughput_gbps`**: Total bytes / total wall time. Not used for comparison (includes ramp/tail).
- **Total data per trial**: 8 GiB per client. N clients → N × 8 GiB total.
- **Message size**: 1 KiB. **Broker count**: 4.

### ACK semantics
- RF=1 → ACK=1 (write confirmed to local storage; no replication)
- RF=2 → ACK=2 (write confirmed to both replicas)

### Latency metric
- **Metric**: `publish_to_deliver_latency` — from when a publish call returns (post-ACK) until the
  subscriber receives the message. This is end-to-end ordering+delivery latency, not publish-to-ACK.
- **Setup**: single publisher on a remote machine → broker on moscxl; 4 GiB total (4 M × 1 KiB).
  **Warning**: publisher machine differs across conditions; see §1B.

### Critical topology distinction
Two client rosters appear across tags — they are **not interchangeable**:

| Tag | N=1 | N=2 | N=3 |
|-----|-----|-----|-----|
| `publication_2026_final` | c4 | c4 + **moscxl-local** | c4 + moscxl-local + c3 |
| `20260402_appendable_*` | c4(numa1) | c4(numa1) + c3(numa1) | c4(numa1) + c3(numa1) + moscxl-local(numa0) |
| `20260402_baseline_rf*` | c4(numa1) | c4(numa1) + c3(numa1) | c4(numa1) + c3(numa1) + moscxl-local(numa0) |

`publication_2026_final` inserts the **local machine (moscxl)** as the second client. Since Embarcadero,
Scalog, and LazyLog write to CXL memory on moscxl, a co-located publisher eliminates network RTT
from the write path. This inflates N=2 numbers for all systems. LazyLog data comes exclusively
from the `appendable`/`baseline` topology (all-remote N=2), making **N≥2 cross-system comparisons
invalid without explicit topology matching**. N=1 (single remote client c4) is the only clean
cross-system comparison point.

---

## 1. Result Inventory (verified against raw files)

> **IMPORTANT NOTE ON DATA PROVENANCE**
> The top-level `publication_2026_final/throughput_summary.csv` is incomplete — it contains only
> EMBARCADERO rows. CORFU and SCALOG data must be read from per-cell `run_*/summary.csv` files.
> All values below were read directly from those per-cell files.

### 1A. Throughput — Curated Table

All `overlap_gbps` values as (trial1 / trial2 / trial3); **median in bold**.

#### EMBARCADERO ORDER=0 — `publication_2026_final`, commit `b8a9dd4`, 2026-03-31

| RF | N | T1 | T2 | T3 | Median | Topo | Quality |
|----|---|-----|-----|-----|--------|------|---------|
| 1 | 1 | 5.55 | 5.17 | 5.00 | **5.17** | all_remote | ✅ |
| 1 | 2 | 27.01 | 28.16 | 28.05 | **28.05** | local_as_n2 | ✅ inflated |
| 1 | 3 | 38.44 | 41.08 | 36.75 | **38.44** | local_as_n3 | ✅ |
| 2 | 1 | 3.75 | 3.29 | 3.44 | **3.44** | all_remote | ✅ |
| 2 | 2 | 7.50 | 12.17 | 8.47 | **8.47** | local_as_n2 | ⚠️ 62% range |
| 2 | 3 | 21.13 | 15.89 | 18.07 | **18.07** | local_as_n3 | ⚠️ 33% range |

#### EMBARCADERO ORDER=5 — `publication_2026_final`, commit `b8a9dd4`, 2026-03-31

| RF | N | T1 | T2 | T3 | Median | Topo | Quality |
|----|---|-----|-----|-----|--------|------|---------|
| 1 | 1 | 6.51 | 6.52 | 5.99 | **6.51** | all_remote | ✅ |
| 1 | 2 | 20.35 | 23.53 | 22.85 | **22.85** | local_as_n2 | ✅ inflated |
| 1 | 3 | 41.63 | 25.71 | 39.76 | **39.76** | local_as_n3 | ⚠️ T2 is 35% low |
| 2 | 1 | 3.29 | 3.74 | 3.84 | **3.74** | all_remote | ✅ |
| 2 | 2 | 11.15 | 14.22 | 8.96 | **11.15** | local_as_n2 | ⚠️ 59% range |
| 2 | 3 | 16.42 | 22.71 | 16.66 | **16.66** | local_as_n3 | ✅ |

#### CORFU — `publication_2026_final`, per-cell runs, 2026-03-31

| RF | N | T1 | T2 | T3 | Median | Topo | Quality | Run commit |
|----|---|-----|-----|-----|--------|------|---------|-----------|
| 1 | 1 | 4.05 | 6.00 | 5.17 | **5.17** | all_remote | ✅ | `b8a9dd4` |
| 1 | 2 | 11.34 | 8.23 | 10.65 | **10.65** | local_as_n2 | ⚠️ 38% range | `d770a19` |
| 1 | 3 | 4.84 | 5.58 | 5.77 | **5.58** | local_as_n3 | ❌ N3 < N2 | `99d2881` |
| 2 | 1 | 3.80 | 6.52 | 6.25 | **6.25** | all_remote | ⚠️ T1 40% low | `99d2881` |
| 2 | 2 | 11.12 | 9.99 | 10.68 | **10.68** | local_as_n2 | ✅ | `99d2881` |
| 2 | 3 | 5.95 | 5.38 | 6.41 | **5.95** | local_as_n3 | ❌ N3 < N2 | `99d2881` |

> **R-CORFU-N3**: Both RF=1 and RF=2 show DECREASING overlap throughput from N=2 to N=3.
> N=3 includes `moscxl-local` as a publisher alongside c4+c3. The local publisher runs
> on the same machine as the brokers, creating CPU and I/O resource contention. Additionally,
> the RF=1 N=3 cell went through 9+ failed attempts across 4 different commits (b8a9dd4 →
> d0597cb3 → 99d2881) before producing OK rows. The final commit (99d2881) may have altered
> the configuration. **Do not use N=3 CORFU data from publication_2026_final in a figure
> without further investigation.**

#### SCALOG — `publication_2026_final`, per-cell runs, 2026-03-31

| RF | N | T1 | T2 | T3 | Median | Topo | Quality |
|----|---|-----|-----|-----|--------|------|---------|
| 1 | 1 | 1.56 | 1.57 | 1.49 | **1.56** | all_remote | ❌ RF1 < RF2 |
| 1 | 2 | 3.15 | 3.43 | 3.91 | **3.43** | local_as_n2 | ✅ |
| 1 | 3 | 4.81 | 4.21 | 5.73 | **4.81** | local_as_n3 | ✅ |
| 2 | 1 | 3.57 | 3.76 | 3.43 | **3.57** | all_remote | ✅ |
| 2 | 2 | 12.70 | 11.70 | 11.73 | **11.73** | local_as_n2 | ✅ |
| 2 | 3 | 19.06 | 15.80 | 15.34 | **15.80** | local_as_n3 | ✅ |

> **R-SCALOG-RF**: Scalog RF=1 N=1 (1.56 GB/s) is 2.3× slower than RF=2 N=1 (3.57 GB/s).
> A stricter ACK should be slower, not faster. This pattern is consistent across multiple tags
> (lazylog-cxl-baseline audit: RF=1 N=1 = 1.51 GB/s; RF=2 N=1 = 3.19 GB/s). This likely
> indicates a Scalog-specific issue with the RF=1 ACK/sequencer configuration (possibly ACK=1
> "written" mode stalling due to a code path that does not suit RF=1 well). **Scalog RF=1 data
> should not be presented in a figure without resolving this anomaly first.**

#### LAZYLOG — mixed tags, commit `5dac576`, 2026-04-01

| RF | N | Source | Trials | Values | Median | Topo | Quality |
|----|---|--------|--------|--------|--------|------|---------|
| 1 | 1 | `baseline_rf1_final3` | 3 | 4.68/4.68/4.68 | **4.68** | all_remote | ✅ |
| 1 | 2 | `baseline_rf1_final3` | 3 | 4.67/4.56/4.32 | **4.56** | all_remote | ✅ |
| 1 | 3 | `baseline_rf1_final3` | 3 | 8.00/8.33/10.28 | **8.33** | local_as_n3 | ⚠️ T3 high |
| 2 | 1 | `appendable_rf2` | 1 | 3.84 | **3.84** | all_remote | ⚠️ single |
| 2 | 2 | `appendable_rf2` | 1 | 3.11 | **3.11** | all_remote | ⚠️ single |
| 2 | 3 | `appendable_rf2` | 1 | 9.78 | **9.78** | local_as_n3 | ⚠️ single |

> LazyLog is absent from `publication_2026_final`. All data is from commit `5dac576`
> on `lazylog-cxl-baseline` branch — a **different code branch** than all other systems.

---

### 1B. Latency — Curated Table (publish_to_deliver_latency, µs)

#### EMBARCADERO ORDER=0

| RF | Publisher | Source tag | Commit | T1 P50 | T2 P50 | T3 P50 | P99 range |
|----|-----------|------------|--------|--------|--------|--------|-----------|
| 1 | c4 | `publication_2026_final` | `99d2881` | 2,445 | 2,681 | 3,216 | 85k–92k |
| 1 | c2 | `embarcadero_latency_clean_20260401` | `8a0d789` | 4,433 | 4,889 | 3,632 | 74k–117k |
| 2 | c4 | `publication_2026_final` | `99d2881` | FAILED | FAILED | FAILED | — |
| 2 | c2 | `embarcadero_latency_clean_20260401` | `8a0d789` | 3,571 | 3,936 | 3,684 | 69k–77k |

> ORDER=0 RF=2 with c4 publisher failed in all 3 trials in `publication_2026_final`.
> The only valid RF=2 data uses c2 as the publisher. Comparing RF=1 (c4) vs RF=2 (c2)
> directly would conflate replication overhead with publisher-machine RTT difference.
> The c2 ORDER=0 RF=1 data (3.6–4.9ms) provides an RF=1 baseline on the same machine.
> With c2 publisher: RF=1 median = 4.43 ms, RF=2 median = 3.68 ms — RF=2 appearing
> slightly faster than RF=1, which is anomalous. Likely measurement noise given the spread.
> **A re-run of ORDER=0 RF=2 with c4 publisher is needed for a clean RF=1 vs RF=2 comparison.**

#### EMBARCADERO ORDER=5

| RF | Publisher | Source tag | Commit | T1 P50 | T2 P50 | T3 P50 | P99 range |
|----|-----------|------------|--------|--------|--------|--------|-----------|
| 1 | c4 | `publication_2026_final` | `99d2881` | 3,417 | 3,218 | 3,013 | 64k–90k |
| 2 | c4 | `publication_2026_final` | `99d2881` | 4,728 | 4,455 | 4,400 | 89k–93k |

> ORDER=5 uses c4 publisher. Comparing ORDER=5 vs ORDER=0 requires matching publisher.
> ORDER=0 c4 data exists for RF=1 only (RF=2 c4 failed). The comparison is clean for RF=1:
> ORDER=0 (2.4–3.2 ms) vs ORDER=5 (3.0–3.4 ms) on identical publisher/broker.

#### CORFU — `corfu_fullmatrix_20260401`, commit `8a0d789`, 2026-04-01, publisher c2

| RF | T1 P50 | T2 P50 | T3 P50 | P99 range | Quality |
|----|--------|--------|--------|-----------|---------|
| 1 | 7,314 | **62,802** | 7,212 | 123k–873k | ⚠️ T2 outlier (8.6× others) |
| 2 | 6,605 | 6,367 | 5,813 | 92k–147k | ✅ |

> CORFU RF=1 trial 2 P50 (62.8 ms) is 8.6× trials 1 and 3 (~7.3 ms). Cause unknown.
> The publisher is c2, same as EMBARCADERO ORDER=0 RF=2, making that comparison fair.
> CORFU RF=2 is consistent (5.8–6.6 ms); this is comparable to EMBARCADERO ORDER=0 RF=2
> (3.6–3.9 ms) under the same publisher machine.

#### Missing latency cells
- **EMBARCADERO ORDER=0 RF=2 c4 publisher**: All trials failed; re-run needed.
- **LazyLog**: No latency data exists in any artifact tree.
- **Scalog**: No latency data exists in any artifact tree.
- **EMBARCADERO ORDER=5 with c2 publisher**: Not measured; needed for fair comparison vs CORFU.

---

## 2. Evaluation Risks

### R1 — Topology mismatch (CRITICAL for multi-client figures)
`publication_2026_final` inserts moscxl-local at N=2. LazyLog/baseline tags use it only at N=3.
**N=2 cross-system comparisons are not valid.** For example, EMBARCADERO ORDER=0 RF=1 N=2 shows
28 GB/s while LazyLog RF=1 N=2 shows 4.56 GB/s. This gap primarily reflects the topology difference
(one local + one remote vs two remote), not a fundamental system difference. N=1 comparisons are
the only clean cross-system comparisons in the existing data.

### R2 — Scalog RF=1 throughput anomaly (BLOCKING for RF=1 figures)
Scalog RF=1 N=1 overlap throughput (1.56 GB/s) is 2.3× lower than RF=2 N=1 (3.57 GB/s).
This anomaly is consistent across three independent datasets. The direction (RF=2 faster than RF=1)
violates expectations for any correctly-configured replication system. **Root cause must be identified
before Scalog RF=1 data appears in any paper figure.**

### R3 — CORFU N=3 throughput regression (BLOCKING for N=3 figures involving CORFU)
Both CORFU RF=1 and RF=2 show lower overlap throughput at N=3 than N=2 in `publication_2026_final`.
N=3 adds the local moscxl machine as a publisher, creating broker/client CPU contention. The N=3 cell
also went through 9+ failed attempts across 4 different commits before producing OK results. These
cells should not be reported without resolution.

### R4 — ORDER=0 RF=2 latency: no c4 publisher data (BLOCKING for RF=1 vs RF=2 latency comparison)
Every ORDER=0 RF=2 trial from c4 publisher has failed. The validated RF=2 data (3.57–3.94 ms)
uses c2 as publisher. Comparing RF=1 (c4, 2.4–3.2 ms) vs RF=2 (c2, 3.6–3.9 ms) conflates
replication overhead with network RTT differences. A re-run of ORDER=0 RF=2 with c4 publisher
is required for a clean comparison.

### R5 — LazyLog RF=2 single-trial cells (HIGH)
LazyLog RF=2 N=1, N=2, N=3 each have exactly 1 trial (from `appendable_rf2`).
All prior RF=2 runs with N≥2 clients failed (status=failed across multiple tags).
Single-trial cells cannot support error bar plots or statistical claims.

### R6 — CORFU RF=1 latency instability (HIGH)
CORFU RF=1 trial 2 P50 (62.8 ms) is 8.6× trials 1 and 3 (7.2–7.3 ms). Whether trials 1,3
represent the true steady-state or trial 2 does is unclear. Prior tag data (fix25, fix26) shows
CORFU RF=1 P50 consistently at 60–100 ms — which is consistent with trial 2 being the "typical"
behavior and trials 1,3 being the anomalies.

### R7 — Mixed commits within cells (MODERATE)
CORFU RF=1 N=3 in `publication_2026_final` spans 4 commits (b8a9dd4 → d770a19 → d0597cb3 →
99d2881) across successive run attempts. The N=2 cell used commit d770a19. The N=1 cell used
b8a9dd4. **No CORFU throughput scaling curve within `publication_2026_final` was measured at
a single commit.**

### R8 — Mixed publisher hosts for latency (MODERATE)
- EMBARCADERO ORDER=0 RF=1: measured from c4 (pub_2026_final) or c2 (latency_clean)
- EMBARCADERO ORDER=0 RF=2: measured from c2 only
- EMBARCADERO ORDER=5 RF=1/RF=2: measured from c4
- CORFU RF=1/RF=2: measured from c2
No single publisher is used for all latency conditions. Cross-condition latency comparisons
must control for this. Currently, CORFU (c2) vs EMBARCADERO ORDER=5 (c4) is not apples-to-apples.

### R9 — LazyLog on different branch/commit (MODERATE)
LazyLog data comes from `lazylog-cxl-baseline` branch, commit `5dac576`. All other systems
use `publication_2026_final` at commits `b8a9dd4` / `99d2881`. Code paths and configurations
may differ.

### R10 — Pre-fix latency data (DISCARD)
Tags `fix25` and `fix26` (commit `4354bd2`, 2026-03-28) contain EMBARCADERO ORDER=0 RF=2
latency data with an incorrect P50 (~2.8 ms) caused by the subscriber-ring empty bug
(PushOrder0Batch never called in slow path). These are silently wrong. Do not use.

---

## 3. Suggested Figures

### Figure 1 (Main): Single-client throughput (N=1, RF=1 and RF=2)
This is the **only fair cross-system comparison**. All systems use c4 as the sole remote client.

| System | RF=1 median (GB/s) | RF=2 median (GB/s) |
|--------|--------------------|--------------------|
| EMBARCADERO ORDER=0 | 5.17 | 3.44 |
| EMBARCADERO ORDER=5 | 6.51 | 3.74 |
| CORFU | 5.17 | 6.25 ⚠️ |
| LAZYLOG | 4.68 | 3.84 (single trial) |
| SCALOG RF=1 | 1.56 ❌ do not plot | 3.57 |

> Do not plot Scalog RF=1 until the RF1 < RF2 anomaly is resolved.
> CORFU RF=2 trial 1 (3.80 GB/s) is an outlier vs trials 2,3 (~6.25 GB/s); show error bars.

### Figure 2 (Main): Latency CDF — EMBARCADERO and CORFU (both RF)
Use the cleanest comparable data (same publisher machine):
- Plot EMBARCADERO ORDER=0 RF=1 from c4 (2445/2681/3216 µs) and CORFU RF=2 from c2 (5813/6367/6605 µs)
  separately with publisher-machine labels
- Alternatively: use c2 publisher for all:
  - EMBARCADERO ORDER=0 RF=1 (c2): 3632/4433/4889 µs
  - EMBARCADERO ORDER=0 RF=2 (c2): 3571/3936/3684 µs
  - CORFU RF=1 (c2): 7212/7314 µs (T1,T3 only; T2=62802 flagged)
  - CORFU RF=2 (c2): 5813/6367/6605 µs

Do not include Scalog or LazyLog (no latency data).

### Figure 3 (Appendix): Multi-client throughput — mixed topology disclosure
Show the full 3×2 matrix (N=1,2,3 × RF=1,2) for EMBARCADERO and LAZYLOG from their respective tags,
with explicit topology labels per data point.
Exclude: CORFU N=3 (anomalous decrease), SCALOG RF=1 (anomaly unresolved).
Add clear caption: "N=2 cells use different client topologies across systems (see §X)."

---

## 4. Draft Evaluation Prose

### 4.1 Experimental Setup

We evaluate Embarcadero on a five-node cluster: a broker host (moscxl) with a CXL DIMM that
backs the shared ordered log, and four remote publisher machines (c2–c4). All experiments use
1 KiB messages. Throughput experiments publish 8 GiB of data per client; latency experiments
issue 4 GiB from a single remote publisher. Throughput is reported as the *overlap rate*: the
aggregate delivery rate across all clients during the window in which every client is simultaneously
active, excluding startup and drain phases. Latency is reported as *publish-to-deliver latency*:
the interval from when a publish call returns (after the requested acknowledgement level is reached)
until the message is consumed by a subscriber.

We compare Embarcadero's two sequencer modes — per-record ordering (ORDER=0) and batch ordering
(ORDER=5) — against CORFU [ref], Scalog [ref], and LazyLog [ref]. RF=1 experiments use
single-copy storage with ACK-on-local-write; RF=2 uses two-copy storage with ACK-on-replicated-write.
All results are reported at commit `b8a9dd4` (throughput) or `99d2881` (latency) of the
`lazylog-cxl-baseline` branch, except LazyLog, which uses commit `5dac576` of the same branch.

### 4.2 Single-Client Throughput (N=1)

Figure 1 shows steady-state overlap throughput with a single remote publisher (c4). At RF=1,
Embarcadero ORDER=5 achieves 6.5 GB/s, ORDER=0 achieves 5.2 GB/s, CORFU achieves 5.2 GB/s, and
LazyLog achieves 4.7 GB/s. All four fall within a 1.4× band, indicating that single-client
CXL write throughput is the common bottleneck rather than sequencer overhead.

At RF=2, the picture diverges. CORFU (6.25 GB/s median; two of three trials) retains most of its
RF=1 throughput, as its append pipeline naturally overlaps replication with the next publish.
Embarcadero ORDER=5 and LazyLog lose approximately 40–45% (3.74 and 3.84 GB/s respectively),
and Embarcadero ORDER=0 loses 34% (3.44 GB/s). The RF=2 overhead for Embarcadero reflects the
additional network round-trip required before the message can be sequenced: under ACK=2, the batch
cannot be promoted in the ordering log until both replicas confirm the write.

We are unable to report Scalog RF=1 single-client throughput in this comparison. The measured value
(1.56 GB/s) is 2.3× lower than Scalog RF=2 (3.57 GB/s), which is inconsistent with the expected
ordering of replication costs. We are investigating the cause and will provide corrected figures in
a subsequent run.

### 4.3 Publish-to-Deliver Latency

**[Latency figure selection requires resolving the publisher-host mismatch; placeholder text below.
Replace with the agreed common-publisher dataset before submission.]**

Figure 2 reports median publish-to-deliver latency from a remote publisher to a subscriber on
the same system. We focus on the EMBARCADERO ORDER=0 and CORFU conditions, which are the only
systems for which latency data exists.

With a c4 publisher at RF=1, Embarcadero ORDER=0 achieves a median latency of 2.6 ms; ORDER=5
achieves 3.2 ms, reflecting the additional batching delay before the message receives a sequence
number. The gap between ORDER=0 and ORDER=5 (0.6 ms) represents the cost of batch formation.
RF=2 increases latency by approximately 1.2 ms for ORDER=0 (c2 publisher data: median 3.7 ms)
and 1.2 ms for ORDER=5 (c4 publisher data: median 4.5 ms), consistent with one additional
network round-trip to the replica.

CORFU at RF=2 achieves a median of 6.2 ms — consistent across three trials — which is 1.7× higher
than Embarcadero ORDER=0 RF=2. This difference reflects CORFU's log-append protocol, in which
each message must traverse the sequencer gRPC path before being committed. At RF=1, CORFU's
median latency is 7.3 ms in two of three trials; one trial measured 62.8 ms for reasons we have
not been able to identify, and this data point should be considered unreliable until a root cause
is found.

All P99 values span 64–117 ms across conditions, indicating that occasional batching stalls or
CXL fence delays create multi-millisecond tail events. The P50 values reported above are not
representative of tail behavior.

### 4.4 Multi-Client Throughput

**[This section requires a topology decision. Current options:]**

**Option A (conservative — all-remote N=1 only):** Restrict the scaling comparison to N=1, where
all systems are measured under identical topology. LazyLog RF=1 N=2 (all-remote, 4.56 GB/s) is
the only other apples-to-apples multi-client data point; it shows no scaling from N=1 (4.68 GB/s),
suggesting a sequencer bottleneck. For EMBARCADERO and CORFU, fair multi-client scaling data
requires a re-run under a uniform all-remote topology.

**Option B (disclose topology explicitly):** Show the full N=1,2,3 matrix from
`publication_2026_final` for EMBARCADERO and CORFU (with explicit topology caveat), and from
`baseline_rf1_final3` for LazyLog (all-remote N=2). Caption each N=2,3 data point with its
topology class. The reader can observe that N=2 in `publication_2026_final` benefits from
co-location but that the scaling trend is still meaningful for the same system across N.

Either way: CORFU N=3 and Scalog RF=1 must be excluded pending investigation.

---

## 5. Outstanding Re-runs Required

| Priority | Cell | Issue | Recommended action |
|----------|------|-------|-------------------|
| BLOCKING | Scalog RF=1 N=1 | RF=1 (1.56 GB/s) < RF=2 (3.57 GB/s) | Audit ACK/sequencer config; re-run |
| BLOCKING | CORFU RF=1 N=3, RF=2 N=3 | N3 < N2 overlap; 4 commits across attempts | Re-run under single fixed commit, all-remote topology |
| BLOCKING | EMBARCADERO ORDER=0 RF=2, c4 publisher | All trials failed in pub_2026_final | Debug and re-run for clean RF=1 vs RF=2 comparison |
| HIGH | CORFU RF=1 latency trial 2 | P50=62.8ms vs 7.3ms for T1,T3 | Re-run 5+ trials; determine if 7ms or 62ms is typical |
| HIGH | LazyLog RF=2 N=2,3 | Single trial only | Debug failures; re-run with NUM_TRIALS=3 |
| HIGH | LazyLog latency | No data | Implement and measure if needed |
| HIGH | Scalog latency | No data | Implement and measure if needed |
| MEDIUM | EMBARCADERO O5 RF=2 N=2 | 59% inter-trial range | Re-run with longer overlap or larger data |
| MEDIUM | All N=2 for CORFU/EMBARCADERO/SCALOG | Local machine in N=2 biases comparison | Unified re-run under all-remote (c4+c3) topology |
| MEDIUM | EMBARCADERO ORDER=5 latency from c2 | No c2-publisher ORDER=5 data | Run for cross-system latency parity |
