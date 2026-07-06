# W5 — Embarcadero-on-RDMA: Two-Variant Redesign Spec

**Status:** DRAFT v2.1 (W1 on-paper closure, plan item 6). Spec first, code after.
**Owner:** Track 05 (`session/05-rdma-variant`). **Fabric:** `sessions/rdma_fabric_setup.md`.
**Source of truth:** `Paper/improvement_plan.md` (v4) — W5 (lines 426–444), E4e (§IV.2 E4e), E1
(§IV.2 E1), E6 (§IV.2 E6), the Thesis + v4 header (lines 20–76). Do not edit the plan; propose
changes to the human.
**Entry criterion for code:** transport scaffolding + hardware bring-up may start now; the concrete
session/lease *logic* is mirrored once **Track 01's design is frozen**. Depends on Track 01's
**interfaces**, not internals.
**Changelog:** v2 revised after an adversarial verification pass (workflow `wcgqf246p`, 24 agents, 0
findings refuted) + fabric bring-up. **v2.1** applies the human's senior-eng calls: (Decision 1) all
failure-critical **metadata lives on the memserver in BOTH variants**, so **payload Blog placement is
the sole full-bandwidth variable**; (Decision 2) funnel protocol is **CC-agnostic, anchored to the raw
multi-QP device ceiling**, network config held identical across variants.

---

## 0. Why this document exists (the thesis role)

The RDMA variants are **primary in-silicon evidence** in v4. With no accessible multi-host CXL, they
are the only place the coherence-free discipline runs against *real* cross-host hazards, and the only
place the paper's central conjunction is **measured**.

**The thesis, in one sentence (plan lines 26–29):**

> Cheap ordering *repair* needs two properties *simultaneously* — **(leg 1) failure independence**
> (log state outlives any host) and **(leg 2) no shared network endpoint** (every host reaches state
> at memory speed with nothing to funnel through). Over RDMA you get **either but not both**. CXL
> delivers **both** because it is **passive memory**. The **conjunction** is the contribution — not
> failure independence alone (the entire RDMA-disaggregated-memory literature: FaRM, Clover, Clio,
> LegoOS, AIFM).

**Legs are defined over the committed-payload path** (full-bandwidth traffic that must survive host
death), not the ~5 MB/s metadata poll — this is Track 06's M1 framing and it is load-bearing here.

W5-A and W5-B each hold exactly one leg. **The sole variable between them is where the payload Blog
lives.** All failure-critical metadata (GOI, `committed_seq`/ControlBlock, CompletionVector, PBR,
sentinel array) lives on the **passive memory server in both variants** — it is tiny, it must live on
*some* host over RDMA regardless, and parking it on the failure-independent memserver in both variants
(a) keeps A/B differing in exactly one thing and (b) is the honest definition of the legs.

| Variant | Payload Blog | Metadata (GOI/CB/CV/PBR/sentinels) | Leg 1 (payload survives host death) | Leg 2 (payload path has no shared NIC) | What it measures |
|---|---|---|---|---|---|
| **W5-A** | **broker host DRAM** | memserver (c3) | ✗ broker-host death loses its Blog | ✓ each broker's NIC serves its own payload | Leg-1 cost: "order survives, data lost" + re-replication |
| **W5-B** | **memserver (c3)** | memserver (c3) | ✓ survives any host death | ✗ memserver NIC funnels all payload | Leg-2 cost: NIC ceiling, CQ contention, latency inflation |
| **CXL** (reference) | CXL shared memory | CXL shared memory | ✓ passive memory | ✓ memory-speed loads, no NIC | **Neither** cost — the money figure (E4e) |

**The A→B delta is purely payload traffic = exactly the leg-2 cost.** The metadata funnel is equal in
both variants (both pull metadata from the memserver), negligible, and cancels.

### 0.1 What RDMA-on-x86 validates in silicon — scoped precisely (C1)

The testbed is x86-64, where **PCIe DMA is architecturally cache-coherent** (root complex snoops CPU
caches; DDIO is a placement optimization *on top of* coherent DMA, not its source). So an RDMA READ of
a location the remote CPU just wrote with ordinary **write-back (WB)** stores returns the new value
**with no writer-side `clflush` and no reader-side invalidate**:

