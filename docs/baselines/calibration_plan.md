# Baseline Calibration Plan (panel #10)

**Purpose.** The E1 waterfall's middle leg is only trustworthy if each baseline reimplementation is
*faithful*. Calibration converts "trust our port" into "our port matches theirs where a comparable
number exists." This plan records, per baseline, the authoritative published numbers, which are
legitimately reproducible on our **single-host** testbed (moscxl: 512 cores, 1.7 TB RAM, ConnectX-5
100 Gb, CXL) and which are not, and the calibration target + tolerance we commit to.

**Overarching honesty rule (applies to all three).** Every original paper ran on a **multi-node
cluster with older/slower NICs** (Corfu 1 GbE; Scalog 10 GbE; LazyLog 25 Gb RDMA/RoCE). Our single
host has a 100 Gb NIC, CXL/DRAM storage, and far more/faster cores. So **tight two-sided (±X%)
tolerance bands are not scientifically defensible** for absolute throughput/latency — the hardware is
strictly newer. The correct calibration is **directional (meet-or-exceed a floor)** for
CPU/algorithm-bound rates, **exact** for structural protocol invariants (RTT counts), and
**explicitly out-of-scope** for cluster-aggregate and storage/NIC-bound absolutes. Falling *below* an
original number on strictly faster hardware is the real fidelity red flag.

Sources were read verbatim from the primary PDFs (multiple independent reads agreed on every figure).

---

## Corfu — token/sequencer rate is the one comparable number

**Published sequencer rates (network sequencer service over RPC):**
- **200K tokens/s** — CORFU, NSDI 2012, §1 + §5.2/Fig. 7 (append bottleneck ≈180K appends/s, sequencer-bound, user-space over TCP/IP).
- **500K tokens/s** — CORFU, ACM TOCS 2013, §1 (one sentence, *no method/figure* — weakest citation).
- **570K req/s no-batch, >2M req/s at batch=4** — Tango, SOSP 2013, §2.1, **Fig. 2** (RIO sequencer; the only rate with a real client-scaling methodology → the reference point).

**Comparable single-host?** YES for the sequencer rate — it is a standalone network-counter
microbenchmark (one sequencer, N client connections, count tokens/s at the plateau); needs no flash,
replication, or multiple hosts. This is exactly what `corfu_mailbox_bench` measures. NOT comparable:
append/read throughput (180K–400K, flash+1 GbE-bound), latency (~750 µs, SSD+NIC-bound), recovery
(~30 ms, multi-unit). Sequencer host CPU/NIC are **unspecified** in all three papers → we cannot
claim "comparable hardware," only "strictly more capable."

