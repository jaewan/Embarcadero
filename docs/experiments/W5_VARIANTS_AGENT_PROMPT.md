# W5-A / W5-B RDMA Variants + E4e Conjunction Contrast — Fresh Agent Build & Measure Prompt

You are a fresh Claude Code coding agent on the **moscxl** testbed. You have no prior context. This prompt is your complete brief. Read it fully before running anything.

---

## 1. MISSION

You are building and measuring **two RDMA design points** for a distributed log system (Embarcadero), to produce the paper's **"money figure" (experiment E4e)** — a three-column conjunction contrast: **W5-A vs W5-B vs CXL-reference**.

### The conjunction thesis you are measuring

Cheap ordering **repair** needs **two properties SIMULTANEOUSLY** on the committed-payload path (full-bandwidth traffic that must survive host death — NOT the ~5 MB/s metadata poll):

- **Leg 1 — failure independence:** log state outlives any host (payload survives when the host that produced it dies).
- **Leg 2 — no shared network endpoint:** every host reaches state at memory speed with nothing to funnel through.

**Over RDMA you can hold EITHER LEG BUT NOT BOTH.** CXL delivers **both** because it is *passive memory* (memory-speed loads, no NIC on the payload path). **The CONJUNCTION is the contribution.** Your job is to make that conjunction *measurable on owned hardware*:

