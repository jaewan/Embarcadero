# W5-A Phase 2 results: host-kill leg-1 cost (RF≥2, real cross-host)

**Date:** 2026-07-08. **Branch:** `w5-variants`. Topology: broker0 on moscxl (10.10.10.10),
broker1 on c1 (10.10.10.11), memserver on c3 (10.10.10.181, 10.10.10.181 never killed — Decision
1), sequencer on moscxl. Raw logs + orchestration script in
[`w5_phase2_raw/`](w5_phase2_raw/).

## 0. Pre-registered deviation from the literal instruction: host-kill method

The task asked for `kill -9` the broker process **and** drop the host (NIC down / power /
cgroup-freeze), reasoning that a plain process kill "leaves the Blog alive." Before implementing,
I flagged this explicitly to the user: c1/c3/moscxl are shared, multi-tenant testbed hosts the D1
agent uses concurrently, and bringing down a shared host's real NIC would disrupt that work and
risk losing access to the host — a destructive, hard-to-reverse action on shared infrastructure I
should not take without explicit sign-off. **The user confirmed `kill -9` process-only** as the
approach, given a fact specific to this harness's implementation: the Blog is anonymous private
DRAM (`mmap`/`malloc`, never a shared-memory segment or file-backed region), so process death
already makes those bytes genuinely and permanently unrecoverable — no successor process can ever
re-attach to them — and the kernel atomically tears down the dead process's QPs/MRs, so the
sequencer's next operation against it gets a real, unfaked completion-level failure rather than a
soft timeout. This satisfies the spec's actual intent (irrecoverable data loss, genuine RDMA-level
failure) without the disproportionate blast radius of a literal host-level action on shared
hardware.

## 1. What was built

- **RF≥2 ring replication** (`rdma_broker.cc`, W5-A only): each broker, after its local DRAM
  store, RDMA-WRITEs the same bytes to a right-neighbor's dedicated replica region (1:1 mirrored
  layout, same `log_idx` offset). A 2-broker ring is mutual: broker0→broker1, broker1→broker0.
  Each broker also runs a replica-acceptor thread (reusing the persistent-listener OOB pattern
  from the earlier accept-race fix) that accepts its left neighbor's connection; the designated
  post-kill recovery *source* broker accepts a second, later connection from the recovery tool.
- **Recovery target on the memserver** (`rdma_memserver.cc`): a `--spare-bytes` region, always on
  c3 (never killed), with its own independent acceptor thread that stays armed for the whole run
  waiting for a late, unpredictable recovery connection — separate from the fixed-count client
  accept loop used for brokers/sequencer.
- **`rdma_recovery` tool** (new binary): connects to the surviving replica-holder (read) and the
  memserver's spare region (write), timing bytes moved end-to-end — the "re-replication
  volume+time" measurement.
- **Sequencer failure detection, two independent paths** (`rdma_sequencer.cc`):
  - *Lease-timeout backstop*: `last_alive_ts[broker]` renewed by every successful Blog read;
    500ms of silence → declared dead. Per the spec's M2 guidance, this is NOT a reused CXL
    constant — it's sized for RDMA RTT + polling-cycle granularity, not CXL's sub-µs coherence
    latency.
  - *RDMA-error fast path*: **a real bug was found and fixed here.** The original plan was "a
    failed `ibv_post_send` on a dead peer's QP is `kPeerDown`" — wrong. `post_send()` only
    enqueues a WR locally; it succeeds regardless of whether the peer is alive. A dead peer only
    surfaces later, as a **bad completion status** (RC retry/timeout exhaustion →
    `IBV_WC_RETRY_EXC_ERR`, or a flush error for anything queued after the QP entered the error
    state) — discovered by *polling*, not *posting*. A kill sanity test caught this directly:
    `!post_one(...)` never fired even once against a genuinely dead broker. Fixed by having the
    pipelined-ops poll loop inspect completions inline (instead of delegating to an opaque helper)
    and report the erroring completion's `qp_num` via a new `on_error` hook, which the sequencer
    maps back to a broker ID via a `qp_num → broker_id` table built at connect time.

## 2. A second real bug found via sanity testing (before any trial was trusted)

