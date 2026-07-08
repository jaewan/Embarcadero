# Sub-phase 3A results: clean leg-2 (fixing Signal B)

**Date:** 2026-07-08. **Branch:** `w5-variants`. Same topology as Phase 1 (memserver on c3,
4 brokers moscxl + 4 brokers c1, sequencer on moscxl), now with NUMA-local placement and a
multi-threaded sequencer. Raw logs in [`w5_phase3a_raw/`](w5_phase3a_raw/).

> ## ⚠ REVIEW QUALIFICATIONS (adversarial review, 2026-07-08) — govern over the body
> Code is correct (no data-tainting race; G1/G2 verified). But the leg-2 CLAIM is re-labeled:
> - **The MT sequencer CONFIRMS the prior prediction:** Phase-1's ">98% collapse to 0.19 Gb/s" was
>   largely the SINGLE-THREADED software ceiling. With N threads it tracks offered load to 42.7 Gb/s.
>   Good — but it changes, not proves, the leg-2 story.
> - **NOT CLAIMABLE:** "~21 Gb/s clean NIC-contention cost" / "caps at the device ceiling" / "control+
>   payload bandwidth contention" as a quantitative attribution. The data refutes a clean cap:
>   sequencer goodput **halves** (42.7→21.2 Gb/s) as offered load **rises** (72→90→108) — a
>   congestion/service-rate **collapse** (detect-p50 21.7µs→61ms ≈2800×; stale_misses ~63%), and the
>   read lane sits at ~21 of ~98 Gb/s, nowhere near the device ceiling. The ~21 magnitude is CONFOUNDED
>   by (a) residual sequencer atomic-counter contention (disclosed, unresolved) and (b) 802.3x global
>   pause coupling read/write streams (RoCE priority isolation NOT done — switch-side).
> - **CLAIMABLE (qualitative, architectural — this IS the leg-2 conjunction point):** on RDMA the
>   sequencer's control-plane reads traverse the SAME shared memserver NIC that carries all broker
>   payload writes, so control-plane progress is coupled to and backpressured by payload load (the read
>   lane collapses exactly as the write funnel plateaus at 84–86 Gb/s). CXL has no such shared-NIC
>   funnel for control access. Report the ~21 Gb/s as a *degradation-under-coupling signal, not a
>   measured NIC-contention cap.*
> - **Signal-A plateau vs the CORRECTED ceiling:** the NUMA-fixed device ceiling is ~98 Gb/s (not 90),
>   so the plateau is ~87–89% of ceiling (an ~11–13% funnel-vs-device gap), NOT "within 3%." The old
>   Phase-1 "within 3%" wording is RETRACTED. (This strengthens the story — a real gap to explain.) The
>   QP-count sweep earns "device ceiling" for **pure-write saturation only**, not the mixed funnel.
> - **Signal-B causal language downgraded:** "coincident with shared-NIC saturation; sequencer
>   atomic-contention is an unseparated confound at these points," NOT "directly attributable."
> - **G2 durability caveat:** Phase-2's "no survivor stall" was measured under FIRE-AND-FORGET
>   replication (publish did NOT imply replicated); `--durable-replicate` (new here) gates sentinel on
>   replica completion but its cost is UNMEASURED (3B must measure it).
> - **MT `committed_seq` is a benchmark-only order counter** — it advances past in-flight cross-thread
>   GOI writes (no cross-thread fence) and MUST NOT feed any CXL commit-watermark consumer.
> - **Provenance gap:** `w5_phase3a_raw/README.md` (seven-facts for the 3A config: 8 seq QPs/CQs,
>   `--seq-threads=8`, NUMA bindings, MR sizes, hugepages) is a required follow-up (3B).
> - **The ~21 Gb/s question is the single experiment that settles the core dispute (3B):** shard the
>   atomic / batch fetch_add / drop per-loop alloc+sort / enlarge windows, re-run 90/108. If goodput
>   rises above 21, the 21 was software-bound → retract; if it holds, the contention story strengthens.

## Item 1: NUMA-local memserver

**c3 (memserver host) needed no fix.** Its NIC (`ens801f0np0`) is on NUMA node 1, which already
had ~19GB of hugepages reserved there — Phase 1's memserver was already NUMA-local by accident of
the host's existing configuration, not by design (no `numactl` was used). The genuinely broken
host was **moscxl**: its NIC (`enp193s0f0np0`) is also on NUMA node 1, but node 1 had **zero**
hugepages reserved (64 on node0, 0 on node1, 2048 on node2) — confirmed in Phase 1's own disclosed
gap. Reserved 512×2MB (1GB) hugepages on moscxl's node 1 (explicit user approval obtained first,
matching the earlier memlock precedent — shared-host system config change). c1's NIC
(`enp24s0f0np0`) is on NUMA node 0, which already had 8200 hugepages reserved — also no fix
needed there.

