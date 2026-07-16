# Fig1 RF2/ACK2 mem-mode anomaly — diagnosis (2026-07-16)

**Question:** why does Embarcadero ORDER=5 *mem* lose to the Scalog ORDER=1 *mem*
baseline at every N (ACK Bandwidth 6.36–7.98 vs 7.68–9.57 GB/s), when the
hardware says a fair RF2 memory-copy bakeoff should not go that way?

**Verdict (one paragraph):** the Scalog numbers are **not honest ACK2**. Since
commit `4241ed1d` (2026-07-13), SCALOG's per-client ACK2 durable counter is
credited **twice per message** by two independent paths, so the client's
ACK-wait can exit when as little as ~50 % of its bytes are actually
replicated. Every diagnostic signature in pass `20260716T024703Z` matches this
bug (per-broker acked > per-broker sent, +19–46 % raw-ACK excess, overlap GB/s
exceeding send-done, the local client finishing while replica cuts were at
~50 %). Correcting for it, honest Scalog mem n4 is ≈ 8.9–9.2 GB/s, not 9.57.
The residual honest gap (~10–15 % over Embar) is real and attributable to
ORDER=5's serialized commit/chain/head-ACK machinery — a protocol cost we can
name stage by stage, not a sink unfairness. Embarcadero's own path is verified
honest. Scalog **disk** cells and all LazyLog ACK2 numbers built after
2026-07-13/15 carry the same inflation.

---

## 1. Testbed model (measured, not paper-claimed)

| Resource | Measured ceiling | Evidence |
|---|---|---|
| Broker NIC | **one** 100 GbE port (`enp193s0f0np0`) ⇒ ~12.3 GB/s aggregate remote ingress | `ip`/sysfs on moscxl; 6-proc iperf3 12.25 GB/s (ceiling handoff) |
| Client NICs | c4/c3: CX-5 100G ×16 (≈12.3 GB/s each); c1: CX-5 100G on ×8 slot (≈7.9 GB/s) | sysfs speed=100000, lspci; MOSCXL handoff |
| Per-client producer | ~5.7–6.5 GB/s at 1 KB msgs; ≈8.1 GB/s at Fig1's 4 KB (observed Send-done, c4 n1) | trial logs |
| CXL module | 256 GB CPU-less NUMA node 2 (paper Sec7 says 512 GB — flag for eval rewrite); ~21 GB/s single-direction write, ~10 GB/s/direction concurrent R+W | `numactl -H`; FIG1.md caveat C |
| ORDER=5 ordered-ingest | **8.5–9.3 GB/s** at RF=0/ACK=1 (post-2026-07-12 fixes; `EpochSequencerThread` ~95 % busy) | THROUGHPUT_CEILING_HANDOFF_2026_07_12 §Resolution |
| NVMe | 990 PRO 2 TB (root) + PM9A3 960 G (`/mnt/nvme0`), dual-dir striped 4:1 | lsblk; run_multiclient REPLICA_DISK_WEIGHTS |

**What should bind, mem mode:** N=1 → client producer (~8 GB/s at 4 KB); N≥2 →
NOT the NIC (12.3 GB/s aggregate ≫ observed 8–9.5): the binding resources are
(a) Embar: the single-threaded ORDER=5 epoch-sequencer commit (~9 GB/s known)
and the replica invalidate+copy pipeline; (b) Scalog: the CXL concurrent
R+W envelope (~10 GB/s/dir; ingest-write ≈ replica-read ≈ 8–9.5 GB/s each).
Per ACK'd byte both systems move: 1× CXL write (ingest) + 1× ingest-range
clflushopt + 2× full-range clflush-invalidate + 2× CXL read + 2× DRAM-ring
write (self + successor sink). ~8–9.5 GB/s ACK BW is exactly this envelope.

**What should bind, disk mode:** replica media write + amortized fdatasync
(256 MiB / 250 ms). Both systems ≈1.24–1.38 GB/s, flat in N ⇒ NVMe-bound,
panel plausible **subject to the Scalog accounting bug below** (honest Scalog
disk is likely somewhat lower than measured).