- **RDMA-on-x86 exercises, in silicon:** (a) single-writer ownership per region; (b) monotonic,
  sentinel-**last** readiness gating; (c) torn-multi-cache-line-read avoidance (never trust body
  fields until the sentinel matches); (d) absence of cheap cross-host atomics (RDMA atomics are
  responder-serialized and *not* atomic vs the host CPU).
- **RDMA-on-x86 does NOT exercise** the host-local cache-management half (`clflushopt`-after-write,
  `clflush`-invalidate-before-read) that CXL 2.0 forces (`topic.cc` is saturated with these;
  `performance_utils.h:437` notes injection is the only way to make a stale read on a coherent host).
  That half is validated **only** by Track 02 TLA⁺ + Track 04 injection.
- **The store-ordering intent survives, transformed:** the CXL `sfence` (body-before-sentinel) becomes
  **RC same-QP message ordering** (§3.3); x86-TSO already orders the local WB body stores before the
  sentinel store.

**Caveat committed to:** all MRs are **WB-mapped ordinary DRAM**; no write-combining/non-temporal
stores. Claim the single-writer / sentinel-last / torn-read / no-atomics core — not the flush half.

---

## 1. Design invariants to preserve

From `src/cxl_manager/cxl_datastructure.h`, `src/embarlet/topic.{cc,h}`,
`src/common/performance_utils.h`:

1. **Single-writer ownership.** No cross-host RMW on the hot path. (RDMA: one initiator per region;
   readers use RDMA READ.)
2. **Monotonic, sentinel-gated updates.** `BatchHeaderPublishCommitted` = `publish_commit !=
   UINT64_MAX && publish_commit == pbr_absolute_index`, plus `batch_complete == 1`
   (`cxl_datastructure.h:432-435`). (RDMA: body WRITE first, sentinel WRITE last.)
3. **Ordering discipline** (was "host-local flush + fence"). On RDMA-on-x86 cache-management calls are
   correctness no-ops (§0.1); only ordering is required, from **RC same-QP message ordering** — *not*
   `IBV_SEND_FENCE` (E3).
4. **Poll-based state transitions.** Sequencer polls PBR; never on the write path. (RDMA: READ a
   packed sentinel array on the memserver — §3.3.)
5. **64 B / 128 B alignment** preserved in MRs so the torn-window analysis carries over.

The port is a **transport swap**, not a protocol redesign — this is what lets W5 isolate substrate
from architecture in E1.

---

## 2. Shared architecture: one transport, payload-placement the only variable

```
        ┌───────────────────────────────────────────────┐
        │  Embarcadero control plane (UNCHANGED logic)   │
        └───────────────────────┬───────────────────────┘
                                 │  RegionAccessor seam (§3.1) — SHARED with Track 04
        ┌────────────────────────┼───────────────────────────────┐
   ┌────┴─────────┐   ┌──────────┴──────────┐   ┌─────────────────┴────────┐
   │ CxlAccessor  │   │ InjectingAccessor   │   │ RdmaAccessor             │
   │ (prod,       │   │ (Track 04 decorator:│   │ (Track 05: one-sided     │
   │  zero-copy)  │   │  delay/stale/torn)  │   │  RoCEv2 verbs)           │
   └──────────────┘   └─────────────────────┘   └───────────┬──────────────┘
                                                             │ RC QPs (RoCEv2)
   ALWAYS on the passive memory server c3 (BOTH variants):   │
     GOI · committed_seq/ControlBlock · CompletionVector ·   │
     PBR rings · packed sentinel array   (metadata, ~5 MB/s) │
                                          ┌──────────────────┴──────────────┐
                                          │   -DBLOG_PLACEMENT (payload)     │
                                 ┌────────┴─────────┐          ┌─────────────┴────────────┐
                                 │ =dram  (W5-A)    │          │ =memserver  (W5-B)        │
                                 │ Blog in each     │          │ Blog on c3 → payload      │
                                 │ broker's DRAM →  │          │ funnels through c3's NIC   │
                                 │ per-broker NIC   │          │ (leg-2 cost)              │
                                 │ (leg-2 held);    │          │ survives any host death   │
                                 │ host death loses │          │ (leg-1 held)              │
                                 │ Blog (leg-1 lost)│          │                           │
                                 └──────────────────┘          └───────────────────────────┘
```

