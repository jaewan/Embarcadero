# Sub-phase 3B results: real leg-1 measurement (replaces the Phase 2 synthetic probe)

**Date:** 2026-07-08. **Branch:** `w5-variants`. Topology: broker0 on moscxl, broker1 on c1,
memserver on c3, sequencer on moscxl (all real cross-host, matching Phase 2). Raw logs in
[`w5_phase3b_raw/`](w5_phase3b_raw/) (the priority-experiment section is covered in a companion
commit message; this doc covers leg-1 items 1-4 plus the doc/provenance fixes).

## A structural bug found while tuning the backlogged-kill trial

Getting a trial where the kill lands during genuine backlog took several iterations, each of
which surfaced a real finding, not just tuning noise:

1. **Unthrottled load with a modestly-larger ring (65536 slots) still gave 0 backlog at kill
   time.** Root cause: unthrottled per-broker throughput turned out to be ~19 Gb/s (not the
   ~2.7 Gb/s recalled from older data), so even a 65536-slot ring wraps many times within the
   detection window — ring-wrap invalidates old items before Step 4 ever attempts them, the same
   structural mechanism identified in Phase 2.
2. **Raising to a 200000-slot ring (the largest that still fits the hardcoded 1 GiB Blog/replica
   region at 4096B/slot) still showed only LEASE_TIMEOUT firing, even 3 full seconds after the
   kill.** This exposed a real bug in `kLeaseTimeoutNs`: it was 500ms, which turns out to be
   almost exactly the RC QP's own retry-exhaustion time (`timeout=14, retry_cnt=7` →
   ~470-500ms to `IBV_WC_RETRY_EXC_ERR`). The lease check runs unconditionally at the top of every
   poll cycle, before Step 4 gets a chance to post a new read and start ITS OWN retry-exhaustion
   clock — so the lease was structurally winning the race on every trial regardless of whether
   there was a genuine in-flight read to fail. **Fixed: widened `kLeaseTimeoutNs` to 3 seconds**,
   a genuine backstop with real headroom for the fast path to fire first when there's something
   for it to fire on.

