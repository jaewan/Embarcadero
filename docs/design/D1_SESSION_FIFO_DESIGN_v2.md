# D1 Design Spec — Session-Based FIFO over Non-Coherent CXL (v2, FREEZE-READY)

**Status: FREEZE-READY v2 — all 14 adversarial-review must-fixes resolved inline. Ready for Track-02 TLA+ coordination and human sign-off.**
**Owner:** Track 01. Produced by the `d1-design` workflow (v1 draft → 5 cluster resolutions → this consolidated freeze). All `file:line` verified against `chore/repo-reorg` @ `7ec0e70`.

> **FIXED DECISION (design around it, do not relitigate):** D1 uses **ONE monotone per-session-global `batch_seq` per `(client_id, session_epoch)`**, decoupled from the current per-broker `order5_batch_seq_per_broker_` (`publisher.cc:2130`). `committed_hwm`, the resubmit suffix, dedup, and `next_expected` are ALL in this per-session-global seq space. This value is the **existing seal-time counter** `QueueBuffer::batch_seq_` (`queue_buffer.h:167`, stamped `queue_buffer.cc:223`) — no new client counter is introduced.

---

## Changes from v1 — must-fix → resolution map

Every one of the 14 must-fixes from the v1 gate (§"⚠ CRITICAL") is resolved inline below. This table maps each fix to its resolution and the governing section.

| # | Must-fix (v1) | Resolution in v2 | Section(s) |
|---|---|---|---|
| **1** | HWM axis bug: release/resubmit must be `committed_batch_seq = next_expected − 1` (batch_seq space), not `per_client_ordered_` (message count) | **RESOLVED.** Sole release/resubmit axis is `committed_batch_seq = next_expected − 1` in the per-session-global batch_seq space. `per_client_ordered_` demoted to diagnostic-only subscriber-delivery frontier; `committed_msg_hwm` is an OPTIONAL diagnostic field, never a release axis. | §1.2 (L1/L2), §4.1, §4.3 |
| **2** | Resubmit dedup reality: cross-broker rendezvous-retransmit gets a NEW `batch_id`, so `CheckAndInsertBatchId` does NOT dedup it; SOLE dedup is `seq < next_expected`; proof must reconstruct `next_expected ≥` every committed before classifying any resubmit (close recovery TOCTOU) | **RESOLVED.** Sole resubmit/rendezvous dedup is `seq < next_expected`. New **R1 reconstruct-before-accept invariant** (§6.2.1): on failover, `next_expected(S) ≥ max committed batch_seq(S)+1` established BEFORE any inbound batch for S is classified; inbound is PARKED until R1 completes. `batch_id` dedup demoted from "primary" to best-effort same-broker filter. | §1.2 (L1/L3), §3.2, §6.2.1 |
| **3** | Dedup window bounded: `FastDeduplicator` is 1M-entry LRU, not permanent | **RESOLVED.** Documented as bounded 1M-LRU (`RING_SIZE=1<<20`, `sequencer_utils.h:127`), best-effort same-broker physical-replay filter only. Durable guarantee is `next_expected` monotonicity. | §1.2 (L1), §3.2 |
| **4** | Fence gate must be a real per-session lease, not the disconnect/shutdown window; the `force_expire_all_frontiers` + flat 50 ms floor false-fences live clients | **RESOLVED.** Detector-armed fence DELETED. Fence predicate is a pure per-session lease on the head gap: fence iff the held entry at `next_expected` has `age_ns ≥ session_lease_ns`, evaluated every tick regardless of any detector. `hold_start_ns` is write-once (true gap age). Detectors retained only as tail-drain latency nudges, fully decoupled from the fence decision. | §3.3, §3.4 |
| **5** | Hold-buffer-full must not fence a live session | **RESOLVED.** The 4 hold-full force-skip tails DELETED and NOT replaced with `state.fence()` (v1 §3.3(2) reversed). Replaced with pure backpressure: spill-to-deferred, then throttle ingest / leave resident in the PBR ring (D3 emergent flow control). No session terminated by load; only the head-gap lease fences. | §3.3(2), §3.4 |
| **6** | Fence state + session table must be durable in CXL, not DRAM-only | **RESOLVED.** CXL `SessionEntry` table promoted to DURABLE TRUTH (not mirror) for `{expected_seq, committed_hwm, fenced, session_epoch, highest_sequenced}`; DRAM `ClientState5` becomes a write-through cache. Strict writer publish protocol with `state_word` written LAST as the release gate; reader invalidate+load_fence protocol. | §2.3, §3.3, §6.1 |
| **7** | TOCTOU: publish fence record + HWM atomically w.r.t. the reconnect read | **RESOLVED.** Reconnect answer derived ONLY from durable committed truth (SessionEntry via invalidate+load_fence), never DRAM `ClientState5.fenced`. `state_word` written after the data words it describes → a mid-flush reader sees whole-old or whole-new, never `!FENCED`-with-new-HWM or `FENCED`-with-stale-HWM. In-band `fenced_clients_hwm_` retained as v1-only fast path under `per_client_mu_`; SessionEntry authoritative on disagreement. | §2.3, §4.1, §4.2 |
| **8** | `session_epoch=0` / 16-bit-wrap aliasing must be disambiguated | **RESOLVED.** DRAM map and CXL table keyed on 64-bit `session_key = (client_id<<32) | full_32bit_session_epoch`, NEVER on the truncated 16-bit `BatchHeader.session_epoch` (which is a transport hint only). `epoch=0` reserved legacy (distinct code path). Ingest validates `header_low16 == (state_word.last_epoch32 & 0xFFFF)` for ACTIVE entries; mismatch → `EPOCH_STALE` reject. Full-32-bit handshake epoch is the tiebreak. | §2.2, §2.3, §3.2 |
| **9** | `batch_seq` is minted PER-BROKER, undefined for multi-home client | **RESOLVED (FIXED DECISION).** The per-session-global batch_seq is the EXISTING seal-time counter `QueueBuffer::batch_seq_` (single per-process `pubQue_`, `publisher.h:186`). For `kOrderStrong` the send path already does NOT reassign (`publisher.cc:2121-2125` is a deliberate no-op). The per-broker `order5_batch_seq_per_broker_` (`publisher.h:194`) is the ORDER=4 (`kOrderClientBrokerStream`) counter, left INTACT. Reset per session via `ResetBatchSeqForNewSession()` (quiesced-only). | §2.1, §2.2, §5.4 |
| **10** | Cumulative-ACK → batch_seq resolution missing (ACK wire is per-socket message count) | **RESOLVED.** Client maintains a local monotone prefix-sum map `batch_seq → cumulative_num_msg`; on each head-broker ACK `M` (message count), advance `released_through_seq` to the last batch fully covered by `M` via O(k) prefix-pop over `UnackedBuffer` ordered by batch_seq. This is the OPTIMISTIC frontier; AUTHORITATIVE axis stays `committed_hwm`. RTT sampled only when `attempt==0` (Karn). No wire change. | §4.1, §5.1, §5.5 |
| **11** | Rendezvous hash must mix batch_seq and provably exclude the dead broker | **RESOLVED.** `RendezvousBroker` computes HRW score `score_b = splitmix64_finalize(mix(client_id, batch_seq, b))` per candidate `b ∈ survivors, b ≠ exclude_broker`, argmax over the filtered set. Does NOT reuse the client_id-only home finalizer (`publisher.cc:323-327`). Dead broker structurally excluded; 1-survivor case deterministic; survivor set snapshotted once under `mutex_` for racing-duplicate determinism. | §5.3 |
| **12** | Re-derive δ from open-load append/ack p99.9, not steady 1.68 ms | **RESOLVED + MEASURED (2026-07-07).** `kDeltaFloorMs=1.7`/`kDeltaCapMs=12` derived from the OPEN-LOAD `append_send_to_ack` p99.9 offered-load sweep, NOT steady pacing (steady 1.68 ms only seeds the estimator at t=0). Measured worst-trial ack p99.9 = 4.95 ms; the ~83 ms open-load e2e tail is subscriber-side and does NOT track the ack path, so the earlier "≥166 ms" hypothesis is **RETIRED**. cap = 2×(worst ack p99.9 + wire) ≈ 12 ms. δ is a BACKSTOP (D2 NACK primary) so a larger cap errs safe. Measurement gate CLOSED — `delta_measurement_results.md`. | §5.2 |
| **13** | Bound unACKed buffer against failure stall (`offered_rate × session_lease`), specify producer backpressure + client lock order | **RESOLVED.** Buffer capped by BYTES = `offered_rate_bytes_per_sec × session_lease_seconds` (failure-stall envelope). On cap, `PublishThread` stops dequeuing from `pubQue_` (producer backpressure). Strict lock order `UnackedBuffer.mu → mutex_ → pubQue_`, never a send under `UnackedBuffer.mu`; absl thread-safety annotations at the 3 sites. | §5.1, §5.5 |
| **14** | ACK-relay epoch guard with a terminal bound to avoid infinite δ-retransmit | **RESOLVED.** `AckThread` epoch guard: each `per_client_ordered_` advance stamped with producing `ControlBlock.epoch`; before any ACK advance, withhold if `producing_epoch < ControlBlock.epoch`. Terminal bound: after a bounded withhold window (≥ session_lease) with `ControlBlock.epoch` advanced, emit terminal `SessionFenced{reason=EPOCH_STALE}` instead of withholding forever. | §6.4 |

**Cross-cluster reconciliations applied in v2:**
- **R1 uses MAX, not MIN, for accept-time `next_expected`** (§6.2.1): Cluster E chooses `next_expected(S) = MAX(GOI-scan+1, SessionEntry.expected_seq)` for accept-time correctness (never re-accept a committed batch). Cluster C's `min(GOI, SessionEntry)` rule is retained ONLY for the reconnect *answer* to the client (never claim committed beyond durable truth). These are two different questions (what to accept vs. what to tell the client) and both signs are conservative for their respective safety property. Documented at §6.2.1.
- **`session_epoch` must be persisted in the GOI** for recovery to disambiguate two generations of the same `client_id` (residual risk, Cluster A(4)): `GOIEntry._pad` (`cxl_datastructure.h:270`) absorbs a `uint32_t session_epoch` (additive, keeps 128B). Flagged to Cluster C/durability; see §2.3 and §8.

---

## Synthesized design (v2 — all fixes applied inline)

# D1 Design Spec — Session-Based FIFO over Non-Coherent CXL (Embarcadero)

*All `file:line` anchors verified against `chore/repo-reorg` @ `7ec0e70`.*

---

## 1. Overview + Guarantee Theorem

### 1.1 What D1 is

Embarcadero today gives per-session in-order commit with ACK-on-commit that **holds across epoch seals** (W1.2, `docs/experiments/w1_findings.md`). The *only* ordering violations are three deliberate **gap-skips** that advance a session's `next_expected` past a missing batch and emit the out-of-order suffix:

| Skip class | Site | Verified |
|---|---|---|
| EXPIRY gap-skip | `src/embarlet/topic.cc:5985` (`// Sequence is beyond next_expected - skip the gap`) | ✓ |
| HOLD-BUFFER-FULL forced skip (×4) | `topic.cc:5436`, `:5538`, `:5657`, `:5772` | ✓ |
| SCANNER TARGETED skip | `topic.cc:5342-5344` (`[[SKIP_MARKER_FILTER]]`, prelude at `:5321`) | ✓ |

**D1 replaces gap-skip with session-fencing.** When a gap at `next_expected` outlives a session's full lease, the sequencer *freezes* `next_expected` at the committed frontier, *drops* the uncommitted suffix (never emits it), and surfaces `SESSION_FENCED` + the committed high-water-mark (HWM) to the reconnecting client. The client opens a new session generation (`session_epoch+1`) and resubmits exactly the suffix `> committed_hwm`, yielding exactly-once across fencing with **no separate HWM query**.

