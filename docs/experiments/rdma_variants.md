# RDMA Variants — Experiment Protocol (W5-A / W5-B)

**Owner:** Track 05 (`session/05-rdma-variant`). **Design:** `docs/design/W5_RDMA_VARIANTS_SPEC.md`.
**Source of truth:** `Paper/improvement_plan.md` (v4) — E4e, E1, E6, IV.0 methodology rules.

This doc is the *run book* for the three RDMA-variant experiments plus the reproduced naive RDMA
sequencer. The spec says *what the variants are*; this says *what we measure, how, and what counts as
a valid result*.

---

## Methodology rules (IV.0 — never broken)

1. Every number = **median ± range of ≥3 trials** (5 for tail/P99.9 claims). No single-trial cells.
2. No headline includes a **co-located publisher**. Module-saturation is a labeled ceiling
   microbenchmark only.
3. msgs/s **and** GB/s both reported; message-size sweep for headline experiments
   ({128 B, 512 B, 1 KB, 4 KB, 16 KB}).
4. Latency reported as **latency-vs-load curves** at fractions of each system's own peak — no
   past-saturation tails.
5. One script per figure, committed. Artifact-evaluation-ready from the start.
6. All cross-host runs under the testbed **`flock`** (see "Run harness" below).

---

## Hardware / topology

**Transport is RoCEv2** on ConnectX-5 `mlx5_0`, subnet `10.10.10.0/24`, MTU 9000 (RoCE path MTU
4096). Fabric details, IPs, and per-host GID indices: `sessions/rdma_fabric_setup.md`. Verified
single-QP: broker↔c1 = **54 Gb/s**, broker↔c3 = **88 Gb/s**.

| Role | Host | RDMA IP | Notes |
|---|---|---|---|
| sequencer / broker | broker = `moscxl` | 10.10.10.10 | GID idx 3 |
| broker | c1 = `mos143` | 10.10.10.11 | GID idx 5; kill target for W5-A leg-1 |
| **memory server (W5-B)** | **c3 = `mos181`** | 10.10.10.13 | GID idx 5; 1.26 TB; funnel ceiling ~88 Gb/s @1QP → toward line rate multi-QP |
| broker (scale) | c2 = `mos144`, c4 = `mos182` | .12 / .14 | TODO — need sudo (fabric doc) |

**All metadata (GOI, committed_seq/ControlBlock, CV, PBR, sentinel array) lives on the c3 memserver in
BOTH variants** (spec Decision 1) — payload Blog placement is the *sole* variable, so the metadata
funnel is equal in both and cancels; the A→B delta is pure payload = the leg-2 cost.

- **W5-A (`embarlet_rdma_dram`):** broker + c1 as broker hosts; **Blog (payload) in each broker's own
  DRAM**, metadata on c3. Payload is served by each broker's own NIC (leg-2 held for the payload path).
  Kill a broker **host** → its Blog dies, but the GOI/PBR on c3 still hold the order → **"order
  survives, data lost"** (leg-1 lost for payload).
- **W5-B (`embarlet_rdma_memserver`):** broker + c1 as broker hosts, **c3 as the passive memory server
  holding metadata + the Blog**. State survives any host death (leg-1 held); all payload writes +
  subscriber/replica reads funnel through **c3's one NIC** (leg-2 lost).
- **CXL reference:** existing Embarcadero-on-CXL build on moscxl (single-host CXL; process-level
  failure independence + single-host substrate numbers). Host-level CXL is W7 upside, labeled as such.

**Fair-funnel requirement (V.4 red-team).** Verified host CC state (2026-07-05, `broker`): inbox
`mlx5_core`, per-priority **PFC OFF** on all 8 priorities, link-level **802.3x global pause ON**
(RX/TX), DCQCN `cc_params` present at defaults (CNP DSCP 48 / prio 6). Switch-side ECN/PFC is
**unconfirmed** (switch-admin — human/infra step, flagged like c2/c4 sudo). Target regime: **DCQCN
(ECN) active + PFC lossless backstop on a dedicated RoCE priority**; if only global pause is available,
isolate RoCE on its own priority/VLAN (global pause risks HOL blocking that makes the funnel look worse
than inherent) and disclose in the fairness appendix. **The robust, CC-agnostic claim:** anchor the
funnel to the **raw multi-QP device ceiling** — report `ib_write_bw -q N` aggregate at c3 as the
reference line and show funnel goodput **plateaus at ≈that ceiling and does not rise as senders/QPs are
added**. That plateau statement is the actual thesis claim and survives any DCQCN-vs-PFC dispute.
**Hold network/CC config identical across W5-A, W5-B, and any RDMA baseline.** Report the CC config
with every W5-B number.

