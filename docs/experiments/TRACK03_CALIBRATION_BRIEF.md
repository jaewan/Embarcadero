# Track-03 baseline calibration brief — for the moscxl agent

**Goal:** convert "trust our CXL-mailbox baseline ports" into "our ports meet-or-exceed the published
floors where a single-host-comparable number exists." The port CODE is done and functionally verified
on broker (all mailbox/corfu/scalog/lazylog smoke+bench checks pass). What's missing is **calibrated
throughput/latency at scale** — the current benches run tiny N and are warmup-dominated, so their
numbers are not yet calibration-grade. Authoritative targets + honesty rules live in
`docs/baselines/calibration_plan.md`; **read it first** — this brief operationalizes it.

## The honesty rule (non-negotiable, from calibration_plan.md)
Every original paper ran on a multi-node cluster with older/slower NICs; our single host (moscxl:
512c, 1.7TB, 100Gb CX-5, CXL) is strictly newer. So calibration is **directional (meet-or-exceed a
floor)** for CPU/algorithm-bound rates, **exact** for structural invariants (RTT counts), and
**explicitly out-of-scope** for cluster-aggregate + storage/NIC-bound absolutes. Falling *below* a
published floor on faster hardware is the real red flag — and if it happens, **report it honestly**
(e.g. a sub-floor Corfu token rate would mean the mailbox round-trip, not the counter, bounds
Corfu-on-CXL — a real E10 finding, not something to hide).

## Committed targets (the floors to meet-or-exceed)
| Baseline | Comparable number (floor) | Source |
|---|---|---|
| **Corfu** | sequencer **≥570K tokens/s no-batch**, **>2M tokens/s at batch=4** | Tango SOSP'13 Fig.2 |
| **Scalog** | single-shard **≥18.7K writes/s @4KB**; latency ~1.3 ms envelope (±30–50%) at 0.1 ms interleave | NSDI'20 §6.3 |
| **LazyLog** | single-leader ordering **≥1.34M metadata-appends/s**; **1-RTT append** (structural, exact) | SOSP'24 §6.6 |
Out of scope (state, don't measure): cluster aggregates (Scalog 52M, LazyLog 700K/10-shard), absolute
µs latencies (RDMA/SSD-bound), Scalog p99 (none published).

## The benches (Track-03-owned code you may edit + commit)
`src/cxl_manager/{corfu,scalog,lazylog}_mailbox_bench.cc`, CTest targets `corfu_mailbox_bench`,
`scalog_mailbox_bench`, `lazylog_mailbox_bench`. They currently:
- use **compile-time constants** and ignore argv → to scale you **edit the constant and recompile**;
- Corfu: `kMessagesPerRequest` (batch size; **=1 for no-batch, =4 for batched**), `kNumClients=8`, plus
  a per-client request count — find it and scale so the 30 s deadline loop runs seconds at plateau,
  not ~40 ms of warmup. It already times `min_post→max_recv` and prints tokens/s.
- Scalog/LazyLog: `kEpochs=2000` → scale up (10–50×) so the run is seconds long; they print cuts/s and
  ordered-msg/s (Scalog) / bindings/s + ordered-msg/s (LazyLog).

## What to do
1. **Scale N** in each bench so the measured window is a **steady-state plateau** (seconds, warmup
   amortized), not a cold burst. Report the plateau rate, not the whole-run average that includes setup.
2. **Corfu:** run **two configs** — no-batch (`kMessagesPerRequest=1`) and batch=4 — vs the 570K / 2M
   floors. This is the one directly-comparable number, so it's the priority.
3. **Scalog / LazyLog:** report plateau throughput vs their floors (Scalog ≥18.7K/shard-equiv;
   LazyLog ≥1.34M metadata-appends/s). LazyLog's 1-RTT append is already structurally preserved by the
   mailbox (no gRPC coordinator round-trip) — state that as the exact-match invariant.
4. **Latency (only where a target exists):** Scalog single-shard latency (~1.3 ms envelope). The
   benches don't sample per-op latency today — if adding a small per-op latency histogram (P50/P99) is
   straightforward, do it and report; if it balloons scope, report throughput and note latency
   percentiles as new data (Scalog publishes no p99 anyway). The **throughput floor is the required
   deliverable; latency percentiles are best-effort.**
5. **≥3 trials** per config (5 if you make a latency claim), under `flock`; report **median ± range**.
6. **Optional (flag, don't block):** the calibration_plan's TCP-vs-CXL delta (E1 leg-1→leg-2) needs a
   TCP-transport throughput bench per baseline; those may not exist yet. If a cheap TCP number is
   available (e.g. via the existing gRPC sequencer path), report it as the delta; otherwise mark
   "TCP-baseline delta = follow-up" and just deliver the CXL floors.

## Deliverable
Add a **calibration results** section to `docs/baselines/fairness_appendix.md` (the calibration_plan
method says record there when run) — or a new `docs/baselines/calibration_results.md` if cleaner —
with, per baseline: **target / measured (median ± range, N, trial count) / tolerance / verdict
(meets-floor | below-floor-honest-finding | out-of-scope)**. Note the scaled N used and that it is a
plateau number. If any baseline is below floor, write the honest mechanism (mailbox round-trip cost).

## Discipline + commit
Build outside the flock lock; hold the lock only for runs. **Never `pkill -f throughput_test/embarlet`**
(self-kills the shell); kill strays by explicit PID. Verify lock free + no strays after. Commit the
bench edits + results doc on a work branch (`--no-verify --no-gpg-sign`; Co-Authored-By trailer), then
`git bundle create /tmp/track03_calib.bundle <base>..<branch>` and report the SHA + bundle path + the
results table.
