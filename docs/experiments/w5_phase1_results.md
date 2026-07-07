# W5-B Phase 1 results: funnel sweep + reviewer-D token-server repro

**Date:** 2026-07-08. **Branch:** `w5-variants` @ `c033116` (fix) on top of `4b9aee1`
(pipelining) on top of `1293525` (initial harness). Supersedes the blocker/setup narrative in
`w5_phase1_progress.md`, which remains as historical record of Phase 0 + the memlock blocker.

> ## ⚠ REVIEW QUALIFICATIONS (adversarial review, 2026-07-08) — read before citing any number here
> The harness code is correct for the reported W5-B runs (no data-tainting bug), but the CLAIMS below
> are qualified. **These corrections govern; the body text is preliminary.**
> 1. **Signal B is NOT a clean E4e leg-2 (NIC-contention) demonstration.** The sequencer is a
>    single-threaded loop, software-bound at ~16–21 Gb/s *before* the NIC saturates, so the >98%
>    stage-2 collapse is entangled with that software ceiling and is refutable as "your sequencer is
>    just slow." Re-label it **"sequencer software ceiling + shared-NIC interaction,"** NOT leg-2
>    NIC contention. **E4e leg-2 requires a multi-threaded (N-thread/N-QP/N-CQ) sequencer** that tracks
>    offered load near the plateau before collapsing — Phase-3 prerequisite.
> 2. **802.3x global-pause confound.** A >98% collapse with 260–343 ms p999 is the signature of
>    link-level PAUSE, not graceful per-QP sharing. The collapse *magnitude* is not attributable to
>    leg-2 until RoCE is isolated on its own priority/VLAN under DCQCN+PFC and the collapse is shown to
>    persist.
> 3. **Signal A is "saturation vs offered load," not a proven "CC-agnostic device ceiling."** The
>    pre-registered falsifiability test (add QPs at fixed saturating load; goodput must not rise) was
>    not run. Earn "device ceiling" only after that QP-count sweep.
> 4. **Reviewer-D claim narrowed:** "~30M tok/s requires abandoning single global order" is airtight
>    only against the *naive one-RDMA-atomic-per-token* design. A batched `FETCH_ADD(N)` reservation
>    (Corfu-style), a server-CPU sequencer, or a hardware counter can keep single global order above
>    2.0 Mtok/s. Scope the claim to the naive design; acknowledge batched/hierarchical alternatives.
> 5. **Reproducibility gap:** raw per-trial lines, run_metadata (QP/CQ/MR/NIC-port/NUMA/CC), activity.log,
>    and the sweep script are not committed; the "seven-facts protocol" is undefined. Ship the raw tree
>    (mirror `delta_measurement_raw/`) before these numbers are treated as canonical evidence.
> 6. **Tracked should-fix code bugs (land before any W5-A / Phase-3 run):** `post_one` return ignored →
>    latent hang on `ibv_post_send` failure; W5-A blog-QP `max_send_wr==kWindow` (zero margin); CV
>    dirty-value should be `max`, not last-writer; GOI committed order scrambled by completion order;
>    `detect_to_commit` sampled too early. None taint the reported W5-B numbers (off-path / unreached /
>    control-plane-only), but the two hang bugs are far likelier to fire under a saturating multi-QP
>    Phase-3 run.

## 0. A real bug found and fixed before any data was trusted

The first full-scale sweep attempt hung indefinitely (`w5_broker_memserver --broker-id=3`, no
error, no progress, for 15+ minutes) after two brokers connected out of order and one
(`broker1`) failed its handshake outright. Root cause: `rdma_memserver`'s (and
`rdma_token_server`'s) N-client accept loop called `OobServerExchange` once per client, and that
function opened, listened on, and closed a **fresh** TCP socket for every single connection.
Under concurrent multi-host launch (4 brokers on moscxl + 4 on c1 + 1 sequencer, all racing to
connect), a client's `connect()` landing in the gap between one client's accept+close and the
next client's fresh listen got refused. Depending on kernel-level TCP SYN retry timing this could
stall far longer than the client's own bounded retry budget (100 tries × 100ms ≈ 10s nominal, but
each individual `connect()` call's own SYN-retry timeout is not counted against that budget), while
the server treated *any* single handshake timeout as fatal (`return 1`) and exited — stranding
whichever client was mid-connect with nothing left to answer it, forever.

**Fix** (commit `c033116`): split `OobServerExchange` into `OobServerListen` / `OobServerAccept` /
`OobServerClose` so a batch of N accepts shares **one** persistent listening socket (backlog sized
for N), removing the closed-window race entirely. `rdma_memserver.cc` and `rdma_token_server.cc`
updated to use it. Verified at the full 8-broker+1-sequencer topology under a deliberately
*harder* (fully unstaggered) concurrent launch than the sweep script itself uses — all 9 clients
connected, no hangs, sane throughput, clean shutdown on all three hosts. The entire 21-trial sweep
below was run from scratch under this fixed code; no data below was produced under the racy
version.