Record for every run: host↔host QP count, CQ depth, MR sizes, NIC port count in use, RoCEv2 CC
config, hugepage/NUMA placement, and the exact `flock` `activity.log` line.

---

## E4e — Conjunction contrast (the money figure, panel #1)

**Claim under test:** CXL uniquely holds **both** legs; each RDMA variant surrenders exactly one, and
the cost of surrendering it is measurable on owned hardware.

Three columns, same workload, same client load generator:

| Column | Build | Event injected | Primary metric (leg cost) |
|---|---|---|---|
| **W5-A** | `embarlet_rdma_dram` | **Broker-host kill** (`kill -9` the broker process **and** drop the host: `ip link set down` the NIC, or power/cgroup-freeze the host) | **Leg-1 cost:** bytes of Blog lost, re-replication volume + time to restore RF, session stall duration on affected clients |
| **W5-B** | `embarlet_rdma_memserver` | **Load ramp** to memserver-NIC saturation (no kill) | **Leg-2 cost:** achieved bandwidth vs memserver-NIC line rate, poll/payload CQ contention (poll latency inflation as payload load rises), end-to-end latency inflation curve |
| **CXL** | Embarcadero-on-CXL | **Broker *process* kill** (process-level) | **Neither:** Blog survives in CXL (no loss), no shared NIC endpoint (no funnel); report substrate latency/bw + process-level recovery |

**W5-A host-kill procedure (leg-1):**
1. Steady-state publish at ~60% of W5-A peak, RF≥2, M independent client sessions.
2. At t₀, kill the broker host (process kill + NIC down, to model true host loss — the DRAM Blog is
   gone, not just the process).
3. Measure: (a) Blog bytes unrecoverable from that host; (b) re-replication cost to restore RF from
   surviving replicas; (c) per-session stall distribution (all M sessions), MTTR incl. session-table
   recovery + PBR rescan. Compare stall to the CXL column (expect ≈δ for CXL vs a re-replication-bound
   stall for W5-A).
4. **Failure-detection signal (spec §3.4, C5):** record detection latency via both paths — the
   sequencer's RDMA READs/WRITEs to the dead host completing in error (RC RETRY_EXC → QP ERROR =
   `kPeerDown`, the fast path) and the lease timeout (backstop). Report which fired first.
5. ≥3 trials; report distribution, not a point.

**W5-B funnel procedure (leg-2):**
0. Enable + record RoCEv2 CC (PFC or DCQCN/ECN) so the knee is the **c3 NIC bandwidth ceiling**
   (~88 Gb/s @1QP → toward ~100 Gb/s line rate multi-QP), not drop-induced RC-retransmit collapse.
1. Sweep offered load from 10% → >100% of the c3-NIC ceiling. Multi-QP (`-q n`) to reach the device
   ceiling, not a single-queue artifact.
2. Measure achieved READ+WRITE bandwidth (payload writes + sequencer **pull** polls + subscriber reads
   mixed), poll latency vs payload load, e2e P50/P99 vs load. Show the knee at the NIC ceiling.
3. **Pull-path amplification (spec §3.3):** the memserver is passive (to hold leg-1), so the sequencer
   must *pull* the sentinel array — quantify the poll bandwidth's share of the funnel at saturation.
   The passivity that grants leg-1 is what forces the pull that worsens leg-2 — the thesis in one plot.
4. Add-a-NIC control: bond a second c3 NIC/port and show the funnel *moves* but does not disappear
   (topological argument, spec §4.1).
5. ≥3 trials.

**CXL column:** process-level broker kill (Blog persists in CXL shared memory → no data loss),
single-host substrate latency/bandwidth. Explicitly label "process-level, not host-level" per plan
line 530. The leg-1/leg-2 *contrast* is fully carried by the two RDMA columns.

