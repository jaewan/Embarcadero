# W5 Sub-phase 3A raw audit data

Raw per-trial stderr logs behind `../w5_phase3a_results.md`, mirroring `../w5_phase1_raw/`'s
pattern (small text logs, fully archived, not a curated subset).

**Source:** `w5-variants` branch, the RETRY run (`activity.log`
`[2026-07-08T00:01:13Z] START Sub-phase 3A funnel sweep RETRY` through
`[2026-07-08T00:07:19Z] END Sub-phase 3A funnel sweep RETRY (rc=0)`) — the first attempt
(`START`..`END ... interrupted after point4500`) was killed and discarded before this data was
collected, because it only NUMA-pinned the memserver, not the sequencer/brokers; see the note in
that log line. Memserver on c3 (`10.10.10.181`), 4 brokers on moscxl (`10.10.10.10`) + 4 on c1
(`10.10.10.11`), 8-thread sequencer on moscxl.

**Layout:**
- `funnel_sweep/point<N>mbps_trial<T>_{ms,broker0..7,seq}.txt` — one memserver log, 8 per-broker
  logs, one sequencer log, per point/trial. 21 point-trials (7 offered-load points × 3 trials).
- `qp_sweep/`, `numa_ib_write_bw/` — Sub-phase 3A items 1 and 4 (`ib_write_bw` NUMA-local
  re-anchor and QP-count-at-fixed-load sweep).
- `sweep_script.sh` — the exact orchestration script that produced `funnel_sweep/`.

## The seven per-run facts, defined

Same seven facts as `../w5_phase1_raw/README.md` §"The seven per-run facts, defined", with this
sub-phase's actual values (measured 2026-07-08, funnel-sweep RETRY run). Only what changed from
Phase 1's single-threaded sequencer config is called out in full; unchanged facts reference Phase
1's README.

1. **Host↔host QP count.** Same 9 base RC QPs as Phase 1 (8 broker→memserver + previously 1
   sequencer→memserver), **except the sequencer now opens 8 QPs, one per `--seq-threads` worker**
   (`[seq-worker N] connected to memserver qpn=...` × 8 in every `*_seq.txt`), each owning a
   disjoint broker range (`owns brokers [N, N+1)` for the 8-broker config used here). Total: 8
   broker QPs + 8 sequencer-worker QPs = **16 RC QPs terminating on c3**, up from Phase 1's 9.
2. **CQ depth.** Broker and memserver CQ depths unchanged from Phase 1 (4096 CQEs, see Phase 1's
   README fact 2). Each of the 8 sequencer-worker QPs uses the priority-experiment window
   (`kWindow=256`, `CreateRcQp(&d, 4096, 2048, ...)` in `rdma_sequencer.cc`'s `SeqWorkerW5B`) — CQ
   depth 4096, max outstanding WRs 2048 per worker QP (up from Phase 1's single-threaded
   512-depth/64-window sequencer QP).
3. **MR sizes** (8-broker config, from `point9000mbps_trial1_ms.txt`, identical across all 21
   runs): `control=64B cv=512B sentinel=64B goi=128000000B pbr=16777216B blog=2147483648B` —
   byte-for-byte identical to Phase 1 (this sub-phase changes the sequencer's thread count and
   placement, not the memserver's region layout).
4. **NIC port count in use.** 1 (single port, `mlx5_0`/`enp193s0f0np0`/`ens801f0np0`, unchanged
   from Phase 1 — no bonding/multi-port striping; the "add-a-NIC control" was not run this
   sub-phase either).
5. **RoCEv2 CC config.** Unchanged from Phase 1: global pause ON (RX=on, TX=on), autonegotiate
   off, no per-priority PFC/RoCE VLAN isolation, no switch-side ECN/DCQCN change (`mlnx_qos` not
   installed) — still the STOP-listed RoCE priority isolation gap, not resolved in Sub-phase 3A or
   3B.
6. **Hugepage/NUMA placement — the item Sub-phase 3A specifically fixed.** Unlike Phase 1 (which
   disclosed zero hugepages on the NIC's NUMA node and no CPU pinning at all), this sub-phase's
   RETRY run applies `numactl --cpunodebind=1 --membind=1` to **every** process whose host's RDMA
   NIC sits on NUMA node 1 — memserver (c3), sequencer (moscxl, all 8 worker threads), and the 4
   moscxl-side brokers. c1's 4 brokers use `--cpunodebind=0 --membind=0` (c1's NIC is on node 0).
   512×2MB (1GiB) hugepages were reserved on moscxl's node 1 via
   `/sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages` (user-approved through
   `AskUserQuestion`, applied via `sudo tee`, since a direct write was blocked by the auto-mode
   safety classifier as a shared-resource modification); c3 and c1 already had sufficient
   reservations on their own NIC-local nodes. The FIRST attempt at this sweep (discarded, not
   archived here) only pinned the memserver and was killed mid-run once this gap was noticed — see
   `activity.log`'s interrupted-RETRY0 entry.
7. **Exact flock `activity.log` line.**
   `[2026-07-08T00:01:13Z] w5-variants: START Sub-phase 3A funnel sweep RETRY (full NUMA pinning:
   memserver+sequencer+brokers, 7 points x3 trials)` through
   `[2026-07-08T00:07:19Z] w5-variants: END Sub-phase 3A funnel sweep RETRY (rc=0)` (the dataset in
   `funnel_sweep/` is from this run; the preceding interrupted attempt in the same log,
   `START Sub-phase 3A funnel sweep re-run` .. `END ... interrupted after point4500`, is NOT the
   source of this data and was not archived).

Facts 1-4 and 7 are precise; fact 5 discloses the same unaddressed RoCE-isolation gap Phase 1
disclosed (still open going into Sub-phase 3B); fact 6 documents this sub-phase's actual fix (full
NUMA pinning), closing the gap Phase 1's README flagged as unresolved.

## `committed_seq` is a benchmark-only order counter, not a real chronological global order

The multi-threaded sequencer (`SeqWorkerW5B` in `../../../src/rdma_transport/rdma_sequencer.cc`)
mints each batch's `total_order` as `thread_id + local_seq * num_seq_threads` — globally unique
across threads, but **interleaved by construction, not by arrival time**: thread 3's 100th batch
and thread 5's 3rd batch can carry orders that don't reflect which one the sequencer actually
observed first. This is fine for THIS benchmark's purpose (measuring aggregate sequencer
throughput/latency under N independent read streams), but `total_order` must never be treated as,
or fed into, a real CXL commit-watermark or downstream consumer that depends on true global
ordering — the production single-threaded sequencer path is unaffected and remains the only
order-correct implementation in this codebase.

The ControlBlock's `committed_seq` watermark published to readers is NOT the raw interleaved
`total_order` value — it is the **minimum of all 8 threads' independently-owned progress
counters**, scaled by thread count (`min(all_progress) * num_seq_threads`), i.e. "every thread has
drained at least this many batches." This is a genuinely monotonic, safe-to-consume watermark (no
reader can be told about a batch some thread hasn't actually finished), even though the
underlying per-batch `total_order` numbering is benchmark-only. See the inline comment on
`SeqWorkerW5B` in `rdma_sequencer.cc` for the same explanation at the point of definition.
