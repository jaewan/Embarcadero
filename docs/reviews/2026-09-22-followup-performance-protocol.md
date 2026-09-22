# Follow-up DRAM comparison: fixed protocol

This protocol is fixed before examining follow-up measurements. It does not
pool the preceding source09 campaign or change its inconclusive one-broker result.

Compare baseline `ae9959dd2892af821878aa42d4e2c05fcb635466` with a frozen final
follow-up build. Use the same audited follow-up client for both broker versions.
Run one and three brokers separately, with two excluded full-workload qualification
trials followed by exactly 12 paired measurements, alternating AB/BA (six of each).
Do not add pairs, discard outliers, or stop because an interval becomes favorable.
A failed correctness/cleanup gate invalidates its trial; retain it and diagnose
before any separately identified replacement campaign.

Keep the existing `paired-dram-v3-fixed-mapping-base` configuration: explicit
DRAM emulation, fresh 64GiB mappings at 0x400000000000, 4 GiB segments, node 0 client
CPUs 0–31, node 1 broker groups 128–159/160–191/192–223, ORDER5/ACK1/RF0,
2 GiB of 4 KiB indexed payloads with 2 MiB batches and a 2 s retransmission timeout.
Require exact ordered delivery, no retransmissions/fences, positive routing to
all brokers, normal process exits, verified binary identity, and owned cleanup.
No concurrent builds, sanitizer runs, or other test clusters during measurement.

Primary endpoint: throughput through authoritative ACK completion (Publish+Poll).
Secondary endpoint: throughput including serial indexed delivery audit. Report
all paired ratios, geometric means and paired log-ratio Student-t 95% intervals
separately by topology, plus AB-first/BA-first diagnostics. Intervals are
exploratory and unadjusted for the four endpoints. A lower confidence bound above
0.95 is a 5% non-regression signal; equivalence requires the whole interval inside
[0.95,1.05]. Do not call unchanged point estimates proof of either condition.

At the previous one-broker primary log-ratio standard deviation (approximately
0.253), 12 pairs may still leave roughly 17% uncertainty around unity. This fixed
campaign strengthens evidence but cannot promise a 5% conclusion. It measures a
finite single-session local DRAM broker workload, not sustainable capacity,
whole-client-stack improvement, fixed-load latency, or real-CXL performance.