**Figure:** grouped bars or a 3-panel — left: leg-1 cost (W5-A loss/repair) vs CXL (≈0); middle: leg-2
cost (W5-B funnel knee) vs CXL (no knee); right: the summary "CXL pays neither."

---

## E1 — Decomposition waterfall, leg 3

**Claim under test:** separate transport gain from architecture gain from substrate gain.

Four legs (plan §IV.2 E1):
1. baseline-on-TCP
2. baseline-on-CXL-transport (W4, porting-rule-constrained) — *Track 03/04*
3. **Embarcadero-on-RDMA (W5-A)** — *this track*
4. Embarcadero-on-CXL — *Track 01*

**Track 05's contribution:** leg 3. Run `embarlet_rdma_dram` on the *same architecture* as leg 4 so
that leg4−leg3 isolates **substrate gain (RDMA→CXL)** with architecture held fixed. Same workload,
same message-size sweep, latency-vs-load curves.

> **This experiment can shoot the author (plan line 508):** if the RDMA→CXL gap is small, the SLO
> headline is mostly architecture, not substrate — publish it anyway. Pre-register the expected
> RDMA↔CXL delta before running.

---

## E6 — Model validation (R-inflation under load, both variants)

**Claim under test:** the regime model's repair parameter R inflates with payload load, and the
inflation is substrate-dependent — worst for W5-B (poll+payload contend on one NIC).

- For **both** variants: sweep payload load; measure R (repair latency / hold-window fill time) vs
  load. Overlay predicted vs observed (tie to Track 02/regime model).
- **W5-B is the sharp case:** as payload load rises, sequencer poll traffic and payload writes contend
  on the memserver NIC → R inflates fastest. W5-A inflates less (per-broker NICs). CXL inflates least.
- tc-netem jitter injection swept against the hold window (shared with E6's CXL side): predicted vs
  observed repair success as jitter → T.

---

## Reproduced naive RDMA sequencer (reviewer D, co-opted)

**Goal:** reproduce reviewer D's ~30 M tokens/s naive RDMA sequencer, then show token *rate* was never
the binding constraint.

1. **Reproduce the rate.** Minimal RDMA token server (one-sided `FETCH_ADD` token, or a WRITE-based
   token ring). Confirm ~30 M tokens/s in isolation on ConnectX-5. Document config so we cannot be
   accused of strawmanning it.
2. **Show what it ignores:**
   - **Token RTT on the client critical path:** every ordered message pays a sequencer round-trip
     before proceeding. Measure the added client-path latency at various loads.
   - **The repair loop:** straggler wait / gap fill / retransmit — where FIFO actually costs.
     Embarcadero pays neither on the ACK path (per-session prefix from local state).
3. **Framing:** "30 M tok/s is real and irrelevant — the binding constraints are token RTT and
   repair, which the token rate does not touch."

---

## Run harness (exclusive testbed, BACKGROUND protocol)

Compile in your own tree **outside** the lock; take the lock only to execute. Note **broker + c1 + c3**
(c3 = memory server for W5-B) usage in `activity.log`.

```bash
ssh broker 'flock -w 2400 ~/Embarcadero-sessions/testbed.lock -c "
  cd ~/Embarcadero-sessions/05-rdma-variant &&
  echo \"[$(date -u +%FT%TZ)] 05-rdma-variant: START E4e W5-A host-kill (broker+c1+c3)\" >> ~/Embarcadero-sessions/activity.log &&
  ./scripts/<rdma-run-script>.sh --variant=dram --trial=1 ;
  echo \"[$(date -u +%FT%TZ)] 05-rdma-variant: END\" >> ~/Embarcadero-sessions/activity.log
"'
```

Each experiment gets one committed script (IV.0 rule 6). Scripts land under `scripts/` per repo
convention; figure-generation under `scripts/plot/` or `scripts/publication/`.

---

## Done criteria (per plan / track file)

- E4e (W5-A host-kill vs W5-B funnel vs CXL), E1 leg 3, E6 data collected, **≥3 trials each, under
  lock**.
- Reproduced RDMA token server documented.
- Every figure has a committed one-script generator.
- Results written up; memserver design noted for the V.4 red-team (spec §4.1).