**Consolidations / framing:**
- **Seam shared with Track 04 (Q2).** `RegionAccessor` is defined/exposed by **Track 01 at freeze**;
  three implementations — `CxlAccessor` (prod), `InjectingAccessor` (Track 04 decorator over
  `CxlAccessor`: delayed-flush / stale-read / torn-window), `RdmaAccessor` (Track 05). One choke point
  for "where faults are injected" and "where the transport is swapped." Do **not** build a 05-only seam.
- **Payload placement is the sole variable (Decision 1).** Metadata is on the memserver in both
  variants; `-DBLOG_PLACEMENT` moves only the payload Blog. This isolates one independent variable.
- **Red-team guardrail (V.4):** present E4e as **{W5-A, W5-B} vs CXL — two RDMA design points, each
  engineered to hold one leg, neither able to hold both** — *not* a single-knob A-vs-B ablation. The
  `-DBLOG_PLACEMENT` switch is a **code-sharing device, not the claimed scientific variable.**
  Pre-empt "just keep metadata distributed and move only payload": **no** — it is the *payload* that
  must survive for leg 1, and payload is the funnel; moving the 5 MB/s metadata rescues nothing.

### 2.1 What maps to what

| CXL construct | RDMA realization | W5-A placement | W5-B placement |
|---|---|---|---|
| `ControlBlock` (epoch, lease, sequencer_id, committed_seq) | small MR, sequencer single-writer | **memserver** | **memserver** |
| `CompletionVector` | **sequencer/tail-replica WRITEs; owning broker READs** (M1 fixed) | **memserver** | **memserver** |
| GOI (global order index) | large MR, sequencer single-writer | **memserver** | **memserver** |
| PBR ring (per-broker `BatchHeader`) | per-broker ring MR; broker WRITEs body+sentinel; sequencer polls | **memserver** | **memserver** |
| Packed sentinel array (E2) | contiguous per-broker `publish_commit`; broker WRITEs its slot; sequencer READs whole array in one op | **memserver** | **memserver** |
| **Blog** (payload, 64 B records) | per-broker payload MR | **broker DRAM** | **memserver** |
| Per-client HWM / session table | unchanged; sequencer DRAM, not shared-memory | sequencer host | sequencer host |
| CXL `flush/fence` | RC same-QP ordering (no `clflush`, no `SEND_FENCE`) | transport | transport |

Only the **Blog** row differs. Everything else is identical across variants → the metadata funnel
cancels; the payload funnel is the isolated A→B delta.

---

## 3. The transport seam (`src/rdma_transport/` + the shared `RegionAccessor`)

### 3.1 The accessor interface (owned by Track 01; implemented by 04 and 05)

**Resolves C3/C4/C5/E1:** split into a **cold path** (virtual — map/connect/lease/injection hooks)
and a **hot path** that is **statically dispatched** (build-time typedef / CRTP) so the ~1 M-poll/s
loop keeps inlining. The hot path must (a) give CXL **zero-copy in-place** access, (b) carry an
**ordering context** so body+sentinel land on one QP in order, (c) return **completion status** so a
failed op to a dead host is the failure-detection signal.

