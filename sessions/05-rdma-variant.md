# Track 05 — Embarcadero-on-RDMA: TWO variants (W5-A + W5-B)

**Read `sessions/00-BACKGROUND.md` first.** Track id: `05-rdma-variant`. Branch:
`session/05-rdma-variant`. (One track, **two** RDMA variants — W5-A and W5-B.)

**Goal (v4 — this is now the thesis's in-silicon evidence, not just a waterfall leg).** Build **two**
RDMA variants that together *measure the conjunction* (leg 1 ∧ leg 2): each RDMA design can hold only
one leg, and the pair proves it on hardware we own. Both run **genuinely cross-host today** on
c2/c4/moscxl ConnectX-5 — and because one-sided RDMA to remote memory is **itself cross-host
non-coherent**, these variants also validate the coherence-free discipline (I.4 claim 2) **in
silicon**. This is E4e, "the money figure."

**Entry criterion:** transport scaffolding + hardware bring-up can start now; mirror the concrete
session/lease logic once **Track 01's design is frozen**.

**Plan sections:** `Paper/improvement_plan.md` → **W5 (both variants), E4e (conjunction contrast),
E1 leg 3, E6, I.1 (leg 1/leg 2), the Thesis + v4 header (lines 20–76).** Item (6) — the two-variant
W5 redesign spec — is your **W1 on-paper closure (~2-week deadline): write the spec first, code after.**

## The two variants (the whole point)
- **W5-A — Blogs in broker DRAM → loses leg 1 (failure independence).** Same architecture, control
  plane over one-sided verbs (PBR polling via RDMA READ, leases via RDMA WRITE). Kill the broker
  **host** → its DRAM Blog dies with it → **measure data loss / re-replication cost**. This is the
  leg-1 failure contrast (and waterfall leg 3 in E1).
- **W5-B — Blogs on a dedicated memory server → loses leg 2 (no-shared-endpoint).** State survives
  host death, but every payload write, sequencer poll, and replica/subscriber read funnels through
  the memory server's NIC → **measure the funnel**: memserver-NIC bandwidth ceiling, poll/payload CQ
  contention, latency inflation under load. This is the on-owned-hardware proof that "failure
  independence over RDMA costs the shared endpoint." **Build the best memory server you can**
  (batched, kernel-bypass) — it must show the funnel is *inherent*, not badly built (it will be
  red-teamed, V.4).

CXL holds **both** legs (neither cost) — that contrast is E4e.

## You OWN
- New `src/rdma_transport/` (create it): one-sided verbs control plane mirroring the CXL control
  plane (PBR polling, heartbeat/lease lines, session-HWM). Structure it so **W5-A and W5-B share the
  transport** and differ only in Blog placement.
- Build targets `embarlet_rdma_dram` (W5-A) and `embarlet_rdma_memserver` (W5-B) — or a
  `-DBLOG_PLACEMENT={dram,memserver}` switch.
- Reproduction of reviewer-D's naive ~30 M tokens/s RDMA sequencer, co-opted (token *rate* was never
  the binding constraint — the token RTT on the client path and the repair loop are).
- `docs/experiments/rdma_variants.md` (new) + the two-variant spec (W1 closure).

## Do NOT touch
- CXL protocol logic in `src/embarlet/topic.{cc,h}` (Track 01) — depend on its **interfaces**, not
  internals; coordinate a minimal transport seam with Track 01 if needed.
- Baseline ports (Track 03), fault-injection (Track 04), `Paper/Text/*.tex`.

## Purposes (map to experiments)
- **E4e conjunction contrast (money figure):** W5-A host-kill (leg-1 cost) vs W5-B memserver funnel
  (leg-2 cost) vs CXL (neither).
- **E1 leg 3:** W5-A vs Embarcadero-on-CXL isolates substrate gain.
- **E6:** R-inflation under payload load for **both** variants (T/R load-dependence — W5-B bites hardest).
- **Claim 2 in silicon:** the single-writer / monotonic / spatial-guard discipline runs under real
  cross-host non-coherence here — cite alongside Track 02 TLA⁺ and Track 04 injection.

## Build/test (BACKGROUND protocol)
- Build in `~/Embarcadero-sessions/05-rdma-variant/` (`-j 64`). Needs RDMA/verbs libs — confirm on
  `broker`; ConnectX-5 NICs are on c2/c4/moscxl.
- **Cross-host runs are exclusive** (NICs, ports, multiple hosts): **take the `flock` lock**, and note
  in `activity.log` that you're using c2/c4 (+ a memserver host for W5-B) as well as moscxl.

## Done criteria
- Two-variant spec written (W1 closure). Both variants build and run cross-host on c2/c4/moscxl.
- E4e (A host-kill vs B funnel), E1 leg-3, E6 data collected (≥3 trials, under lock); reproduced RDMA
  token server documented. Handoff note with staged pathspec + commit message; flag any interface
  needs to Track 01; note the memserver design for the V.4 red-team.