**Re-anchored `ib_write_bw` under matching NUMA placement** (`numactl --cpunodebind=1 --membind=1`
both sides, moscxl↔c3): single-QP **98.03 Gb/s**, multi-QP (`-q 8`) **98.04 Gb/s** — both
noticeably higher than Phase 0's original non-NUMA-pinned ~90 Gb/s reference (**+~9%**), confirming
NUMA locality is a real, measurable effect and giving Sub-phase 3A its own, more accurate device
ceiling: **~98 Gb/s**, not ~90 Gb/s.

All broker/sequencer processes in the funnel-sweep re-run below are launched under
`numactl --cpunodebind=<nic_node> --membind=<nic_node>` on every host (moscxl node 1, c1 node 0,
c3 node 1) — not just the memserver.

## Item 2: multi-threaded sequencer (N-thread/N-QP/N-CQ)

Added `--seq-threads=N` to the sequencer (W5-B path only — W5-A's Phase-2-validated
dead-broker/lease-timeout logic is untouched and always single-threaded regardless of this flag).
Each of the 8 worker threads (matching `num_brokers=8`) opens its own dedicated QP+CQ to the
memserver and owns one broker exclusively; GOI's global `total_order` is minted via one shared
atomic counter; only thread 0 writes ControlBlock, reading the atomic's current value, so no two
threads race to write the same field from different, unordered QPs.

**Full NUMA pinning did not meaningfully change the multi-threaded sequencer's own throughput**
(23.404 Gb/s partial-pin vs 23.464 Gb/s full-pin at the point4500 comparison point — within noise)
— node 1 has 256 CPUs available, ruling out core count as the constraint. The remaining
sub-linear scaling (8 threads yielding roughly 2-2.5x, not 8x, over the single-threaded baseline
at comparable load) is most likely contention on the shared `g_committed_seq` atomic counter under
very high commit rates (cache-line bouncing across 8 cores), though this was not conclusively
isolated — **flagged as a follow-up diagnosis, not fully resolved.**

## Items 3-5

- **Item 3 (RoCE priority isolation): STOPPED, reported per the original STOP-list.**
  Investigated locally: no `mlnx_qos`, `dcbtool`, or `lldptool` available; no `/sys/class/net/.../dcb`
  sysfs interface exposed by the driver; `ethtool` has no per-priority PFC subcommand on this
  system. Genuine RoCE priority/VLAN isolation under DCQCN+PFC requires switch-side QoS
  configuration (trusting DSCP/PCP markings, enabling PFC on a specific priority class) — this is
  explicitly the "switch-side ECN/PFC config confirmation" item the original agent prompt's
  STOP-list calls out as out of scope. Not attempted further. Global pause remains on (RX=on,
  TX=on, unchanged from Phase 1) for all measurements in this sub-phase.
- **Item 4 (QP-count-at-fixed-load sweep): done, clean result.** `ib_write_bw` at `-q` = 1, 2, 4,
  8, 16, 32 (NUMA-pinned, 5s duration each) all land at **98.03-98.05 Gb/s** — flat, invariant to
  QP count. This earns "device ceiling," not just "saturation vs. offered load."
- **Item 5 (spread brokers across enough sender hosts): constraint disclosed, not satisfiable.**
  Only 2 non-memserver hosts are available in this environment (moscxl, c1); c3 is the dedicated
  memserver. With 8 brokers split 4-per-host, each host sources up to half the aggregate offered
  load — **43-54 Gb/s per host at the 90-108 Gb/s aggregate points**, arithmetically derived from
  the 2-host split, not an independent hardware ceiling (both hosts' NICs are 100 Gb/s-rated, per
  `ethtool`). Per-host achieved egress from the funnel-sweep re-run (below) is reported explicitly
  so the reader can judge whether host-side contention (not device-side) is a confound; more hosts
  would be needed to fully rule it out, and none are available.

## The funnel-sweep re-run (7 points × 3 trials, NUMA-pinned + 8-thread sequencer)

Same config as Phase 1 (8 brokers, 4096B messages, inflight=64, `pbr_slots=16384`,
`goi_entries=2000000`) except: NUMA-local placement everywhere, `--seq-threads=8`.