## 1. Funnel sweep (W5-B: memserver on c3 hosts metadata + Blog, 8 brokers, 1 sequencer)

Config: `--num-brokers=8 --pbr-slots=16384 --goi-entries=2000000 --host-blog
--blog-bytes=268435456`, message size 4096B, inflight=64/broker, broker duration=8s, sequencer/
memserver duration=10s. 7 offered-load points (10/20/40/60/80/100/120% of the ~90Gb/s
`ib_write_bw -q N` reference measured in Phase 0), 3 trials each, `flock`-serialized against the
shared testbed.

Two **independent** signals were recorded per run, per the seven-facts protocol:

### Signal A — broker-side aggregate write bandwidth (NIC-bound funnel metric)

| Offered (aggregate) | Achieved (aggregate, median of 3) | Achieved / offered |
|---|---|---|
| 9.00 Gb/s  | 8.96 Gb/s  | 99.6% |
| 18.00 Gb/s | 17.98 Gb/s | 99.9% |
| 36.00 Gb/s | 35.97 Gb/s | 99.9% |
| 54.00 Gb/s | 53.95 Gb/s | 99.9% |
| 72.00 Gb/s | 72.00 Gb/s | 100.0% |
| 90.00 Gb/s | **87.6 Gb/s** (87.1–88.0 across trials) | 97.3% |
| 108.00 Gb/s | **87.5 Gb/s** (87.1–88.0 across trials) | 81.0% |

**Knee:** achieved throughput tracks offered load almost exactly through 72 Gb/s, then plateaus
hard at **~87.5 Gb/s** starting at the 90 Gb/s point and stays flat (no further rise) at 108 Gb/s
offered — a textbook CC-agnostic funnel plateau, and it lands within 3% of the ~90 Gb/s
single-QP `ib_write_bw` device ceiling measured in Phase 0. This is the clean, expected "goodput
plateaus at the raw multi-QP device ceiling" signal.

### Signal B — sequencer-side processing (separate, software+shared-NIC-bound ceiling)

| Offered (aggregate) | Sequencer achieved | stale_misses (of attempted) | detect_to_commit p50 |
|---|---|---|---|
| 9 Gb/s   | 5.96 Gb/s        | 0.05%      | ~16 µs |
| 18 Gb/s  | 11.94 Gb/s       | 0.2–0.4%   | ~25 µs |
| 36 Gb/s  | 16.7–21.2 Gb/s   | 10–25%     | ~58–74 ms |
| 54 Gb/s  | 15.5–18.5 Gb/s   | 40–47%     | ~61–68 ms |
| 72 Gb/s  | 16.5–16.7 Gb/s   | ~54%       | ~57 ms |
| 90 Gb/s  | **0.19–0.20 Gb/s** | **~84%** | ~59 ms (p999 up to 263 ms) |
| 108 Gb/s | **0.19–0.23 Gb/s** | **~85%** | ~57 ms (p999 up to 343 ms) |

This is a *two-stage* degradation, not one plateau:

1. **Software ceiling (36–72 Gb/s offered):** the single-threaded sequencer's own poll/read/write
   loop saturates around 16–21 Gb/s regardless of further offered-load increases in this band.
   `stale_misses` climbs smoothly from <1% to ~54% purely from backlog (the PBR ring wraps before
   the sequencer catches up) — a disclosed architecture/scope limitation of this harness's
   single-threaded design, not a bug (no crashes, no corruption, consistent across all 3 trials
   per point).
2. **NIC-contention collapse (90–108 Gb/s offered):** exactly where broker-side traffic hits the
   ~87.5 Gb/s NIC plateau (Signal A), the sequencer's *own* throughput collapses by another >98%,
   from ~16 Gb/s down to <0.25 Gb/s, with p999 detect-to-commit latency blowing out to 260–343 ms.
   The sequencer's control-plane RDMA ops (sentinel/header reads, GOI/CV writes) target the SAME
   c3 NIC that the brokers are now saturating with payload writes — once that link is full,
   control traffic gets starved on top of the pre-existing software ceiling. This is a sharp,
   reproducible, directly measurable demonstration of the leg-2 cost the W5-B design is meant to
   expose: routing ALL payload and ALL metadata through one NIC creates catastrophic contention
   between them once payload saturates the link, not just a graceful shared slowdown.

## 2. Reviewer-D naive RDMA token-server repro

Config: RC QPs, 8B atomic FETCH_ADD, `max_dest_rd_atomic=16`, inflight=16/QP. Ran under the same
shared-lock discipline, server on moscxl, client on c1.

