# Handoff — Track 05 (RDMA variants, W5-A + W5-B)

**Date:** 2026-07-05. **Track id:** `05-rdma-variant`.

## What I did

1. **Two-variant W5 redesign spec** — the W1 on-paper closure (plan item 6).
   `docs/design/W5_RDMA_VARIANTS_SPEC.md`, now **v2** (revised after an adversarial review + the
   fabric bring-up).
2. **Experiment protocol** — `docs/experiments/rdma_variants.md` (E4e / E1 leg-3 / E6 + reproduced
   naive RDMA token server), updated for the real RoCEv2 fabric.
3. **BrokerInfo RDMA field proposal** — `docs/design/W5_BROKERINFO_RDMA_PROPOSAL.md` (Q3 deliverable;
   additive proto fields for Track 01 to apply).
4. **Self-review + adversarial verification.** I brutally reviewed the v1 spec (found 5 correctness /
   3 efficiency / 5 minor issues), then ran a dynamic workflow (`wcgqf246p`: 24 agents — adversarial
   verifiers with double-voting on the high-stakes claims, cluster drafters, synthesis, 3-lens critic)
   to verify each finding against IB/RoCE semantics + the codebase. **0 findings refuted.** I then
   authored a coherent v2 rewrite from the verified facts (not the raw synthesis, which the critics
   flagged for offset/consistency reconciliation).