**Calibration target (committed):** the Corfu-on-CXL and Corfu-on-TCP sequencer must **meet or exceed
570K tokens/s no-batch and >2M at batch=4** (Tango Fig. 2), framed directionally ("≥, as expected on
faster hardware"). If we insist on a band: **within 2× of 570K no-batch**, justified by the ~100×
NIC gap. Do **not** calibrate against the 200K/500K numbers except as historical footnotes.

> ⚠️ Current `corfu_mailbox_bench` reports ~0.35–0.40 M tokens/s — **below the 570K floor**. This is a
> *benchmark-shape artifact, not a fidelity failure yet*: N=16,384 tokens over ~40 ms is warmup-
> dominated, and the per-token cost is the mailbox flush/fence floor (~0.6 M msg/s raw, and each token
> is a request+grant round-trip ≈ 2–4 mailbox ops). Before claiming calibration we must (a) scale N to
> millions over seconds, (b) report the steady-state plateau, and (c) if still <570K, that is a real
> finding about CXL-mailbox token latency vs a batched RIO/TCP sequencer and must be reported honestly
> (it would mean the *mailbox round-trip*, not the counter, bounds Corfu-on-CXL — relevant to E10).

## Scalog — per-shard rate (floor) + single-shard latency envelope

**Published (NSDI 2020, 40× CloudLab c220g1, 2×E5-2630v3, 10 GbE, SATA SSD, 4 KB records):**
- **Per-shard peak 18.7K writes/s** (§6.3.1; their Corfu 13.9K/shard) — the single-host-comparable unit (a shard = f+1 storage servers + local-cut path, co-locatable).
- **~1.3 ms single-shard client latency** at a **0.1 ms interleaving interval** (§6.3.2, Fig. 5).
- Centralized-sequencer comparison point: **~530K writes/s** (§6.3.1) — cross-confirms Corfu.
- **52M records/s headline is EMULATED cluster-aggregate** (footnote 4); 2.34M partial-emulation; 255K fully-real @17 shards. **No p50/p99 percentiles anywhere.**

**Comparable single-host?** ONLY the per-shard peak (as a floor) and the single-shard latency
envelope. The 52M/2.34M/255K aggregates are cluster-scale (and partly emulated) → **out of scope**.

**Calibration target (committed):** single-shard datapath **≥18.7K writes/s @4KB** (one-sided floor;
report the higher number our NIC/CXL enables and note the gap is hardware, not algorithm). Latency:
match the **0.1 ms interleaving-interval design point** and land within the **~1.3 ms envelope ±30–50%**
(wide band justified: paper gives a mean point value, no variance, and our NIC is faster). Report
intra-host multi-shard scaling as a **separate, clearly-labeled** experiment, NOT compared to
Scalog's inter-node scaling. State plainly: no p99 to calibrate against; if we report tails they are
new data.

## LazyLog — ordering ceiling (floor) + 1-RTT invariant (exact) + latency ratio

**Published (SOSP 2024 — Erwin-■/Erwin-st; xl170, E5-2640v4 10-core, 25 Gb ConnectX-4 RDMA/RoCE +
eRPC, SATA SSD):**
- **~1.34M metadata-appends/s** single-leader ordering ceiling (§6.6) — CPU/algorithm-bound.
- **~700K appends/s @4KB×10 shards, 29 µs** (Fig. 13) — aggregate, cluster.
- **~1M appends/s @100 B** (Fig. 12); per-shard ~32–34 KOps/s disk-bound (§6.1).
- **1-RTT append** (fast path writes f+1 replicas in parallel, ordering lazy in background — append
  returns a durability bool, not a position); **~4× (≤3.8×) lower append latency vs Corfu** (Fig. 6),
  ~2 orders vs Scalog. **No isolated "binding-round latency" number exists.**

**Comparable single-host?** The transport is **RDMA/eRPC, not TCP** — so a TCP port differs on
transport + topology + hardware generation simultaneously; **no absolute-µs number is apples-to-apples.**
Fidelity must be argued on *protocol behavior*.

**Calibration target (committed):**
1. **Structural (strongest):** reproduce **1-RTT append + lazy background binding** exactly — a
   round-trip-count invariant, transport-independent. Binary match.
2. **Quantitative floor:** single-leader ordering **≥1.34M metadata-appends/s** (directional; our
   host is stronger).
3. **Cross-hardware anchor:** build a Corfu-style eager baseline on the *same* TCP stack and reproduce
   the **~4× (3–5×) append-latency reduction ratio** (ratios cancel transport/hardware differences).
4. **Out of scope (state honestly):** absolute µs latencies (RDMA/SSD-bound), the 700K/10-shard
   scaling curve (needs a cluster), recovery (~600 µs, needs ZooKeeper/multi-node).

**CXL-port status: Pending.** The LazyLog coordinator (binding-round) path is now ported onto the
CXL mailbox (Track 03 / L1–L6; `lazylog_binding_core.{h,cc}`, `lazylog_mailbox_sequencer.{h,cc}`,
`lazylog_mailbox_messages.h`, `lazylog_mailbox_bench.cc`) as a protocol-preserving transport swap —
see the LazyLog section of `fairness_appendix.md`. The mailbox removes the gRPC coordinator
round-trip, so binding delivery should approach the 1-RTT ceiling; `lazylog_mailbox_bench` reports
bindings/sec + ordered-msg/sec and independently recomputes the per-round binding delta as the
correctness guard. The three committed numbers above (≥1.34M metadata-appends/s, 1-RTT append,
~4× latency ratio) are not yet measured on our host — author-check and calibration runs pending.

> Naming note for the paper: the LazyLog *systems* are **Erwin-■** (data through the sequencing layer)
> and **Erwin-st** (only metadata through it). Cite as Luo et al., SOSP '24.

---

## What we will NOT claim (the honest scope statement for the paper)
- No reproduction of any **cluster-aggregate** headline (Scalog 52M, LazyLog 700K/10-shard) on one host.
- No **absolute latency parity** for LazyLog (RDMA) or storage-bound append/read latencies (all three).
- No calibration of **tail (p99) latency** against Scalog (it publishes none).
- Corfu sequencer "comparable hardware" is unverifiable (specs unpublished) → "strictly more capable."

## Method (per baseline, recorded in `fairness_appendix.md` when run)
For each: run the reimplementation **on TCP** at the comparable microbenchmark, ≥3 trials (5 for any
latency/tail claim), report median ± range, and state target / measured / tolerance / verdict. The
CXL-transport port is then the same microbenchmark with the mailbox swapped in — the *delta* between
TCP and CXL is the transport contribution (E1 leg-1→leg-2), which is the number we actually want.

## Baseline-author sanity check (panel #10)
Draft short emails to the **Scalog (Ding et al.)** and **LazyLog (Luo et al.)** authors asking them to
review our port descriptions in `fairness_appendix.md`; record "authors reviewed" status there. Draft
lives in `docs/baselines/author_review_emails.md` (to be written); flag to the human to send.