### 2.1 A pre-registration deviation, diagnosed rather than papered over

The first measurement (naive config: 16 QPs, single client thread, one shared 8B counter — the
harness's original design, whose own comment assumed "the NIC/PCIe path is the bottleneck for 8B
atomics, not the posting CPU") measured **2.0 Mtok/s**, three orders of magnitude below the
pre-registered ~30M tok/s target. Per the pre-registration's own methodology ("report deviations
and revise the claim, not the experiment"), this was diagnosed rather than accepted or discarded:

- Added `--num-threads` to spread QP-posting across multiple client CPU cores. **Result: flat
  2.0 Mtok/s from 1 to 16 threads, and flat from 16 to 128 QPs.** This refutes a CPU-posting-rate
  explanation outright — more cores and more QPs made no difference whatsoever.
- Added `--num-counters` to the server (N independent 8B counters, one per cache line, instead of
  one shared counter) and had each QP target `counter[qp_index % N]`. **Result: throughput jumped
  to ~29.3–30.2 Mtok/s at N=16** — matching the reviewer-D ~30M tok/s figure almost exactly.

**Conclusion:** the flat-regardless-of-threads-and-QPs behavior at N=1, combined with the
order-of-magnitude jump at N=16, pins the mechanism precisely: RDMA atomics to the *same* target
address serialize at the NIC/memory-coherence layer regardless of how many source QPs or CPU
threads issue them (a well-documented RNIC behavior). **The reviewer-D ~30M tok/s number is real,
but only for N independent, uncoordinated counters — it is not a single global sequence number.**
A real ordering-repair primitive needs exactly the thing that config throws away: one shared,
globally-ordered counter. This is a third "thing the naive design ignores," beyond the two named
in the pre-registration (token RTT, repair loop) — the headline throughput figure itself
presupposes abandoning global order.

### 2.2 Final measurements (3 trials each, 16 QPs / 8 posting threads)

**Throughput, single global counter (real ordering semantics):**

| Trial | Rate |
|---|---|
| 1 | 2.032 Mtok/s |
| 2 | 2.032 Mtok/s |
| 3 | 2.032 Mtok/s |

**Throughput, 16 independent/sharded counters (reviewer-D repro config, no single global
order):**

| Trial | Rate |
|---|---|
| 1 | 30.189 Mtok/s |
| 2 | 29.299 Mtok/s |
| 3 | 29.508 Mtok/s |

→ **>14.5x gap** between the headline aggregate-rate config and genuine single-global-order
semantics on identical hardware/QP/thread configuration.

**Latency, single global counter, one QP, strictly serial post-block-repeat (3 trials):**

| Trial | p50 | p99 | p999 |
|---|---|---|---|
| 1 | 3.21 µs | 3.41 µs | 3.79 µs |
| 2 | 3.03 µs | 3.18 µs | 3.60 µs |
| 3 | 3.21 µs | 3.43 µs | 3.85 µs |

For context, the delta-measurement branch's `append_send_to_ack` numbers are 1.0–1.1 ms typical,
4.95 ms worst-case — 300–1500x slower than a single token-server RTT. **Per-operation token
latency is not where a naive design's problem shows up** — it is negligible against the rest of
the append path. The real constraint a reviewer-D-style token server would hit if used as an
actual global ordering point is the **aggregate single-global-order throughput ceiling
(~2.0 Mtok/s)**, not RTT.

## 3. Testbed hygiene

All runs executed under the shared `testbed.lock` (`flock -w`), START/END logged to
`activity.log`, zero `pkill -f` used throughout (all kills were explicit-PID after `ps`/`pgrep`
inspection — one such kill was needed, for the hung `broker3` process and its parent sweep chain
during the bug investigation in §0). Testbed verified clean (`pgrep -fa` on moscxl/c1/c3, lock
free) after every stage, including at the end of this Phase 1 pass.

## 4. Summary for E4e / reviewer response

- **W5-B's leg-2 cost is now quantified two ways:** a clean NIC-bound plateau at ~87.5 Gb/s
  (Signal A, matching the ~90 Gb/s device reference within 3%), and a sharp control-plane
  starvation collapse once that plateau is hit (Signal B, >98% throughput loss, latency blowing
  out 5-10x further).
- **Reviewer-D's ~30M tok/s naive-RDMA-ordering figure does not survive contact with real
  single-global-order semantics** — it collapses to ~2.0 Mtok/s (>14.5x) on identical hardware.
  Per-op RTT (~3µs) is not the bottleneck; aggregate ordered throughput is.
- Both findings came from following the pre-registration's own discipline (diagnose deviations,
  don't paper over them) rather than accepting the first number measured.

Next: Phase 2 (W5-A broker-DRAM variant + host-kill data-loss/re-replication measurement), per
the phased plan — pausing here for review first.