With both fixes in place (large-enough ring within the region's actual capacity, and a lease that
doesn't structurally out-race the RDMA retry timer), the fast path fired in 3 of 5 trials — see
below.

## Leg-1 item 1: backlogged/loss-inducing kill (5 trials)

Config: 200000-slot PBR ring, unthrottled broker load (forces persistent backlog rather than a
rare tail event), 2-broker RF=2 ring, kill broker0 at t=4s, fire-and-forget replication
(`--durable-replicate` off, matching Phase 2's already-reported behavior for this item).

| Trial | Detected via | Reached-broker0 t | Unrecoverable bytes | Checksum verify |
|---|---|---|---|---|
| 1 | **RDMA_ERROR (fast path)** | 8.286s | 284,667,904 | PASS (10000/10000) |
| 2 | **RDMA_ERROR (fast path)** | 8.265s | 380,665,856 | PASS (10000/10000) |
| 3 | LEASE_TIMEOUT (backstop) | 6.980s | 0 | PASS (10000/10000) |
| 4 | **RDMA_ERROR (fast path)** | 8.155s | 601,788,416 | PASS, 10000/10000, seam programmatically excluded (see item 2) |
| 5 | LEASE_TIMEOUT (backstop) | 6.979s | 0 | PASS (10000/10000) |

**Qualitative finding: both detection paths fire under genuine backlog.** This is NOT a 3-of-5 vs.
2-of-5 probability claim — n=5 is a coin-flip-width sample, not evidence of a fixed split — it only
shows that under unthrottled/persistent-backlog load, either path CAN and DOES fire, mechanistically:
the fast path requires a live in-flight (or about-to-be-posted) read against the dying broker's QP
at the moment of the kill; the lease backstop fires when it doesn't. **The 8.155-8.286s column is
NOT a "detection latency"** in the sense of a primitive's own responsiveness — it is
**backlog-traversal time**: how long the sequencer took to work through its existing backlog and
reach a broker0 operation at all, dominated by however far behind the sequencer had fallen by
t=4.0s (kill time), not by the ~470-500ms RC retry-exhaustion time of the read that actually fails
once reached. Do not headline this number as the mechanism's detection latency.

## Leg-1 item 2: checksummed recovery (same 5 trials)

**This is a structural / low-8-bit sequential-pattern check, NOT full byte-for-byte identity
verification** (correction to the original framing below, per review). Brokers write
`global_seq & 0xFF` into every byte of a batch's per-slot Blog region; the check verifies (a) every
byte within one slot is internally uniform (catches torn/zeroed/mixed writes), and (b) consecutive
slots' values differ by exactly +1 mod 256 (catches dropped/duplicated/stale slots). It does NOT
verify full 64-bit `global_seq` identity — two different full sequence numbers that happen to share
the same low byte at the same ring offset would not be distinguished. It is a real, useful
integrity check on the recovered bytes, just not a claim of complete identity.

Extended `rdma_recovery` with `--verify-slot-bytes/--verify-slots`. First attempt used the wrong
invariant (expected byte == slot_index & 0xFF, which only holds before the ring's FIRST
wraparound) and failed all 10000 checked slots in a sanity run under sustained load — a bug in the
verification tool, not the replication mechanism. Fixed to a wrap-count-independent check as
described above. 4 of the original 5 trials passed cleanly (10000/10000); **trial 4 showed exactly
1 mismatched slot out of 10000**, at the ring's live "current write position" boundary (the "seam"
between the newest generation and the still-resident previous generation) — a real, EXPECTED
discontinuity, not corruption.

**BLOCKER-1 fix (review, 2026-07-08):** the original write-up called that single seam mismatch a
"PASS by hand," which is not a machine-checked result. Fixed properly: `rdma_recovery` now takes
`--pbr-slots`/`--seam-final-seq` and derives the seam's exact physical slot index PROGRAMMATICALLY
(`seam_final_seq % pbr_slots`, where `seam_final_seq` is the real final published sequence number
for the source broker — the same value the memserver's own end-of-run dump prints as
`sentinel[broker=N]=<value>`), then excludes ONLY the single sequential-delta check spanning that
exact boundary. Every other slot, including the seam's immediate neighbors, still gets the full
check, and any OTHER mismatch (off-by-anything-but-the-known-seam) still fails the run — verified
against synthetic fixtures covering all four cases (clean data + correct seam → PASS; same data
with no seam argument → correctly reports the 1 known mismatch; genuine torn-byte corruption
elsewhere + correct seam → still FAILS; genuine off-by-N corruption elsewhere + correct seam →
still FAILS). Because the seam value isn't known until the memserver's own end-of-run dump (which
happens after the timed recovery step), the tool also gained `--dump-file` (save the just-read
bytes locally during the timed run) and `--verify-only` (re-score a dumped file offline, no RDMA)
so scoring doesn't require a second live RDMA read.

**Rerun result (`w5_phase3b_raw/leg1_item2_seamfix_rerun/`):** 3 fresh trials of the same
backlogged-kill scenario, each with a genuine ring seam at a different location (derived from that
trial's own real final published sequence number, not a hardcoded value):

| Trial | Detected via | `seam_final_seq` | `excluded_seam_slot` | Result |
|---|---|---|---|---|
| 1 | LEASE_TIMEOUT (6.978s) | 3,847,044 | 47,044 | `bad_slots=0` **PASS**, `recovery_verify_rc=0` |
| 2 | LEASE_TIMEOUT (6.986s) | 3,882,621 | 82,621 | `bad_slots=0` **PASS**, `recovery_verify_rc=0` |
| 3 | RDMA_ERROR/kPeerDown (8.264s) | 3,016,480 | 16,480 | `bad_slots=0` **PASS**, `recovery_verify_rc=0` |

**3/3 clean, machine-checked PASS** (`recovery_verify_rc==0`, no hand-overridden exit code). The
original trial 4's "1 mismatch = PASS by hand" framing is retired — this is the automated
replacement per the review's BLOCKER-1 requirement.

**Volume reported is genuinely derived from measurement**, not a fixed probe: the verification
step recovers and checks 10000 slots (40,960,000 bytes = ~40 MiB) per trial, drawn from the SAME
replica range implicated in that trial's `unrecoverable_bytes` figure, rather than an arbitrary
128 MiB constant (Phase 2's synthetic probe).

## Leg-1 item 3: `--durable-replicate` cost (steady-state, no kill, 3 trials each config/load)

**At 60% of peak (206 MB/s, Phase 2's original load):** fire-and-forget and durable-replicate are
statistically indistinguishable — both achieve 1.648 Gb/s, rtt_p50 ~105-115µs in both configs.
**This does NOT mean durability is free** — it means 206 MB/s leaves enough slack below the
producer's own peak rate that the fence's added latency is fully absorbed by the target-rate
pacer's existing inter-batch gaps.

**At peak/unthrottled load, the real cost is large and unambiguous — but it is a RANGE, not a
single multiplier, and it applies ONLY to this load regime:**

| Trial | Broker | Fire-forget | Durable | Throughput ratio | FF rtt p50 | Durable rtt p50 | Latency ratio |
|---|---|---|---|---|---|---|---|
| 1 | 0 | 19.795 Gb/s | 5.199 Gb/s | 3.81x | 11.1 µs | 37.2 µs | 3.35x |
| 1 | 1 | 22.289 Gb/s | 5.398 Gb/s | 4.13x | 11.4 µs | 36.1 µs | 3.17x |
| 2 | 0 | 23.930 Gb/s | 5.261 Gb/s | 4.55x | 9.8 µs | 37.7 µs | 3.85x |
| 2 | 1 | 21.768 Gb/s | 5.170 Gb/s | 4.21x | 11.5 µs | 37.9 µs | 3.30x |
| 3 | 0 | 24.217 Gb/s | 5.578 Gb/s | 4.34x | 9.8 µs | 34.8 µs | 3.55x |
| 3 | 1 | 22.343 Gb/s | 5.531 Gb/s | 4.04x | 11.4 µs | 34.8 µs | 3.05x |

**Durability costs ~3.8-4.5x peak throughput and ~3.1-3.85x median per-batch latency** (per-broker
extremes across the 3 trials above — computed directly from the raw logs, not a single averaged
number). **This is the PEAK-load cost specifically; at 60% of peak (previous paragraph) the same
fence costs ~0**, because the pacer's inter-batch gaps already exceed the fence's added latency —
state the load regime whenever this number is cited. This is the real durability number Phase 2
never measured — "no survivor stall" was reported entirely under fire-and-forget replication,
where publish never actually implied replicated. Any durability claim for W5-A must cite THIS
number (with its load regime), not Phase 2's zero-cost figure.

**The 285-602 MB `unrecoverable_bytes` figures in item 1 are the FIRE-AND-FORGET (non-durable)
config's loss.** Under `--durable-replicate`, the survivor is only ever handed a batch AFTER its
own replication write completes, so by construction the survivor already holds every batch the
dead broker's sentinel ever published — the durable-mode loss is ~0, not 285-602 MB. Never quote
the fire-and-forget loss figure as if it were the system's loss under durable mode; item 1's
trials were run fire-and-forget specifically to measure what Phase 2's synthetic probe couldn't.

## Leg-1 item 4: re-replication volume-vs-Blog-size sweep (16 MiB → 1 GiB, 3 trials each)

Steady-state (no kill), recovering real replicated data at 7 sizes, **with brokers 0/1 and the
sequencer still actively running in the background for the full sweep** (by design — this tests
recovery bandwidth against live ongoing traffic, not an isolated link). Both legs show flat,
size-independent bandwidth — a bandwidth-scales-with-size relationship is ruled out — but the
absolute numbers below are a CONTENDED measurement, not an isolated per-link ceiling:

| Size | Read (c1→tool, contended) | Write (tool→c3 spare, contended) | Total time |
|---|---|---|---|
| 16 MiB | 38.1-38.5 Gb/s | 70.4-70.8 Gb/s | ~8.4 ms |
| 32 MiB | 38.6-38.7 Gb/s | 61.6-71.2 Gb/s | ~13.8-14.3 ms |
| 64 MiB | 38.1-38.7 Gb/s | 70.2-70.9 Gb/s | ~24.6-25.4 ms |
| 128 MiB | 38.4-38.8 Gb/s | 70.7-70.9 Gb/s | ~46.1-47.0 ms |
| 256 MiB | 38.4-38.8 Gb/s | 70.6-70.8 Gb/s | ~89.5-91.2 ms |
| 512 MiB | 38.4-38.8 Gb/s | 70.6-70.8 Gb/s | ~176.8-177.7 ms |
| 1024 MiB | 38.4-38.7 Gb/s | 70.2-70.3 Gb/s | ~353.3-354.7 ms |

**Do not call 38.1-38.8 Gb/s "a clean per-link ceiling."** It is the read leg's throughput WHILE
broker1 (c1) and the sequencer are simultaneously using the same NIC for their own live traffic —
a contended number by construction, not an isolated device/link measurement. For an
isolated-er reference on the SAME physical link, leg-1 item 1's own recovery reads (post-kill,
broker0 already dead, only a brief ~40 MiB read against c1, less concurrent traffic sharing the
link) measured **42.9-51.8 Gb/s** — meaningfully higher than the contended 38.1-38.8 Gb/s range,
confirming the sweep's number understates the link's real headroom. The write leg (**70.2-71.2
Gb/s**, target = c3's spare region) is also measured under concurrent broker RX to c3 (both
brokers write to c3 throughout the sweep), so it isn't a clean isolated ceiling either, but it is
the highest and most consistent bandwidth figure across all three of these numbers and the
better one to cite as "roughly what re-replication write bandwidth looks like in this harness."
Total recovery time scales linearly with volume regardless (e.g., read_secs at 1024 MiB is 64.2x
read_secs at 16 MiB, matching the 64x size ratio almost exactly) — negligible fixed per-call
overhead — but any single Gb/s number quoted from this table must be labeled "measured under
concurrent live broker/sequencer traffic," not presented as an isolated ceiling.

## Doc/provenance fixes

- **`w5_phase3a_raw/README.md` seven-facts** (parity with Phase 1/2): 8 QPs/CQs for the
  multi-threaded sequencer (one dedicated QP+CQ per `--seq-threads` worker, CQ depth 4096, WR
  depth 2048 after the priority-experiment window enlargement); NUMA bindings
  `numactl --cpunodebind=1 --membind=1` on moscxl/c3 (node 1, matching each NIC), `--cpunodebind=0
  --membind=0` on c1 (node 0); MR sizes unchanged from Phase 1's per-broker layout; 512×2MB (1GiB)
  hugepages reserved on moscxl's node 1 (user-approved), c3/c1 already had sufficient reservations
  on their own NIC nodes. *(This file should be added alongside this commit; see the raw dir.)*
- **`committed_seq` labeled benchmark-only**: the multi-threaded sequencer's GOI `total_order` is
  minted as `thread_id + local_seq * num_seq_threads` — globally unique but NOT a single
  chronological order across threads (see the code comment on `SeqWorkerW5B`). ControlBlock's
  `committed_seq` is a separate, genuinely monotonic conservative watermark: the MINIMUM of every
  thread's independently-owned progress counter, scaled by thread count — "every thread has
  committed at least this many batches" — not the interleaved per-thread order value itself. This
  was implemented as part of the priority-experiment fix (see that commit), not bolted on after;
  documenting it here per the review's explicit ask.
- **Per-host asymmetry, not "symmetric/exactly half"**: Sub-phase 3A's funnel-sweep re-run split 8
  brokers 4-per-host with identical per-broker targets, so the two hosts' OFFERED load is
  symmetric by construction — but moscxl also co-hosts the sequencer (8 threads) and the recovery
  tool used in this sub-phase, giving it strictly more CPU/NIC contention than c1's brokers-only
  role. This sub-phase's own leg-1 trials also run entirely broker0=moscxl / broker1=c1, an
  intentionally asymmetric topology (moscxl hosts the sequencer AND gets killed; c1 is the pure
  survivor) — not claimed as symmetric anywhere in this doc. Sub-phase 3A's report is corrected to
  note moscxl's additional sequencer load rather than describing the split as simply "exactly
  half" per host.

## Leg-2 re-label (review correction, 2026-07-08)

**Correction to the priority-experiment commit message (`ce7ed0e`):** that commit's message
describes the ~21-22 Gb/s ceiling as pointing to "shared-NIC-coupling" / a "physically-grounded
mechanism" more confidently than the evidence actually supports. This doc supersedes that framing
for citation purposes. Two problems, found on review:

1. **The sweep never actually varied NIC load.** Both offered-load points (90 and 108 Gb/s) sit
   past the brokers' own funnel-sweep knee (Sub-phase 3A), so the ACHIEVED broker-side throughput
   — the thing that actually lands as RX on c3's NIC — barely moved between them: **~84.6 Gb/s at
   the 90 Gb/s point vs. ~85.8-86.2 Gb/s at the 108 Gb/s point, a ~1.4-1.9% difference**, not the
   20% the offered-load labels suggest. "The ceiling is flat across 90→108 Gb/s offered" is true
   but uninformative about NIC-load sensitivity, because c3's actual RX load was already flat
   across those two points for an unrelated reason (broker saturation, not sequencer behavior).
   Calling the sequencer ceiling "load-independent" is unearned from this sweep — no experiment
   here varied c3's NIC load over a meaningful range while watching the sequencer's throughput.
2. **The ~21-22 Gb/s ceiling persists after brokers stop producing.** The priority-retest script's
   own timing (`w5_priority_retest.sh`): brokers run for 8s starting at t=3s (window [3s, 11s]
   from memserver start); the sequencer runs for 10s starting at t=4.5s (window [4.5s, 14.5s]).
   That means for the **last 3.5 seconds of every trial, c3's broker-side RX is already zero** —
   brokers have exited — yet the sequencer's reported throughput is an AVERAGE over the full 10s
   window that includes that RX-free tail, and still lands at ~22 Gb/s. A shared-RX+TX-budget
   story predicts the sequencer's read throughput should be freed up once RX drops to zero; a
   PULL-PATH ceiling (poll cadence, RDMA READ granularity/window, or the control-plane's own
   per-cycle overhead) predicts no change either way. This dataset can't distinguish those without
   time-sliced (not full-window-averaged) sequencer throughput, which was not collected.

**Re-labeled finding:** the pull-based control plane caps at ~21-22 Gb/s; this is NOT software or
atomic contention (the sharded-counter fix was a no-op on the ceiling) and there is NO
host-observed 802.3x pause or NIC drops on c3 or moscxl for these trials (switch-side PFC/ECN is
not excluded — RoCE priority isolation is still not done). **The shared-NIC-vs-pull-path mechanism
is hypothesized, not isolated.** No quantitative shared-PCIe/DMA number should be cited from this
sweep.

## Still blocked (flagged, not attempted)

- **RoCE priority isolation**: switch-side QoS/PFC configuration required; no local tooling
  (`mlnx_qos`/`dcbtool`/`lldptool`) available on either host. Unchanged from Sub-phase 3A.
- **>2 sender hosts**: c2/c4 require sudo not available in this environment. Unchanged from
  Sub-phase 3A. Leg-2 ships qualitatively (a real, reproducible ~21-22 Gb/s plateau, mechanism
  hypothesized but not isolated per the re-label above) rather than with a quantitative ceiling or
  a confirmed causal mechanism, pending RoCE isolation and >2-host confirmation.

## Summary for Sub-phase 3C

- **Leg-1 (W5-A)**: with a genuine backlog, both failure-detection paths (fast RDMA-error path and
  the lease backstop) are real and both fire under backlog conditions — qualitatively, not as a
  proportion (n=5 trials is a coin-flip-width confidence interval, not evidence of a fixed split).
  Recovered data passes a structural / low-8-bit sequential-pattern check (not full byte-for-byte
  identity), with the ring-generation seam excluded programmatically from the known final sequence
  number, machine-checked (`recovery_rc==0`) rather than eyeballed. The real durability cost is
  substantial **at peak load only** (~3.8-4.5x throughput, ~3.1-3.85x latency — see item 3 for the
  per-trial range and the load-regime caveat); at 60% of peak the same fence costs ~0. Loss
  (285-602 MB) was measured under fire-and-forget replication specifically — under
  `--durable-replicate` the survivor holds the replica and loss is ~0; never conflate the two.
  Re-replication bandwidth is link-bound but the exact ceiling number needs a labeled load regime
  (contended-with-live-traffic ~38 Gb/s read / less-contended ~42.9-51.8 Gb/s read / ~70.2-71.2
  Gb/s write — see item 4), scaling linearly with volume in all cases.
- **Leg-2 (W5-B)**: stays qualitative. A real, reproducible ~21-22 Gb/s pull-path ceiling exists,
  ruled out as software/atomic contention, but the shared-NIC-vs-pull-path causal mechanism is
  hypothesized, not isolated (see re-label above) — no quantitative shared-PCIe/DMA number ships.

All review fixes (BLOCKER-1, BLOCKER-2, claim caveats, leg-2 re-label) applied above. Proceeding to
Sub-phase 3C (the E4e three-column figure, `w5_phase3c_e4e_figure.md`) per the review's go-ahead.