- **W5-A** (`embarlet_rdma_dram`, `-DBLOG_PLACEMENT=dram`): payload Blog lives in **each broker's host DRAM**. **HOLDS leg 2** (each broker's own NIC serves its own payload — no shared NIC on the payload path). **SURRENDERS leg 1** (broker-host death loses its Blog; order survives via GOI/PBR on the memserver, data is lost). → measures the **leg-1 cost**: "order survives, data lost" + re-replication.
- **W5-B** (`embarlet_rdma_memserver`, `-DBLOG_PLACEMENT=memserver`): payload Blog lives on the **dedicated memory server c3**. **HOLDS leg 1** (survives any host death while c3 stays up). **SURRENDERS leg 2** (c3's ONE NIC funnels ALL payload). → measures the **leg-2 cost**: NIC ceiling, CQ contention, latency inflation.
- **CXL reference** (existing Embarcadero-on-CXL build): payload + metadata in CXL shared memory. **Holds BOTH legs.** Pays neither cost.

### The single-variable design that makes the contrast clean

**Decision 1 (load-bearing, non-negotiable):** **ALL failure-critical metadata** — GOI, `committed_seq`/ControlBlock, CompletionVector, PBR, and the sentinel array — lives on the **c3 memserver in BOTH variants**. Therefore **payload Blog placement is the SOLE full-bandwidth variable.** `-DBLOG_PLACEMENT` moves *only* the payload Blog. The metadata funnel is equal in both variants, negligible, and **cancels** → the **A→B delta is PURE payload traffic = exactly the leg-2 cost.**

### Framing guardrail (do not violate in any report or plot)

Present E4e as **{W5-A, W5-B} vs CXL — two RDMA design points, each engineered to hold one leg, neither able to hold both** — **NOT** a single-knob A-vs-B ablation. `-DBLOG_PLACEMENT` is a **code-sharing device, not the claimed scientific variable.** Pre-empt "just keep metadata distributed and move only payload": **NO** — it is the *payload* that must survive for leg 1, and payload is the funnel; moving the 5 MB/s metadata rescues nothing.

---

## 2. FRESH-CLONE + STANDALONE-BUILD SETUP (exact commands)

Everything you build on (RDMA foundation + the MTU write-fix) is already on `origin/main @ 2ab8d48`.

```bash
# Fresh clone into W5's OWN dir — SEPARATE from the D1 agent's ~/Embarcadero-sessions/d1-impl
git clone https://github.com/jaewan/Embarcadero.git ~/Embarcadero-sessions/w5-variants
cd ~/Embarcadero-sessions/w5-variants
git checkout main
git switch -c w5-variants
```

> **GitHub creds note:** both the `git clone` above and the later `git push -u origin w5-variants` require the **interactive session's** GitHub credentials — the agent cannot authenticate GitHub on its own. If clone/push fails on auth, **STOP and ask the human** to run that one step.

### Standalone build of the RDMA lane (needs only `libibverbs-dev`, no gRPC/main-tree build)

```bash
cd ~/Embarcadero-sessions/w5-variants/src/rdma_transport
cmake -S . -B build_rdma && cmake --build build_rdma -j    # COMPILE — do this OUTSIDE the flock lock
```

`src/rdma_transport/` has its own `CMakeLists.txt` and depends only on `libibverbs-dev`. **You do not need the full gRPC build for this work.** Your entire code lane is `src/rdma_transport/` + W5's own docs.

---

## 3. ⛔ HARD RULES — CO-EXISTENCE WITH A LIVE D1 AGENT (READ TWICE)

**A D1 agent is ALREADY RUNNING on this same machine and repo**, in `~/Embarcadero-sessions/d1-impl` (branch `d1-impl`), editing `src/embarlet/topic.cc` and running loopback ORDER=5 tests. Violating any rule below can **kill the other agent's live run, corrupt its tree, or strand the shared testbed.**

1. **SEPARATE CODE LANE.** Work ONLY in `~/Embarcadero-sessions/w5-variants` on branch `w5-variants` with your own build dir. Touch **ONLY** `src/rdma_transport/` + W5's own docs. **NEVER** edit `src/embarlet/topic.cc`, `topic.h`, `cxl_datastructure.h`, `cxl_manager`, `network_manager`, `sequencer_utils.h`, `queue_buffer`, `publisher`, `subscriber`, `common`, or **any** other D1-owned file. The lanes never overlap because they are different dirs/branches/file-trees.

2. **SHARED FLOCK LOCK — EVERY RUN SERIALIZES.** Any **RUN** (broker/embarlet, `throughput_test`, `ib_write_bw`, the memory server, token server, ORDER=5 gate, or ANY process that touches CXL/ports/the NIC) MUST hold:
   ```bash
   ssh broker 'flock -w 2400 ~/Embarcadero-sessions/testbed.lock -c "<your run command>"'
   ```
   This is a **SINGLE SHARED lock file** used by BOTH agents. Only one agent runs at a time. **You will frequently block on D1's runs — that is expected; wait, do not spin up a parallel run.** Do NOT bypass, copy, or make a second lock file.

3. **ACTIVITY LOG.** Log `START` and `END` of every run to `~/Embarcadero-sessions/activity.log`. **Check this log before assuming the testbed is idle.** This is how D1 and the human see who holds the testbed.

4. **COMPILE OUTSIDE THE LOCK.** Hold the flock lock **ONLY for runs, never for compiles.** Compiles may safely overlap between agents because they live in different clone/build dirs. Take the lock → run → log END → release. Keep the held window as short as possible.

5. **NEVER `pkill -f`.** `pkill -f throughput_test` / `pkill -f embarlet` / **any** `pkill -f <pattern>` matches the OTHER agent's processes **AND your own shell argv (self-kill).** This is the single most dangerous command on this box. **Kill strays ONLY by explicit PID**, after PID inspection (`ps`, `pgrep` to *inspect*, not to bulk-kill). Before releasing the lock, verify no stray process / stranded server remains **by PID**.

6. **DIRECTORY ISOLATION.** NEVER touch `~/Embarcadero` (the human's live tree), `~/Embarcadero-sessions/d1-impl` (D1), `~/Embarcadero-sessions/s2-work` (a prior session — do NOT reuse it), or any other `~/Embarcadero-sessions/*` dir. W5 lives ONLY in `~/Embarcadero-sessions/w5-variants`.

7. **LOCK HYGIENE ON FAILURE.** A client that dies before the OOB/RDMA connect completes can strand the server holding the lock. After **every** run, explicitly confirm the lock is free AND no stranded server / stray PID remains (by PID) before yielding the testbed back to D1. The RDMA foundation already has a 30 s `accept()` timeout to reduce this risk, but verify anyway.

---

## 4. RAMP-UP READING LIST (read all four before Phase 1)

- **`docs/design/W5_RDMA_VARIANTS_SPEC.md` (DRAFT v2.1)** — the authoritative spec: Decision 1 (metadata-on-c3-both), the two-leg thesis, RegionAccessor hot/cold seam, the producer WRITE sequence (C2), the sentinel array (E2), no-`IBV_SEND_FENCE` (E3), lease timing (M2), coherence scope (C1). Also read `docs/design/W5_BROKERINFO_RDMA_PROPOSAL.md` for the additive proto fields.
- **`docs/experiments/rdma_variants.md`** — the primary run book for E4e / E1-leg3 / E6 / reviewer-D; declares `Paper/improvement_plan.md` v4 as source of truth.
- **`sessions/handoff-05.md`** — where the prior W5 session left off (what's built, the TODO-next list, fabric caveats).
- **`sessions/rdma_fabric_setup.md`** — the fabric table, verified bandwidths, GID auto-scan, `ib_write_bw` invocation, non-persistent netplan config.

---

## 5. WHAT IS ALREADY DONE (build ON this, not from scratch)

The RoCEv2 RC foundation in `src/rdma_transport/` is **built and verified** on `origin/main`:

- `rdma_common.{h,cc}` + `rdma_bench_main.cc` + `CMakeLists.txt`: device open, **GID auto-scan** (sysfs, RoCEv2 IPv4 — pass `gid=-1`), PD/CQ/MR (MRs already carry all `REMOTE_*` flags), manual RC QP bring-up (`INIT→RTR→RTS` with GRH), one-sided **`PostRead` / `PostWrite` / `PostFetchAdd`**, `PollCq` (blocking spin, WC-error logging), TCP OOB exchange with a **30 s bounded `accept()`** (`select()` timeout so a dead client can't strand the server).
- **Verified bandwidths:** WRITE **53.65 Gb/s @ 64 KiB**, READ **56.18 Gb/s**, FETCH_ADD **1.99 Mops/s @ 1 QP** (matches `ib_write_bw` device ceiling ~54 Gb/s broker↔c1).
- **MTU write-fix LANDED:** `QpEndpoint` now carries `mtu` (active-MTU enum); `ConnectRcQp` sets `path_mtu = min(local active, remote active)` to stop multi-packet WRITE `REM_INV_REQ` rejection under active-MTU divergence. `QpEndpoint.mtu` is `uint8_t`; `mtu=0` = legacy peer (no negotiation) — both ends must be rebuilt together.
- Confirmed non-issues: MR already has `REMOTE_WRITE`; no `IBV_SEND_INLINE` anywhere (all use sglist) — verified correct. Client reads-before-writes vs server writes-then-reads to avoid a kernel-buffer deadlock. Atomics are capped at 16 outstanding/QP (so ~30M tok/s needs multi-QP/thread).

Key signatures/locations to reuse: `rdma_common.h:32-84` (DeviceCtx, QpEndpoint), `:90-102` (PostRead/PostWrite/PostFetchAdd/PollCq); `rdma_common.cc:32-55` (GID scan), `:57-80` (OpenDevice auto-scan), `:90-98` (RegisterMr), `:102-130` (CreateRcQp), `:148-195` (ConnectRcQp, MTU negotiation at 156-157, GRH 167-175), `:204-245` (Post*), `:247-265` (PollCq), `:279-326` (OOB exchange); `rdma_bench_main.cc` for the CLI harness pattern (`--role/--op/--dev/--gid=-1/--ip/--port/--size/--bytes/--inflight/--secs`).

---

## 6. FABRIC FACTS + NON-PERSISTENCE CAVEAT + BUILD NOTES

| Host | Name | IP (RoCE) | GID idx (do NOT hardcode — scan) | Role |
|---|---|---|---|---|
| broker | moscxl | 10.10.10.10 | 3 | sequencer/broker |
| c1 | mos143 | 10.10.10.11 | 5 | W5-A leg-1 **kill target** |
| c3 | mos181 | 10.10.10.13 | 5 | **memory server** (1.26 TB) |
| c2 | mos144 | 10.10.10.12 | — | scale-out (**needs sudo** — human-gated) |
| c4 | mos182 | 10.10.10.14 | — | scale-out (**needs sudo** — human-gated) |

- **Transport:** RoCEv2 RC on ConnectX-5 `mlx5_0`, subnet `10.10.10.0/24`, **MTU 9000 (RoCE path MTU 4096)**.
- **Verified single-QP bandwidth:** broker↔c1 = **54 Gb/s**, broker↔c3 = **88 Gb/s**. Funnel ceiling **~88 Gb/s @ 1 QP → toward ~100 Gb/s line rate multi-QP.**
- **⚠ GID indices are NOT persistent** — c1 drifted GID 5→3 on reboot; c1 lost its RoCEv2 IPv4 GID on a reboot once. **Never hardcode; always pass `gid=-1` for auto-scan.** If a peer lost its GID after a reboot, **re-apply per `sessions/rdma_fabric_setup.md`** (netplan `99-roce.yaml`) — **c1 has sudo for this**; c2/c4 do not.
- **CC ground truth (verified 2026-07-05, broker):** `mlx5_core` FW `16.28.2006`; **per-priority PFC OFF on all 8 priorities**; only **link-level 802.3x global pause ON (RX/TX)**; DCQCN `cc_params` at defaults (CNP on **DSCP 48 / prio 6**). **Switch-side ECN/PFC is UNCONFIRMED** (human/infra step — see STOP list).
- **c3 build note:** c3 lacks `cmake` → build any c3-resident binary with a **`g++` one-liner** against `libibverbs` (mirror `CMakeLists.txt` flags). c1 is verified for cmake builds.
- **`ib_write_bw` invocation (the funnel reference line):** `ib_write_bw -d mlx5_0 -x <gid_idx> -q N` (multi-QP aggregate). This must run under the flock lock.

---

## 7. PHASED BUILD + MEASURE PLAN (pause for human review at each ⏸)

**Methodology rules (NEVER broken, apply to every measurement below):**
1. Every number = **MEDIAN ± RANGE of ≥3 trials** (**5 trials** for tail / P99.9 claims). No single-trial cells, ever.
2. No headline includes a **co-located publisher** — module-saturation is a *labeled ceiling microbench only*.
3. Report **msgs/s AND GB/s** both, with the message-size sweep **{128 B, 512 B, 1 KB, 4 KB, 16 KB}** for headline experiments.
4. Latency = **latency-vs-load CURVES** at fractions of *each system's OWN peak* (10/50/70/90%) — no past-saturation tails.
5. **ONE SCRIPT PER FIGURE**, committed under `scripts/` (plots under `scripts/plot/` or `scripts/publication/`), artifact-evaluation-ready.
6. All cross-host runs under the **flock lock** with `activity.log` START/END lines.
7. **Every run must record:** host↔host QP count, CQ depth, MR sizes, NIC port count in use, RoCEv2 CC config, hugepage/NUMA placement, and the exact flock `activity.log` line. Missing any of these makes the run non-artifact-evaluable.

### Protocol invariants you MUST preserve (transport swap, not a redesign)

- **BatchHeader** `sizeof == 128`. Broker-owned body = **`[0, 80)`**. Field offsets: `batch_complete@16`, `pbr_absolute_index@48`, `total_size@56`, `start_logical_offset@64`, `log_idx@72` (broker-written body); `ordered@24`, `total_order@80`, `batch_off_to_export@88` (sequencer-written); **`publish_commit@96` = the sentinel, written LAST (8 B WRITE).** `[80,96)` is left for the sequencer. (Anchors: `cxl_datastructure.h:393` BatchHeader, `:432` BatchHeaderPublishCommitted — **read-only reference; do NOT edit that D1-owned file.**)
- **Producer WRITE sequence on ONE RC QP (C2):** (1) Write Blog payload **64 B-aligned** [W5-A = local CPU store to broker DRAM, no network; W5-B = RDMA WRITE to memserver Blog MR = the payload funnel]; (2) **one contiguous** RDMA WRITE of BatchHeader body `[0,80)` to memserver PBR (both variants; includes `batch_complete=1`, `pbr_absolute_index`, `log_idx`; leaves `[80,96)` for the sequencer); (3) a **SEPARATE, later** RDMA WRITE of **ONLY `publish_commit@96` (8 B)** to PBR AND this broker's sentinel-array slot, posted after step 2 **on the SAME RC QP**. Same-QP RC ordering guarantees the sentinel lands after the body.
  - **WHY separate (do not fold into the body WRITE):** the trailing word of a single WRITE may land out of order internally; correctness rests on a distinct, later, same-QP sentinel WRITE.
  - **NO `IBV_SEND_FENCE` (E3):** the CXL `sfence` becomes RC same-QP message ordering; x86-TSO already orders local WB body stores before the sentinel store.
- **Sentinel readiness gate:** `BatchHeaderPublishCommitted = (publish_commit != UINT64_MAX && publish_commit == pbr_absolute_index) && batch_complete == 1`. Never trust body until the sentinel matches (torn-window safety identical to CXL).
- **Packed contiguous sentinel array (E2):** contiguous per-broker `publish_commit` slots on the memserver; each broker WRITEs its own slot; the **sequencer READs the WHOLE array in ONE contiguous RDMA READ**. This REPLACES per-ring gather reads — a gather READ over the rings is **impossible** (an RDMA READ's SGE scatters into LOCAL buffers only; its remote side is a single contiguous range).
- **RegionId / RdmaRegionKind:** `kControlBlock=1, kCompletionVector=2, kPbrRing=3, kBlog=4, kSentinelArray=5, kGoi=6` (mirror `RDMA_REGION_*`).
- **AccessStatus** `{ kOk, kRetry, kPeerDown, kTransportError }`. `kPeerDown` = RC `RETRY_EXC → QP ERROR` on a dead peer = the failure-detection fast path. For a killed W5-A broker host, the sequencer's Blog READ returning `kPeerDown` is the "data lost" signal (order survives, payload lost).
- **Single-writer ownership** (no cross-host RMW on the hot path). RDMA atomics are responder-serialized and NOT atomic vs the host CPU — cold-path membership/lease only.
- **Coherence scope (C1):** on x86-64, PCIe DMA is architecturally cache-coherent, so an RDMA READ of a location the remote CPU just wrote with ordinary WB stores returns the new value with NO writer-side `clflush`, NO reader-side invalidate. All MRs are **WB-mapped ordinary DRAM** (no write-combining / non-temporal stores). RDMA-on-x86 exercises single-writer ownership, sentinel-last readiness, torn-read avoidance, absence of cheap cross-host atomics — it does **NOT** exercise the host-local cache-management half (`clflushopt`/`clflush`); that is Track 02 TLA+ + Track 04 injection only. **Do not claim the flush half.**
- **Lease timing (M2):** do NOT reuse CXL lease constants unchanged over RDMA — inflate the margin to cover `renewal_latency_p99 + clock_skew_bound` (µs-scale latency + jitter). Logic (`kEpochCheckInterval=100`, `RefreshBrokerEpochFromCXL`, renewal/expiry) inherited from Track 01; only timing constants recomputed.

---

### PHASE 1 — W5-B memory server on c3 + funnel sweep

**Build (in `src/rdma_transport/` only):**
- A **memory server** that runs on **c3**: multi-QP / multi-CQ, **hugepage + NUMA-local** allocation, registers **all metadata MRs (ControlBlock, CompletionVector, PBR ring, sentinel array, GOI) AND the payload Blog MR** (this is the W5-B placement). Use GID auto-scan (`gid=-1`). Build on c3 via the **`g++` one-liner** (c3 has no cmake).
- The producer/sequencer path against c3 implementing the **C2 WRITE sequence** and the **packed sentinel-array READ** (E2) as specified above.

**Measure — the funnel sweep (leg-2 cost primary):**
- **Set CC FIRST** (record it) so the knee is the **c3-NIC bandwidth ceiling**, not RC-retransmit collapse. With current host state (PFC off, global pause on), **isolate RoCE on its own priority/VLAN** if you can, and disclose in the fairness appendix.
- Establish the reference line: `ib_write_bw -d mlx5_0 -x <gid> -q N` **aggregate at c3** (multi-QP). This is the **CC-agnostic device ceiling**.
- Sweep offered payload load **10% → >100% of the c3-NIC ceiling, multi-QP (`-q n`)** to reach the device ceiling (not a single-queue artifact). Report a **MIXED** load: payload writes + sequencer PULL polls + subscriber reads.

**Report bar (Phase 1):**
- Achieved READ+WRITE goodput vs c3-NIC line rate; the **KNEE at the NIC ceiling**.
- **PLATEAU statement (the actual pass bar — NOT "we hit line rate"):** goodput **plateaus at ~the raw multi-QP `ib_write_bw` ceiling and does NOT rise as senders/QPs are added.** That plateau survives any DCQCN-vs-PFC dispute.
- Poll/payload **CQ contention** = poll-latency inflation as payload load rises.
- End-to-end **P50/P99 latency inflation curve** vs load.
- **Pull-path amplification:** quantify the sentinel-poll bandwidth's share of the funnel at saturation ("the passivity that grants leg-1 forces the pull that worsens leg-2 — the thesis in one plot").
- ≥3 trials (median ± range), all seven per-run facts recorded, CC config reported with **every** number. **⏸ PAUSE for human review.**

---

### PHASE 2 — W5-A broker-DRAM variant + host-kill data-loss/re-replication

**Build (in `src/rdma_transport/` only):**
- The **W5-A placement**: payload Blog in **each broker's own host DRAM** (local CPU store, no network for the payload write); **metadata still on c3** (Decision 1). The control plane (BatchHeader body + sentinel to c3's PBR/sentinel array; sequencer Blog READ **broker-direct**) runs over the one-sided verbs already built. Sequencer's Blog READ to a dead broker returns `kPeerDown`.

**Measure — leg-1 cost primary:**
- Procedure: (1) steady-state publish at **~60% of W5-A peak**, **RF ≥ 2**, **M independent client sessions**; (2) at `t0` **kill the broker HOST** — this is a **HOST kill, not a process kill**: `kill -9` the broker process **AND drop the host** (`ip link set down` the NIC, or power / cgroup-freeze) so the **DRAM Blog actually dies**. (Killing only the process leaves the Blog alive and does NOT exercise leg-1.) (3) measure.

**Report bar (Phase 2):**
- (a) Blog bytes **unrecoverable** from that host; (b) **re-replication volume + time** to restore RF from surviving replicas; (c) **per-session stall distribution across ALL M sessions**, MTTR incl. session-table recovery + PBR rescan; (d) **failure-detection latency via BOTH paths** — sequencer RDMA READ/WRITE to the dead host completing in error (`RETRY_EXC → QP ERROR = kPeerDown`, fast path) **vs** lease timeout (backstop) — **report WHICH FIRED FIRST**.
- ≥3 trials (median ± range); all seven per-run facts recorded. **⏸ PAUSE for human review.**

---

### PHASE 3 — E4e contrast (money figure) + E1-leg3 + E6

**⏸ PRE-REGISTRATION GATE (hard ordering constraint — do this BEFORE running E1/E4e):** commit to the repo, before running: per-baseline knee locations; expected throughput compression ~1.2–1.6× vs CXL-upgraded baselines; the expectation that low-load latency is largely transport / the knee is architecture; and **for E1 specifically the expected RDMA↔CXL delta.** **RULE: any result contradicting a pre-registered prediction → report it and REVISE THE CLAIM, not the experiment.**

**E4e — the three-column money figure (SAME workload + SAME client load generator across all three):**
- **W5-A column (leg-1 cost):** `embarlet_rdma_dram`, event = **broker-HOST kill** (as Phase 2). Outputs = the Phase-2 leg-1 metrics. Expect a re-replication-bound stall.
- **W5-B column (leg-2 cost):** `embarlet_rdma_memserver`, event = **LOAD RAMP to memserver-NIC saturation (NO kill)** (as Phase 1). Outputs = the Phase-1 funnel/plateau/pull-amplification metrics.
- **CXL column (neither cost):** existing Embarcadero-on-CXL moscxl build, event = **broker PROCESS kill (process-level, NOT host-level)** — Blog survives in CXL shared memory (no data loss), no shared NIC (no funnel). Output = substrate latency/bandwidth (single-host) + process-level recovery. **MUST explicitly label "process-level, not host-level"** (plan line 530). **Do NOT overclaim host-level CXL failure independence — that is W7 upside, out of scope here.** The leg-1/leg-2 contrast itself is fully carried by the two RDMA columns.
- **Figure:** grouped bars or 3-panel — **left:** leg-1 cost (W5-A loss/repair) vs CXL (~0); **middle:** leg-2 cost (W5-B funnel knee) vs CXL (no knee); **right:** summary "CXL pays neither." Add the **add-a-NIC control** for W5-B: bond a second c3 NIC/port, show the funnel **MOVES but does not disappear** (topological argument).

**E1 leg-3 = Embarcadero-on-RDMA (W5-A):** run `embarlet_rdma_dram` on the **SAME ARCHITECTURE as leg-4 (Embarcadero-on-CXL)** so `leg4 − leg3` isolates **substrate gain (RDMA→CXL)** with architecture held fixed. Same workload, same message-size sweep, latency-vs-load curves. **E1 can shoot the author** (plan line 508): if the RDMA→CXL gap is small, the SLO headline is mostly architecture not substrate — **publish anyway** and revise the claim. (Pre-register the RDMA↔CXL delta first.)

**E6 = R-inflation under payload load (BOTH variants):** sweep payload load; measure **R = repair latency / hold-window fill time** vs load; overlay **PREDICTED vs OBSERVED** (tie to Track 02 / regime model). Expected ranking: **W5-B inflates fastest > W5-A (per-broker NICs) inflates less > CXL inflates least.** PLUS `tc-netem` jitter injection swept against the hold window: predicted vs observed repair success as jitter → T.

**(Optional co-opt) Reviewer-D ~30M tok/s naive RDMA token server:** reproduce the rate with a minimal RDMA token server (one-sided FETCH_ADD token, or WRITE-based token ring), **multi-QP/thread** (single-QP atomics cap at 16 outstanding / 1.99 Mops/s — you need multi-QP for ~30M tok/s). **Document the config in enough detail to NOT be a strawman.** Then show what it ignores: (a) **token RTT on the client critical path** (every ordered message pays a sequencer round-trip — measure added client-path latency at various loads); (b) **the repair loop** (straggler wait / gap fill / retransmit — where FIFO actually costs). Framing: **"30M tok/s is real and irrelevant — the binding constraints are token RTT and repair, which the token rate does not touch."**

**Report bar (Phase 3):** the three-column E4e figure + E1 waterfall leg-3 + E6 predicted-vs-observed curves, each with ≥3 trials (5 for P99.9), CC config on every W5-B number, pre-registration committed before running, all seven per-run facts. **⏸ PAUSE for human review.**

---

## 8. COMMIT / PUSH / BUNDLE + REPORT-BACK

- **Commit** (scoped pathspec — ONLY `src/rdma_transport/` + W5's own docs; **never** `git commit -a`, never stage the repo-reorg or any D1 file):
  ```bash
  git commit --no-verify --no-gpg-sign <scoped W5-owned paths>
  ```
  `--no-verify --no-gpg-sign` are **REQUIRED** (gpg signing hangs; the pre-commit hook is interactive). Every commit message ends **VERBATIM** with:
  ```
  Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
  ```
- **Reconcile — NEVER push to main.** Push the branch and hand back a bundle for human review:
  ```bash
  git push -u origin w5-variants          # needs interactive session's GitHub creds
  git bundle create /tmp/w5.bundle origin/main..w5-variants
  ```
- **Report back** at each ⏸ pause: what was built, the measurement results (median ± range), the recorded per-run facts, the CC config, and any deviation from a pre-registered prediction (with the revised claim).

---

## 9. ⛔ STOP-AND-REPORT LIST (do NOT block on these — flag to the human and continue where possible)

- **c2 / c4 scale-out** is blocked on **sudo onboarding** (fabric TODO). Do NOT assume >2 broker hosts are available. Flag; do not attempt to onboard.
- **Switch-side ECN/PFC is UNCONFIRMED** (switch-admin fact). Until confirmed, rely on the **CC-agnostic raw-device-ceiling plateau** claim. If only global pause is available, isolate RoCE on its own priority/VLAN and disclose in the fairness appendix (global pause risks HOL blocking that would make the funnel look *worse* than inherent). Flag as a human/infra step.
- **Anything needing the Track-01 `RegionAccessor` freeze:** the shared cold/virtual + hot/static (non-virtual `ReadView`/`Publish`/`ReadSentinelArray`) seam is owned by Track 01 and shared with Track 04 — **do NOT build a W5-only seam** or edit `topic.{cc,h}`. If your work needs the frozen interface and it is not yet available, **STOP and report.**
- **Any fabric config change needing sudo** beyond re-applying c1's GID (c1 has sudo) — e.g. c3/c2/c4 netplan, switch config. Flag; do not attempt.
- **GitHub auth** for clone/push — needs the interactive session's creds. If it fails, STOP and ask the human to run that one step.
- **Any need to touch a D1-owned file** to make something work — STOP and report; do NOT edit outside `src/rdma_transport/` + W5's docs.
