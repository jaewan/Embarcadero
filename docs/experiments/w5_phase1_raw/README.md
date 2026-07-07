# W5 Phase 1 raw audit data

Raw per-trial stderr logs and metadata behind `../w5_phase1_results.md`, mirroring
`../delta_measurement_raw/`'s pattern. Unlike that dataset, the full run here is small (~1 MB
total — this harness's runs emit a handful of one-line RESULT/status logs, not multi-GB CSV
traces), so everything is archived, not a curated subset.

**Source:** `w5-variants` branch, commit `0e32aa2` (post OOB-accept-fix, post sequencer-pipeline
fix), sweep script `sweep_script.sh` (copied verbatim from the scratchpad it actually ran from).
Memserver on c3 (`10.10.10.181`), 4 brokers on moscxl (`10.10.10.10`) + 4 on c1 (`10.10.10.x`,
via one `ssh` call backgrounding all 4), sequencer on moscxl.

**Layout:**
- `funnel_sweep/point<N>mbps_trial<T>_{ms,broker0..7,c1_brokers,seq}.txt` — one memserver log, 8
  per-broker logs, one c1-launcher-shell log (usually empty — the 4 c1 brokers redirect their own
  output to files on c1, scp'd back into `broker4..7.txt`), and one sequencer log, per point/trial
  (renamed `.log`→`.txt` on archival only, to not collide with the repo's `*.log` gitignore rule —
  content is unmodified). 21 point-trials (7 offered-load points × 3 trials).
- `funnel_sweep/summary.csv` — parsed RESULT-line summary across all 21 point-trials (offered vs.
  achieved broker-side bandwidth, sequencer batches/throughput/stale_misses).
- `token_repro/{global,shard}_thr_{1,2,3}_{srv,cli}.txt`, `lat_{1,2,3}_{srv,cli}.txt` — the
  reviewer-D token-server measurement's 9 runs (3 throughput trials × 2 counter configs, 3
  latency trials).
- `sweep_script.sh` — the exact orchestration script that produced `funnel_sweep/`.

## The seven per-run facts, defined

The agent prompt (`W5_VARIANTS_AGENT_PROMPT.md` §7) requires seven facts recorded for every run
without naming them individually beyond a paraphrase. Spelled out explicitly here, with Phase 1's
actual values (measured 2026-07-08, same fabric state as the sweep):

1. **Host↔host QP count.** Phase 1 (W5-B) opens 9 RC QPs terminating on c3: 8 broker→memserver
   (one per broker) + 1 sequencer→memserver (`meta_qp`). No broker-to-broker or broker-direct
   sequencer QPs in W5-B (Blog lives on the memserver, not per-broker — that's W5-A/Phase 2).
2. **CQ depth.** Memserver: 4096 CQEs per client QP (`CreateRcQp(&d, 4096, 512, ...)` in
   `rdma_memserver.cc:143`). Broker: 4096 CQEs on its meta_qp (`rdma_broker.cc:117`). Sequencer:
   4096 CQEs on its meta_qp (`rdma_sequencer.cc:112`). Max outstanding WRs (send+recv) per QP:
   512 (memserver, sequencer), 2048 (broker).
3. **MR sizes** (8-broker Phase 1 config, from `point*_ms.txt`, identical across all 21 runs):
   `control=64B cv=512B sentinel=64B goi=128000000B pbr=16777216B blog=2147483648B` (goi =
   2,000,000 entries × 64B; pbr = 16384 slots/broker × 8 brokers × 128B; blog = 256MiB/broker × 8).
4. **NIC port count in use.** 1 (single port, `mlx5_0`, no bonding/multi-port striping — the
   "add-a-NIC control" from the Phase 3 plan, which would change this, has not been run).
5. **RoCEv2 CC config.** `ethtool -a` on moscxl's RDMA interface (`enp193s0f0np0`) at the time of
   the sweep: **global pause ON (RX=on, TX=on)**, autonegotiate off. **No per-priority PFC and no
   dedicated RoCE VLAN/priority isolation were configured** — `mlnx_qos` is not installed and no
   switch-side PFC/ECN/DCQCN change was made (out of the STOP-list scope: switch config needs
   confirmation this session didn't have). This is the SAME uncontrolled CC state Phase 0 measured
   the ~90 Gb/s `ib_write_bw` reference under, so Phase 1's knee is measured under consistent
   conditions with its own reference line — but it is **not** the isolated-RoCE-priority state the
   Phase 3 prerequisites correctly call out as still needed before Signal B's collapse can be
   attributed to "NIC saturation" rather than "global-pause-triggered collapse specifically."
6. **Hugepage/NUMA placement.** Memserver's MRs are hugepage-backed (`MAP_HUGETLB`, confirmed by
   the "hugepage alloc ok" lines in every `*_ms.txt`). **NUMA locality was NOT controlled**: the
   RDMA NIC (`enp193s0f0np0`) sits on NUMA node 1 (`/sys/class/net/.../device/numa_node`), but
   node 1 has **zero** 2 MB hugepages reserved on this host (`node0=64, node1=0, node2=2048` in
   `/sys/devices/system/node/node*/hugepages/hugepages-2048kB/nr_hugepages`) — so the memserver's
   hugepage-backed regions were necessarily allocated from a NUMA node other than the NIC's own,
   and no `numactl`/CPU-pinning was used to keep the memserver process itself NUMA-local either.
   This is a real, disclosed gap, not an oversight papered over: Phase 1's numbers may be
   understating achievable bandwidth/inflating latency by some cross-NUMA-hop amount not otherwise
   isolated in this dataset.
7. **Exact flock `activity.log` line.** `[2026-07-07T19:39:22Z] w5-variants: START Phase 1 funnel
   sweep RETRY2 (post OOB-accept-race fix, full restart from point1 trial1, 8 QPs: moscxl x4 + c1
   x4, memserver on c3, 7 points x3 trials)` through `[2026-07-07T20:08:58Z] w5-variants: END
   Phase 1 funnel sweep RETRY2 (rc=0)` (the dataset in `funnel_sweep/` is from this run — the two
   earlier START/END pairs for the same nominal sweep in `activity.log` are the interrupted,
   pre-OOB-fix attempt and are NOT the source of this data, per the note in `../w5_phase1_results.md`
   §0).

Facts 1-4 and 7 are precise; facts 5-6 disclose real, unaddressed gaps rather than asserting
control the run didn't actually have. Closing those two gaps is exactly what the Phase 3
prerequisites (RoCE priority isolation, and — implicitly — NUMA-aware placement) are for.