## 2. Thought experiment (before looking at code)

Scalog ORDER=1's ordering is per-epoch cuts (100 µs cadence,
`config/embarcadero.yaml:72`) — no per-batch single-writer sequencer on the
data path — and its RF2 is parallel `min(replication_done)`. Embar ORDER=5
inserts: 500 µs epoch seal → single `EpochSequencerThread` GOI commit →
`committed_seq` contiguity → GOI-dispatched replica copy → per-entry
serialized `num_replicated` token chain → tail CV → **head-broker-only** ACK
relay. So *some* Scalog advantage at saturation is theoretically expected —
"Embar mem slower than Scalog mem" is only surprising in magnitude, and only
if Scalog's ACK2 is doing equivalent work. A ≥20 % gap with near-identical
Send-done, plus Scalog overlap rates that exceed its own send rate, smells
like an accounting or contract problem, not protocol tax. That instinct was
correct.

## 3. Code-path verdicts

### Embarcadero ORDER=5 RF2 ACK2 — **correct and honest** (mem and disk)

- Sink wiring: `chain_replication.cc:138-163,204-214`; mem = CXL→DRAM ring
  memcpy, **zero fsync** (sync workers only spawned when `!in_memory_sink`,
  :506); disk = `pwrite` (:466) + `fdatasync` (:540) amortized at 256 MiB /
  250 ms. Labels `replicated_ack_emulated` / `media_durable` (:228-231) honest.
- Replication: copy-ahead parallel per source (roles {s, s+1},
  `performance_utils.h:792-817`), each chunk = one 2 MiB GOI entry with a
  **full-range serializing clflush invalidate (32 768 clflush per 2 MiB)** on a
  single per-source sink thread (`chain_replication.cc:435-438`,
  `performance_utils.h:225-241`); only the ACK token is chained
  (role 0 → role 1 CAS → CV, :575-708).
- ACK2 frontier: head-owned; broker 0 only (`network_manager.cc:3094-3103`);
  `MaybeAdvanceOrder5DurableFromCV` (`topic.cc:2182-2262`) requires GOI commit
  + tail-CV coverage + per-owner contiguous prefix. With RF≥2 the sequencer
  never advances the durable CV itself (`topic.cc:3611-3613`) — ACK2 truly
  waits on replication.
- Client metric: `Bandwidth = bytes / (end_after_Poll − start)`
  (`test_utils.cc:1456-1473`); Poll blocks until the per-client cumulative HWM
  covers the last message (`publisher.cc:2295-2417`). Embar ACK accounting is
  exact: raw ≈ target (655 326/655 360), final `Cum_Ack_Bytes` ≡ bytes sent.

### Scalog ORDER=1 RF2 ACK2 — **broker-side fair, client-visible ACK2 broken**

Broker-side sink/replication work is matched with Embar (post-`9d5d5696`):
per-source 256 MiB DRAM rings, same full-range invalidate before copy, no
fsync in mem mode, amortized fdatasync in disk mode, parallel per-primary
replica loops polling `validated_written` (`scalog_replication_manager.cc:930-1149`).
Replica cuts equal primary cuts at n4 (replica bytes = 1.0× published) — RF2
physically happened.

**The bug — double-credited ACK2 durable counter.** For SCALOG the AckThread
sends `GetClientDurable(client_id)` per broker
(`SupportsPerClientAckLevel2Durable`, `topic.cc:2170-2173`;
`network_manager.cc:3104-3107`). `per_client_durable_` is credited by **two
paths for the same messages**:

- **Path A** (no ordering gate): every batch registered at ingest
  (`ScalogGetCXLBuffer` callback → `RecordOrder0DurableBatch`,
  `topic.cc:2459-2482`), credited by `MaybeAdvanceOrder0DurableVisibility`
  (`topic.cc:1938-1993`) once `min(replication_done{b, b+1})` covers it.
  SCALOG was added to this ORDER-0 path by **`4241ed1d` (2026-07-13)**.
