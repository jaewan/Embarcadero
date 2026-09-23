# Refactor review evidence available in this checkout

The paired measurement summaries below are copied byte-for-byte from local
analysis outputs. They include every measured pair, the reported ratios and
intervals, and an explicit `qualification_claim: false`. They let a reader
recalculate the descriptive statistics; they do **not** contain the complete
per-process logs, binary archives, source inventories, or placement snapshots
needed to re-audit a trial's correctness and provenance. Those raw artifacts
remain in ignored `results/` directories and must be published separately with
a release before describing the campaigns as independently reproducible.

| Study | Tracked summary | Original analysis SHA-256 | Original run index SHA-256 |
|---|---|---|---|
| One-broker 8 GiB whole-stack source14/source5 pilot | [all four pairs](2026-09-23-long8g-pilot-analysis.json) | `d6eeb8f530432062d4c4e713b7c4d39f77b68b7ef4feade2cdf217fea00b1b86` | `097e697afc387c52ae707e3be77534e55e0617aa56a9c3df3d357221a27304a3` |
| One-broker 8 GiB broker-only comparison with common source14 client | [all four pairs](2026-09-23-common-client-broker-analysis.json) | `43199abcb115f0887db377fcc7ec17fc1f8b67755f559fddf5c1d9c7bb024048` | `57bd84ef32def381a8c1f827d81590adcd26b73836109373b3a9027b8c3b6ba5` |

Both studies used one broker, ORDER5/ACK1/RF0, 8 GiB of 4096-byte indexed
application messages, fresh 64 GiB DRAM-emulated shared mappings, 16 GiB
segments, and local client NUMA node 0/broker node 1 placement. Each had one
excluded qualification per arm followed by four adjacent balanced AB/BA pairs.
The whole-stack study changed both broker and client, including serial versus
streaming delivery-audit scheduling. The broker-only study held the source14
serial-audit client fixed. Neither study is a 5% nonregression test. See the
[interpretation and limitations](../2026-09-23-workload-followup.md).

Historical review notes may name paths under `results/` or `Paper/`. These are
local artifact identifiers, not links to files supplied by a source checkout.
The paper manuscript is separately managed. The [bounded implementation
contract](../../architecture/refactoring-boundaries.md) and [support
matrix](../../support-matrix.md) are tracked here.