### Key corrections baked into v2 (things v1 got wrong)
- **Coherence claim scoped (C1):** x86 PCIe DMA is coherent, so RDMA validates the *cross-host*
  discipline (single-writer / sentinel-last / torn-read / no-atomics) but **not** the clflush/
  invalidate half (that's Track 02 TLA⁺ + Track 04 injection). MRs must be WB-mapped. Ordering intent
  survives as RC same-QP ordering.
- **Producer sequence + offsets (C2):** verified via `offsetof` — `log_idx@72`, `publish_commit@96`,
  broker body `[0,80)`. Producer = WRITE Blog → WRITE header `[0,80)` → **separate** WRITE
  `publish_commit@96` last, same RC QP.
- **No `IBV_SEND_FENCE` (E3, 2/2 verifiers):** RC same-QP ordering already orders WRITE-after-WRITE.
- **Seam redesigned + consolidated (C3/C4/C5/E1, Q2):** zero-copy `ReadView` for CXL (no fast-path
  regression), an `OrderingContext` for body+sentinel ordering, an `AccessStatus` completion surface
  (`kPeerDown` = failure signal), cold/virtual + hot/static-dispatch split. **Shared with Track 04**
  (injection decorator); Track 01 exposes it at freeze.
- **Poll scaling (E2):** a single gather READ over the rings is impossible (RDMA READ SGE scatters
  *locally*); fixed with a **packed contiguous sentinel array** (= `RDMA_REGION_SENTINEL_ARRAY`).
  W5-A pushes (`WRITE_WITH_IMM`), W5-B pulls (passive memserver) — the pull amplifies the funnel.
- **W5-B holds leg-1 properly (M3 → Decision 1):** all failure-critical **metadata (GOI,
  committed_seq, CV, PBR, sentinel array) lives on the memserver in BOTH variants**, so **payload Blog
  placement is the sole full-bandwidth variable** (A→B delta = payload = leg-2 cost; metadata funnel
  cancels). W5-A broker-host kill = "order survives (GOI/PBR on c3), payload lost" → maps to the CXL
  ACK1→ACK2 / `ERR_DATA_LOSS` clause. Framed as {W5-A, W5-B} vs CXL — two design points, not a
  single-knob ablation; `-DBLOG_PLACEMENT` is a code-sharing device, not the scientific variable.
- **CV direction fixed (M1):** sequencer/tail-replica WRITEs, owning broker READs.
- **Lease timing RDMA-aware (M2):** margins inflated for renewal latency/jitter + skew; not verbatim.
- **RoCEv2 reality:** lossy Ethernet → PFC/DCQCN required for a fair funnel measurement.

## Resolved with the human (this session)
- **Q1 memserver = c3 (`mos181`, 1.26 TB).** Fabric up + verified (RoCEv2, MTU 9000): broker↔c1
  54 Gb/s, broker↔c3 88 Gb/s. Triangle broker+c1+c3 unblocked. See `sessions/rdma_fabric_setup.md`.
- **Q2 RegionAccessor = shared with Track 04**, exposed by Track 01 at freeze. Do **not** build a
  05-only seam. (Baked into spec §3.1.)
- **Q3 BrokerInfo = additive, proposed** in `W5_BROKERINFO_RDMA_PROPOSAL.md` for Track 01 to apply.
- **Scaffolding greenlit:** rdma_transport accessor + verbs bring-up, memory server, token server.
  "Complete" = spec + scaffolding (not evidence). Memserver must be genuinely fast (multi-QP,
  kernel-bypass) — it will be red-teamed.

## What I staged
**Nothing committed** (shared working tree is mid-reorg on `session/03-baseline-ports` == `chore/repo-reorg`
at `7ec0e70`; I did not touch git). New untracked files, collision-free:
- `docs/design/W5_RDMA_VARIANTS_SPEC.md`
- `docs/design/W5_BROKERINFO_RDMA_PROPOSAL.md`
- `docs/experiments/rdma_variants.md`
- `sessions/handoff-05.md`

Intended (once reorg lands + branch exists):
```
git checkout -b session/05-rdma-variant   # off the integration branch
git add docs/design/W5_RDMA_VARIANTS_SPEC.md docs/design/W5_BROKERINFO_RDMA_PROPOSAL.md \
        docs/experiments/rdma_variants.md sessions/handoff-05.md
# commit --no-verify (docs); message:
# docs(w5): RDMA two-variant spec v2 + BrokerInfo proposal + experiment protocol (W1 closure)
# Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

## What I verified
- BatchHeader offsets empirically (`offsetof`): `batch_complete@16, pbr_absolute_index@48,
  total_size@56, start_logical_offset@64, log_idx@72, total_order@80, batch_off_to_export@88,
  publish_commit@96`, `sizeof==128`.
- Every review finding adversarially verified (workflow `wcgqf246p`; sources cited include the IB/RoCE
  literature and `performance_utils.h:437`, `topic.cc` flush/invalidate sites, `cxl_datastructure.h`).
- **Not built on `broker`** — no code yet (correct for this phase). Next: confirm `libibverbs`/
  `librdmacm` on the fabric hosts (perftest is present).

## For Track 01
- Define/expose the **shared `RegionAccessor`** (spec §3.1) — cold/virtual + hot/static split,
  zero-copy `ReadView`, `OrderingContext`, `AccessStatus`. Coordinate ONE interface with Track 04.
- Apply the additive `BrokerInfo` RDMA fields (`W5_BROKERINFO_RDMA_PROPOSAL.md`).
- Freeze lease/session **logic** (W5 recomputes RDMA timing); confirm the routed-access list (§7.2),
  incl. CV direction (M1).

## For Track 04
- The injection seam is now the **same** `RegionAccessor` (Q2). Plan for `InjectingAccessor` as a
  decorator over `CxlAccessor` (delayed-flush / stale-read / torn-window). Coordinate the interface
  with Track 01 so 04 + 05 don't fork it.

## For Tracks 06 / 07 (coordination — leg/substrate tables)
- The substrate/leg tables must use **identical** wording: **metadata (GOI/committed_seq/CV/PBR/
  sentinels) on the memory server in BOTH variants; legs defined over the committed-payload path;
  payload Blog placement is the sole variable.** Present E4e as **{W5-A, W5-B} vs CXL — two RDMA
  design points**, not a single-knob A/B ablation. (Spec §7a.)

## CC ground truth (verified 2026-07-05 on `broker`)
- Inbox `mlx5_core` (FW 16.28.2006). **Per-priority PFC OFF** (all 8); link-level **802.3x global pause
  ON** (RX/TX). DCQCN `cc_params` present at defaults (CNP DSCP 48 / prio 6). Switch-side ECN/PFC
  **unconfirmed** → human/infra step. Funnel protocol anchors to the **raw multi-QP device ceiling**
  (CC-agnostic); CC held identical across variants. (Spec §3.2, §4.1; E-doc funnel section.)

## Open questions for the human
1. **c2/c4 onboarding** (need sudo for RoCEv2 config) — timeline for scale-out (E2 all-remote)?
2. **Switch PFC/ECN config** — is the RoCE switch doing ECN marking / PFC on a dedicated priority?
   Likely a human/infra step (like c2/c4 sudo). Funnel is anchored to the raw device ceiling regardless.
3. **CXL reference for E4e:** confirm existing Embarcadero-on-CXL build (process-level), host-level CXL
   = W7 upside.
4. **PBR placement:** I place PBR (metadata) on the memserver in both variants so the metadata funnel
   cancels; resolved to memserver-both unless you object.

**Resolved this session:** Decision 1 (metadata-on-memserver-both; payload Blog sole variable),
Decision 2 (CC-agnostic funnel anchored to device ceiling), Q1 (memserver = c3), Q2 (shared seam with
Track 04), Q3 (BrokerInfo proposal filed).

## Scaffolding status (code — greenlit, non-`topic.cc`)

**DONE — foundation built + validated cross-host (broker↔c1, RoCEv2, under flock):**
- `src/rdma_transport/rdma_common.{h,cc}` — RoCEv2 RC verbs foundation: device open + **GID
  auto-scan** (sysfs, RoCEv2 IPv4), PD/CQ/MR, manual RC QP bring-up (INIT→RTR→RTS with GRH), one-sided
  `PostRead/PostWrite/PostFetchAdd`, `PollCq`, and a tiny TCP OOB exchange (bounded `accept`).
- `src/rdma_transport/rdma_bench_main.cc` — cross-host micro (server/client; `--op=write|read|
  fetchadd`); doubles as the W5-B funnel probe and reviewer-D token repro.
- `src/rdma_transport/CMakeLists.txt` — **standalone** build (no main-tree dep); builds with only
  `libibverbs-dev`. Manual RC + OOB chosen over `librdmacm` because `librdmacm-dev` is absent on broker
  (documented optional upgrade).
- **Verified 2026-07-06 (broker server ↔ c1 client, `gid=-1` auto-scan):** WRITE **53.95 Gb/s**
  (== fabric doc's `ib_write_bw` 54 Gb/s → transport hits the device ceiling), READ 56.19 Gb/s,
  FETCH_ADD 1.55 Mops/s (1 QP; atomic outstanding capped at 16 — multi-QP/thread needed for ~30 M).
- **Bugs found+fixed during bring-up:** OOB read/write ordering deadlock; `accept()`-forever hang that
  wedged the testbed lock (now a 30 s `select` timeout — post-mortem: a client that dies before the OOB
  connect must not strand the server holding `flock`); reliance on hardcoded GID indices (must scan).

**TODO (next):**
- Extend the passive target into the real **W5-B memory server** on c3 (multi-QP/CQ, hugepage/NUMA-
  local, registers all metadata + Blog MRs); multi-QP funnel sweep anchored to `ib_write_bw -q N`.
- Multi-QP/multi-thread token server to reproduce ~30 M tok/s (then co-opt).
- `RdmaAccessor` skeleton against Track 01's `RegionAccessor` once frozen; packed-sentinel-array READ.
- c3 build: needs `libibverbs-dev` (absent) + no `cmake` — build via `g++` one-liner like c1.

## ⚠ Fabric caveat for the human (action may be needed)
The RoCE fabric config is **runtime/non-persistent** (fabric doc warns this). **c1 had lost its RoCE
IP + RoCEv2 GID** when I tested (likely an overnight reboot) — only `broker` retained config. I
re-applied c1's documented recipe (`ip addr add 10.10.10.11/24` + link bounce; c1 has sudo), and its
RoCEv2 IPv4 GID came back **at index 3, not the doc's 5** (fresh re-apply reindexed it — reinforces
"scan, don't hardcode"). **Recommend the netplan persistence step** (fabric doc "Make it durable") on
broker/c1/c3 so the fabric survives reboots; otherwise every reboot needs re-applying. c3 and c2/c4
likely need the same (c3 also needs `libibverbs-dev` for building).

## Then
- Cross-host bring-up on broker+c1+c3 under `flock`; E4e / E1-leg3 / E6 (≥3 trials).