| Offered (aggregate) | Broker achieved (median) | moscxl side (4 brokers) | c1 side (4 brokers) | Sequencer achieved (median) | Sequencer p50 detect-to-commit |
|---|---|---|---|---|---|
| 9.0 Gb/s | 8.96 Gb/s | 4.48 | 4.48 | 5.946 Gb/s | 9.2 µs |
| 18.0 Gb/s | 17.984 Gb/s | 8.99 | 8.99 | 11.925 Gb/s | 10.6 µs |
| 36.0 Gb/s | 35.968 Gb/s | 17.98 | 17.98 | 23.486 Gb/s | 12.9 µs |
| 54.0 Gb/s | 53.952 Gb/s | 26.98 | 26.98 | 33.446 Gb/s | 15.8 µs |
| 72.0 Gb/s | 72.0 Gb/s | 36.0 | 36.0 | 42.715 Gb/s | 21.7 µs |
| 90.0 Gb/s | **~84.6 Gb/s** | ~42.3 | ~42.3 | **~21.2 Gb/s** | **~61.1 ms** |
| 108.0 Gb/s | **~86.2 Gb/s** | ~43.1 | ~43.1 | **~21.1 Gb/s** | **~61.2 ms** |

(Per-host split is symmetric — 4 identically-configured brokers per host in this topology — so
each host's egress is exactly half the aggregate; neither host approaches its own ~98 Gb/s
NUMA-local device ceiling even at the highest points, which is the honest answer item 5's
constraint permits: **sender-host egress does not appear to be the binding constraint** in this
2-host topology, though a >2-host topology was not available to confirm this more rigorously.)

### Signal A (broker-side, NIC-bound funnel) — confirms and sharpens Phase 1's finding

Tracks offered load almost exactly through 72 Gb/s (99.6-100%), then plateaus at **~84.6-86.2
Gb/s** from 90→108 Gb/s offered — flat despite 20% more offered load, the same qualitative
plateau Phase 1 found (~87.5 Gb/s), now measured against a more accurate **~98 Gb/s** NUMA-local
device ceiling (Phase 1 used the ~90 Gb/s non-NUMA-pinned reference). The plateau sits at
**86-88% of the true device ceiling**, not 97% as Phase 1's own (less accurate) reference implied
— a real, if modest, gap plausibly explained by this being a mixed read/write/poll traffic
pattern (sentinel reads, header reads, GOI/CV/CB writes competing with payload writes) rather than
`ib_write_bw`'s pure one-directional write stream, and/or residual sender-host-side effects.

### Signal B (sequencer-side) — the multi-threaded fix substantially works, with a disclosed residual

**Points 1-5 (9-72 Gb/s offered):** the sequencer now **tracks offered load cleanly** up to
**42.7 Gb/s** at the 72 Gb/s point — compare Phase 1's single-threaded sequencer, which was
**already flat at ~16.5-16.7 Gb/s** at this same offered load. This is the core claim Sub-phase 3A
needed: through the entire range where the broker-side signal is still climbing, the sequencer is
no longer the dominant confound.

**Points 6-7 (90-108 Gb/s offered):** the sequencer's throughput drops to **~21.1-21.2 Gb/s** and
p50 detect-to-commit latency explodes to **~61 ms** — a real, sharp degradation, coincident with
exactly the point where Signal A plateaus (shared-NIC saturation). This is qualitatively the same
phenomenon Phase 1 found, but **quantitatively far less catastrophic**: Phase 1's single-threaded
sequencer collapsed to **0.19-0.23 Gb/s** (>98% loss from its own recent peak) at this point;
the multi-threaded sequencer degrades to ~21 Gb/s (~50% loss from its 42.7 Gb/s peak at point 5) —
a real cost of NIC contention, but the multi-QP design is markedly more resilient to it than a
single shared QP was. **This is now a legitimate leg-2 signal**: the sequencer's degradation at
90+ Gb/s is directly attributable to the SAME shared-NIC saturation Signal A shows, not an
independent low software ceiling confounding the story below that point.

## Summary against Sub-phase 3A's stated goal

"Demonstrate [the sequencer] tracks offered load to near the plateau, THEN show the collapse at
~90 Gb/s where the ONLY changed variable is shared-NIC saturation" — **substantially achieved,
not perfectly**: the sequencer tracks cleanly through 72 Gb/s (80% of the ~90 Gb/s knee) before
any degradation, a dramatic improvement over Phase 1's confound (which began at ~36 Gb/s, 40% of
the knee). The residual gap — sequencer still degrades (not fully holds the line) at 90+ Gb/s, and
the atomic-counter contention hypothesis for its own sub-linear scaling is unconfirmed — are
disclosed rather than papered over. Combined with items 3 (stopped, switch access required) and 5
(disclosed 2-host constraint), this sub-phase's leg-2 column is meaningfully cleaner than Phase 1's
but carries three explicit, named caveats into Sub-phase 3C's money figure: no RoCE priority
isolation confirmed, sequencer contention only partially resolved, and sender-host-egress ruled
out only within a 2-host topology.

Raw data, `ib_write_bw`/QP-sweep logs, the sweep script, and a parsed `summary.csv` are archived
in [`w5_phase3a_raw/`](w5_phase3a_raw/).

Pausing here for review per the user's instructions before Sub-phase 3B.
