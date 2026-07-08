# Sub-phase 3C: E4e conjunction-contrast figure (money figure, panel #1)

**Claim under test** (`docs/experiments/rdma_variants.md` §E4e): CXL uniquely holds both
failure-independence (leg-1) and no-shared-network-endpoint (leg-2) properties simultaneously;
each RDMA design point (W5-A, W5-B) surrenders exactly one, and the cost of surrendering it is
measurable on owned hardware.

**Definition of "unrecoverable" (required by review):** a byte of a killed broker's DRAM Blog is
counted as unrecoverable if, at the moment of detection, the sequencer's own GOI/committed-seq
frontier had not yet advanced past that byte's batch — i.e. the batch was published (sentinel set)
but never read/ordered by the sequencer before the broker died, and no replica of it exists on any
surviving host. It is measured directly from the surviving replica's own state (the gap between
the dead broker's last-replicated sentinel and the sequencer's last-committed order for that
broker), not inferred from a fixed byte count.

**Source:** Sub-phase 3B (`w5_phase3b_results.md`), Sub-phase 3A (`w5_phase3a_results.md`), the
priority experiment (commit `ce7ed0e`, re-labeled below per review). Every number below is pinned
to its config and load regime — no bare scalars.

| | **W5-A** (`embarlet_rdma_dram`, broker-host kill) | **W5-B** (`embarlet_rdma_memserver`, load ramp) | **CXL** (Embarcadero-on-CXL, process kill) |
|---|---|---|---|
| **Event injected** | `kill -9` broker0 at t=4s, unthrottled 2-broker RF=2 ring (200000-slot PBR), mid-backlog | Offered load swept to ~90-108 Gb/s aggregate (past the funnel knee) | Broker **process** kill (not host-level) — architectural reference, not re-measured by this track |
| **Detection** | Both the fast RDMA-error path (`RDMA_ERROR`/`kPeerDown`) and the lease backstop (`LEASE_TIMEOUT`, 3s) fire under genuine backlog — **qualitative only**: n=5 trials is a coin-flip-width sample, not a probability estimate. The fast path's observed time-to-fire (8.155-8.286s from process start, ~4.2-4.3s post-kill) is **backlog-traversal time** (how long the sequencer took to reach a broker0 op in its existing backlog), not the RDMA primitive's own detection latency (~470-500ms RC retry-exhaustion once actually reached). | N/A (no kill in this column) | N/A (process-level; D1's session-fencing numbers apply, not measured here) |
| **Loss** | **Fire-and-forget config:** 284.7-601.8 MB unrecoverable (3 trials that hit the fast path; 2 trials hit the lease backstop with 0 loss — same 5-trial caveat as Detection). **`--durable-replicate` config:** ~0 — the survivor only ever receives a batch after its own replication write completes, so by construction it already holds everything the dead broker published. **Never conflate these two configs' loss numbers.** | N/A | ≈0 (Blog persists in CXL shared memory across a process kill — architectural property, not a fresh measurement this track ran) |
| **Durability-cost** | **At peak/unthrottled load: ~3.8-4.5x throughput reduction, ~3.1-3.85x per-batch latency increase** (per-broker extremes across 3 trials, `--durable-replicate` vs fire-and-forget). **At 60% of peak: ~0 cost** (pacer's inter-batch gaps absorb the fence). State the load regime whenever either number is cited — this is not one multiplier. | N/A (no durability fence in this column's leg-2 cost model) | Not applicable — CXL's durability is a shared-memory write, not a network fence |
| **Any leg-2 cell** | Per-broker NICs on the payload path — the metadata-only shared channel (sentinel array poll) is the sole shared-NIC surface for W5-A, not separately quantified this sub-phase | **Qualitative only, no quantitative shared-PCIe/DMA number.** A real, reproducible ~21-22 Gb/s pull-based-control-plane ceiling exists (ruled out: software/atomic contention — sharded-counter fix was a no-op). NOT confirmed: shared RX+TX PCIe/DMA contention on c3's NIC — the sweep never varied c3's actual achieved NIC load over a meaningful range (90 vs 108 Gb/s offered achieved only ~84.6 vs ~85.8-86.2 Gb/s at the memserver, a ~1.4-1.9% difference) and the ceiling persists through the last ~3.5s of every trial when brokers have already stopped producing (RX≈0) — both facts argue AGAINST attributing the ceiling to concurrent RX+TX contention alone. No host-observed 802.3x pause or NIC discards on c3/moscxl in these trials; switch-side PFC/ECN is not excluded (RoCE priority isolation still not done). The mechanism is **hypothesized, not isolated**. | No shared network endpoint by construction — no funnel to measure |
| **Re-replication bandwidth** (leg-1 repair cost) | Link-bound, scales linearly with volume (16 MiB-1 GiB swept) — but labeled by contention: **~38.1-38.8 Gb/s read** measured WITH brokers+sequencer concurrently sharing the same NIC (contended); **~42.9-51.8 Gb/s read** measured post-kill with less concurrent traffic on the same physical link (less contended); **~70.2-71.2 Gb/s write** to the memserver's spare region (also under concurrent broker RX to that host, not isolated either). No single number here is a clean isolated device ceiling — all three are labeled by their concurrent-load condition. | N/A | N/A |

## Still blocked (carried from Sub-phase 3A/3B, unchanged)

- **RoCE priority isolation**: switch-side QoS/PFC/DCQCN configuration; no local tooling
  (`mlnx_qos`/`dcbtool`/`lldptool`) on either host. Leg-2's shared-NIC-vs-pull-path question cannot
  be fully resolved without this.
- **>2 sender hosts**: c2/c4 are sudo-gated in this environment; the leg-2 funnel was only
  characterized with a 2-host split.

## Caveats that must travel with this figure

1. **Detection and Loss's 3-vs-2 trial split is illustrative, not a rate.** Report "both paths fire
   under backlog," never "60% fast-path probability."
2. **The fire-and-forget loss figures (285-602 MB) are NOT the system's loss under
   `--durable-replicate`.** The two configs are mutually exclusive per trial; durable-mode loss is
   ~0 by construction.
3. **The durability-cost multiplier is peak-load-specific.** At 60% of peak it is ~0. Any citation
   of "~4x" without a load regime is a category error against this data.
4. **No quantitative shared-PCIe/DMA number for leg-2.** The ~21-22 Gb/s ceiling is real and
   reproducible; its cause is hypothesized (shared-NIC contention, or a sequencer pull-path/poll-
   cadence limit) but not isolated. Do not cite a specific contended-bandwidth "tax" figure for
   leg-2 in the paper's headline text.
5. **Checksummed-recovery verification is structural / low-8-bit, not full byte-for-byte
   identity** — see `w5_phase3b_results.md` item 2. The ring-generation seam is now excluded
   programmatically (derived from the real final published sequence number, machine-checked via
   `recovery_rc==0`), not by a hand-overridden exit code.
6. **The CXL column is an architectural reference, not a fresh Track 05 measurement** — cited from
   the design's own passive-shared-memory property (Blog survives a process kill by construction)
   and explicitly labeled process-level, not host-level, per the pre-registration
   (`w5_e4e_preregistration.md`).

## BLOCKER-1 verification rerun

3-trial rerun of the backlogged-kill scenario with the fixed `rdma_recovery` (programmatic seam
exclusion via `--pbr-slots`/`--seam-final-seq`, `--dump-file`/`--verify-only` to defer scoring
until the memserver's end-of-run sentinel dump is available) — see
`w5_phase3b_raw/leg1_item2_seamfix_rerun/`. **3/3 trials: clean, machine-checked
`recovery_verify_rc=0` PASS**, each with a genuine ring seam at a different, trial-derived physical
slot (47044, 82621, 16480 — none hardcoded). No hand-overridden exit code. Full detail in
`w5_phase3b_results.md` item 2.