```cpp
// region_accessor.h — DEFINED BY TRACK 01 at freeze; implemented by CxlAccessor / InjectingAccessor / RdmaAccessor.
namespace embarcadero {

enum class RegionId : uint32_t {   // fixes M5; mirrors RdmaRegionKind in the proto proposal
  kControlBlock = 1, kCompletionVector = 2, kPbrRing = 3, kBlog = 4, kSentinelArray = 5, kGoi = 6,
};

struct RegionHandle {
  void*    local_view = nullptr;  // CXL: mapped pointer (in-place). RDMA: base of the local registered mirror.
  uint64_t remote_addr = 0;       // RDMA only
  uint32_t rkey = 0;              // RDMA only
  size_t   size = 0;
  int      owner_host = -1;       // single-writer host id (-1 = memserver/shared)
};

enum class AccessStatus { kOk, kRetry, kPeerDown, kTransportError };  // C5: completion surface

class OrderingContext;  // opaque; one in-order issue stream (a QP for RDMA; no-op for CXL) per (region, writer-thread)

// COLD PATH — virtual, called rarely.
class RegionAccessor {
 public:
  virtual ~RegionAccessor() = default;
  virtual RegionHandle MapRegion(RegionId, int owner_host) = 0;
  virtual OrderingContext* WriterStream(const RegionHandle&) = 0;
  virtual AccessStatus Connect(int peer_host) = 0;                  // RDMA: RC bring-up; CXL: no-op
  // Track 04 hooks live here (delayed-flush / stale / torn); no-ops in Cxl/Rdma accessors.
};

// HOT PATH — provided by each concrete accessor as NON-virtual (typedef/CRTP), contract:
//   template <class T> const T* ReadView(const RegionHandle&, size_t off, AccessStatus*);  // CXL: no copy; RDMA: into local mirror
//   AccessStatus Publish(OrderingContext*, const RegionHandle&,                            // body then sentinel, same QP, RC-ordered, NO SEND_FENCE
//                        size_t body_off, const void* body, size_t body_len,
//                        size_t sentinel_off, uint64_t sentinel);
//   AccessStatus ReadSentinelArray(const RegionHandle&, uint64_t* dst, size_t n);          // one contiguous READ of N sentinels
}  // namespace embarcadero
```

- **C3:** `ReadView` returns a pointer; **CXL never copies** (no fast-path regression, plan E8).
- **C4:** `Publish` takes an `OrderingContext*` — ordering is *in the interface*.
- **C5:** every hot op yields `AccessStatus`; `kPeerDown` drives failure detection (§3.4).
- **E1:** hot methods non-virtual; only the cold base is virtual.

> **Boundary:** Track 05 does not edit `topic.{cc,h}`. Track 01 exposes the interface + injection
> point at freeze; 04 and 05 supply implementations. Fallback if late: forked TU with the accessor
> macro'd (§7 / handoff).

### 3.2 Connection model (RoCEv2)

- **RoCEv2** RC QPs on ConnectX-5 `mlx5_0`, subnet `10.10.10.0/24`, MTU 9000 (RoCE path MTU 4096).
  One-sided verbs on the hot path. RC gives per-QP in-order delivery + reliable completion (needed for
  sentinel-last ordering and `kPeerDown`).
- **Topology (fabric doc):** broker=`moscxl`/`.10`, c1=`mos143`/`.11`, **c3=`mos181`/`.13` = memory
  server** (1.26 TB; ~88 Gb/s single-QP measured). c2/c4 join for scale later.
- **Setup:** `librdmacm` for address resolution + QP handshake over the RDMA IP; MR descriptors
  advertised via additive `BrokerInfo` fields (`docs/design/W5_BROKERINFO_RDMA_PROPOSAL.md`, Q3). GID
  indices differ per host — **scan, don't hardcode**.
- **Congestion control (Decision 2 — verified ground truth, 2026-07-05):** on `broker` the inbox
  `mlx5_core` driver (FW 16.28.2006) has **per-priority PFC OFF on all 8 priorities**; only link-level
  802.3x **global pause is on** (RX/TX); DCQCN endpoint `cc_params` are present at defaults (CNP on
  DSCP 48 / prio 6). Switch-side ECN marking / PFC is **unconfirmed** (switch-admin fact — flag as a
  human/infra step, like c2/c4 sudo). **Target regime:** DCQCN (ECN) active + PFC lossless backstop on
  a dedicated RoCE priority. If only global pause is available, isolate RoCE on its own priority/VLAN
  and disclose in the fairness appendix (global pause risks HOL blocking that would make the funnel
  look worse than inherent). **Network config held identical across W5-A, W5-B, and any RDMA baseline.**

### 3.3 Producer / consumer over RDMA — verified offsets (C2, E3, E2)

Verified `BatchHeader` layout (`offsetof`; `sizeof==128`): broker-owned body = **`[0, 80)`**
(`batch_complete@16, pbr_absolute_index@48, total_size@56, start_logical_offset@64, log_idx@72`);
`ordered@24, total_order@80, batch_off_to_export@88` are sequencer-written; `publish_commit@96` is the
sentinel, written **last**.