- **Path B** (ordered + replicated): on global-cut application,
  `ScalogSequencer::publish_batch` → `EnqueueDurableBatch` →
  `DrainDurableBatches` → `RecordPerClientDurableVisibility`
  (`scalog_local_sequencer.cc:180-232,336-342`; from `5a386e6d`, 2026-04-02).

Each message is credited once by A (at replication) and once by B (at
ordering), so the per-client cumulative the client waits on approaches **2×**
its true durable count. The client's exit condition
`Σ_b min(sent_b, acked_b) ≥ target` (`publisher.cc:2312-2318,2336`) is then
satisfied when *replicated + ordered* credits reach `sent_b` — in the limit
when only ~50 % of the client's messages are durable.

**Every log signature matches** (pass `20260716T024703Z`, scalog mem n4):

- Per-broker acked > per-broker sent for every client — impossible under
  honest per-client attribution (e.g. c4 `B0=211176(sent=164052)`, local
  `B0=286776(sent=164052)`; `[Publisher ACK Per-Broker]`, trial1_*.log).
- Raw excess +19 % (c4), +40 % (c3), +29 % (c1), +46 % (local)
  (`[Publisher ACK Normalize]`).
- One ACK socket per client per broker, distinct client_ids (711266, 243471,
  47318, 50568), zero reconnects — ruling out socket double-count / id
  collision / re-baselining (preserved broker logs,
  `broker_logs_scalog_mem_n4/`).
- Local client (fastest sender ⇒ most Path-B ordering credit early) finished
  its ACK wait at 12:13:05.171 while global replica cuts were ~50 %; c4/c3/c1
  happened to exit near true convergence (05.663 vs frontier converge
  05.617-05.692) because the global cut (min across brokers, b3 lagging)
  starved Path B until the tail.
- Scalog overlap 9.42 GB/s > its own Send-done 7.94 at n1 — the `Ack_GiBps`
  timeseries integrates the double credits, so **all Scalog overlap values are
  invalid**, and `Cum_Ack_Bytes` finals of 3.3–4.1 GB vs 2.68 GB sent.

