# Baseline Fairness Appendix

Per-baseline record of what each CXL port KEPT (frozen protocol) vs CHANGED (transport only) under the porting rule (docs/baselines/porting_rule.md), with the reason and calibration status. A port is honest only if the four frozen invariants — messages, exchange pattern, state machine, durability coupling — are preserved and only the wire changes; this is what keeps the E1 fairness waterfall unbiased.

## Corfu (CXL port, Track 03 / E10)

**KEPT (protocol preserved):**
- Message fields: `TotalOrderRequest{client_id, batch_seq, num_msg, total_size, broker_id}` -> `TotalOrderResponse{total_order, log_idx, broker_batch_seq}`, re-encoded byte-for-byte as the `CorfuTokenRequest` / `CorfuTokenGrant` PODs; no field added to or removed from the protocol.
- Sequencer state machine: per-client FIFO (one mutex per `client_id` so a single client's order is respected across brokers), the `expected_batch_seq` gate (already-processed -> reject, out-of-order -> retry), and the monotonic `next_order_` counter — the token-serialization point that is the E1/E10 knee.
- Decision function / arithmetic: one `next_order_.fetch_add(num_msg)` per batch (batched granularity, NOT per-message), plus the per-broker `log_idx` and `broker_batch_seq` counters, unchanged.
- Causal order: the token request is posted before the grant is read (request-before-order), the same unary request/response as the gRPC call.
- The gRPC baseline and the CXL port call literally the same code: `CorfuSequencerImpl::AssignToken` (steps 1-5 of `GetTotalOrder`) is shared, so no ordering logic is duplicated or altered.

**CHANGED (transport only) — proxy-relay, NOT requester collapse:**
- The sequencer-facing hop is swapped from a unary gRPC call to the CXL mailbox request/response rings (`up(b)` request, `down(b)` grant), with `client_id` + `batch_seq` echoed in the grant for defensive correlation.
- Because an external client cannot address CXL, the client's token request reaches the co-located sequencer via its **ingress broker acting as a pure transport proxy**: `client →(net)→ broker →(CXL mailbox)→ sequencer →(CXL mailbox)→ broker →(net)→ client`. **The client still issues the token request and still blocks for the token before its payload write is ordered or acked** — token-before-write and the client's two network round trips (token, then write) are preserved.

**Explicitly NOT changed (the forbidden move we did NOT make):** we did not collapse the token round trip — the broker does not request the token, write the payload, and ack the client in one `client→broker` RTT. Doing so would convert Corfu to write-before-order (Embarcadero-minus-hold-buffer) and void the E1 waterfall (panel #4 / porting_rule.md §5.1). Corfu's 2-RTT client path is the architectural cost E1 measures, kept intact.

**WHY:**
Co-locating the sequencer and relaying the token over the mailbox is the faithful way to give Corfu a *CXL transport for its sequencer hop* (reviewer C's ask) without altering the client-visible protocol. The sequencer still observes the same per-batch request stream and applies the same ordering logic, preserving Corfu's token-serialization point (the knee). CXL does **not** shorten the client-visible token RTT — that hop stays network-bound because the client is outside the pod — which is the honest, expected result for Corfu (contrast Scalog/LazyLog, whose coordination is pod-internal and *does* benefit from CXL).

**Scope so far / ownership:** The E10 pod-internal broker↔sequencer hop is implemented (`CorfuMailboxSequencer` + `corfu_mailbox_bench`; the bench's per-broker driver plays the broker-relay role and does not collapse any client RTT). The **client-visible** path (Unit 3b) is deferred and will live in a **forked Track-03 Corfu baseline client**, not in Embarcadero's shared `src/client/publisher.*` (Track 01). Corfu today co-opts the shared publisher via `src/client/corfu_client.h`; that sharing is itself a clean-baseline smell and will be forked, not extended. Any unavoidable shared-publisher hook is specced to Track 01, not applied by Track 03.

**Calibration status:** Pending (see `calibration_plan.md`). Committed Corfu target: sequencer **≥570K tokens/s no-batch, >2M at batch=4** (Tango SOSP'13, Fig. 2), framed directionally. The current bench (~0.35–0.40 M tokens/s) is a warmup-dominated 16K-token run and is **not** a calibration result; a scaled steady-state run is required, and if it stays below the floor that is a reportable finding about mailbox round-trip cost, not something to hide. Author-check pending (`author_review_emails.md`, TBD).

**Notes:**
- Batch size (`num_msg` per token request) is implementer-chosen and logged in `corfu_mailbox_bench`.
- Single-writer discipline (one thread per `up(b)`; the sequencer sole writer of every `down(b)`) enforces SPSC at the transport layer; a transport property, not a change to the ordering algorithm. Delivery is gated on `down.HasSpace()` before a token is assigned, so a wedged broker never head-of-line-blocks the others (regression-tested).
- The existing `corfu_sequencer_fifo_smoke` test passes unchanged, validating that the `AssignToken` extraction is behavior-preserving.

## Scalog (CXL port, Track 03 / S1–S5)

**KEPT (protocol preserved):**
- Message fields: `LocalCut{int64 local_cut, string topic, int64 broker_id, int64 epoch, int64 replica_id}` and `GlobalCut{map<int64,int64> global_cut}` re-encoded as the `ScalogLocalCutMsg` / `ScalogGlobalCutMsg` PODs; no field added to or removed from the protocol. `topic` is re-encoded as a `uint32 topic_id` (the single-topic bench uses id 0; a real multi-topic integration maps a topic NAME → id at configuration time — an application-layer concern, not part of the transport).
- Decision function: the **element-wise MIN across replicas per shard** — the durable-prefix decision — computed in `ScalogGlobalOrderingCore::ComputeGlobalCut`, shared **identically** by the gRPC path and the CXL path. The empty-replica-set → 0 and fewer-than-rf-replicas → 0 fallbacks and the regressing-cut rejection in `AddLocalCut` are extracted bit-for-bit from the baseline `SendGlobalCut`/`ReceiveLocalCut`, and the accumulation (`cumulative_cut_`) / last-raw-offset (`logical_offsets_`) bookkeeping is unchanged.
- Readiness / durability coupling (SCALOG_LIMITATION.md §5c/§5d): a cut is emitted only once **every expected broker/replica has reported** the round (`total_num_replicas == expected_replicas`, floored at one stream/broker for RF=0); the cut is the MIN across all reported replicas; there is **NO artificial per-tick cap**. This count gate is **exactly the gRPC baseline's original readiness** and is the ONLY readiness the gRPC path (E1 leg 1) uses — its behaviour is unchanged bit-for-bit (`use_epoch_barrier=false`).

**CHANGED (transport only):**
- The gRPC bidi stream `HandleSendLocalCut(stream LocalCut) → stream GlobalCut` is swapped for the CXL mailbox single-writer ring topology: each broker's local sequencer posts `LocalCut` on its `up(b)` ring (broker = single writer); the global sequencer's single poll thread is the sole reader of every `up(b)` and the sole writer of every `down(b)`, delivering `GlobalCut` via `MailboxSegment::BroadcastDown`.
- The proto `map<int64,int64> global_cut` is encoded as a **fixed by-broker array** `cut[kMaxBrokers=32]` (sentinel `-1` for absent brokers) so the record stays a pointer-free fixed POD that crosses CXL.
- **Cadence (mailbox path only):** the poll-driven sequencer is not message-driven, so the count gate stays permanently satisfied after warmup and cannot by itself mark a fresh report round. The mailbox path therefore opts into a per-epoch-completion barrier (`use_epoch_barrier=true`): emit one broadcast per round, when the closed epoch E = MIN over replicas of their latest reported epoch advances. This changes **when** a cut is emitted on the mailbox path only — never the MIN VALUES or the durability coupling — and the **gRPC baseline does not use it** (count-only, unchanged). `last_epoch` is monotonic (regression-hardened) and the up-ring is FIFO, so the barrier cannot deadlock on reordering.
- Back-pressure: gRPC per-stream flow control becomes finite CXL rings. A wedged broker (full `down(b)`, not draining) does **not** head-of-line-block the others and does **not** hang `Stop()`: `BroadcastDown` makes one bounded, decoupled pass and returns per-ring `BroadcastStatus`; a `WEDGED` ring is logged and skipped, and — because cuts are cumulative/idempotent — the wedged broker gets the newer cut on a later ready round. Regression-tested (`TestWedgedBrokerDoesNotBlock`).

**WHY:**
Scalog's cut/report coordination is **pod-internal** and is bottlenecked on the gRPC **coordinator round-trip** (SCALOG_LIMITATION.md §6: on CXL, Scalog and LazyLog converge to gRPC-coordinated write-before-order, and the coordinator hop is the cost the mailbox removes). Swapping that hop to the CXL mailbox is a **faithful transport swap**: the MIN-across-replicas decision and the durability coupling are identical (shared core), the gRPC baseline's readiness is untouched, and the mailbox path differs only in a poll-driven cadence barrier that paces one broadcast per completed round without changing any cut value.

**Scope & ownership:**
- Core extraction (S1): `ScalogGlobalOrderingCore` (`src/cxl_manager/scalog_global_ordering_core.{h,cc}`), shared by the gRPC path (`scalog_global_sequencer.cc` refactored to call it) and the mailbox path.
- POD encoding (S2): `ScalogLocalCutMsg` / `ScalogGlobalCutMsg` in `src/cxl_manager/scalog_mailbox_messages.h` (32 B / 272 B, `static_assert`ed; record_size ≥ 512).
- Mailbox sequencer (S3): `ScalogMailboxSequencer` (`src/cxl_manager/scalog_mailbox_sequencer.{h,cc}`); single poll thread drains `up(b)` rings, calls the core, broadcasts via `BroadcastDown`.
- Bench (S4): `scalog_mailbox_bench.cc` (anonymous MAP_SHARED segment, one driver + one receiver thread per broker, per-epoch lockstep so the ready snapshot maps to a well-defined epoch; independent MIN recomputation, monotonicity, broadcast-fidelity, wedged-broker regression; cuts/sec + ordered-msg/sec).
- The broker-resident **local** sequencer (`src/cxl_manager/scalog_local_sequencer.cc`, compiled into `embarlet`) is **untouched**: it still generates and applies cuts as before; only the global sequencer hop is ported.

**Durability-coupling caveat (open question, PRE-EXISTING — not introduced by this port):** the core computes MIN over the per-`(broker,replica)` `LocalCut`s it **receives**. The bench feeds explicit, distinct per-replica cuts, so the core's min-across-replicas is **directly validated** by the independent recompute. What the broker-resident local sequencer actually *reports* per round (true per-replica cuts vs. self-replica-only progress under RF>1) is a pre-existing property of the untouched `scalog_local_sequencer.cc`. If it reports self-only, the *end-to-end* durable prefix would be weaker than the core's capability — analogous to the LazyLog self-replica issue fixed in SCALOG_LIMITATION.md §5c. This is **flagged for the human as a separate baseline-fidelity question**; this appendix claims min-across-replicas for the **global-sequencer core** (bench-verified), not end-to-end across the untouched local sequencer.

**Calibration status:** Pending (see `calibration_plan.md`). Committed Scalog target: single-shard datapath **≥18.7K writes/s @4KB** (NSDI 2020, one-sided directional floor). The mailbox removes the gRPC coordinator round-trip Scalog is bottlenecked on, so CXL cut delivery should match or beat the gRPC round-trip; the harness reports cuts/sec and ordered-msg/sec, and independent per-epoch MIN recomputation validates global-cut correctness. Author-check pending (`author_review_emails.md`, TBD).

**Notes:**
- Single-writer discipline (one thread per `up(b)`; the sequencer poll thread sole writer of every `down(b)`) enforces SPSC at the transport layer — a transport property, not a change to the ordering algorithm.
- Readiness gates cadence: no cut is emitted until every expected broker/replica has reported for the round (`ComputeGlobalCut` returns false otherwise), preserving the durability coupling.
- **Behavior guard (honest scope):** `scalog_ack_invariant_test` is a standalone gtest of ACK1/ACK2 clamping and does **not** exercise `ScalogGlobalOrderingCore` — so it is *not* the guard for the S1 extraction. The actual correctness guard for the min-across-replicas core is **`scalog_mailbox_bench`'s independent per-round MIN recomputation** (it recomputes the expected cut in the harness and compares). The gRPC path's behavior-preservation is that its readiness is now count-only (`use_epoch_barrier=false`) — identical to the original `total_num_replicas == expected_replicas` gate — and `scalog_global_sequencer` still builds.

## LazyLog (CXL port, Track 03 / L1–L6)

**KEPT (protocol preserved):**
- Message fields: `LocalProgress{int64 local_progress, string topic, int64 broker_id, int64 epoch}` and `GlobalBinding{map<int64,int64> global_binding}` re-encoded as the `LazyLogLocalProgressMsg` / `LazyLogGlobalBindingMsg` PODs; no field added to or removed from the protocol. `topic` is re-encoded as a `uint32 topic_id` (the single-topic bench uses id 0; a real multi-topic integration maps a topic NAME → id at configuration time — an application-layer concern, not part of the transport). LazyLog's `LocalProgress` carries no `replica_id`: the broker's local sequencer already reports the MIN-across-its-replicas durable frontier as its single `local_progress` (SCALOG_LIMITATION.md §5c), so the coordinator tracks one cumulative value per broker.
- Decision function: the **per-broker binding delta** — `available = reported - already_bound`, the newly available progress for the round, bound in full (no cap) — computed in `LazyLogBindingCore::ComputeGlobalBinding`, shared **identically** by the gRPC path and the CXL path. The regressing-progress rejection in `AddLocalProgress` and the accumulation (`last_progress_`) / already-bound (`bound_progress_`) bookkeeping are extracted bit-for-bit from the baseline `SendGlobalBinding` (lines 177–191) / `ReceiveLocalProgress`.
- Readiness / durability coupling (SCALOG_LIMITATION.md §5c/§5d): a binding is emitted only once **every expected broker has registered and reported at least once** (`registered_brokers_.size() >= expected_brokers && reported_brokers_.size() >= expected_brokers`); progress is the MIN across all replicas in the set (reported per-broker by the untouched local sequencer, §5c); there is **NO artificial per-tick binding cap** (§5d: `kMaxBindingsPerBrokerPerTick == std::numeric_limits<int64_t>::max()`, so every ready round binds all available progress). This count gate is **exactly the gRPC baseline's original readiness** and is the ONLY readiness the gRPC path (E1 leg 1) uses — its behaviour is unchanged bit-for-bit (`use_epoch_barrier=false`).

**CHANGED (transport only):**
- The gRPC bidi stream `HandleSendLocalProgress(stream LocalProgress) → stream GlobalBinding` is swapped for the CXL mailbox single-writer ring topology: each broker's local sequencer posts `LocalProgress` on its `up(b)` ring (broker = single writer); the coordinator's single poll thread is the sole reader of every `up(b)` and the sole writer of every `down(b)`, delivering `GlobalBinding` via `MailboxSegment::BroadcastDown`.
- The proto `map<int64,int64> global_binding` is encoded as a **fixed by-broker array** `binding[kMaxBrokers=32]` (sentinel `-1` for brokers with nothing newly bound) so the record stays a pointer-free fixed POD that crosses CXL.
- **Cadence (mailbox path only):** the poll-driven sequencer is not message-driven, so the count gate stays permanently satisfied after warmup and cannot by itself mark a fresh report round. The mailbox path therefore opts into a per-epoch-completion barrier (`use_epoch_barrier=true`): emit one broadcast per round, when the closed epoch E = MIN over brokers of their latest reported epoch advances. This changes **when** a binding is emitted on the mailbox path only — never the binding delta VALUES or the durability coupling — and the **gRPC baseline does not use it** (count-only, unchanged). `last_epoch` is monotonic (regression-hardened) and the up-ring is FIFO, so the barrier cannot deadlock on reordering.
- Back-pressure: gRPC per-stream flow control becomes finite CXL rings. A wedged broker (full `down(b)`, not draining) does **not** head-of-line-block the others and does **not** hang `Stop()`: `BroadcastDown` makes one bounded, decoupled pass and returns per-ring `BroadcastStatus`; a `WEDGED` ring is logged and skipped, and — because binding deltas are cumulative/idempotent — the wedged broker gets the aggregate of the rounds it missed on a later ready round. Regression-tested (`TestWedgedBrokerDoesNotBlock`).

**WHY:**
LazyLog's binding-round coordination is **pod-internal** and is bottlenecked on the gRPC **coordinator round-trip** (SCALOG_LIMITATION.md §6: on CXL, Scalog and LazyLog converge to gRPC-coordinated write-before-order, and the coordinator hop is the cost the mailbox removes). Swapping that hop to the CXL mailbox is a **faithful transport swap**: the per-broker binding-delta decision and the min-across-replicas durability coupling are identical (shared core), the gRPC baseline's readiness is untouched, and the mailbox path differs only in a poll-driven cadence barrier that paces one broadcast per completed round without changing any binding value.

**Scope & ownership:**
- Core extraction (L1): `LazyLogBindingCore` (`src/cxl_manager/lazylog_binding_core.{h,cc}`), the transport-independent binding state machine, callable by both the gRPC path and the mailbox path. The gRPC `lazylog_global_sequencer.{h,cc}` is **left untouched** in this port (it still runs its inline logic, which is bit-for-bit what the core reproduces with `use_epoch_barrier=false`); a future refactor may point it at the core, and MUST pass `use_epoch_barrier=false`.
- POD encoding (L2): `LazyLogLocalProgressMsg` / `LazyLogGlobalBindingMsg` in `src/cxl_manager/lazylog_mailbox_messages.h` (32 B / 272 B, `static_assert`ed; record_size ≥ 512).
- Mailbox sequencer (L3): `LazyLogMailboxSequencer` (`src/cxl_manager/lazylog_mailbox_sequencer.{h,cc}`); single poll thread drains `up(b)` rings, calls the core, broadcasts via `BroadcastDown`.
- Bench (L4): `lazylog_mailbox_bench.cc` (anonymous MAP_SHARED segment, one driver + one receiver thread per broker, per-epoch lockstep so the ready snapshot maps to a well-defined epoch; independent binding-delta recomputation, monotonicity, broadcast-fidelity, wedged-broker regression; bindings/sec + ordered-msg/sec).
- The broker-resident **local** sequencer (`src/cxl_manager/lazylog_local_sequencer.cc`, compiled into `embarlet`) is **untouched**: it still generates progress and applies bindings as before; only the global sequencer hop is ported. No Track-01 file is edited.

**Durability-coupling caveat (open question, PRE-EXISTING — not introduced by this port):** the core computes binding deltas from the per-broker `LocalProgress`es it **receives**. The bench feeds explicit, deterministic per-round progress, so the core's delta computation is **directly validated** by the independent recompute. What the broker-resident local sequencer actually *reports* per round (true MIN-across-replicas progress vs. self-replica-only under replication) is a pre-existing property of the untouched `lazylog_local_sequencer.cc` (the §5c fix). If it reports self-only, the *end-to-end* durable frontier would be weaker than the core's capability. This is **flagged for the human as a separate baseline-fidelity question**; this appendix claims the binding-delta decision (with min-across-replicas progress as its input) for the **global-sequencer core** (bench-verified), not end-to-end across the untouched local sequencer.

**Calibration status:** Pending (see `calibration_plan.md`). Committed LazyLog target: single-leader ordering **≥1.34M metadata-appends/s** (ceiling, CPU/algorithm-bound), the **1-RTT append invariant** (exact, protocol-structural), and a **~4× latency ratio vs. Corfu** (cross-hardware anchor). The mailbox removes the gRPC coordinator round-trip LazyLog is bottlenecked on, so binding delivery should improve latency and approach the 1-RTT ceiling; the harness reports bindings/sec and ordered-msg/sec, and independent per-round binding-delta recomputation validates binding correctness. Author-check pending (`author_review_emails.md`, TBD).

**Notes:**
- Single-writer discipline (one thread per `up(b)`; the sequencer poll thread sole writer of every `down(b)`) enforces SPSC at the transport layer — a transport property, not a change to the ordering algorithm.
- Readiness gates cadence: no binding is emitted until every expected broker has registered and reported for the round (`ComputeGlobalBinding` returns false otherwise), preserving the durability coupling and the count-only readiness of the gRPC baseline.
- **Behavior guard (honest scope):** `lazylog_latency_invariant_test` is a standalone gtest of utility functions and does **not** exercise `LazyLogBindingCore` — so it is *not* the guard for the L1 extraction. The actual correctness guard for the binding-delta core is **`lazylog_mailbox_bench`'s independent per-round binding-delta recomputation** (it recomputes the expected delta in the harness and compares). The gRPC path's behavior-preservation is that its readiness is count-only (`use_epoch_barrier=false`) — identical to the original `registered_brokers_.size() >= expected_brokers && reported_brokers_.size() >= expected_brokers` gate — and `lazylog_global_sequencer` still builds (untouched).

---

## Calibration results (2026-07-07, moscxl, branch `track03-calib`)

**Method** (per `calibration_plan.md`): CXL-mailbox benches scaled to seconds-long steady-state
plateaus via new env-overridable shape knobs (`CORFU_BENCH_REQUESTS_PER_BROKER`,
`CORFU_BENCH_MSGS_PER_REQ`, `SCALOG_BENCH_EPOCHS`, `LAZYLOG_BENCH_EPOCHS`; smoke defaults
unchanged). 3 trials/config under the testbed lock; median (range). All correctness checks
passed in every trial. Timing window is first-post→last-recv, so a ~10 s run amortizes warmup
(plateau-grade). Latency percentiles: not sampled by these benches — best-effort item deferred
(Scalog publishes no p99 anyway; its ~1.3 ms envelope remains unmeasured here).

| Baseline | Config / scale | Target (floor) | Measured, median (range) | Verdict |
|---|---|---|---|---|
| Corfu | no-batch, 4×1M reqs (~10 s) | ≥570K tokens/s | **0.370 M tokens/s** (0.370–0.404) | **below-floor — honest finding** |
| Corfu | batch=4, 4×1M reqs (~10.7 s) | >2M tokens/s | **1.490 M tokens/s** (1.487–1.505); 0.372 M req/s | **below-floor — honest finding** |
| Scalog | 100K epochs (~2.1 s) | ≥18.7K writes/s/shard | **47.9K global-cut rounds/s** (47.8–48.0K); 383K local-cut inputs/s | **meets-floor (structural)** |
| LazyLog | 100K epochs (~1.4 s) | ≥1.34M metadata-appends/s | **73.7K binding-rounds/s** (73.2–73.8K); 294K progress-reports/s | **meets-floor (structural)**; unit differs — see below |
| LazyLog | 1-RTT append (exact, structural) | binary match | appends never wait on a binding round (lazy background binding preserved by the mailbox port) | **exact match** |

**Corfu — the honest finding (E10-relevant).** The request rate is FLAT across batch factors
(0.370 M/s at batch=1 vs 0.372 M/s at batch=4, 10-second plateaus): the bound is the
**per-request mailbox round-trip** (~2.7 µs: up-ring produce + sequencer poll + down-ring
grant + consume, each with clflushopt/sfence non-coherent-CXL discipline), NOT the counter
work and NOT warmup (the calibration_plan's ⚠ scenario (c) confirmed — scaling N from 16K to
4M left the rate unchanged). Tango's RIO/TCP sequencer exceeds 570K req/s by amortizing many
requests per NIC interrupt/syscall; a non-coherent shared-memory mailbox cannot amortize below
its per-record flush/fence floor. Consequence for E10: Corfu-on-CXL's token path is
mailbox-RTT-bound, so its knee is set by token latency, not counter throughput — report as a
finding, not tuned away.

**Why this is the faithful-port ceiling, not an untuned number** (verified against the port
code, to preempt the "did you optimize?" objection): the three ways to beat a per-request
round-trip floor are already exhausted or forbidden. (1) *Warmup* — excluded: scaling N 16K→4M
left the rate unchanged. (2) *Client pipelining* — already deep: the bench driver produces to
ring-full (up to 1024 outstanding) before draining grants, so up-flushes overlap sequencer
work. (3) *Sequencer poll batching* — already high: the poll thread drains `K_=64`
requests/broker/pass (`corfu_mailbox_sequencer` default), so idle-spin is amortized and each
request still costs its own consume(invalidate+fence) + `AssignToken` + produce(flush). The
only remaining lever is **multiple sequencer threads**, which would shard Corfu's single
monotonic `next_order_` counter — a **protocol-rule violation** (`porting_rule.md`: the
centralized single-counter sequencer IS Corfu's architecture; sharding it is a redesign, not a
port). So 370K req/s is the honest ceiling for a *faithful* single-counter Corfu sequencer over
a non-coherent mailbox that must durably cross-link-write each grant — Tango's 570K is beaten
only because its RIO counter needs no per-request durable cross-link write. Message-level
throughput still scales with batch factor (1.49M msgs/s at batch=4), so Corfu-on-CXL remains
usable for the E1/E10 comparisons at realistic batch sizes.

**Scalog — unit honesty.** The port is the cut/report path (the transport swap under test);
the 18.7K writes/s/shard floor is data-plane (4 KB records to storage — out of the port's
scope). The comparable structural statement: the mailbox cut path sustains **47.9K global-cut
rounds/s**, 4.8× the paper's 0.1 ms-interleave design point (10K cuts/s), so ordering capacity
cannot be the binder for an 18.7K writes/s shard (writes-per-cut ≥ 1). Verdict: the ordering
path meets the floor structurally; data-plane write throughput was not (and cannot be)
measured by this bench.

**LazyLog — unit honesty.** Erwin's 1.34M metadata-appends/s is a per-append single-leader
ceiling; in this port the coordinator aggregates per-broker progress counters, so it is
structurally not per-append-bound. At the measured **73.7K binding-rounds/s**, covering
1.34M appends/s requires only ~4.6 appends per broker per round — satisfied at any real load;
the coordinator ceiling exceeds the floor for every workload with ≥5-append rounds. The
exact-match calibration item is the structural invariant: **1-RTT append with lazy background
binding** (appends return on durability, never wait for a binding round), which the mailbox
port preserves (no gRPC coordinator round-trip on the append path). The ~4× latency-ratio
anchor (vs a same-stack eager baseline) remains follow-up.

**TCP-vs-CXL delta (E1 leg-1→leg-2):** follow-up — no TCP-transport twin of these
microbenches exists yet; the gRPC sequencer paths are not shape-comparable without work.

**Scaled-N provenance:** raw output preserved at `/tmp/t03_calib_out.log` on moscxl
(12/12 trials `ALL CHECKS PASSED`).

---

## Measurement methodology: warm-up discard + RF-order interleaving (mandatory)

This testbed exhibits a **deterministic first-run warm-up**: the first trial after a fresh cluster
start is reproducibly the slowest, even on an idle host (verified — SCALOG RF=1 publish goodput
`2979 → 3611 → 3588 …` MB/s with trial 1 at load 0.01; CORFU RF=1 `2902 → 11298 → 12633` monotonic).
Run all-RF1-then-all-RF2, this made RF=2 spuriously appear *faster* than RF=1 (a physical
impossibility for a replication cost). Full analysis: `docs/experiments/rf_throughput_warmup_artifact.md`.

To keep every baseline-vs-Embarcadero comparison fair, all matrix runs MUST:

1. **Discard the first (cold) trial** from the aggregate mean (raw trials preserved). Enforced by
   `aggregate_e2e_throughput.py --warmup-trials` (default 1), wired via `WARMUP_TRIALS` in
   `run_e2e_throughput_benchmark.sh`. Use `NUM_TRIALS ≥ WARMUP_TRIALS + 3`. The same rule applies to
   latency runs (drop the cold trial before computing percentiles).
2. **Interleave / randomize replication-factor and sequencer ordering** — never run all of one config
   then all of another back-to-back; the warm-up otherwise biases whichever config runs later.
3. For publication-grade confidence intervals, use an **exclusive host** (cgroups / exclusive
   scheduling). This is only needed to suppress the *secondary* ambient-contention scatter of the
   shared box (a concurrent session drove load 0.01 → 26 mid-probe); it is NOT required to obtain the
   correct RF *ordering*, which the warm-up discard + interleaving already restore.

RF-conditional throughput deltas measured WITHOUT (1) and (2) are not a reliable signal of replication
cost and must not be cited as such.