**Publish (single-writer broker), on one RC QP:**
1. Write the Blog payload record(s), 64 B aligned. **W5-A:** local CPU store to the broker's own DRAM
   (no network). **W5-B:** RDMA WRITE to the memserver Blog MR (this is the payload funnel).
2. RDMA WRITE the `BatchHeader` body `[0, 80)` to the memserver PBR (both variants) — includes
   `batch_complete=1`, `pbr_absolute_index`, `log_idx`. One contiguous WRITE; leaves `[80,96)` for the
   sequencer.
3. RDMA WRITE **only** `publish_commit@96` (8 B) to the PBR, and this broker's slot in the packed
   **sentinel array** — a **separate** WRITE posted after step 2 on the **same RC QP**. Same-QP RC
   ordering guarantees the sentinel lands after the body. **No `IBV_SEND_FENCE`** (E3, 2/2 verifiers).

*Why separate (C2):* the trailing word of a single WRITE may land out of order internally; correctness
rests on a distinct, later, same-QP sentinel WRITE.

**Poll / read (sequencer) — identical mechanism in both variants:**
1. RDMA READ the whole **packed sentinel array** on the memserver (N brokers × 8 B) in one contiguous
   op. (A single gather READ over the *rings* is impossible — an RDMA READ's SGE scatters into *local*
   buffers only; its remote side is one contiguous range — hence the dedicated array, E2.)
2. For each advanced broker: RDMA READ its `BatchHeader` body `[0,80)` from the memserver PBR, verify
   `publish_commit == pbr_absolute_index && batch_complete == 1`, then read the Blog at `log_idx` —
   **broker-direct (W5-A)** or **from the memserver (W5-B)**. *This Blog read is the only step whose
   endpoint differs between variants — the isolated leg-2 cost.*
3. **Torn-window safety (identical to CXL):** never trust body fields until the sentinel matches; the
   sentinel is written strictly last. A concurrent reader may see a mid-body state — it doesn't match
   yet and retries.

**Passive memserver ⇒ pull, in both variants (equal, cancels).** The memory server is kept passive
(its CPU is not in the data path) so it holds leg-1; consequently the sequencer must *pull* the
sentinel array rather than be pushed to. Because metadata is on the memserver in **both** variants,
this pull cost is identical across A and B and cancels out of the A→B delta — only the payload Blog
endpoint differs.

**No hot-path cross-host atomics** (responder-serialized, not atomic vs host CPU). Cold-path
membership/lease uses single-writer WRITEs.

### 3.4 Lease / heartbeat / epoch over RDMA (M2 — not verbatim)

Mirrors CXL `ControlBlock` (on the memserver, both variants), timing RDMA-aware:
- Active sequencer is the single writer; renews `sequencer_lease` by RDMA WRITE. Brokers/watchdog RDMA
  READ it and compare to local `now_ns()`.
- **RDMA safety condition (M2):** the expiry test compares a remote-written timestamp against a local
  clock over a renewal path with **µs-scale latency + jitter** plus clock skew. Inflate the margin to
  cover `renewal_latency_p99 + clock_skew_bound`; do not reuse CXL lease constants unchanged. Logic
  (renewal, expiry, `kEpochCheckInterval=100`, `RefreshBrokerEpochFromCXL`) is inherited from Track 01;
  only timing constants are recomputed.
- **Dual-signal failure detection (C5):** RDMA `kPeerDown` (RC RETRY_EXC → QP ERROR on a dead peer) is
  the fast path; the lease is the backstop. For a killed W5-A broker host, the sequencer's Blog READ to
  that host returns `kPeerDown` — that is the "data lost" signal (the GOI/PBR on the memserver still
  hold the order: **order survives, payload lost**, mapping onto the CXL ACK1→ACK2 / `ERR_DATA_LOSS`
  clause).

### 3.5 Visibility vs durability vs persistence (M4)

- **Visibility:** RDMA READ returns responder memory current at service time; on x86 reflects committed
  WB stores with no flush (§0.1).