`rdma_recovery`'s first connection attempt against the surviving broker failed
(`errno=111 Connection refused`) — the broker's replica-acceptor was written to accept exactly
ONE connection (the left-neighbor's) and then stop listening. The recovery tool's later,
unpredictable connection had nowhere to land. Fixed with `--replica-max-accepts` (default 1,
set to 2 on whichever broker is the designated recovery source for a trial), reusing the same
accept-loop structure to take a second, later connection without re-opening the listener.

## 3. A third finding, from the FIRST kill attempt: the fast path structurally didn't fire

At the low offered load used for initial mechanism testing (20 Mbps/broker), lease-timeout
detected the kill every time; the fast path never did — because the sequencer had **zero backlog**
for the dying broker at the instant of death (it had already read everything broker0 ever
committed). Retested at unthrottled load to force a backlog: **still** only lease-timeout fired,
because at that extreme a *different* mechanism intervenes — heavy backlog gets discarded as
`stale_misses` (PBR ring-wrap on the metaserver, unrelated to the broker's death) before Step 4
ever reaches a real Blog-read attempt against the dying broker's QP. Both extremes independently
prevented the fast path from ever getting a live operation to fail against. This is the load
regime the final 3-trial run (§4) was run at — see §5 for why the same result holds there too.

## 4. Final 3-trial measurement (steady-state ~60% of peak, real cross-host kill)

**Config:** 2 brokers (RF=2 mutual ring), message size 4096B, inflight=32, `target_mbps=206`
(≈60% of the ~344 MB/s / 2.749 Gb/s single-broker unthrottled peak measured in the mechanism
sanity check), 30s run, broker0 killed via `kill -9` at t=10s (steady-state established well
before the kill). `flock`-serialized against the shared testbed; `kill -9` by explicit PID only.

| Trial | Detected at (t) | Via | Blog bytes unrecoverable | Recovery bytes | Recovery time | Broker1 (survivor) throughput | Broker1 stall |
|---|---|---|---|---|---|---|---|
| 1 | 10.478s | lease_timeout | 0 | 134,217,728 (128 MiB, fixed probe) | 35.51 ms (20.59ms read + 11.00ms write) | 1.648 Gb/s, rtt p50=100.4µs | none observed |
| 2 | 10.479s | lease_timeout | 0 | 134,217,728 | 34.96 ms (20.53ms read + 11.00ms write) | 1.648 Gb/s, rtt p50=101.2µs | none observed |
| 3 | 10.479s | lease_timeout | 0 | 134,217,728 | 34.76 ms (20.54ms read + 11.00ms write) | 1.648 Gb/s, rtt p50=100.5µs | none observed |

**Median ± range:** detect latency 10.479s ± 0.001s (i.e., **478–479 ms after the kill**,
matching the 500ms lease timeout minus polling granularity almost exactly); recovery 134.2 MiB in
34.96 ms ± 0.55 ms (**read ~52.2 Gb/s, write ~97.6 Gb/s**); unrecoverable bytes 0 in all 3 trials;
survivor throughput 1.648 Gb/s in all 3 trials, identical rtt distribution (p99=198.3µs,
p999=198.4µs) whether broker0 is alive or dead.

### (a) Blog bytes unrecoverable: 0 in all 3 trials

At 60% of peak — the spec's prescribed steady-state load — the sequencer's per-item processing is
fast enough (median detect-to-commit ~19–20µs, from Phase 1-style instrumentation) that it had
already consumed every batch broker0 ever committed before the 478ms lease timeout fired. This is
a genuine, positive result, not a non-finding: **at realistic (sub-saturating) load, W5-A's
failure window is dominated by detection latency, not unread backlog.** RF≥2 replication's value
in this regime is insurance against a slower or momentarily-backlogged sequencer, not a correction
for typical-case loss — Phase 1's overload probes separately confirmed a backlogged sequencer CAN
lose data to PBR ring-wrap under sustained overload, but that is a capacity/ring-sizing property
orthogonal to host death, not something a broker kill specifically triggers.

### (b) Re-replication volume + time: 128 MiB in ~35ms (~30 Gb/s effective end-to-end)

The 128 MiB figure is a **fixed probe size**, not a value derived from broker0's actual unflushed
backlog (which was 0 bytes, per (a) — there was nothing that actually *needed* recovering in these
trials). It demonstrates the recovery mechanism's throughput ceiling: how fast a meaningful chunk
of replicated data CAN be moved from a surviving replica-holder to a fresh, guaranteed-reachable
target once a recovery is triggered, not how much data these specific trials needed to move. The
read leg (broker1→recovery tool, ~52 Gb/s) is the bottleneck relative to the write leg
(recovery tool→memserver, ~97.6 Gb/s) — both single-QP, unpipelined (one op each, matching a
one-shot recovery of a specific byte range rather than a sustained transfer).

### (c) Per-session stall distribution across all M (M=2, one per broker)

Broker0 (killed): its producer session ends permanently at the kill instant — by design, a real
host kill has no session-level recovery, only data-level recovery (b). Broker1 (survivor): **zero
measurable stall** — identical throughput (1.648 Gb/s) and RTT distribution across all 3 trials
whether measured before or after broker0's death. This is because W5-A's per-broker DRAM writes
are causally independent of other brokers' health; the only broker0-death-triggered activity on
broker1 is its own outbound replication writes to the now-dead broker0, which are fire-and-forget
and do not gate broker1's primary publish path (see the known limitation below). This is a direct,
favorable contrast to Phase 1's W5-B Signal B, where one broker's saturating load could starve
*other* brokers' control-plane traffic through the shared memserver NIC — W5-A's per-broker NICs
structurally can't propagate that kind of cross-broker interference.

### (d) Failure detection race: lease-timeout wins every trial; the fast path never fires

Per §3's mechanistic analysis, this is not a coin-flip result — it is what the design *should*
produce at a load where the sequencer keeps pace: the fast RDMA-error path requires a live,
in-flight operation against the dying broker's QP at the failure instant, and a system that isn't
backlogged has nothing in flight to fail. **Report as a genuine finding, not a tuning artifact:**
the naive expectation that "the fast path should normally win because it's faster" does not hold
once you account for *when* it gets a chance to fire at all — in practice, for this workload
pattern, the lease-timeout backstop is the primary detection mechanism, not a rarely-needed
fallback.

## 5. Known limitation (disclosed, not a correctness issue for (a)–(d))

Broker1's replication QP (`replica_client_qp`) has no backpressure or circuit-breaker: once
broker0 dies, its outstanding WRs never complete (stuck on RC retry/timeout against a QP that no
longer exists), the send queue fills, and every subsequent `ibv_post_send` fails locally with
`ENOSPC` for the remainder of the run (~180K repeated log lines per trial, truncated in the
archived raw logs — see `w5_phase2_raw/trial*/broker1.txt`). This is silent noise, not corruption:
broker1's own primary publish path (a separate QP) is completely unaffected, as confirmed by its
identical throughput/RTT across all 3 trials. A production design would want a circuit-breaker on
the replication path once a peer is known dead; this measurement harness does not need one to
produce correct (a)–(d) numbers, so it was left as a disclosed gap rather than built out further.

## 6. Seven per-run facts

1. **QP count:** per trial, at peak: broker0↔memserver=1, broker1↔memserver=1, sequencer↔memserver=1,
   broker0↔broker1 replication=2 (one each direction), broker0/1↔sequencer direct Blog=2,
   recovery↔broker1=1, recovery↔memserver=1 — 9 RC QPs total, spanning 3 hosts.
2. **CQ depth:** 4096 for all broker/sequencer/memserver-client QPs; 64 for the tiny
   recovery/replica-handoff QPs (fixed at connect time, matching Phase 1's convention).
3. **MR sizes:** per broker — local Blog 1 GiB, replica-of-neighbor 1 GiB. Memserver (2-broker
   layout) — control/cv/sentinel/goi/pbr sized per `MemserverLayout` for num_brokers=2,
   pbr_slots=16384, goi_entries=2,000,000; spare region 256 MiB.
4. **NIC port count:** 1 per host (mlx5_0-class, no bonding), 3 hosts involved (moscxl, c1, c3).
5. **RoCEv2 CC config:** unchanged from Phase 1's disclosed state — global pause on
   (`ethtool -a`: RX=on, TX=on), no per-priority PFC, no dedicated RoCE VLAN/priority isolation.
   Same caveat applies: this is not yet the isolated-RoCE state the Phase 3 prerequisites call for.
6. **Hugepage/NUMA placement:** hugepage-backed (`MAP_HUGETLB`) on all three hosts; NUMA locality
   not explicitly controlled (same disclosed gap as Phase 1 — not re-verified per-host here, but
   no reason to expect it differs from Phase 1's moscxl finding).
7. **Exact flock `activity.log` lines:** `[2026-07-07T22:28:51Z]` through `[2026-07-07T22:31:48Z]`
   spanning all 3 trials — `w5-variants: START/END Phase 2 host-kill trial {1,2,3} (real
   cross-host, fixed durations)`.

## 7. Summary for E4e / reviewer response

- **Leg-1 cost is measured and is small at realistic load**: zero data loss, ~35ms recovery for a
  128 MiB probe, zero stall on the surviving session, ~478ms detection latency — all via the
  backstop path, since the fast path structurally doesn't get a chance to fire when the system
  isn't backlogged. This is the "expect a re-replication-bound stall" prediction from the Phase 3
  plan's W5-A column description — at THIS load, that stall didn't materialize because there was
  nothing left to re-replicate; a higher-load or higher-loss-inducing trial would be needed to
  observe a non-trivial re-replication-bound stall, which Phase 3's E4e headline run should account
  for rather than assume.
- **A structural insight about failure detection**, not anticipated in the original plan: the
  "fast path vs backstop" framing implicitly assumes the fast path usually wins; in a
  non-backlogged system it structurally cannot get a chance to fire, making the backstop the
  practical primary mechanism. Worth stating explicitly in any reviewer-facing framing of W5-A's
  failure detection story.

Next: Phase 3 prerequisites (flagged, not started — multi-threaded sequencer, RoCE priority
isolation re-run of Signal B, QP-count-at-fixed-load sweep, spreading brokers across enough sender
hosts) per the user's explicit instruction not to begin Phase 3 without them. Pausing here for
review.