**Two of the three skip classes change behavior differently in v2 (must-fix #5):**
- **EXPIRY** and **SCANNER** skips → **session-fence** (the terminal action).
- **HOLD-BUFFER-FULL** skips → **pure backpressure**, NEVER a fence (v1's "replace with `state.fence()`" is REVERSED). Load saturation is a shard-wide signal, not evidence THIS session's gap is unfillable.

### 1.2 Guarantee theorem (provable form)

> **Session-FIFO-with-Fencing (exactly-once across fencing).** For any client session `S = (client_id, session_epoch)`, the batches committed to the Global Order Index (GOI) under `S` — keyed by the **per-session-global `batch_seq`** (FIXED DECISION) — form a **gap-free, duplicate-free prefix** of the client's submission stream for that session. Late, duplicate, and cross-broker-retransmit arrivals for `S` are **rejected, never reordered and never re-emitted**. A session is *fenced* iff a gap at `next_expected` outlives the full session lease; upon fencing, no `batch_seq > committed_hwm` is ever committed under `S`. The union over the generation chain `S₁ ⊂ S₂ ⊂ …` (each `Sₖ₊₁` resubmitting exactly the suffix `> committed_hwm(Sₖ)`) is a gap-free, duplicate-free prefix of the client's global submission stream = **exactly-once ordering across fencing**. ACKs never lie across elections and liveness is bounded.

The theorem is provable from four lemmas (Cluster E):

- **L1 (single dedup):** for session `S`, a batch with per-session-global `batch_seq = seq` is committed iff `seq == next_expected(S)` at classify time; `seq < next_expected(S)` ⇒ rejected as late (never reordered, never re-emitted). This is the SOLE dedup on the resubmit/rendezvous path (must-fix #2). `batch_id` dedup is best-effort, same-broker, bounded-LRU, **non-authoritative** (must-fix #3): `batch_id = (broker_id<<48)|pbr_absolute_index` (`topic.cc:1714`, `:2198`) differs per broker, so a cross-broker retransmit produces a new `batch_id` (`FastDeduplicator` RING_SIZE=1<<20, `sequencer_utils.h:127`).
- **L2 (monotonicity):** `next_expected(S)` and `committed_hwm(S)` are non-decreasing; `fence()` is idempotent/monotonic (never un-fences, never lowers `committed_hwm`) [invariant §1.2.5]; `ControlBlock.committed_seq` is monotone (`cxl_datastructure.h:162`).
- **L3 (reconstruct-before-accept, R1):** on `S_new` (`ControlBlock.epoch++`, `topic.cc:97`), `next_expected(S) ≥ 1 + max committed batch_seq(S)` BEFORE any accept; inbound for `S` is PARKED until R1 completes (§6.2.1). No committed batch is ever re-accepted across failover (closes the recovery TOCTOU, must-fix #2).
- **L4 (ACK epoch validity):** an ACK for `S` is relayed only if its backing frontier's `producing_epoch ≥ ControlBlock.epoch`; a stale relay is withheld and, if unresolved within the lease, converted to a terminal `EPOCH_STALE` fence (must-fix #14). Hence ACKed ⇒ committed-under-the-current-epoch ⇒ eventually-visible, and withholding cannot loop forever.

**Underlying already-verified invariants:**

1. **In-order commit (W1.2):** `next_expected` advances *only* on in-order GOI commit. Verified: `topic.cc:5410` (EQ branch), `:4433` (`per_client_ordered_` advance in CommitEpoch). **Note (must-fix #1):** `per_client_ordered_` is a cumulative MESSAGE count (accumulated `per_client_delta_epoch[cid] += m.num_msg` at `topic.cc:4321/:4382`, folded `+= cnt` at `:4433`), NOT a batch_seq — it is the subscriber-delivery ACK frontier only, never the release/resubmit key.
2. **Assign-at-release (D4, W1.3):** held batches are placeholders (`is_held_marker`, `num_msg=0`) consuming **no** GOI/order index; the GOI index is assigned at commit/release, not at reservation.
3. **committed_seq monotonicity (spatial-guard input):** `ControlBlock.committed_seq` (`cxl_datastructure.h:159-162`) advances only over the gap-free contiguous GOI prefix, stored with `store_fence+flush_cacheline` by `CommittedSeqUpdaterThread`.
4. **Single-writer discipline:** all per-session DRAM state is mutated by exactly one shard/EpochSequencer worker; all CXL session state is single-writer (sequencer), 64B-TLP-atomic, monotonic.
5. **[NEW] Fence terminality:** `fence()` is idempotent and monotonic — a session never un-fences within its `session_epoch`, and `committed_hwm` never lowers. A fenced suffix NEVER resurfaces across GC/failover (§2.3 fence-terminality rules, §6.1).

---

## 2. Session Identity & CXL Session Table

### 2.1 Session identity = `(client_id, session_epoch)` and the per-session-global batch_seq

**`client_id`** stays the 32-bit random per-process value (`publisher.cc:265`, `GenerateRandomNum()`). **`session_epoch`** is a new **32-bit** client-owned counter:

- First connect: `session_epoch_ = 1` (0 reserved = "no session / legacy, never fenced").
- Reconnect, `SESSION_FENCED` receipt, or adaptive-δ failover to a new chain: `session_epoch_++` (monotonic per process, never reused, persists across broker reconnects within one process).

**The per-session-global `batch_seq` (FIXED DECISION, must-fix #9).** The per-session-global batch_seq is **not newly introduced on the send path** — it is the EXISTING seal-time counter:

- `Publisher` owns exactly ONE `QueueBuffer pubQue_` (`src/client/publisher.h:186`, constructed at `publisher.cc:273`). This single shared queue is why one seal counter suffices for the whole session (architectural anchor).
- `QueueBuffer::SealCurrentAndAdvance()` stamps `h->batch_seq = batch_seq_.fetch_add(1)` from a single per-QueueBuffer counter (`src/client/queue_buffer.cc:223`, counter declared `queue_buffer.h:167`). All N `PublishThread`s pull already-sealed batches from this one shared queue and merely route them to different broker sockets.
- **THEREFORE:** the seal-time `batch_seq_` IS the per-session-global monotone counter. The send path for `kOrderStrong` already refrains from reassigning it (`publisher.cc:2121-2125` is a deliberate no-op branch — CONFIRM it stays for D1).

**Do NOT touch `order5_batch_seq_per_broker_` (`publisher.h:194`).** Despite its name, the per-broker reassignment at `publisher.cc:2129-2132` belongs to `kOrderClientBrokerStream` (ORDER=4) and MUST be left intact for that order. The v1 defect (#9) is real only for ORDER=4; for D1 (`kOrderStrong`) the seal-time global seq is already correct and out of D1's mutation scope.

The wire ordering key becomes `(client_id, session_epoch, batch_seq)`. `batch_seq` **resets to 0 per session generation** — but ONLY via `QueueBuffer::ResetBatchSeqForNewSession()` (§5.4), which stores 0 into `batch_seq_` and **MUST be called only when the producer is quiesced** (SealAll drained, unACKed suffix re-stamped) since `batch_seq_` is single-producer. Never reset mid-flight. A reconnecting client's fresh `batch_seq=0` is accepted because it hashes to a fresh `ClientState5{next_expected=0}` (§3.2) under the new `session_epoch`.

### 2.2 Carriage — two additive, versioned changes

**(a) Handshake.** `EmbarcaderoReq` (`network_manager.h:33`, `alignas(64)`) is *not* Track-01-owned and is packed. **Do not widen it.** Session identity rides a new versioned protobuf `session.proto` (§4) on the gRPC handshake/redirect channel. The **authoritative 32-bit `session_epoch`** is established at handshake (`SessionOpen`) and held per-connection in `NetworkManager`. Old brokers ignoring `session_epoch` ⇒ treat as `epoch=0` legacy.

**(b) Per-batch.** Carry a **16-bit transport hint** of `session_epoch` in `BatchHeader` (Track-01-owned) by consuming the free `uint16_t _pad0` at `cxl_datastructure.h:415`:

```cpp
uint32_t flags;
uint16_t epoch_created;    // existing: control-block epoch (Phase-1a zombie fencing, D2) — DISTINCT concept
uint16_t _pad0;   -->  uint16_t session_epoch;   // NEW (low 16 bits of client session_epoch) — TRANSPORT HINT ONLY
```

This keeps `BatchHeader` at **128B** and `session_epoch` in the **first cache line** alongside `client_id`. Add `static_assert(offsetof(BatchHeader, session_epoch) < 64)` next to the existing asserts (`cxl_datastructure.h:442-446`).

**Mint vs. carriage (must-fix #9, Cluster A(2)):** The `batch_seq` is stamped at **seal** (`queue_buffer.cc:223`) and is NOT re-stamped for `kOrderStrong`. The `session_epoch` is stamped on the **send path** next to `client_id`, so retransmit re-stamp works. At `publisher.cc:2105`, alongside the existing `batch_header->client_id = client_id_`:

```cpp
batch_header->session_epoch = static_cast<uint16_t>(session_epoch_.load(std::memory_order_relaxed));
```

`BatchHeader.batch_seq` (`cxl_datastructure.h:407`) now holds the per-session-global value verbatim (documented as the `(client_id, session_epoch)` per-session-global value for `kOrderStrong`). `GOIEntry.client_seq` (`cxl_datastructure.h:261`, populated from `BatchHeader.batch_seq` at `topic.cc:4166/:4189`) durably records the per-session-global seq for recovery.

**16-bit wrap disambiguation (must-fix #8).** The in-header 16-bit `session_epoch` is only a **low-order transport hint**. The authoritative key is the **64-bit `session_key = (uint64(client_id)<<32) | full_32bit_session_epoch`** (§2.3, §3.2). On a 16-bit wrap, ingest validates `header_epoch_low16 == (state_word.last_epoch32 & 0xFFFF)` for an ACTIVE non-fenced `SessionEntry`; mismatch ⇒ `EPOCH_STALE` reject (never silently key into the wrong session). A wrapped generation only lands after the prior generation's `SessionEntry` is FENCED or GC-reclaimed, so the full-32-bit key never aliases.

> **Resolved contradiction (F2/F3 open-Q):** `session_epoch` is **distinct** from `epoch_created`. `epoch_created` = cluster/control-block fencing epoch (D2); `session_epoch` = client session generation (D1). They coexist in the first cache line.

### 2.3 CXL session table (new durable region, single-writer) — the DURABLE TRUTH

**Do not grow `ControlBlock`** (128B is the exact 2-cache-line prefetch contract, `cxl_datastructure.h:83`). The fixed low metadata plane (`ControlBlock@0x0 → CV@0x1000 → GOI@0x2000..16 GiB`, ending at `kPhase2MetadataEnd=0x4'0000'2000`) is packed — there is **NO** free slot for a 256 KB region there. **RESOLVED — Track-04 sign-off 2026-07-07:** place the session table as a **COMPUTED region in the dynamic plane, between `batchHeaders_` and `segments_`** in `cxl_manager.cc`, mirroring exactly how `batchHeaders_` is placed (deterministic from config ⇒ every broker computes the identical base ⇒ no cross-broker mismatch — the corruption risk). NOT a fixed hex `kSessionTableBase` (the low plane has no room). One 256 KB block/topic:

```cpp
// cxl_manager.cc, in the region-setup block (:290-327), inserted AFTER batchHeaders_ / BEFORE segments_:
//   session_table_ = base_for_regions_ + TInode_Region_size + Bitmap_Region_size + BatchHeaders_Region_size
//   SessionTable_Region_size = kMaxSessions * sizeof(SessionEntry) * MAX_TOPIC_SIZE   // 256KB*MAX_TOPIC_SIZE = 8 MB @ 32
//   segments_ = session_table_ + SessionTable_Region_size                             // rebased (was = batchHeaders_end)
//   Segment_Region_size -= SessionTable_Region_size                                   // + update the :310 "too small" check
//   GetSessionTable(topic_idx) = session_table_ + topic_idx * (kMaxSessions * sizeof(SessionEntry))  // 256 KB stride
constexpr size_t kMaxSessions      = 4096;   // per topic; 4096 × 64B = 256 KB/topic

struct alignas(64) SessionEntry {              // exactly 64B → one TLP-atomic cache line
    std::atomic<uint64_t> session_key{0};      // (client_id<<32)|full_32bit_session_epoch; 0 = free
    std::atomic<uint64_t> expected_seq{0};     // next per-session-global batch_seq to commit; monotone
    std::atomic<uint64_t> committed_hwm{0};    // == expected_seq-1 at publish; monotone; SESSION_FENCED payload
    std::atomic<uint64_t> highest_sequenced{0};// monotone
    std::atomic<uint64_t> state_word{0};       // [flags:8 | reserved:24 | last_epoch_full32:32]; bit0=FENCED bit1=ACTIVE. WRITTEN LAST = release gate.
    std::atomic<uint64_t> reserved0{0};
    uint8_t _pad[16];
};
static_assert(sizeof(SessionEntry) == 64);
```

**Role change from v1 (must-fix #6): DURABLE TRUTH, not a mirror.** The CXL `SessionEntry` table is the **DURABLE TRUTH** for per-session `{expected_seq, committed_hwm, fenced, session_epoch, highest_sequenced}`. The DRAM `ClientState5` (`OptimizedClientState`, `sequencer_utils.h:18`; via `topic.h:886 shard.client_state`) becomes a **write-through CACHE**. Every field is monotone; the FENCED bit only sets. Each entry is exactly 64B ⇒ one TLP-atomic line ⇒ a poll reader sees whole-old or whole-new, never torn.

- **Ownership:** the **sequencer is the single writer** for every field. Open-addressed by `hash(session_key) % kMaxSessions` with linear probe. `session_key` uses the **FULL 32-bit** `session_epoch`; `epoch=0` maps to a distinct legacy code path (no SessionEntry, no fence).
- **Replicated via the existing per-replica-topic pattern (D7), not per-offset mirroring** (Track-04 sign-off 2026-07-07): a replica topic already gets its own TInode hash slot (`GetReplicaTInode`, `cxl_manager.cc:512-517`), so its SessionTable lands in its own slot via the same computed formula (replica `topic_idx`). No special mirror needed.

**Writer publish protocol (must-fix #6, #7 — TOCTOU-safe).** Single-writer, at commit/fence time, batched **per-epoch-per-session inside `CommitEpoch`** (`topic.cc:4429-4435` per-client flush block), NOT per-batch (preserves the fast path):

```cpp
// 1. data words first
e.expected_seq.store(ne, relaxed);
e.committed_hwm.store(ne - 1, relaxed);
e.highest_sequenced.store(hs, relaxed);
CXL::store_fence(); CXL::flush_cacheline(&e); CXL::store_fence();
// 2. release gate LAST (carries FENCED|ACTIVE + full 32-bit last_epoch)
e.state_word.store((uint64_t(last_epoch32) << 32) | flags, release);  // bit0=FENCED bit1=ACTIVE
CXL::store_fence(); CXL::flush_cacheline(&e.state_word); CXL::store_fence();
```

`state_word` is the release gate: FENCED/ACTIVE become observable strictly after the numbers they describe are flushed.

**Reader protocol (non-coherent — failover reconstruction, replicas, brokers).** MUST invalidate before read:

```cpp
CXL::invalidate_cacheline_for_read(&e); CXL::load_fence();
uint64_t sw   = e.state_word.load(acquire);
bool     fenced = sw & FENCED;
uint64_t hwm  = e.committed_hwm.load(acquire);  // consistent: sw written LAST
```

Because `state_word` is written last, reading `FENCED=1` guarantees `committed_hwm` on the same line is the fence HWM (pattern already at `cxl_datastructure.h:738-743`). A client reconnecting mid-flush reads either (old whole line: `!FENCED`, old HWM) or (new whole line: `FENCED`, fence HWM) — never `!FENCED`-with-new-HWM or `FENCED`-with-stale-HWM.

**Fence terminality (must-fix #6, across GC/failover).** A fenced suffix NEVER resurfaces, enforced by three composed rules:
1. On fence, set FENCED in the durable `SessionEntry.state_word` **BEFORE** purging DRAM holds (`PurgeFencedSession`, §3.3) — durability precedes eviction.
2. `ClientGc` (`topic.cc:5177`) may evict the DRAM `ClientState5` cache line for a fenced session, but the `SessionEntry` retains key + FENCED + `committed_hwm` (`session_key` never transitions FENCED→free within its epoch).
3. On failover, `RecoverSequencer5State` reads the `SessionEntry` table and inherits FENCED; a stale-epoch resubmit hits the FENCED fast-path drop (§3.2 branch E) or `seq < next_expected` dedup.

**`session_epoch` persistence in GOI (cross-cluster reconciliation, Cluster A residual risk).** Recovery (§6.2) matches GOI entries by `(client_id, session_epoch)`, but `GOIEntry` (`cxl_datastructure.h:240-271`) currently stores `client_id + client_seq` and has NO `session_epoch` field. The 40B `_pad` at `GOIEntry:270` absorbs a `uint32_t session_epoch` (additive, keeps 128B). **This MUST be added** or recovery cannot disambiguate two generations of the same `client_id` in the GOI scan. Widen to ≥32 bits to hold the full epoch (the 16-bit header field cannot disambiguate a wrap).

---

## 3. Sequencer Hold-Layer: expected_seq, dedup, in-order commit, and SESSION-FENCING

### 3.1 Per-session fence state (new fields on `OptimizedClientState` — a write-through cache of SessionEntry)

`sequencer_utils.h:18` — single-writer, no lock. **This struct is now a write-through CACHE of the durable `SessionEntry` (§2.3).**

```cpp
struct OptimizedClientState {   // = ClientState5 (topic.h:95) — DRAM cache of durable SessionEntry
    uint64_t next_expected{0};        // existing (0-based) — per-session-global batch_seq space
    uint64_t highest_sequenced{0};    // existing
    uint64_t last_epoch{0};           // existing
    // ... existing 1024-window bitmap ...

    // --- NEW (D1) ---
    bool     fenced{false};
    uint64_t committed_hwm{0};   // == next_expected at fence time (per-session-global batch_seq space)
    uint64_t fence_epoch{0};     // current_epoch_for_hold_ at fence (GC/diagnostics)
    uint32_t session_epoch{0};   // 32-bit session generation this state serves

    void fence() {               // idempotent, monotonic — never lowers HWM, never un-fences
        if (!fenced) { fenced = true; committed_hwm = next_expected; }
    }
};
```

### 3.2 Dedup fast path (one comparison) + re-key on the FULL epoch

`Level5ShardState::client_state` (`topic.h:886`) and siblings (`hold_buffer` `:888`, `clients_with_held_batches` `:890`, `client_emitted_tracker`, `commit_order_last_seq_`) re-key from `client_id` to `session_key = (uint64(client_id)<<32) | full_32bit_session_epoch` (must-fix #8). `MakeClientBrokerStreamKey` (`topic.h:68`) folds the full `session_epoch` in for legacy per-stream paths. Legacy v0 (`epoch32==0`) keys as `(client_id<<32)|0` and is excluded from all fence logic; v1 keys with `epoch32≥1`. A v1 session and its own legacy v0 entry are distinct map slots — no collision.

The fast path in the S2-unified classifier (§7) is:

```cpp
const uint64_t session_key = (uint64_t(client_id) << 32) | session_epoch32;  // epoch32>=1 for v1; 0=legacy
ClientState5& st = shard.client_state[session_key];   // DRAM cache; new epoch → fresh {next_expected=0}
if (st.fenced) { CheckAndInsertBatchId(shard, batch_id); continue; }   // (E) drop for fenced session (cache hit)
// legacy (epoch32==0): skip all fence logic below.
if (seq < st.next_expected) { record_dup(); continue; }               // DEDUP: the SOLE authoritative compare (#2)
if (seq == st.next_expected) { emit; st.advance_next_expected(); }     // EQ
else { hold_or_fence; }                                                // GT (hold; fence only via §3.3 head-gap lease)
```

**Dedup is a single compare (must-fix #2, #3).** On the resubmit/rendezvous path the SOLE dedup is `seq < st.next_expected` in the per-session-global batch_seq space. `batch_id` dedup (`CheckAndInsertBatchId` → `FastDeduplicator.check_and_insert`, `sequencer_utils.h:146`) is DEMOTED: it filters only same-broker physical replays and is a bounded 1M-LRU (`RING_SIZE=1<<20`, `:127`); it MUST NOT be described as primary and MUST NOT be relied on for cross-broker retransmit dedup, because `batch_id = (broker_id<<48)|pbr_absolute_index` (`topic.cc:1714`) differs per broker. The 1024-window bitmap (`sequencer_utils.h:28-48`) is retained only as a secondary in-window dup guard.

The fenced-session drop (E) is one bool load on the fast path. Its durable authority is the `SessionEntry` (read on reconnect/failover, §2.3); the DRAM `fenced` bit is the hot-path cache.

### 3.3 SESSION-FENCING and BACKPRESSURE replacing the three gap-skips (exact functions)

**v2 scope split (must-fix #4, #5):** EXPIRY and SCANNER skips become **fence**; HOLD-BUFFER-FULL becomes **pure backpressure** (NOT fence — v1 §3.3(2) REVERSED).

Add to the shard worker:

```cpp
void PurgeFencedSession(Level5ShardState& shard, size_t session_key);  // (D)
// erases hold_buffer[key], hold_buffer_size -= n, clients_with_held_batches.erase(key)

void PublishSessionEntry(size_t session_key, uint32_t flags);          // durable writer protocol (§2.3), state_word LAST

auto record_fence = [&](size_t key){ order5_session_fences_.fetch_add(1);
                                     PublishSessionEntry(key, FENCED|ACTIVE);  // DURABLE before purge (fence terminality)
                                     PurgeFencedSession(shard, key); };
```

**(1) EXPIRY** `topic.cc:5984-5999`. The fence decision moves entirely into the per-head-gap lease predicate in the expiry loop (see §3.4). Delete the `// skip the gap` tail (`state.next_expected = ent.seq + 1; state.mark_sequenced(ent.seq); … ready.push_back`) and the old gap-skip expiry bodies (`topic.cc:5896-5999`): no gap-skip, no `next_expected` advance-past-gap, no `ready.push_back` for skipped entries. Replace with, at the lease-fire site:

```cpp
state.fence();                 // DRAM cache: fenced=true, committed_hwm=next_expected (idempotent, monotone)
record_fence(session_key);     // PublishSessionEntry(FENCED) durable, THEN PurgeFencedSession
// DROP the head gap: no ready.push_back, no MarkEmitted, no CheckAndInsertBatchId.
```

**(2) HOLD-BUFFER-FULL** `topic.cc:5436`, `:5538`, `:5657`, `:5772` — **DELETE the terminal force-skip; do NOT fence (must-fix #5).** The four sites already do graduated backpressure: on `hold_buffer_size >= kHoldBufferMaxEntries` (=100000) they `sleep_for(10us)` then spill into `deferred_level5` (bounded by `kDeferredL5MaxEntries = 2*kHoldBufferMaxEntries`, `topic.h:914`), force-skipping only when BOTH are full. D1 removes the terminal force-skip (`topic.cc:5435-5442` + 3 siblings) entirely, replaced with:
   - **(a)** keep spill-to-deferred as tier-1;
   - **(b)** when deferred is also full, do NOT skip — `break`/`return` the shard drain tick WITHOUT advancing `state` (leave the batch resident in the PBR ring). This converts hold saturation into ring fill = D3 emergent flow control (§6.2). **NO `state.fence()` at these sites.**
   - **(c)** the client sees stalled ACK advance and its D2 lease-driven NACK / delta-backstop (§5.2) throttles offered load. No session is terminated by load; only the head gap's own lease timer fences.

Rationale: saturation is a shard-wide load signal, not evidence THIS session's gap is unfillable; fencing on it would terminate a live session for a neighbor's load. "Rare under D3" is now a performance statement, not a safety argument.

**(3) SCANNER TARGETED skip** `topic.cc:5342-5344`. Replace the `if (batch_seq >= next_expected) next_expected = batch_seq+1` advance with `state.fence(); record_fence(session_key);` — BUT only when the head-gap lease has fired (§3.4). **CRITICAL — still push the marker:** the skip marker (`num_msg=0`, `is_held_marker` semantics) must still `ready.push_back(std::move(*skip_it))` to advance `consumed_through` (ring drain). It consumes no GOI index (W1.3). *Fence the session state; keep the marker emission* — dropping it re-introduces the `BrokerScannerWorker5` freeze class (`code_review_w1.md:156-188`). This preserves the scanner marker push for ring-drain even under backpressure.

### 3.4 Fence predicate — a genuine per-session lease (must-fix #4, #5)

**Detector-armed fence is DEAD (must-fix #4).** Delete the fence dependence on `force_expire_all_frontiers` and the flat `kForceExpireMinAgeNs = 50ms` floor (`topic.cc:5888-5890`, `:5899`) from the fence decision. The v1 gate `force_expire_all_frontiers && age_ns >= kForceExpireMinAgeNs` is exactly the false-fence hole: `force_expire_hold_until_ns_` is armed by three DETECTORS — disconnect (`RequestOrder5HoldExpiryOnce`, `topic.cc:2286-2290`; window 2.5s/90s), idle-stall (`run_idle_hold_tick` → `ArmOrder5ForceExpiryWindow`, `topic.cc:4613-4615`), and shutdown final drain (`topic.cc:4939`, 20ms) — so any arming event + a >50ms in-flight gap would fence a live client.

**Fence becomes a plain lease timer on the SPECIFIC gap.** New fence-clock config (separate from the steady hold timeout which stays 0):

```cpp
static uint64_t GetSessionLeaseNs(bool replicated) {
    // env EMBARCADERO_SESSION_LEASE_MS overrides; D3 provisioning inequality sets the ceiling; D2 sets the value.
    const uint64_t default_ms = replicated ? 2000ULL : 1000ULL;  // PLACEHOLDER pending D2
    return default_ms * 1'000'000ULL;
}
// GetOrder5HoldTimeoutNs() stays 0 (never age-skips) and is NOT the fence clock.
```

**Rewritten expiry-loop fence gate** (`topic.cc:5888-5907`), evaluated EVERY tick regardless of any detector:

```cpp
const uint64_t now_ns          = SteadyNowNs();
const uint64_t session_lease_ns = GetSessionLeaseNs(replication_factor_ > 0);
for (size_t key : shard.clients_with_held_batches) {
    auto map_it = shard.hold_buffer.find(key);
    if (map_it == shard.hold_buffer.end() || map_it->second.empty()) continue;
    ClientState5& st = shard.client_state[key];
    if (st.fenced) continue;                        // idempotent (§3.1)
    auto head = map_it->second.find(st.next_expected);
    if (head == map_it->second.end()) continue;     // head gap not yet resident -> nothing aged at next_expected
    const uint64_t gap_age_ns = now_ns - head->second.hold_start_ns;
    if (gap_age_ns >= session_lease_ns) {
        st.fence();                                 // committed_hwm = next_expected, monotone
        record_fence(key);                          // order5_session_fences_++; PublishSessionEntry(FENCED); PurgeFencedSession
    }
}
```

Fence is decided ONLY on the age of the head gap (the `seq == next_expected` slot's `hold_start_ns`), independent of any detector. Entries with `seq > next_expected` never trigger fence directly — they are collateral of the head gap and dropped by `PurgeFencedSession` once the head fences. Entries with `seq == next_expected` that are the awaited batch drain normally.

**`hold_start_ns` is the authoritative gap clock, write-once (must-fix #4).** Set exactly once at first hold-insert per `(session_key, seq)` (`topic.h:99`, stamped at hold-insert e.g. `topic.cc:5459`); NEVER overwritten on re-touch. On failover reconstruction (§6.2) a re-held slot sets `hold_start_ns = SteadyNowNs()` (conservative: ≤1 extra lease over-hold, never premature).

**`session_lease_ns` is a per-`(client_id, session_epoch)`-global lease** measured against the ONE per-session-global batch_seq gap head (FIXED DECISION): `next_expected` is in that seq space, so `map.find(st.next_expected)` is well-defined for a multi-home client. `committed_hwm` captured at fence is in the same per-session-global space (the `SESSION_FENCED` payload the client resubmits against).

**Detectors retained only as tail-drain latency nudges.** `force_expire_hold_until_ns_` / `RequestOrder5HoldExpiryOnce` (`topic.cc:2286`) / `run_idle_hold_tick` arming (`:4613-4615`) / shutdown drain (`:4939`) are RETAINED only to nudge the epoch-machine advance for tail-drain latency, **fully DECOUPLED from the fence decision** — no longer safety-load-bearing.

**Safety-never-depends-on-detector (CONFIRMED).** After #4, fence = `age_of_head_gap ≥ session_lease_ns` on a monotone `hold_start_ns` clock — a plain per-session lease timer that runs whether or not any disconnect/idle/shutdown detector fires. The D2 NACK / disconnect arming now only SHRINK the constant (fast recovery); if every detector is broken, the lease still fences the dead session after `session_lease_ns` — satisfying the theorem "a session is fenced iff a gap at `next_expected` outlives the full session lease."

**GOI/index invariance preserved:** held entries consume no GOI index (W1.3), so backpressure (not skip) does not stall `committed_seq` for other sessions already contiguous — only the saturated shard throttles.

### 3.5 Purge-on-fence subsumes the W1 residual

`code_review_w1.md:189-193,211-213` reports a held entry occasionally removed without commit (~2 batches, ~0.005%). That path *is* a gap-skip/expiry emitting an entry that should not have committed. Once §3.3 removes the EXPIRY/SCANNER gap-skip emission (and HOLD-FULL becomes backpressure), **the residual path no longer exists** — fencing subsumes it. No separate fix.

### 3.6 GC interaction (fence terminality)

`ClientGc` (`topic.cc:5177-5211`) evicts idle sessions after `kClientTtlEpochs`. A fenced session with an empty hold buffer is GC-eligible for its DRAM cache line; correct, because the durable `SessionEntry` retains key + FENCED + `committed_hwm` (`session_key` never transitions FENCED→free within its epoch, §2.3). Shard DRAM eviction is safe; the durable truth persists.

---

## 4. SESSION_FENCED Protocol + committed-HWM (protobuf, path)

### 4.1 The HWM source (must-fix #1, #7)

**Sole release/resubmit axis: `committed_batch_seq = next_expected − 1`** in the per-session-global batch_seq space (FIXED DECISION). `per_client_ordered_` / `GetClientOrdered` (`topic.h:418`, `topic.cc:1514-1517`, advanced only in `CommitEpoch` `:4433`) is a cumulative MESSAGE count (accumulated `per_client_delta_epoch[cid] += m.num_msg` at `:4321/:4382`), **NOT invertible to a batch_seq** — it is DROPPED as a release/resubmit axis and demoted to the subscriber-delivery ACK frontier plus an OPTIONAL diagnostic (`committed_msg_hwm`) in `SessionFenced`.

- `committed_batch_seq` = `next_expected - 1` (last strictly in-order committed batch → client resubmits `batch_seq > this`).
- `committed_msg_hwm` = `per_client_ordered_[client_id]` (cumulative committed message count) — DIAGNOSTIC ONLY; never a release axis.

**TOCTOU-safe derivation (must-fix #7).** The reconnect answer is derived ONLY from the durable committed truth. The reconnect handler (`NetworkManager::HandlePublishRequest`, `network_manager.cc:783`) computes `committed_batch_seq` and fenced-status by reading the durable `SessionEntry` (invalidate+load_fence, §2.3 reader protocol) — it does NOT consult DRAM `ClientState5.fenced`. Since `state_word` is the atomic release gate written after `expected_seq`/`committed_hwm`, a client reconnecting mid-flush reads either (old whole line: `!FENCED`, old HWM) or (new whole line: `FENCED`, fence HWM).

**Reconnect-answer HWM rule:** `committed_batch_seq = committed_hwm = expected_seq − 1` read from the `SessionEntry`. For the *reconnect answer to the client*, use the conservative `min(GOI-scan-derived, SessionEntry)` (never claim committed beyond durable truth). This is distinct from the *accept-time* `next_expected` reconstruction, which uses MAX (§6.2.1) — see the cross-cluster reconciliation note in "Changes from v1."

### 4.2 Head-side fence publication (v1-only fast path; SessionEntry authoritative)

```cpp
// topic.h (guarded by per_client_mu_)
absl::flat_hash_map<uint32_t, uint64_t> fenced_clients_hwm_ ABSL_GUARDED_BY(per_client_mu_);
struct FenceRecord { uint32_t session_epoch; uint16_t reason;
                     uint64_t committed_batch_seq; uint64_t committed_msg_hwm;  // committed_msg_hwm diagnostic-only
                     uint64_t control_epoch; bool delivered; };
bool     IsClientFenced(uint32_t client_id) const;
uint64_t GetClientFenceHwm(uint32_t client_id) const;
std::optional<FenceRecord> TakeSessionFence(uint32_t client_id);  // read-and-mark-once, analogous to GetClientOrdered
```

**Head-map consistency (must-fix #7).** Keep the head-side `fenced_clients_hwm_` / `TakeSessionFence` path as an in-band FAST-PATH optimization for already-connected v1 clients ONLY, published inside the SAME `per_client_mu_` critical section (`topic.cc:1515/:4431`) that flushes `per_client_ordered_`, snapshotted together with `ControlBlock.epoch`. But designate the durable `SessionEntry` (read on reconnect) as **AUTHORITATIVE**: if the in-band frame and the `SessionEntry` ever disagree, the `SessionEntry` wins. This eliminates the DRAM-only race for the reconnect path (the only path that gates exactly-once resubmit).

The shard worker publishes `cid + committed_hwm + FenceRecord` to the head map in the same per-client flush where `per_client_ordered_` is flushed (`topic.cc:6053-6055`), and (for the durable truth) `PublishSessionEntry(FENCED)` BEFORE `PurgeFencedSession` (§3.3).

### 4.3 Wire locus — protobuf on reconnect (primary) + capability-gated in-band (fallback)

**Primary = protobuf on the handshake/redirect channel.** The in-band ACK-sentinel is rejected as primary due to sentinel-collision and v0/v1 stream-desync hazards; retained only as a **capability-gated** fast-path optimization for already-connected v1 clients, never for legacy (`session_epoch=0`) clients.

New file **`src/protobuf/session.proto`** (additive + versioned):

```protobuf
syntax = "proto3";
package embarcadero;

message SessionOpen    { uint32 client_id = 1; uint32 session_epoch = 2;   // full 32-bit session_epoch (authoritative)
                         string topic = 3; uint32 requested_ack = 4; }
message SessionOpenAck { uint64 committed_hwm = 1; uint32 status = 2;       // committed_hwm = committed_batch_seq; status carries FENCED-on-reconnect
                         uint32 assigned_session_epoch = 3; }              // optional server-assigned generation
message SessionFenced  { uint64 client_id = 1; uint64 session_epoch = 2;   // full 32-bit
                         uint64 committed_batch_seq = 3;   // AUTHORITATIVE resubmit key: resubmit batch_seq > this
                         uint64 committed_msg_hwm  = 4;    // OPTIONAL DIAGNOSTIC ONLY (never a release axis, #1)
                         uint64 control_epoch      = 5;    // staleness/anti-replay (== ControlBlock.epoch at fence)
                         uint32 reason             = 6; }  // {HOLD_EXPIRY=1, EPOCH_STALE=2, LEASE_LOST=3, DUP_PAST_EXPECTED=4}
```

`SessionFenced.reason` gains **`EPOCH_STALE=2` as a first-class terminal reason** (must-fix #14). `committed_batch_seq` (per-session-global batch_seq) is the authoritative resubmit key; `committed_msg_hwm` is diagnostic-only. No existing wire message changed; the cumulative-ACK `size_t` wire is untouched.

**Path:**
1. A fenced-but-live client reconnects, presenting `(client_id, old_session_epoch)` in `SessionOpen`.
2. The broker reads the durable `SessionEntry` (invalidate+load_fence); if FENCED it replies with `SessionFenced` (or `SessionOpenAck{committed_hwm, status=FENCED}`) **before any ACK**.
3. The client reads `committed_batch_seq`, opens `(client_id, session_epoch+1)`, resubmits exactly the suffix `> committed_batch_seq` (§5.4).

The bare `size_t` cumulative ACK stream (`network_manager.cc:2480-2529`) is **unchanged** for normal ACKs.

> **Ordering constraint:** any in-band fence must be sent **strictly after** the last real ACK for `committed_batch_seq` on the same fd, else the client resubmits an already-committed batch (safe via dedup, but wasteful).

---

## 5. Client Library: unACKed buffer, adaptive δ, rendezvous survivor selection, resubmit-under-new-session

Today the publisher is fire-and-forget: `PublishThread` (`publisher.cc:1900-2358`) sends `BatchHeader`+payload then *immediately* `pubQue_.ReleaseBatch()` (`:2356`) — **no copy survives the socket write**, so retransmission is impossible. D1 adds four mechanisms in `publisher.{cc,h}` + `common.{cc,h}`. **D2's lease-driven NACK is the PRIMARY retransmit trigger; the δ timer is a BACKSTOP for silent loss.** Safety (no gap-skip) lives on the sequencer; the client only decides *when* and *where* to retransmit.

### 5.1 UnACKed buffer (byte-capped against the failure stall — must-fix #13)

```cpp
struct UnackedEntry {
    uint64_t batch_seq;                 // per-session-global (FIXED DECISION); rendezvous key component
    uint32_t num_msg;                   // cumulative-ACK release math (local prefix-sum ledger)
    int      cur_broker_id;
    uint8_t  attempt;                   // Karn: no RTT sample if >0
    steady_clock::time_point first_send_ts;   // RTT sample (first attempt only)
    steady_clock::time_point last_send_ts;    // δ backstop expiry
    void*    batch_copy; size_t copy_len;     // owned copy of BatchHeader+payload
};
struct UnackedBuffer {
    absl::Mutex mu;
    std::map<uint64_t, UnackedEntry> outstanding;   // ordered by per-session-global batch_seq
    uint64_t released_through_seq{0};               // batch_seq frontier (optimistic)
    uint64_t released_through_msg{0};               // head-ACK message frontier
};
```

**Lifecycle:**
- **Buffer-on-send** (replace `ReleaseBatch` at `publisher.cc:2356`): copy `sizeof(BatchHeader)+len` into a bounded pooled DRAM buffer, insert the entry, *then* `ReleaseBatch` the QueueBuffer slot. The copy is mandatory — the slot is reused immediately. Insert into `UnackedBuffer` **AFTER the send completes and BEFORE `pubQue_.ReleaseBatch`**, and must not hold `mutex_` while taking `UnackedBuffer.mu` (lock order, §5.5).
- **Release-on-ACK** (§5.5): pop entries whose cumulative-msg prefix ≤ the head-ACK message total; free their copies; Karn-gated RTT sample.
- **Bound = failure-stall envelope, NOT BDP (must-fix #13):** cap by **BYTES = `offered_rate_bytes_per_sec × session_lease_seconds`** (the failure-stall envelope, seconds — dominates BDP by orders of magnitude, since lease is seconds and RTT is ms). During a full session-lease stall the client keeps producing at offered_rate but releases nothing (no ACK advances while stalled awaiting fence). Byte cap (not count) because batch sizes vary. When the byte cap is reached, `PublishThread` applies PRODUCER BACKPRESSURE: it stops pulling from `pubQue_` and spins/yields (the client mirror of D3 emergent flow control) until release-on-ACK or fence frees bytes. No per-session credit window. **Assert at config load** that `(offered_rate × session_lease)` fits the client DRAM pool budget, else reduce `offered_rate` or `session_lease`.

### 5.2 Adaptive δ (TCP-RTO style) — derived from OPEN-LOAD tail (must-fix #12)

```cpp
struct DeltaEstimator { double srtt_ms{0}, rttvar_ms{0}; bool seeded{false};
                        std::atomic<double> delta_ms{kDeltaFloorMs}; };
// MEASURED (2026-07-07, docs/experiments/delta_measurement_results.md; open-load offered-load sweep,
// ORDER=5/ACK=1, append_send_to_ack p99.9 — NOT steady pacing, per must-fix #12):
//   kDeltaFloorMs = 1.7   (>= open-load TYPICAL p99.9 ~1.1 ms with margin; the steady 1.68 ms only
//                          seeds srtt/rttvar at t=0 — it is NOT the floor's justification, must-fix #12)
//   kDeltaCapMs   = 12.0  (>= 2x worst open-load append_send_to_ack p99.9 incl. real-wire +0.2 ms:
//                          2x(4.951+0.2)=10.30 ms; 12.0 clears it with honest slack)
// delta_ms = clamp(srtt + 4*rttvar, kDeltaFloorMs, kDeltaCapMs), α=1/8, β=1/4 (RFC6298), Karn sampling.
// NOTE: in the measured regime srtt+4*rttvar ≈ 0.3 ms clamps UP to the floor, so δ == floor == 1.7 ms
// in normal operation and the cap is essentially never the operative timer (see §5.2 floor-safety note).
```

**δ constants MUST be derived from OPEN-LOAD `append_send_to_ack` p99.9, not steady pacing (must-fix #12).** `w1_findings.md:222-223` states append/ack p99.9 = 1.68 ms, but line 224 explicitly annotates "(Note: this run used steady pacing; not comparable to the paper open-loop 83ms e2e figure)"; lines 158-160 give the OPEN-LOAD sweep e2e p99 = 82888.8µs (~82.9 ms) at 1000 MB/s. Since the v1 `kDeltaCapMs=50` is BELOW the open-load ack tail (~82.9 ms), a spurious backstop would fire on every open-load tail batch. Resolution:
- **(B, then A) Recommended:** measure open-load `append_send_to_ack` p99.9 first (the `stage_latency_summary.csv` `append_send_to_ack` stage under the offered-load sweep, NOT steady pacing), set `floor ≈ that p99.9`, `cap ≈ 2–3× it`. Alternatively **(A)** raise `kDeltaCapMs` strictly above the measured open-load append/ack p99.9 with headroom (≥2×). **Now measured (2026-07-07): ack p99.9 = 4.95 ms worst-trial and does NOT track e2e → `cap = 12 ms`; the earlier "≥166 ms if the ack tail tracks e2e" conditional is RETIRED.** See the gate-CLOSED note below.
- The steady 1.68 ms may only SEED the estimator, never bound the cap.

**δ is a BACKSTOP, not the primary trigger.** D2 lease-driven NACK is PRIMARY (~lease time, ms); δ is only the silent-loss backstop. An over-large cap costs only backstop latency on genuinely-lost batches, not correctness (safety lives on the sequencer). So erring toward a LARGER cap is safe; erring small causes the spurious-retransmit storm (`w1_findings.md:200-202`).

- **Sample source (Karn):** on release of a first-attempt batch (`attempt==0`), `R = now − first_send_ts`. NEVER sample when `attempt>0`. Update co-located with the release fold (`publisher.cc:1847-1853`) — no new syscalls.
- `delta_ms = clamp(srtt + 4·rttvar, floor, cap)`.
- **New `RetransmitThread`** (started in `Init()` near `publisher.cc:758`): every `min(δ/2, 2ms)` scans `UnackedBuffer`; for any entry past `last_send_ts + backoff(δ, attempt)` (exponential `δ·2^attempt`) not superseded by a D2 NACK, triggers retransmit. Silent-loss backstop only; the D2 NACK path calls the same routine immediately.

**Pre-freeze measurement gate: CLOSED (2026-07-07).** The offered-load sweep was run on the S2
tree (loopback open-loop, ORDER=5/ACK=1, 1000–8000 MB/s, 3 trials/point —
`docs/experiments/delta_measurement_results.md`). Measured open-load `append_send_to_ack`
p99.9 ≈ 1.0–1.1 ms typical, **4.95 ms worst single trial** at operating load (knee onset ~8000 MB/s;
achieved tracks target through 6000). The feared "ack tail tracks the ~83 ms e2e tail ⇒ cap
≥166 ms" scenario is **REFUTED** — the e2e tail is subscriber-side (`w1_findings.md` §W1.1), not on
the publisher ACK path. Constants frozen: `kDeltaFloorMs = 1.7`, `kDeltaCapMs = 12` (≥2× the worst
measured tail including the +0.2 ms real-wire adder — 2×(4.951+0.2)=10.30 ms — so 12 clears it with
honest slack, unlike a bare 10 which is only 1.01× its own basis). `kDeltaCapMs < session_lease_ms`
holds against the current 1000–2000 ms placeholder — see §9 residual #1 for the D2-owned lease
constraint. Revisit only if E-matrix real-wire runs show a larger send→ack p99.9 (cap then = 2× that;
a larger cap errs safe — δ is a NACK-backed, idempotent backstop).

**Floor-safety — the operative timer is the FLOOR, not the cap.** With the measured stream
(p50 ≈ 0.3 ms), `srtt + 4·rttvar` sits well below the floor, so in normal operation **δ = floor =
1.7 ms** and the cap is reached only transiently; the cap value is therefore not on the correctness
path. The floor (1.7 ms) sits *below* the worst-trial p99.9 (4.95 ms), so in a bad trial ≲0.1 % of
batches see one *spurious* first retransmit — exactly the idempotent, deduped (`per-session
batch_seq` at the sequencer), exponentially-backed-off case the design tolerates (≈5 duplicate 1 KiB
sends per stalled batch, all deduped; bounded bandwidth, never a correctness event). Because a
retransmitted (slow) batch ACKs with `attempt>0`, Karn suppresses its RTT sample, so srtt/rttvar
stay biased low and **δ effectively equals the floor by design** — the floor, not the estimator, is
the operative timer. Accepted for a backstop sitting behind the D2 NACK primary.

### 5.3 Rendezvous survivor selection (must-fix #11)

```cpp
// common.{cc,h} — reusable
int RendezvousBroker(uint32_t client_id, uint64_t batch_seq,
                     const std::vector<int>& survivors, int exclude_broker);
// score_b = splitmix64_finalize(mix(client_id, batch_seq, b)) for b in survivors, b != exclude_broker; return argmax_b
```

**Do NOT reuse the ORDER=5 home finalizer (must-fix #11).** The v1 instruction to "reuse the same splitmix64 finalizer as ORDER=5 home selection (`publisher.cc:323-327`)" is the BUG the must-fix names: that finalizer builds `mixed_client` from `client_id_` ALONE (`publisher.cc:323-327`) then does `mixed_client % brokers_.size()` (`:330`) over ALL brokers, not survivors — yielding herd COLLISION (all of one client's batches map to the same index). `RendezvousBroker` instead:
- **(a)** folds `batch_seq` into the hash per candidate broker (HRW/rendezvous scoring, not modulo-of-one-hash);
- **(b)** iterates only over the `survivors` set with `exclude_broker` filtered out;
- **(c)** argmaxes the per-broker score.

This gives herd SPREAD (consecutive `batch_seq` of one client score-differently per broker) instead of the client_id-only collision. `batch_seq` is the per-session-global seq (FIXED DECISION).

**Dead-broker exclusion (proof).** With `survivors` = live set and `exclude_broker` = the failed/suspected broker, `RendezvousBroker` scores only `b ∈ survivors, b ≠ exclude_broker` and returns argmax over that filtered set, so the dead broker is structurally never in the candidate set. **2-broker/1-survivor case:** `survivors={live}`, `exclude={dead}` ⇒ candidate set has exactly one element ⇒ returns the survivor deterministically, never the dead one. **Empty candidate set** (survivors → 0): fall back to stall→fence bounded by δ, **never gap-skip**.

**Determinism across a racing duplicate.** `score_b` is a pure function of the immutable tuple `(client_id, batch_seq, broker_id)` plus the survivor set. Two racing retransmits of the SAME `(client_id, session_epoch, batch_seq)` with the same survivor snapshot compute the identical argmax ⇒ land on the same broker. Because `(client_id, session_epoch, batch_seq)` is stable across normal retransmit (§5.4: `batch_seq` is only reset on `SESSION_FENCED` under a new `session_epoch`), the sequencer's `seq < next_expected` dedup collapses the duplicate. **REQUIREMENT:** the survivor set must be a **stable snapshot per retransmit decision (read once under `mutex_`)** so both racers see the same candidate set; membership churn is the only source of divergence, bounded by the ≥2-live-socket invariant.

**Replaces** the round-robin reroute at `publisher.cc:2287` (`brokers_[(pubQuesIdx % num_threads_per_broker_) % brokers_.size()]`). Retransmit: rewrite `batch_header->broker_id = nb`, re-send `batch_copy`, `++attempt`, `last_send_ts=now`, **no RTT resample** (Karn). **Invariant:** maintain ≥2 live broker sockets whenever the cluster has ≥2 brokers (shared with D2). The ORDER=5 home finalizer (`publisher.cc:323-327` / `:330`) is left as-is for home selection and is explicitly NOT the rendezvous survivor path (add a comment cross-reference so it is not conflated).

### 5.4 Resubmit-under-new-session on SESSION_FENCED

On `SessionFenced{committed_batch_seq}`:
1. Stop treating the current session as live; drain the producer (SealAll drained); re-stamp the unACKed suffix; then call `pubQue_.ResetBatchSeqForNewSession()` and `session_epoch_++` (the SINGLE point where `batch_seq` resets to 0 — quiesced-only, single-producer safe, must-fix #9). Re-handshake warm connections under the new epoch.
2. From `UnackedBuffer`, release entries `≤ committed_batch_seq` (committed, exactly-once done); take exactly the suffix `> committed_batch_seq`.
3. Re-stamp each suffix entry: new contiguous `batch_seq'` from 0 under `session_epoch'` **preserving submission order** (map old→new so ACK reconciliation stays correct); **keep `num_msg` attached to the LOGICAL batch, not the physical send** (so the ACK→batch_seq frontier computation, §5.5, does not drift). Rewrite the copy's `BatchHeader` (`client_id` unchanged, `session_epoch'`, `batch_seq'`), retransmit via rendezvous.

This is the **only** place the client resets `batch_seq`. Normal retransmit (§5.1-5.3) keeps `(client_id, session_epoch, batch_seq)` stable so the sequencer's `seq < next_expected` dedup drops duplicates — the client never causes reorder.

> **`session_epoch` minting:** minted **client-local monotonic** for v1 (simplest, no round-trip on open); a server-assigned epoch in `SessionOpenAck.assigned_session_epoch` is a future option if the session table needs authoritative generation numbers.

### 5.5 ACK → batch_seq frontier + client lock order (must-fix #10, #13)

**ACK → batch_seq resolution (must-fix #10).** The ACK wire stays a cumulative per-SOCKET message COUNT (`publisher.cc:1818` `acked_msg = partial.first`, folded at `:1847-1853`), carrying no batch_seq — and `per_client_ordered_` (message count) is NOT invertible to a batch_seq (Cluster A(3)/E #1). For ORDER=5 there is exactly ONE authoritative session-global ACK stream: the head broker `broker_id_==0` emits `GetClientOrdered(client_id)`; every non-head broker sends sentinel `(size_t)-1` (`network_manager.cc:2333-2358`), which the client's `AckThread` already drops via `acked_msg >= prev_acked` (`publisher.cc:1834, 1851-1853`). No N-socket reassembly is required.

Resolution — the client maintains a local monotone prefix-sum ledger and derives the batch_seq frontier from it plus its own `num_msg`:

```cpp
// On head-broker ACK M (monotone message count, publisher.cc:1834), under UnackedBuffer.mu ONLY:
//   walk `outstanding` in batch_seq order, accumulating running_msg_total += e.num_msg;
//   advance released_through_seq to the LAST batch fully covered by M
//     (pop entries in batch_seq order while running_msg_total <= M);
//   for each popped e:
//     free e.batch_copy;
//     if (e.attempt == 0) sample RTT R = now - e.first_send_ts and feed DeltaEstimator;  // Karn
//     else skip RTT (attempt>0);                                                          // Karn suppression
//   committed_batch_seq (optimistic release/suffix) := released_through_seq;
```

This is O(k) prefix-pop over the `std::map` ordered by batch_seq; no protobuf/wire change. **This local mapping is the OPTIMISTIC frontier only.** The AUTHORITATIVE release/resubmit axis is `committed_batch_seq = committed_hwm` (= `next_expected − 1`, §4.1) delivered in `SessionFenced`/`SessionOpenAck`. On any fence or reconnect the client releases entries `≤ committed_batch_seq` and resubmits the suffix `> committed_batch_seq` (§5.4). The per-socket message-count ACK only advances the optimistic frontier between fences; it never governs exactly-once.

**Head-broker failover note:** if broker 0 fails, the client loses its message-count frontier until a new head is elected. The client MUST treat a head-socket gap as "withhold release," not "assume committed" (ties to must-fix #14 ACK-relay epoch guard). Release/RTT stall is bounded by δ/lease and is acceptable for safety.

**Client lock order to avoid the retransmit/ACK-release deadlock (must-fix #13).** The deadlock is real: the reroute path takes `mutex_` at `publisher.cc:2270` and again at `:2313` while about to re-send and eventually touch `pubQue_` (`:2356`); the `AckThread` release fold (`:1847-1853`) must take `UnackedBuffer.mu` to pop released entries; a retransmit may need `mutex_` (survivor read for `RendezvousBroker`) and `pubQue_`. Impose a **strict global lock order `UnackedBuffer.mu → mutex_ → pubQue_`** (the internal `QueueBuffer` mutexes at `queue_buffer.h:147/199`):
1. The `RetransmitThread`/NACK handler acquires `UnackedBuffer.mu` first, snapshots the entries + survivor-need, RELEASES `UnackedBuffer.mu`, then acquires `mutex_` to read the survivor set for `RendezvousBroker`, then does the socket send (no lock) — NEVER hold `UnackedBuffer.mu` across a send or across a `mutex_` acquisition that could re-enter release.
2. The `AckThread` release fold acquires `UnackedBuffer.mu` ONLY, never `mutex_` or `pubQue_` while holding it.
3. Buffer-on-send inserts into `UnackedBuffer` (`UnackedBuffer.mu`) AFTER the send completes and BEFORE `pubQue_.ReleaseBatch`, and must not hold `mutex_` while taking `UnackedBuffer.mu`.

Add `ABSL_GUARDED_BY` / lock-order annotations (absl thread-safety analysis) at `publisher.cc:1847-1853`, `2270/2313`, `2356`, mirroring the existing `ABSL_GUARDED_BY(mutex_)` usage in `publisher.h`. Note: the `QueueBuffer` internal mutexes are `std::mutex` (not `absl::Mutex`) and may not be annotated — a manual audit of the retransmit path is still required to prove no send occurs under `UnackedBuffer.mu`.

---

## 6. Failover / Durability (D3 interplay) + D4 spatial-guard & stale-CV (Track 02)

### 6.1 Durability split

| Plane | State |
|---|---|
| **CXL (DURABLE TRUTH, single-writer, monotonic, flushed)** | `ControlBlock.{epoch, sequencer_id, committed_seq}`; GOI entries (`client_id, client_seq, session_epoch [NEW, §2.3], broker_id, total_order, epoch_sequenced`); PBR `BatchHeader`; `TInode.offsets[b].{ordered, batch_headers_consumed_through}`; CV; **`SessionEntry` table (§2.3) — now the DURABLE TRUTH for per-session `{expected_seq, committed_hwm, fenced, session_epoch, highest_sequenced}`** |
| **DRAM (volatile CACHE, reconstructible)** | hold buffer (`HoldEntry5`/`HoldBatchMetadata`), `per_client_ordered_` (message count, diagnostic), **write-through cache `ClientState5` (`next_expected`, `fenced`, `committed_hwm`, window) of `SessionEntry`**, completed-ranges ring, `fenced_clients_hwm_` (v1-only fast path), `hold_start_ns` (gap clock) |

### 6.2 D3 (commit-driven PBR) — the reconstruction prerequisite

Today PBR reclamation is **scan-driven** (W1.4): `AdvanceConsumedThroughForProcessedSlots` (`topic.cc:4479`) advances `batch_headers_consumed_through` for **all** processed slots including held/skipped, so a held-but-uncommitted batch is **not reconstructible after failover**. D3 changes this:

- In `CommitEpoch`, **do not** advance `batch_headers_consumed_through` over `is_held_marker` slots — build a held-slot exclusion set and skip them in the tinode write-back (`topic.cc:4485-4489`). Keep advancing committed + dedup-dropped slots for ring-drain liveness. **Backpressure (§3.3(2)) also leaves the head-gap batch resident in the PBR ring**, so it is reconstructible.
- Move `InvalidateOrder5HeldSlot` (writes `flags=kBatchHeaderFlagRetired`, `topic.cc:173`) from **hold-insert time** to **drain/commit time**, so a held slot stays a *valid published slot*. `kBatchHeaderFlagRetired` is set only at commit.
- Delete per-session credit windows; backpressure emerges from ring fill. **Assert `session_lease_ns < PBR_fill_time` at config load** — else a fenced-timescale stall couples other sessions on the ring (the D3 fairness coupling). **Config-load inequality: `kDeltaCapMs (client backstop) < session_lease_ms < PBR_fill_time_ms`, and `session_lease_ns ≥ open-load append/ack p99.9` (must-fix #12)** so a live-but-slow session is never fenced.

**`RecoverSequencer5State()`** (new, near `topic.cc:3597-3606`) — reconstructs per-session `next_expected` in batch_seq space at `S_new` WITHOUT trusting DRAM (must-fix #2, #6):
1. Read `ControlBlock.committed_seq`; scan `GOI[0..committed_seq]`, and for each entry matching `session_key=(client_id<<32)|session_epoch` (session_epoch read from the GOI, §2.3 — MUST be persisted) take `max(client_seq)+1` as the GOI-derived next_expected. This rebuilds `per_client_ordered_` (message count) as the authoritative ACKed==committed frontier; never re-ACK below it.
2. Read the CXL `SessionEntry` (invalidate+load_fence) → per-session `expected_seq` + FENCED bit; inherit FENCED.
3. Establish `next_expected(S)`. **Two rules for two questions (cross-cluster reconciliation):**
   - **Accept-time `next_expected` (R1, §6.2.1): `next_expected = MAX(GOI-derived, SessionEntry.expected_seq)`** — conservative for NOT re-accepting an already-committed batch. `committed_hwm = next_expected − 1` in batch_seq space (drop `per_client_ordered_` as a release axis, #1).
   - **Reconnect-answer HWM to the client (§4.1): `min(GOI-derived, SessionEntry.expected_seq)`** — conservative for NOT claiming committed beyond durable truth.
4. Rescan each PBR ring from the per-broker committed frontier; re-classify resident slots vs reconstructed `next_expected` (`==`→ready, `>`→re-hold with `hold_start_ns = now`, `<`→dedup). Because D3 keeps uncommitted held slots resident, the hold buffer is **fully reconstructible** and **no held batch is lost**.

Dedup after recovery is the single compare `seq < next_expected` (#2). A fenced session's `SessionEntry.state_word` FENCED bit makes `S_new` reject any `batch_seq > committed_hwm` under the old `session_epoch`.

**Purge-on-fence is safe across failover:** a fenced suffix is not resurrected because `S_new` rescans from the committed frontier and the `SessionEntry` records FENCED/`committed_hwm` → `S_new` rejects the fenced session's stale-epoch resubmission via (E) drop + `seq < next_expected` dedup.

### 6.2.1 Recovery ordering invariant R1 — reconstruct-before-accept (must-fix #2)

**R1 (reconstruct-before-accept):** On failover/election producing `S_new` (`ControlBlock.epoch++`, `topic.cc:97`), for EVERY session `S` the sequencer MUST reconstruct `next_expected(S)` to a value `≥ (max committed batch_seq(S) in the durable GOI prefix) + 1` BEFORE classifying ANY inbound batch for `S`. Reconstruction = `MAX` over: (a) `GOI[0..ControlBlock.committed_seq]` scan for `S` (authoritative), (b) the CXL `SessionEntry.expected_seq` (§2.3). Until R1 completes for `S`, inbound batches for `S` are **PARKED** (not classified, not committed, not dedup-dropped). This makes the sole `seq < next_expected` compare sound at the instant it first runs, closing the recovery TOCTOU before any resubmit is classified.

The `MAX` here (accept-time) intentionally differs from the `min` used for the reconnect *answer* to the client (§4.1): `MAX` is conservative for not re-accepting; `min` is conservative for not claiming committed beyond durable truth. Both are safe for their respective properties. **Cross-cluster sign-off required (flagged in residual risks).**

### 6.3 D4 spatial-guard duplicate at S_new (Track-02 Scenario A)

Scenario: after suspected `S_old` failure, a client rendezvous-retransmits batch `n` (SAME `(client_id, session_epoch, batch_seq)`, NEW `batch_id` because a different broker) to a different broker; `S_new` is elected (`epoch++`); the retransmit arrives while `committed_seq` is catching up. **Three composed rejections** (D4 residual = one audited guard in the S2-unified `classify()`):

1. **Dedup (primary):** by R1 (§6.2.1), `S_new` reconstructs `next_expected(S) ≥ max committed + 1` BEFORE classifying. `n.batch_seq < next_expected` ⇒ already committed ⇒ rejected as late. **Model dedup alone (`seq < next_expected`) as sufficient for safety; `batch_id` dedup is NOT a safety primitive** (it filters only same-broker replays, bounded LRU).
2. **committed_seq monotonicity:** a duplicate cannot lower `committed_seq`; it yields at most a stale/overlapping `CompletedRange` popped by `CommittedSeqUpdaterThread` (`topic.cc:3132-3136`). Guard input never regresses.
3. **Wrap-fence (liveness-only):** `n.epoch_created < ControlBlock.epoch` (minted under `S_old`); GOI reclamation is gated by the wrap-fence (D2/D6); `epoch_sequenced` on the GOI entry lets readers discard cross-epoch stragglers.

**D4 residual code:** in `classify()`, before assigning a GOI index to a from-scanner batch, assert `(a) epoch_created not below the active epoch floor OR route-to-dedup`, and `(b) batch_seq >= reconstructed next_expected`. Add `order5_spatial_guard_rejects_`. One comparison on the fast path, no new atomics. Because dropping a fenced suffix consumes **no** GOI index (W1.3), `committed_seq` stays monotonic.

### 6.4 Stale-CV ACK relay after election (Track-02 Scenario B, D2) — ACK-relay epoch guard (must-fix #14)

**Gap confirmed:** the only epoch validation (`CheckEpochOnce`/`RefreshBrokerEpochFromCXL`, `topic.cc:1931-1937`, `:1641-1655`) is on the **ingest** path (`network_manager.cc:944`). The **ACK-relay** path (`AckThread`, `network_manager.cc:2330`, `:2337`, `:2353`) reads `GetClientOrdered` with **no** control-block epoch check → a post-election stale sequencer could relay an ACK for an entry a later epoch discards.

**Fix (with a terminal bound to avoid infinite δ-retransmit):**
1. **Epoch-stamp:** extend the per-client ordered map so each advance records `producing_epoch`. Inside `per_client_mu_` (`topic.cc:4431-4435`), store the cached `ControlBlock.epoch` alongside `per_client_ordered_[cid] += cnt` (`:4433`). The stamp write must be atomic with the count advance under `per_client_mu_`.
2. **Guard:** add `Topic::AckRelayEpochValid(uint32_t client_id)` reading `ControlBlock.epoch` on the `kEpochCheckInterval` cadence (`topic.h:832`; flush_cacheline+load_fence, mirroring `RefreshBrokerEpochFromCXL`). In `AckThread`, before each advance at `network_manager.cc:2330` (`use_per_client_ack_level1`), `:2337` (order5 ACK1 head), and `:2353` (order5 ACK2 head), require `producing_epoch(client_id) >= ControlBlock.epoch`; otherwise **WITHHOLD** (do not send this ACK; the client δ/lease-NACK path retransmits). Preserves "ACKed iff committed."
3. **Terminal bound (liveness):** track `withhold_start` per `(client_id)`. If withholding persists past a bounded window (`≥ session_lease`, coordinated with D2) AND `ControlBlock.epoch > this node's cached producing epoch`, escalate to terminal: deliver `SessionFenced{reason=EPOCH_STALE(=2), control_epoch=ControlBlock.epoch, committed_batch_seq=next_expected-1}` via the reconnect/redirect channel (`session.proto`). The client re-sessions (`session_epoch+1`) and resubmits the suffix `> committed_batch_seq`. This bounds δ-retransmit and preserves ACKed-iff-committed.

### 6.5 Track-02 TLA⁺ deliverables (coordinate-before-finalizing gate)

Two models must be finalized **before** the D3 / D4-residual / D2-ACK-guard code merges:

- **Scenario A (retransmitted duplicate at `S_new`):** State `{epoch, committedSeq, seqId}`, per-client `nextExpected/committedHWM` (in per-session-global batch_seq space), durable GOI, inflight msgs. Actions `CommitBatch` (only if `s==nextExpected`), `SequencerFail` (`epoch++`, Recover establishing R1), `Retransmit` (cross-broker: SAME seq, NEW `batch_id`), `ClassifyAtSnew`. **Invariants:** `committedSeq` monotone; `NoDuplicateCommit`; `AckedIffCommitted`; **`ReconstructBeforeAccept` (`nextExpected(S) >= maxCommitted(S)+1` holds at every `ClassifyAtSnew`)**. Model dedup (`seq < nextExpected`) alone as sufficient for safety; `batch_id` dedup NOT modeled as a safety primitive; wrap-fence liveness-only.
- **Scenario B (stale-CV ACK relay after election):** State `{epoch, orderedFrontier[c] stamped producingEpoch, brokerRelayedAck[c], withholdStart[c]}`. **Unguarded `RelayAck` must VIOLATE `AckedImpliesEventuallyVisible`.** Guarded `RelayAck` (relay only if `producingEpoch >= controlBlock.epoch`, else withhold; withhold bounded → terminal `EPOCH_STALE` fence) must **SATISFY** both `AckedImpliesEventuallyVisible` and `NoInfiniteWithhold` (liveness).

---

## 7. S2 Refactor (landing prerequisite) + Implementation Sequencing

The seq-classification decision tree is **duplicated across 4 inner loops** in `ProcessLevel5BatchesShard` (`topic.cc:5255-6230`), all under `true_client_chain = UsesTrueClientChainOrdering(order_)` (`:5258`). For ORDER=5 only L1/L3 execute:

| Loop | Site | Key |
|---|---|---|
| L1 fast/true_chain | `topic.cc:5393` (`for (PendingBatch5* p : ordered)`) | client_id → session_key |
| L2 fast/legacy | `topic.cc:5500` (`for (PendingBatch5* p : vec)`) | stream_key |
| L3 general/true_chain | `topic.cc:5612` (`for (auto jt = it; jt != group_end; ++jt)`) | client_id → session_key |
| L4 general/legacy | `topic.cc:5723` (`for (auto jt = bit; jt != broker_end; ++jt)`) | stream_key |

**S2 = extract one classifier** so D1 lands in one place instead of four.

```cpp
// topic.h (private Topic members)
enum class L5Class { kDupTerminal, kEmittedTerminal, kEmit, kLate, kBackpressure, kHold, kFenced };
static L5Class ClassifyLevel5(const ClientState5& st, const ClientEmitTracker& em,
                              uint64_t seq, size_t hold_buffer_size);   // pure, no side effects
void ApplyLevel5Record(Level5ShardState& shard, ClientState5& st, ClientEmitTracker& em,
                       uint64_t key, bool true_client_chain, PendingBatch5* p,
                       std::vector<PendingBatch5>& ready,
                       absl::flat_hash_map<uint32_t,uint64_t>& terminalized_delta);
```

`ClassifyLevel5` encodes the tree order **exactly**: `(fenced→kFenced)`, `(is_duplicate&&IsEmitted→kDupTerminal)`, `(IsEmitted→kEmittedTerminal)`, `(seq==next_expected→kEmit)`, `(seq<next_expected→kLate)`, `(hold full→kBackpressure)`, `else kHold`. Note `kHoldFull` from v1 is renamed **`kBackpressure`** to reflect the must-fix #5 change: this class now triggers spill-to-deferred/throttle, NOT a fence. `ApplyLevel5Record` reproduces each current body **verbatim**, branching on `true_client_chain` at the two divergence points (kEmittedTerminal re-emit, kLate drop-vs-emit) — **do NOT flatten the divergent arms** (that would silently change ORDER=4). Preserve the C3 meta-first/move-last order in every hold-insert body (`5460/5562/5683/5798`).

**Behavior-preservation for ORDER=5:** only L1/L3 run; both feed the same body against the same `(state,emitted)` for the same session_key with the same tree order → emitted `ready`, `hold_buffer`, `ClientState5`/`ClientEmitTracker`, CV accumulation, and every `order5_*` counter are byte-identical (before D1 fence/backpressure behavior lands). The W1.2 gate (`EMBAR_ASSERT_COMMIT_ORDER=1`, 0 violations) is preserved by construction for the pure-extract commit A.

### Sequencing (commits) — implementable in order

| Commit | Content | Scope | Gate |
|---|---|---|---|
| **A. S2 extract** | `ClassifyLevel5` + `ApplyLevel5Record`; 4 call sites delegate; seeds left in place | Track-01 only (topic.cc/.h) | Re-run W1.2 (`EMBAR_ASSERT_COMMIT_ORDER=1`) → 0 violations, byte-identical ORDER=5 |
| **B. Seed-unify (C5)** | legacy seeds `topic.cc:5498`/`:5720` → 0 | Track-01, legacy-only (ORDER=5 no-op) | Keep separate from A so drift is isolable |
| **C. Re-key to session_key** | `client_state` + siblings re-keyed to `(client_id<<32 | full_32bit_session_epoch)` (§3.2); fields on `OptimizedClientState` (§3.1); `ResetBatchSeqForNewSession()` decl on `QueueBuffer` | Track-01 | Re-run W1.2 |
| **D. CXL session table** | `SessionEntry` region (§2.3), writer publish protocol (state_word LAST), reader invalidate+load_fence, `GOIEntry.session_epoch` field | Track-01 + Track-04 (CXL layout) + D7 (replication) | 64B-atomic torn-read test; single-writer audit |
| **E. Sequencer fencing** | head-gap lease predicate (§3.4), fence the EXPIRY+SCANNER sites (§3.3(1),(3)), DELETE the 4 hold-full force-skips → backpressure (§3.3(2)), `PurgeFencedSession`, `PublishSessionEntry(FENCED)` before purge, fenced drop-guard, `RecoverSequencer5State` with R1 (§6.2.1) | Track-01 | broker-kill smoke test; false-fence test (slow-but-live session survives) |
| **F. Protobuf** | `session.proto` (§4.3), reconnect fence delivery (`network_manager.cc:783`), ACK-relay epoch guard + terminal `EPOCH_STALE` (§6.4), D4 spatial-guard reject | Track-01 + Track-02 | **Track-02 TLA⁺ models (A & B) finalized first** |
| **G. Client** | per-session-global `batch_seq` stamping (§2.2), `session_epoch_`, `UnackedBuffer` (byte cap), δ (open-load-derived), `RetransmitThread`, `RendezvousBroker`, ACK→batch_seq ledger (§5.5), lock order, resubmit-under-new-session (§5.4) | Track-01 client | **open-load δ measurement gate (§5.2) first**; end-to-end broker-kill |

**Hard ordering (implementable in sequence):** S2 (A) **before** the re-key (C). CXL session table (D) **before** sequencer fencing (E) since fence durability (`PublishSessionEntry`) requires the region. Protobuf (F) **after** Track-02 models scenarios A & B. Client (G) last, gated on the open-load δ measurement. The re-key (C) must precede sequencer fencing (E) because fencing keys on `session_key`.

---

## 8. New / Changed Files & Protobufs

**New files:**
- `src/protobuf/session.proto` — `SessionOpen`, `SessionOpenAck`, `SessionFenced` (additive + versioned; `reason` gains `EPOCH_STALE=2`; `committed_batch_seq` authoritative, `committed_msg_hwm` diagnostic-only).

**Changed — client:**
- `src/client/queue_buffer.h:167` — `batch_seq_` (re)designated the per-session-global monotone seq; add `void ResetBatchSeqForNewSession();` decl (single-producer, quiesced-only).
- `src/client/queue_buffer.cc:223` — seal-time stamp `h->batch_seq = batch_seq_.fetch_add(1)` is the authoritative mint (unchanged mechanism); add `ResetBatchSeqForNewSession()` body storing 0.
- `src/client/publisher.h:186` — single `pubQue_` `QueueBuffer` is the architectural anchor (one seal counter suffices for the whole session).
- `src/client/publisher.h:194` — `order5_batch_seq_per_broker_` documented as the ORDER=4 (`kOrderClientBrokerStream`) per-broker counter; LEAVE INTACT, out of D1 scope.
- `src/client/publisher.cc:2105` — stamp `batch_header->session_epoch` next to `client_id` (send path, so retransmit re-stamp works).
- `src/client/publisher.cc:2121-2125` — CONFIRM `kOrderStrong` send-path no-op stays (`batch_seq` NOT reassigned) — already correct for D1.
- `src/client/publisher.cc:2129-2132` — ORDER=4 per-broker reassignment; LEAVE INTACT.
- `src/client/publisher.cc:2287` — replace round-robin reroute with `RendezvousBroker` over the live-survivor snapshot.
- `src/client/publisher.cc:1834, 1847-1853` — head-ACK (message count) → batch_seq frontier mapping (O(k) prefix pop over `UnackedBuffer`); RTT sample + Karn keyed on `attempt==0`; pop under `UnackedBuffer.mu` only.
- `src/client/publisher.cc:323-327, :330` — ORDER=5 home finalizer left as-is; explicitly NOT the rendezvous survivor path (comment cross-reference).
- `src/client/publisher.{h,cc}` — `session_epoch_` (`std::atomic<uint32_t>`), `UnackedBuffer`/`UnackedEntry` (byte cap = `offered_rate × session_lease`), local per-session prefix-sum map `batch_seq → cumulative_num_msg`, `DeltaEstimator` (open-load-derived floor/cap), `RetransmitThread`, retained-copy pool; producer-backpressure hook in `PublishThread`; `SessionFenced` handling + `ResetBatchSeqForNewSession` on resubmit; lock order `UnackedBuffer.mu → mutex_ → pubQue_` with absl annotations at `:1847-1853, :2270/:2313, :2356`.
- `src/client/common.{h,cc}` — `RendezvousBroker(uint32_t client_id, uint64_t batch_seq, const std::vector<int>& survivors, int exclude_broker)` — HRW/argmax over `survivors\{exclude}`, `batch_seq` mixed per candidate; does NOT reuse `publisher.cc:323-327` finalizer.
- `src/client/subscriber.*` — read durable committed HWM per session (D5 default-safe); grep release math to confirm no consumer treats `committed_msg_hwm` as a batch_seq before demoting it.

**Changed — sequencer/CXL:**
- `src/embarlet/sequencer_utils.h:18` — `OptimizedClientState`: add `fenced`, `committed_hwm`, `fence_epoch`, `session_epoch` (32-bit), `fence()`; now a write-through CACHE of the durable `SessionEntry`.
- `src/embarlet/sequencer_utils.h:20` — `next_expected` reconstructed in batch_seq space; accept-time `MAX(GOI-derived, SessionEntry.expected_seq)` (R1), reconnect-answer `min(...)`.
- `src/embarlet/sequencer_utils.h:125-127` — `FastDeduplicator` (`TABLE_SIZE=1<<21`, `RING_SIZE=1<<20`) documented as best-effort same-broker bounded-LRU, NOT the exactly-once guarantee.
- `src/embarlet/topic.h` — re-key `client_state`(`:886`)/`hold_buffer`(`:888`)/`clients_with_held_batches`(`:890`)/`client_emitted_tracker`/`commit_order_last_seq_` to `session_key=(client_id<<32)|full_32bit_session_epoch`; `MakeClientBrokerStreamKey`(`:68`) folds full `session_epoch`; add `L5Class` enum (`kBackpressure` replaces `kHoldFull`) + `ClassifyLevel5`/`ApplyLevel5Record`; `order5_session_fences_`, `order5_spatial_guard_rejects_` counters; `fenced_clients_hwm_` + `FenceRecord` (v1-only fast path, guarded by `per_client_mu_` `:691`); `IsClientFenced`/`GetClientFenceHwm`/`TakeSessionFence`/`AckRelayEpochValid`/`RecoverSequencer5State`/`PublishSessionEntry`/`GetSessionLeaseNs` decls; `SessionEntry` accessor/allocator. `hold_start_ns`(`:99`) documented as the write-once authoritative gap clock.
- `src/embarlet/topic.cc` — S2 extract at L1-L4 (`:5393/:5500/:5612/:5723`); seed-unify `:5498`/`:5720`; head-gap lease fence predicate rewrite (`:5888-5907`); delete gap-skip expiry bodies (`:5896-5999`); fence EXPIRY + SCANNER (`:5342-5344`); DELETE the 4 hold-full force-skip tails (`:5435-5442` + siblings `:5537../:5656../:5771..`) → backpressure; RETAIN detector arming (`:2286`, `:4613-4615`, `:4939`) as latency nudge only (decoupled from fence); `PurgeFencedSession`; `PublishSessionEntry(FENCED)` BEFORE `PurgeFencedSession`; fenced drop-guard in classify; publish fenced HWM in per-client flush (`:6053`); `PublishSessionEntry` (data words flushed, then `state_word` LAST) in `CommitEpoch` (`:4429-4435`, per-epoch-per-session batched); D3 held-slot exclusion in `AdvanceConsumedThroughForProcessedSlots` (`:4479/:4485-4489`); move `InvalidateOrder5HeldSlot` (`:173`) to drain/commit; `RecoverSequencer5State` (near `:3597`) with R1 park-before-accept; `AckRelayEpochValid` + epoch-stamped `per_client_ordered_` (`:4431-4435`) + terminal `EPOCH_STALE`; D4 spatial-guard reject. `GetClientOrdered`(`:1514`)/`per_client_ordered_`(`:4433`) clarified as MESSAGE-count, head-broker-only ACK source. `ClientGc`(`:5177`) allows DRAM cache eviction of FENCED sessions (SessionEntry retains FENCED).
- `src/cxl_manager/cxl_datastructure.h:415` — `_pad0` → `uint16_t session_epoch` (16-bit TRANSPORT HINT only, not the key) + `static_assert(offsetof(BatchHeader, session_epoch) < 64)`.
- `src/cxl_manager/cxl_datastructure.h:407` — `batch_seq` documented as per-session-global `(client_id, session_epoch)` value for `kOrderStrong`.
- `src/cxl_manager/cxl_datastructure.h:261, :270` — `GOIEntry.client_seq` (from `batch_seq` at `topic.cc:4166/:4189`) is the durable per-session-global seq; add `uint32_t session_epoch` into the 40B `_pad` at `:270` (additive, keeps 128B) so recovery can disambiguate generations.
- `src/cxl_manager/cxl_datastructure.h` (new region) — `struct alignas(64) SessionEntry` with `state_word` as LAST-written release gate carrying FENCED|ACTIVE + full-32-bit `last_epoch`; `kSessionTableBase` in BLog tail, `kMaxSessions=4096` (256KB/topic); `static_assert(sizeof==64)`; document held-slot non-reclamation on `batch_headers_consumed_through`(`:316`).
- `src/cxl_manager/cxl_manager.{cc,h}` — map/init the session-table region; wire into chain replication (D7).

**Changed — network:**
- `src/network_manager/network_manager.cc:783` — `HandlePublishRequest` reconnect derives `committed_batch_seq`/fenced from the durable `SessionEntry` (invalidate+load_fence), NOT DRAM; validate `header_low16 == (state_word.last_epoch32 & 0xFFFF)` at ingest (`EPOCH_STALE` on mismatch).
- `src/network_manager/network_manager.cc:2330/:2337/:2353` — `AckThread` ACK-relay epoch guard before each ACK advance (withhold if stale); bounded withhold timer → terminal `SessionFenced{EPOCH_STALE}`.
- `src/network_manager/network_manager.cc:2333-2358` — documents that ORDER=5 ACK is head-broker-only; non-head sockets emit `-1` (no N-socket batch_seq reassembly on the client).
- `src/network_manager/network_manager.h:33` — `EmbarcaderoReq` **unchanged** (session identity rides `session.proto`); copy the authoritative 32-bit `session_epoch` from the handshake into ingest slots.

**Protobufs:** `session.proto` only (new). No existing message changed; the cumulative-ACK `size_t` wire is untouched.

---

## 9. Open Questions for the Human (residual, post-resolution)

The 14 must-fixes are resolved inline. These residuals remain (from the cluster resolutions) and need human/cross-track sign-off before code merges:

1. **`session_lease_ns` value (D2-owned).** The 1s/2s default (§3.4) is a PLACEHOLDER; the authoritative value is a D2 deliverable. Config-load inequality `kDeltaCapMs < session_lease_ms < PBR_fill_time_ms` and `session_lease_ns ≥ open-load append/ack p99.9` must hold. **D2 MUST set `session_lease_ms > kDeltaCapMs` (i.e. > 12 ms)** — the inequality currently holds only against the 1000–2000 ms placeholder; an aggressive sub-12 ms lease would violate it. Too low re-introduces false-fence of slow sessions; too high slows recovery and risks D3 ring-coupling.
2. **Open-load δ measurement gate — CLOSED (2026-07-07).** Offered-load sweep run on the S2 tree; open-load `append_send_to_ack` p99.9 = 1.0–1.1 ms typical / 4.95 ms worst trial at operating load (knee onset ~8000 MB/s). Constants frozen in §5.2: `kDeltaFloorMs=1.7`, `kDeltaCapMs=12`. Full table + knee + caveats: `docs/experiments/delta_measurement_results.md`. **Follow-up validation (D2 phase, non-blocking for D1 freeze):** the 4.95 ms anchor is a single nearest-rank p99.9 sample at the anti-monotone 4000 MB/s point on loopback — re-run the 4000-point in isolation, add a real-wire (`SCENARIO=remote`) point and a multi-client point, and commit the raw per-point CDF CSVs so the anchor is auditable. Raising the cap to 12 already absorbs this single-sample fragility for the backstop.
3. **`GOIEntry.session_epoch` field + SessionEntry region — CLOSED (Track-04 sign-off 2026-07-07).** Recovery (§6.2) requires `session_epoch` persisted in the GOI to disambiguate generations of the same `client_id`: the additive `uint32_t session_epoch` in `GOIEntry._pad` (`:270`, keeps 128B) is **accepted**. The SessionEntry table is placed as a **computed region between `batchHeaders_` and `segments_`** (§2.3), 8 MB total (256 KB × MAX_TOPIC_SIZE) out of the ~26 GB segment pool, replicated via the per-replica-topic slot pattern. Implemented in D1 Commit D.
4. **R1 MAX vs. reconnect-answer min reconciliation (Cluster C ↔ E).** §6.2.1 uses `MAX` for accept-time `next_expected`; §4.1 uses `min` for the reconnect answer. Confirm both signs (both conservative for their property). Cross-cluster sign-off required.
5. **`SessionEntry` slot reclamation (ties to D6).** Open-addressed, never-freed-within-epoch (fence terminality) fills 4096/topic under many-generation long runs. When is a FENCED entry's slot freed (GC frontier, co-design with D6 so no in-flight old-epoch client can still present the key)?
6. **`ResetBatchSeqForNewSession` quiescence (Cluster A residual).** `batch_seq_` is single-producer; resetting to 0 while any `PublishThread` is mid-seal corrupts the monotone invariant. The reset (§5.4) must provably follow SealAll drain + producer pause. If quiescence cannot be guaranteed, fall back to NOT resetting (let `session_epoch` alone namespace the seq) — but that changes reconnect-at-0 accept on the sequencer. Client-library cluster decision.
7. **Per-epoch-per-session `SessionEntry` flush cost.** One 64B flush per `(session, epoch)` that committed that epoch; a fan-out of many active sessions per epoch could add measurable CXL flush traffic in `CommitEpoch`. Needs the same benchmark check as the v1 per-client flush risk.
8. **`committed_msg_hwm` demotion audit.** Confirm no current consumer (subscriber release math) treats `committed_msg_hwm` as a batch_seq before removing it as a release axis (grep `subscriber.*`).
9. **Head-broker failover (must-fix #14 tie-in).** The single authoritative ACK stream is broker 0; on its failure the client must "withhold release," not "assume committed," until a new head is elected (§5.5). Confirm the head-socket-gap policy with D2.
10. **Track-02 invariant shapes (§6.5).** Confirm exact invariant names/shapes for Scenarios A & B (including `ReconstructBeforeAccept` and `NoInfiniteWithhold`) before finalizing the D3 / D4-residual / D2-ACK-guard code.

---

*Verification note: all `topic.cc` line anchors (`5342/5435-5442/5537/5656/5771/5985` skips; `5393/5500/5612/5723` classifier loops; `5888-5907` expiry/fence loop; `1514-1517` GetClientOrdered; `4321/4382/4433/6053` per-client message-count flush; `2286/4613-4615/4939` detector arming; `4166/4189` GOI client_seq; `4429-4435` CommitEpoch per-client flush) and struct anchors (`sequencer_utils.h:18/20/125-127/146/167`, `topic.h:68/95/99/886/888/890/832`, `cxl_datastructure.h:83/261/270/407/415/442-446/738-743`, `publisher.h:186/194`, `queue_buffer.h:167`, `publisher.cc:265/273/323-327/330/1818/1834/1847-1853/2105/2121-2125/2129-2132/2270/2287/2313/2356`, `network_manager.cc:783/2330/2333-2358/2337/2353`) confirmed against `chore/repo-reorg` @ `7ec0e70`.*

---

## 11. Second-round adversarial review — resolutions (v2 → freeze)

The v2 re-review confirmed **all 14 v1 must-fixes CLOSED**, but the fixes introduced a smaller
second-round set. Resolutions below; with these, the design is **freeze-ready** (remaining items are
implementation/TLA+-time, not open design forks).

**R-A (CRITICAL — fence never fires on the canonical lost-batch case).** The §3.4 head-gap lease
predicate `find(next_expected)` only fences when the missing head slot is physically resident — but
the primary case is a batch *lost* (never received), so nothing is resident at `next_expected` and it
never fences. **Fix:** replace the resident-slot lookup with an explicit **per-session gap clock**:
stamp `gap_since_ns` the first time a batch arrives at `seq > next_expected` while `next_expected` is
unfilled; clear it whenever `next_expected` advances. Fence when `now − gap_since_ns > session_lease`.
This is independent of any held entry and also resolves the MEDIUM `hold_start_ns`-reset concern (the
clock is no longer tied to a re-touchable hold entry). Store `gap_since_ns` in DRAM `ClientState5`
(reconstructed conservatively as "now" on failover).

**R-B (R1 park has no bound — liveness/OOM).** §6.2.1 parks inbound until an O(committed_seq) GOI
scan completes, unbounded. **Fix:** (1) add a park-buffer cap (analogous to `kHoldBufferMaxEntries`);
on overflow, NACK/backpressure the producer (never drop). (2) Add a config assertion
`recovery_reconstruct_time < session_lease` (parallel to the existing `session_lease < PBR_fill_time`
inequality) so a client's byte-capped unACKed buffer never drains to EPOCH_STALE mid-recovery.
(3) Recovery may classify per-session incrementally as the scan passes each session's committed
frontier, rather than a global barrier, to shrink park time.

**R-C (EPOCH_STALE can false-fence a live, current-epoch-committed session).** The §6.4 terminal
`EPOCH_STALE` fires off cached ACK-relay epoch stamps, which lag actual commit → reintroduces the
false-fence class #4/#5 removed on the sequencer side. **Fix:** before firing terminal EPOCH_STALE,
re-read the **durable** `SessionEntry`/GOI committed truth for the client; only escalate if the
session is genuinely uncommitted under `ControlBlock.epoch`. Withhold (not confirm) remains the
interim state; terminal fence requires durable-truth confirmation.

**R-D (reconnect-answer under-reports committed truth).** §4.1 uses `min(GOI, SessionEntry)` for the
reconnect `committed_hwm`; with lazy per-epoch `SessionEntry` flush, `SessionEntry` can lag GOI (the
durable truth) by up to one epoch → the client wastefully resubmits already-committed batches (safe,
deduped, but not "no wasteful resubmit"). **Fix:** reconnect-answer `committed_hwm = MAX(GOI-derived,
SessionEntry)` (GOI is durable truth, strictly ≥). Keep `min` only for the accept-time guard where
conservatism is required, and `MIN` fallback only if GOI is unavailable.

**R-E (fenced-drop churns the shared dedup ring).** §3.2 branch keeps
`if (st.fenced){ CheckAndInsertBatchId(...); continue; }` — pointless (the `fenced` bit already drops
it) and harmful (evicts live same-broker dedup entries for other sessions). **Fix:** the fenced-drop
path must **not** touch `CheckAndInsertBatchId`.

**R-F (16-bit epoch hint unchecked on session create path).** §2.2 validates the 16-bit header hint
only against ACTIVE entries; a fresh/wrapped session's first batch has no ACTIVE entry to check
against, risking realias after GC + 16-bit wrap. **Fix:** on the create path validate the full 32-bit
handshake-established `session_epoch` (carried into the ingest slot) against the incoming batch, not
just the 16-bit hint. The 16-bit header stays a transport hint; the 32-bit handshake value is
authoritative for slot creation.

**R-G (client ACK→seq rebase at the `session_epoch++` boundary).** The head-ACK cumulative message
count is per-`client_id` (spans generations), but the client's unACKed ledger restarts prefix-sum at
`batch_seq=0` per generation → `released_through_seq` is off by the prior generation's total msg
count post-fence, so unACKed bytes won't release. **Fix:** at `session_epoch++`, snapshot the current
per-`client_id` cumulative ACK count as `M_base`; compute the new generation's release frontier from
`M − M_base`. (Alternative: key the head-ACK stream by `session_key` — rejected, contradicts the
per-client-id ACK relay design.)

**R-H (additive fields + measurement gates — not design forks).**
- Add `GOIEntry.session_epoch` (16 b) using the existing `_pad[40]` (`cxl_datastructure.h:270`) — required so failover GOI-scan separates generations of the same `client_id`. Keeps GOIEntry at 128 B.
- **δ cap** measurement gate **CLOSED (2026-07-07)**: measured open-load `append_send_to_ack` p99.9 = 4.95 ms worst-trial (not e2e — the ~83 ms tail is subscriber-side; not steady 1.68 ms). Frozen: `δ_cap = 12 ms` (≥2× incl. +0.2 ms wire), `δ_floor = 1.7 ms` — §5.2. `delta_measurement_results.md`.
- **Lock order** `UnackedBuffer.mu → mutex_ → pubQue_` requires a manual audit (QueueBuffer uses `std::mutex`, not absl-annotated) — an implementation-review checklist item, not a design change.
- **Anchor drift** (`publisher.h:194↔195`, a few ±1-line cites): re-verify line numbers at implementation; non-load-bearing.

**Status after R-A…R-H: design-complete.** No open design forks remain; R-A/R-B/R-C are the
substantive ones (all with concrete fixes above), the rest are localized. Freeze the spec at
v2 + §11, then implement in the §7 order (S2 refactor first). R-A/R-C and Scenarios A/B are the items
Track 02's TLA⁺ should exercise before the corresponding code is finalized.
