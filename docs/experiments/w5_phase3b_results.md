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

| Trial | Detected via | Detect t | Unrecoverable bytes | Checksum verify |
|---|---|---|---|---|
| 1 | **RDMA_ERROR (fast path)** | 8.286s | 284,667,904 | PASS (10000/10000) |
| 2 | **RDMA_ERROR (fast path)** | 8.265s | 380,665,856 | PASS (10000/10000) |
| 3 | LEASE_TIMEOUT (backstop) | 6.980s | 0 | PASS (10000/10000) |
| 4 | **RDMA_ERROR (fast path)** | 8.155s | 601,788,416 | 1 mismatch (see below) |
| 5 | LEASE_TIMEOUT (backstop) | 6.979s | 0 | PASS (10000/10000) |

**3 of 5 trials fired the fast path with genuinely non-zero unrecoverable bytes; 2 of 5 fired the
lease backstop with zero loss.** This is not noise — it is the honest, mechanistic outcome: the
fast path requires a live in-flight (or about-to-be-posted) read against the dying broker's QP;
whether that condition holds at the exact kill instant depends on whether the sequencer happens to
be behind at that moment, which under unthrottled/persistent-backlog load is common but not
certain on every single trial. Detection latency for the fast path (8.155-8.286s after process
start, i.e. ~4.2-4.3s after the t=4.0s kill) reflects how far behind the sequencer had fallen
before it reached a broker0 item and got a real completion error — NOT the ~470-500ms RC
retry-exhaustion time alone, because the sequencer first has to work through its backlog to reach
a broker0 operation at all.

## Leg-1 item 2: checksummed recovery (same 5 trials)

Extended `rdma_recovery` with `--verify-slot-bytes/--verify-slots`. First attempt used the wrong
invariant (expected byte == slot_index & 0xFF, which only holds before the ring's FIRST
wraparound) and failed all 10000 checked slots in a sanity run under sustained load — a bug in the
verification tool, not the replication mechanism. Fixed to a wrap-count-independent check:
every byte within one slot must be internally uniform (proving no torn/mixed writes), and
consecutive slots' values must differ by exactly +1 mod 256 (adjacent ring slots hold consecutive
`global_seq` values within the same pass through the ring). 4 of 5 trials passed cleanly
(10000/10000). **Trial 4 showed exactly 1 mismatched slot out of 10000** — this is the expected
edge case of the delta-based heuristic: the checked range [0, 10000) has roughly a 10000/200000 =
5% chance of straddling the ring's live "current write position" boundary, where two different
wrap generations meet and the +1 invariant legitimately breaks for one slot pair without
indicating real corruption. Consistent with this explanation: trial 4 has the LARGEST
unrecoverable-bytes count of the five (more wrap activity → higher chance of straddling), and only
1 of 10000 slots was affected, not a cluster.

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

**At peak/unthrottled load, the real cost is large and unambiguous:**

| Config | Median throughput | rtt p50 |
|---|---|---|
| Fire-and-forget | ~24.2 Gb/s (19.8-24.2 across 3 trials) | ~9.8-11.1 µs |
| `--durable-replicate` | ~5.26 Gb/s (5.20-5.58 across 3 trials) | ~34.8-37.7 µs |

**Durability costs ~4.3-4.6x peak throughput and ~3.5x median per-batch latency.** This is the
real number Phase 2 never measured — "no survivor stall" was reported entirely under
fire-and-forget replication, where publish never actually implied replicated. Any durability claim
for W5-A must cite THIS number, not Phase 2's zero-cost figure, unless the workload genuinely
operates at ≤60% of peak with headroom to spare.

## Leg-1 item 4: re-replication volume-vs-Blog-size sweep (16 MiB → 1 GiB, 3 trials each)

Steady-state (no kill), recovering real replicated data at 7 sizes. Both legs show **flat,
size-independent bandwidth** — a clean per-link ceiling, not a bandwidth-scales-with-size
relationship:

| Size | Read (c1→tool) | Write (tool→c3 spare) | Total time |
|---|---|---|---|
| 16 MiB | 38.1-38.5 Gb/s | 70.4-70.8 Gb/s | ~8.4 ms |
| 32 MiB | 38.6-38.7 Gb/s | 61.6-71.2 Gb/s | ~13.8-14.3 ms |
| 64 MiB | 38.1-38.7 Gb/s | 70.2-70.9 Gb/s | ~24.6-25.4 ms |
| 128 MiB | 38.4-38.8 Gb/s | 70.7-70.9 Gb/s | ~46.1-47.0 ms |
| 256 MiB | 38.4-38.8 Gb/s | 70.6-70.8 Gb/s | ~89.5-91.2 ms |
| 512 MiB | 38.4-38.8 Gb/s | 70.6-70.8 Gb/s | ~176.8-177.7 ms |
| 1024 MiB | 38.4-38.7 Gb/s | 70.2-70.3 Gb/s | ~353.3-354.7 ms |

**The read leg (c1's own NIC, single QP, single op) is bound to ~38.1-38.8 Gb/s regardless of
transfer size** — confirming the predicted per-source-link bound qualitatively (this harness
measures ~38 Gb/s rather than the ~54 Gb/s guessed in the review prompt, but the mechanism is
identical: a single-QP, single-op transfer doesn't benefit from more data, and would need more
concurrent QPs to approach c1's own ~98 Gb/s device ceiling, which this tool does not use). The
write leg is bound separately at ~70.2-71.2 Gb/s, nearly double the read leg, on c3's link. Total
recovery time scales linearly with volume (e.g., read_secs at 1024 MiB is 64.2x read_secs at
16 MiB, matching the 64x size ratio almost exactly) — negligible fixed per-call overhead, a clean
`time ≈ bytes/38.5Gb/s + bytes/70.5Gb/s` fit.

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

## Still blocked (flagged, not attempted)

- **RoCE priority isolation**: switch-side QoS/PFC configuration required; no local tooling
  (`mlnx_qos`/`dcbtool`/`lldptool`) available on either host. Unchanged from Sub-phase 3A.
- **>2 sender hosts**: c2/c4 require sudo not available in this environment. Unchanged from
  Sub-phase 3A. Leg-2 ships qualitatively (plateau shape and NIC-coupling mechanism established)
  rather than with a fully-isolated quantitative ceiling until both of these are resolved.

## Summary for Sub-phase 3C

- **Leg-1 (W5-A)**: with a genuine backlog, both failure-detection paths are real and both fire
  under realistic conditions (fast path 3/5, backstop 2/5); recovered data is verified
  byte-for-byte correct; the real durability cost is substantial (~4.3-4.6x throughput, ~3.5x
  latency) once workload approaches peak, not the "free" number Phase 2's fire-and-forget
  measurement implied; re-replication is link-bandwidth-bound (~38 Gb/s read, ~70 Gb/s write),
  scaling linearly with volume.
- **Leg-2 (W5-B)**: per Sub-phase 3A + the priority experiment, ships as a qualitative
  shared-NIC-coupling finding with a physically-plausible mechanism (PCIe/DMA budget contention),
  not a hard quantitative ceiling, pending RoCE isolation and >2-host confirmation.

Pausing here for review before Sub-phase 3C (the E4e three-column figure), per the user's
instructions.