- **Ordering:** RC same-QP message ordering (§3.3).
- **Durability/persistence:** a *distinct* guarantee (data reached the memory domain, not just the NIC)
  via RDMA read-after-write or the IB FLUSH verb. The memserver runs **DRAM** → **no persistence
  claim**; failure independence = *survives host death while the memserver stays up*, not
  crash-persistent. (v1's "RDMA flush" language removed.)

---

## 4. The payload-placement switch (Decision 1)

`-DBLOG_PLACEMENT={dram,memserver}` moves **only the payload Blog**; all metadata is on the memserver
in both. Targets: `embarlet_rdma_dram` (**W5-A**), `embarlet_rdma_memserver` (**W5-B**). Control plane,
sequencer, hold buffer, lease logic, wire structs, metadata placement, network/CC config, and the
entire `src/rdma_transport/` path are identical. CI check: both targets build from the same TU set
differing only by the macro.

### 4.1 The memory server c3 (must be the best we can build — V.4 red-team)

- **Passive, one-sided, kernel-bypass.** Registers all metadata MRs (both variants) + the Blog
  (W5-B); its **CPU is not in the data path**. The funnel is purely its **NIC/PCIe bandwidth + CQ
  concurrency** — the inherent leg-2 limit.
- **Multi-QP / multi-CQ, hugepage-backed, NUMA-local to the NIC's PCIe root.** c3 ceiling ~88 Gb/s at
  1 QP; use multiple QPs to reach the device ceiling (measure the *device*, not a single queue).
- **CC per §3.2**, held identical across variants.
- **Anchor the funnel to the raw device ceiling (Decision 2):** report `ib_write_bw -q N` multi-QP
  aggregate at c3 as the reference line, then show funnel goodput **plateaus at ≈that ceiling and does
  not rise as senders/QPs are added**. This "plateau at the hardware ceiling" statement is
  **CC-agnostic** and is the actual thesis claim — it survives any reviewer's view on DCQCN vs PFC.
  Claim: *even a well-built passive memory server funnels all payload through one NIC; the funnel is
  topological (adding a NIC moves it, doesn't remove it).*

---

## 5. The reproduced naive RDMA sequencer (reviewer D, co-opted)

Reproduce reviewer D's ~30 M tokens/s naive RDMA sequencer (one-sided `FETCH_ADD` or WRITE-based token
ring), confirm the rate on ConnectX-5, then show token *rate* was never the binding constraint:
**(i)** token **RTT on the client critical path**; **(ii)** the **repair loop**. Embarcadero pays
neither on the ACK path. Microbenchmark + note in `docs/experiments/rdma_variants.md`.

---

## 6. Experiments enabled (detail in `docs/experiments/rdma_variants.md`)

- **E4e — conjunction contrast (money figure):** W5-A broker-*host* kill → "order survives (GOI/PBR on
  memserver), payload lost" = leg-1 cost, detected via `kPeerDown` + lease; vs W5-B memserver payload
  funnel = leg-2 cost, anchored to c3's raw multi-QP ceiling; vs CXL (neither; process-level failure
  independence + single-host substrate numbers, host-level CXL = W7 upside). Framed as two RDMA design
  points vs CXL (§2 guardrail), not a single-knob ablation.
- **E1 — waterfall leg 3:** the plan uses Embarcadero-on-RDMA (W5-A) vs Embarcadero-on-CXL to isolate
  substrate gain. **Note (Decision 1 corollary):** with all state on the memserver, **W5-B is the
  closer architectural analog to Embarcadero-on-CXL** (a memory pool reached over a transport; only the
  transport differs), so W5-B may be the cleaner substrate-gain reference than W5-A. Worth considering;
  does not block — pre-register the RDMA↔CXL delta either way.
- **E6 — R-inflation under load** for both variants; W5-B bites hardest (payload + poll on the one c3
  NIC).

All runs ≥3 trials (5 for tail), median ± range, under the testbed `flock` (§8). **Hold network/CC
config identical across variants** — cross-variant fairness comes from changing one thing at a time.

---

## 7. Interface needs to flag to Track 01

1. Define/expose the **shared `RegionAccessor`** (§3.1) — cold/virtual + hot/static split, zero-copy
   `ReadView`, `OrderingContext`, `AccessStatus`. **One interface with Track 04.** Then W5 needs zero
   edits to `topic.{cc,h}`.