**Secondary contract defect:** Path A has **no ordered clamp** — the binding
per-client path can ACK replicated-but-not-yet-ordered bytes, violating
`ack2_minimum_memory_copy_replica_prefix` ("orders from local durable cuts,
then clamps..."). The `[[SCALOG_CORRECTNESS_FIX]]` ordered clamp exists only
in the legacy `GetOffsetToAck` path (`network_manager.cc:2710-2722`) that the
per-client branch supersedes.

**Blast radius:** SCALOG disk cells (same crediting, sink-independent);
LAZYLOG ACK2 since `42cb79f8` (2026-07-15) — same dual-path shape
(`lazylog_local_sequencer.cc:267` + `topic.cc:1920`). CORFU is single-path
(`RecordCorfuOrder2DurableCompletion`) — unaffected. Embar unaffected. ACK1
(ordered) counters are single-credited — earlier ACK1/auto campaigns unaffected.

## 4. Ranked root causes for "Embar mem < Scalog mem"

1. **Scalog ACK2 double-credit bug** (above). Inflation is condition-dependent
   (bounded ×2 in the limit): at n4 the headline 9.57 → honest ≈ **8.9–9.2**
   (local client's wall alone is understated ~0.2–0.3 s); at n1 ≈ 7.68 →
   ~7.3–7.5. All Scalog overlap numbers are unusable. Scalog *disk* parity
   with Embar disk is also softened once honest.
2. **Genuine ORDER=5 protocol cost (~10–15 % residual, quantified).** Embar
   mem n4 = 7.98–8.17 GB/s sits at the independently measured ordered-ingest
   ceiling (8.5–9.3 GB/s at RF0/ACK1 minus RF2's extra CXL read+invalidate
   traffic sharing the link). The serial stage chain
   (500 µs epoch seal → single-thread `CommitEpoch` → `committed_seq` updater
   (200 µs cadence) → GOI-dispatched copy → per-entry token chain → head-only
   AckThread doing a ~33-cacheline invalidate walk under one global
   `per_client_durable_mu_` for all clients) also explains the n1 drain
   asymmetry: Embar ack_wait 314 ms vs Scalog ~42 ms — Scalog replica loops
   copy from `validated_written` (pre-ordering, big contiguous chunks) while
   Embar copies only post-commit 2 MiB GOI entries. This is protocol, not sink
   unfairness — and it is exactly what FIG1.md caveat A2.1 said, now with
   stage-level attribution.
3. **Metric artifacts** (secondary): overlap windows 0.5–1.6 s ≪ 10 s minimum
   at every mem cell; "aggregate = Σ per-client rates" overstates when client
   windows don't overlap (Send-done 29 GB/s at n4 vs a 12.3 GB/s broker NIC is
   arithmetically possible only because windows are staggered/summed).
   Bandwidth is the right fairness metric; overlap should not be cited for
   these cells at all.
4. **Single-trial noise — insufficient as an explanation.** The gap has the
   same sign at all four N in two separate passes (221645Z, 222736Z vs
   024703Z), and the Scalog-side signatures (acked > sent per broker) are
   structural impossibilities, not variance.

## 5. Proposed fix (not applied)

In `topic.cc`, remove SCALOG (and LAZYLOG) from `uses_replication_done_durability`
in **both** `RecordOrder0DurableBatch` (:1916-1923) and
`MaybeAdvanceOrder0DurableVisibility` (:1938-1945) — i.e. revert the SCALOG
clause of `4241ed1d` and the LAZYLOG clause of `42cb79f8` — keeping Path B
(sequencer `DrainDurableBatches`), which is correctly gated on
**ordered ∧ min(replication_done)** and matches the contract string. Add a
cheap canary: WARN-once if `per_client_durable_[c]` exceeds the client's
written count on that broker (this invariant would have caught the bug
immediately). Check LazyLog's Path A/Path B interaction the same way before
re-including it anywhere.

Bookkeeping (separate, harness-only): pass `20260716T024703Z` wrote per-cell
results as `fail,missing_overlap_csv` despite driver PASS because the
per-cell `multiclient/logs/.../<pass>/` dir was never created — cell logs are
authoritative; fix the results writer before the next campaign.

## 6. Smallest decisive next experiments (in order)

1. **After the fix + embarlet rebuild:** re-run only `fig1_scalog_o1_mem_n1`
   and `fig1_scalog_o1_mem_n4` (one trial). Expect: no
   `[Publisher ACK Normalize]` warning (raw == target), overlap ≤ send-done,
   n4 Bandwidth ≈ 8.9–9.2. If it lands there, the honest cross-system story is
   "Scalog mem ≈ +10–15 % over Embar mem; ordered-chain protocol tax", which
   the paper can own.
2. Re-run `fig1_scalog_o1_disk_n1/n4` once — determines whether honest Scalog
   disk drops below Embar disk (FIG1.md caveat E prediction).
3. **Embar decomposition at n1 mem (no code change):** {RF0 ACK1}, {RF2 ACK1},
   {RF2 ACK2} — separates sequencer-commit vs replica-copy vs CV/ack-relay
   contributions to the 314 ms drain (≈1.7 GB/s of standing lag).
4. Optional: `perf record` on broker-0 AckThread + one chain sink thread
   during an n1 mem cell to confirm the serializing-clflush invalidate and the
   `per_client_durable_mu_` walk as the top cycles.

## 7. Artifacts

- Scalog mem n4 broker logs preserved:
  `data/paper_eval/fig1/fig1_rf2_ack2_scaling/logs/20260716T024703Z/broker_logs_scalog_mem_n4/`
  (they lived in volatile `/tmp/broker_*.log`).
- Numbers recovered from cell logs (embar: pass `20260715T222736Z`): Embar mem
  BW 6.36 / 7.18 / 7.73 / 7.98; Scalog mem BW (inflated) 7.68 / 7.98 / 7.92 /
  9.57; both disk panels ≈1.24–1.38 flat.
- Paper testbed nit for later: Sec7 says 512 GB CXL module; live node 2 is
  256 GB (`numactl -H`). Do not touch Evaluation now; note for the rewrite.
