# E1 Leg 4 Results: Embarcadero on CXL

**Status:** Phase 1 partial result; E1 leg 4 only.
**Run tag:** `20260708_e1_leg4_order5_cxl`
**Commit:** `0528d4e938b9ae9638944b2d3eeed9c79e3c22cb`
**Raw tree:** `docs/experiments/e1_leg4_raw/20260708_e1_leg4_order5_cxl/`

## Configuration

- System: Embarcadero-on-CXL
- Ordering: `ORDER=5`
- Sequencer: `EMBARCADERO`
- ACK/RF: `ACK=1`, `REPLICATION_FACTOR=1`
- Brokers: 4 on `moscxl`
- Publisher: one remote client, `c4`, NUMA 1
- Broker address used by client: `192.168.60.8`
- Trials: 3 per message size
- Total bytes: 8 GiB per trial
- Message sizes: 128 B, 512 B, 1 KiB, 4 KiB, 16 KiB
- Pattern-kill cleanup disabled with `EMBARCADERO_DISABLE_PATTERN_KILL=1`

An initial preflight run using broker address `10.10.10.10` failed before publishing because `c4`
could not reach that network. The successful run used `192.168.60.8`, which `c4` can route to
directly. The failed preflight is preserved under `data/publication/throughput/` but is not included
in the registered raw tree or medians below.

## Results

Throughput is the overlap-window throughput from the publication harness, reported as GB/s and
messages/s. Ranges are min-max across the three successful trials.

| Message size | Median GB/s | Range GB/s | Median msgs/s |
|---:|---:|---:|---:|
| 128 B | 0.070494 | 0.064977-0.070671 | 591,343.596 |
| 512 B | 0.083637 | 0.081248-0.085253 | 175,399.061 |
| 1 KiB | 0.094571 | 0.092904-0.101135 | 99,164.609 |
| 4 KiB | 0.094597 | 0.092443-0.097205 | 24,798.164 |
| 16 KiB | 0.094742 | 0.091560-0.104026 | 6,208.982 |

## Pre-Registered Comparison

This leg alone does not decide the E1 waterfall claim; it provides the D1-unblocked
Embarcadero-on-CXL column. The 1 KiB median, 0.094571 GB/s, defines the Embarcadero own-peak input
for the E3 ORDER=5 latency-vs-load run: 96.841 MB/s. E3 load points are therefore 10, 48, 68, and
87 MB/s for 10%, 50%, 70%, and 90%.