2. Confirm the routed-access list: PBR poll (`BrokerScannerWorker5`), `RefreshBrokerEpochFromCXL`,
   `AdvanceCVForSequencer` (CV direction M1), Blog read/write, GOI read/write.
3. Freeze lease/session **logic** (W5 recomputes RDMA timing, §3.4); per-client HWM contract.
4. Apply the additive `BrokerInfo` RDMA fields (`W5_BROKERINFO_RDMA_PROPOSAL.md`).

## 7a. Coordination for Tracks 06 / 07

The substrate/leg tables must state the **exact** placement and use **identical leg-1/leg-2 wording**:
**metadata (GOI/committed_seq/CV/PBR/sentinels) on the memory server in BOTH variants; legs defined
over the committed-payload path; payload Blog placement is the sole variable.** E4e is **{W5-A, W5-B}
vs CXL — two design points**, not a single-knob A/B ablation.

---

## 8. Build & run (BACKGROUND protocol)

- **Build** in `~/Embarcadero-sessions/05-rdma-variant/` on `broker`, `-j 64`, cached gRPC. New deps:
  `libibverbs`, `librdmacm` (`perftest` present on fabric hosts). `find_library` guards in top-level
  `CMakeLists.txt`; new `src/rdma_transport/` + targets `embarlet_rdma_dram`,
  `embarlet_rdma_memserver`.
- **Cross-host runs exclusive.** Take the `flock` (`~/Embarcadero-sessions/testbed.lock`, `-w 2400`);
  note **broker + c1 + c3** (c3 = memserver) in `activity.log`. Compile outside the lock. Fabric
  bring-up + GID scan per `sessions/rdma_fabric_setup.md` (gotcha: `pkill -x ib_write_bw`, never `-f`).

---

## 9. Open questions for the human

1. **c2/c4 onboarding** (need sudo for RoCEv2 config) — timeline for scale-out (E2 all-remote)?
2. **Switch PFC/ECN config (Decision 2, infra):** confirm whether the RoCE switch does ECN marking /
   PFC on a dedicated priority. Host side today: PFC off per-priority, global pause on, DCQCN
   `cc_params` defaulted. Target DCQCN + PFC-backstop; funnel result anchored to the raw device ceiling
   regardless. **Likely a human/infra step** (like c2/c4 sudo).
3. **CXL reference for E4e:** confirm existing Embarcadero-on-CXL build (process-level); host-level CXL
   = W7 upside.
4. **PBR on the memserver in both variants:** I place PBR (metadata) on the memserver in both, so the
   metadata funnel cancels; confirm this is the intended reading of Decision 1 (vs co-locating PBR with
   the Blog, which would reintroduce a second per-variable difference). *Resolved by default to
   memserver-both unless you object.*

*Resolved this session:* Decision 1 (metadata on memserver both variants; payload Blog sole variable),
Q1 (memserver = c3), Q2 (shared `RegionAccessor` with Track 04), Q3 (BrokerInfo proposal filed).

---

## Appendix A — CXL constructs referenced

| Construct | File | Notes |
|---|---|---|
| `BatchHeader` (offsets via `offsetof`) | `cxl_datastructure.h:393` | `batch_complete@16, pbr_absolute_index@48, total_size@56, start_logical_offset@64, log_idx@72, total_order@80, batch_off_to_export@88, publish_commit@96`; body `[0,80)` broker-owned |
| `BatchHeaderPublishCommitted` | `cxl_datastructure.h:432` | `publish_commit != UINT64_MAX && == pbr_absolute_index` |
| `ControlBlock` | `cxl_datastructure.h` | 128 B, sequencer single-writer |
| `CompletionVector` | `cxl_datastructure.h` | sequencer/tail writes, broker reads (M1) |
| PBR poll | `topic.cc` (`BrokerScannerWorker5`) | → sentinel-array READ over RDMA |
| Epoch check | `topic.cc` (`RefreshBrokerEpochFromCXL`) | `kEpochCheckInterval=100` |
| Per-client HWM | `topic.h` (`per_client_ordered_`/`_written_`/`_durable_`) | DRAM, not shared mem |
| Coherence note | `performance_utils.h:437` | injection is the only stale-read source on a coherent host (grounds §0.1) |
