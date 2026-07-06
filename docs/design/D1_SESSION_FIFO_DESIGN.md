# D1 Design Spec — Session-Based FIFO over Non-Coherent CXL

**Status: DRAFT — needs human sign-off + Track-02 TLA+ coordination BEFORE implementation.**
**Owner:** Track 01. Produced by the `d1-design` workflow (6 parallel facet analyses → synthesis →
3 adversarial-lens reviews), all `file:line` verified against `chore/repo-reorg` @ `7ec0e70`.

> **Do not start coding from the synthesized design below verbatim.** The adversarial review found
> load-bearing holes (all 3 lenses returned `has_holes`). The consolidated **must-fix list** below
> is the gate: each item must be resolved (design decision, often needing your input) before the
> spec is frozen. The synthesized design (§ onwards) is the substrate those fixes amend.

---

## ⚠ CRITICAL — adversarial-review must-fix (resolve before freezing the spec)

These are concrete holes verified against the code, grouped. Several need a human decision.

### A. Exactly-once / dedup axis (protocol lens)
1. **HWM axis bug:** the committed-HWM the client releases/resubmits against MUST be
   `committed_batch_seq = next_expected − 1` (batch_seq space), NOT `per_client_ordered_` — the
   latter is a cumulative **message count** (`topic.cc:4432`), not invertible to a batch_seq. Drop
   `committed_msg_hwm` as a release axis (diagnostic-only at most).
2. **Resubmit dedup reality:** `batch_id = (broker_id<<48)|pbr_absolute_index` is a per-broker
   physical slot id — a rendezvous-retransmit to a *different* broker gets a NEW batch_id, so
   `CheckAndInsertBatchId` does NOT dedup it. The SOLE resubmit dedup is `seq < next_expected`.
   The exactly-once proof must therefore show `next_expected` at `S_new` is reconstructed ≥ every
   committed batch **before** any resubmit is classified (close the recovery TOCTOU). Stop calling
   batch_id dedup "primary".
3. **Dedup window is bounded:** `FastDeduplicator` is 1M-entry LRU (`sequencer_utils.h`), not
   permanent — reinforces that `next_expected` monotonicity is the real guarantee.

### B. Fence trigger — safety framing is currently contradicted (protocol + correctness lenses)
4. **Fence gate must be a real per-session lease, not the disconnect/shutdown window.** The proposed
   `force_expire_all_frontiers` (`topic.cc:5888`) is armed by `RequestOrder5HoldExpiryOnce()` on
   disconnect/shutdown with a **flat 50 ms** threshold — NOT a lease. As written, a live client with
   a >50 ms in-flight gap during any TCP blip would be fenced (false-fence → dup/loss risk),
   violating "safety never depends on the detector." Introduce a genuine per-session lease timer
   (age of the specific gap's `hold_start_ns` > full lease) and gate fence on THAT. **Coordinate the
   lease value with D2.**
5. **Hold-buffer-full must not fence a live session.** Load-induced saturation (`kHoldBufferMaxEntries`)
   must stay backpressure/NACK, not a terminal fence. "Rare under D3" ≠ safety argument.

### C. Durability / failover (correctness lens)
6. **Fence state + session table must be durable in CXL, not DRAM-only.** `ClientState5.fenced`/
   `next_expected` in DRAM are lost on client-GC and sequencer failover → a fenced suffix could
   reappear. Persist per-session `{expected_seq, committed_hwm, fenced, epoch}` in the CXL session
   table (single-writer sequencer, 64B-atomic, monotonic), reconstructed on failover from D3
   commit-driven PBRs.
7. **TOCTOU: publish fence record + HWM atomically w.r.t. the reconnect read** (same `per_client_mu_`
   critical section, or derive HWM only from already-committed GOI truth), so a client reconnecting
   mid-flush cannot read `!fenced`/stale HWM.
8. **session_epoch=0 / 16-bit-wrap aliasing** in the DRAM `client_state` map must be disambiguated
   (legacy v0 vs a wrapped v1 landing on 0 at the same `client_id`).

### D. Client library (liveness lens)
9. **batch_seq is minted PER-BROKER** (`publisher.cc:2130`), not per-session-global — so
   `committed_batch_seq`/suffix are undefined for a multi-home client. **Decision needed:** introduce
   a per-session-global sequence number, OR redefine HWM/suffix as per-(session,broker) tuples.
10. **Cumulative-ACK → batch_seq resolution is missing.** The ACK wire is a cumulative per-socket
    message count (`publisher.cc:2296`) with no batch_seq; define how it maps to a batch_seq frontier
    for unACKed-buffer release, RTT sampling, and Karn suppression — else δ/release is non-implementable.
11. **Rendezvous hash must mix batch_seq** — reusing the client_id-only finalizer (`publisher.cc:322`)
    gives herd COLLISION, not spread; and must provably exclude the dead broker.
12. **Re-derive δ from open-load append/ack p99.9**, not the steady 1.68 ms (open-load ordered/ack
    p99 ≈ 82.9 ms > a 50 ms δ_cap → spurious backstop). Either raise the cap or measure open-load first.
13. **Bound unACKed buffer against the failure stall** (`offered_rate × session_lease`, seconds), not
    just BDP; specify producer backpressure. Specify client lock order (UnackedBuffer.mu → mutex_ →
    pubQue_) to avoid the retransmit/ACK-release deadlock.
14. **ACK-relay epoch guard (D2):** `AckThread` reads `GetClientOrdered` with no ControlBlock.epoch
    check (`network_manager.cc:2330`) while ingest has `CheckEpochOnce` — add the guard, but pair
    "withhold rather than confirm" with a bound (terminal `EPOCH_STALE`/fence) to avoid an infinite
    δ-retransmit liveness hole. **This is TLA+ scenario (b) for Track 02.**

**Two TLA+ scenarios to hand Track 02 before finalizing code:** (a) retransmitted duplicate at
`S_new` inside the D4 spatial-guard window; (b) stale-CV ACK relay after election (#14). See §6 of
the synthesized design for the precise state/actions/invariants.

---

## Synthesized design (substrate — amend per must-fix above)

`docs/design/` exists. I have all the verified ground truth. Here is the synthesized D1 design spec.

---

# D1 Design Spec — Session-Based FIFO over Non-Coherent CXL (Embarcadero)

*Status: draft for `docs/design/` / Paper input. All `file:line` anchors verified against `chore/repo-reorg` @ `7ec0e70`. F6's post-ORDER=5-fix line numbers are authoritative; F1/F3 stale numbers have been corrected in-place.*

---

## 1. Overview + Guarantee Theorem

### 1.1 What D1 is

Embarcadero today gives per-session in-order commit with ACK-on-commit that **holds across epoch seals** (W1.2, `docs/experiments/w1_findings.md`). The *only* ordering violations are three deliberate **gap-skips** that advance a session's `next_expected` past a missing batch and emit the out-of-order suffix:

| Skip class | Site | Verified |
|---|---|---|
| EXPIRY gap-skip | `src/embarlet/topic.cc:5985` (`// Sequence is beyond next_expected - skip the gap`) | ✓ |
| HOLD-BUFFER-FULL forced skip (×4) | `topic.cc:5436`, `:5538`, `:5657`, `:5772` | ✓ |
| SCANNER TARGETED skip | `topic.cc:5342-5344` (`[[SKIP_MARKER_FILTER]]`, prelude at `:5321`) | ✓ |

**D1 replaces gap-skip with session-fencing.** When a gap cannot be filled within a session's full lease, the sequencer *freezes* `next_expected` at the committed frontier, *drops* the uncommitted suffix (never emits it), and surfaces `SESSION_FENCED` + the committed high-water-mark (HWM) to the reconnecting client. The client opens a new session generation (`session_epoch+1`) and resubmits exactly the suffix `> committed_hwm`, yielding exactly-once across fencing with **no separate HWM query**.

### 1.2 Guarantee theorem (restatement)

> **Session-FIFO-with-Fencing.** For any client session `S = (client_id, session_epoch)`, the batches committed to the Global Order Index (GOI) under `S` form a **gap-free, duplicate-free prefix** of the client's submission stream for that session. Late and duplicate arrivals for `S` are **rejected, never reordered and never re-emitted**. A session is *fenced* iff a gap at `next_expected` outlives the full session lease; upon fencing, no `batch_seq > committed_hwm` is ever committed under `S`. The union over the generation chain `S₁ ⊂ S₂ ⊂ …` (each `Sₖ₊₁` resubmitting exactly the suffix `> committed_hwm(Sₖ)`) is a gap-free, duplicate-free prefix of the client's global submission stream = **exactly-once ordering across fencing.**

The theorem rests on four already-verified invariants plus one new one:

1. **In-order commit (W1.2):** `next_expected` advances *only* on in-order GOI commit; `emitted`/`per_client_ordered_` reflect committed-iff-ACKed. Verified: `topic.cc:5410` (EQ branch), `:4433` (`per_client_ordered_` advance in CommitEpoch).
2. **Assign-at-release (D4, W1.3):** held batches are placeholders (`is_held_marker`, `num_msg=0`) consuming **no** GOI/order index; the GOI index is assigned at commit/release, not at reservation. Verified: `global_batch_seq_.fetch_add` counts only non-skipped/non-held.
3. **committed_seq monotonicity (spatial-guard input):** `ControlBlock.committed_seq` (`cxl_datastructure.h:159-162`) advances only over the gap-free contiguous GOI prefix, stored with `store_fence+flush_cacheline` by `CommittedSeqUpdaterThread`.
4. **Single-writer discipline:** all per-session DRAM state is mutated by exactly one shard/EpochSequencer worker; all CXL session state is single-writer (sequencer), 64B-TLP-atomic, monotonic.
5. **[NEW] Fence terminality:** `fence()` is idempotent and monotonic — a session never un-fences within its `session_epoch`, and `committed_hwm` never lowers. This is what the client's suffix-complement resubmit relies on, and what the D4 spatial guard reads.

---

## 2. Session Identity & CXL Session Table

### 2.1 Session identity = `(client_id, session_epoch)`

**`client_id`** stays the 32-bit random per-process value (`publisher.cc:265`, `GenerateRandomNum()`). **`session_epoch`** is a new 32-bit client-owned counter:

- First connect: `session_epoch_ = 1` (0 reserved = "no session / legacy, never fenced").
- Reconnect, `SESSION_FENCED` receipt, or adaptive-δ failover to a new chain: `session_epoch_++` (monotonic per process, never reused, persists across broker reconnects within one process).

The wire ordering key becomes `(client_id, session_epoch, batch_seq)`. `batch_seq` remains **minted at seal time** (`queue_buffer.cc` seal counter; stamped, not reassigned, at send `publisher.cc:2131`) and **resets to 0 per session generation** — a reconnecting client's fresh `batch_seq=0` is accepted because it hashes to a fresh `ClientState5{next_expected=0}` (§3.2). This fixes the current silent-reject hazard (today a reused `client_id` restarting at 0 is rejected as `seq < next_expected` forever).

### 2.2 Carriage — two additive, versioned changes

**(a) Handshake.** `EmbarcaderoReq` (`network_manager.h:33`, `alignas(64)`) is *not* Track-01-owned and is packed. **Do not widen it.** Session identity rides a new versioned protobuf `session.proto` (§4) on the gRPC handshake/redirect channel. Old brokers ignoring `session_epoch` ⇒ treat as `epoch=0` legacy.

**(b) Per-batch.** Carry `session_epoch` in `BatchHeader` (Track-01-owned) by consuming the free `uint16_t _pad0` at `cxl_datastructure.h:415`:

```cpp
uint32_t flags;
uint16_t epoch_created;    // existing: control-block epoch (Phase-1a zombie fencing, D2) — DISTINCT concept
uint16_t _pad0;   -->  uint16_t session_epoch;   // NEW (low 16 bits of client session_epoch)
```

16 bits suffices (a client re-sessions ≪65k times/process; wrap is detectable against the CXL table's stored epoch). This keeps `BatchHeader` at **128B** and `session_epoch` in the **first cache line** alongside `client_id`. Add `static_assert(offsetof(BatchHeader, session_epoch) < 64)` next to the existing asserts (`cxl_datastructure.h:442-446`). NetworkManager copies it from the handshake-established session into each ingest slot.

> **Resolved contradiction (F2 open-Q, F3 open-Q):** `session_epoch` is **distinct** from `epoch_created`. `epoch_created` = cluster/control-block fencing epoch (D2); `session_epoch` = client session generation (D1). They coexist in the first cache line.

### 2.3 CXL session table (new durable region, single-writer)

**Do not grow `ControlBlock`** (128B is the exact 2-cache-line prefetch contract, `cxl_datastructure.h:83`). The [0x0080, 0x1000) gap before CV (`kCompletionVectorOffset = 0x1000`, `:712`) is only ~3.9 KB — too small for a real table.

> **Resolved contradiction (F2 offered 0x0800, then 0x100, then BLog-tail):** Place the session table in the **~495 GB BLog/PBR tail** at a dedicated per-topic stride, mirroring TInode. A small 0x0100-based table is rejected (cannot hold 4096 entries). One 256 KB block/topic:

```cpp
// cxl_datastructure.h
constexpr size_t kSessionTableBase = /* dedicated base in BLog tail, per-topic stride */;
constexpr size_t kMaxSessions      = 4096;   // per topic; 4096 × 64B = 256 KB/topic

struct alignas(64) SessionEntry {              // exactly 64B → one TLP-atomic cache line
    std::atomic<uint64_t> session_key{0};      // (client_id<<32)|session_epoch; 0 = free
    std::atomic<uint64_t> expected_seq{0};     // next batch_seq to commit (== ClientState5.next_expected)
    std::atomic<uint64_t> committed_hwm{0};     // last committed batch_seq (SESSION_FENCED payload)
    std::atomic<uint64_t> state_word{0};        // [flags:8 | reserved:24 | last_epoch:32]; bit0=FENCED, bit1=ACTIVE
    std::atomic<uint64_t> highest_sequenced{0}; // mirror of ClientState5.highest_sequenced
    std::atomic<uint64_t> reserved0{0};
    uint8_t _pad[16];
};
static_assert(sizeof(SessionEntry) == 64);
```

- **Ownership:** the **sequencer is the single writer** for every field. Brokers/replicas/clients only read (`flush_cacheline` + `load_fence` before load — CXL is non-coherent). Open-addressed by `hash(session_key) % kMaxSessions` with linear probe.
- **Monotonicity:** `expected_seq`, `committed_hwm`, `highest_sequenced` only increase; `session_key` transitions free(0)→active→(FENCED flag set, key retained). Because each entry is exactly 64B and single-writer, a poll-based reader sees the old or new whole line — never a torn one.
- **Role:** the CXL table is the **failover-durable mirror**; the DRAM `client_state` map stays the hot path (§3.2). It is chain-replicated in the same metadata plane as `ControlBlock`/CV so D7 replicates it for free.
- **Write cadence:** written at **commit time in `CommitEpoch`** (`topic.cc:4431-4433` critical section), **batched per-epoch-per-session, not per-batch** (see §6/risk — per-batch CXL flush would regress the fast path). So `expected_seq`/`committed_hwm` in CXL always reflect only committed batches and are crash-consistent.

---

## 3. Sequencer Hold-Layer: expected_seq, dedup, in-order commit, and SESSION-FENCING

### 3.1 Per-session fence state (new fields on `OptimizedClientState`)

`sequencer_utils.h:18` — single-writer, no lock:

```cpp
struct OptimizedClientState {   // = ClientState5 (topic.h:95)
    uint64_t next_expected{0};        // existing (0-based)
    uint64_t highest_sequenced{0};    // existing
    uint64_t last_epoch{0};           // existing
    // ... existing 1024-window bitmap ...

    // --- NEW (D1) ---
    bool     fenced{false};
    uint64_t committed_hwm{0};   // == next_expected at fence time (client-global for true_chain)
    uint64_t fence_epoch{0};     // current_epoch_for_hold_ at fence (GC/diagnostics)
    uint32_t session_epoch{0};   // session generation this state serves

    void fence() {               // idempotent, monotonic — never lowers HWM, never un-fences
        if (!fenced) { fenced = true; committed_hwm = next_expected; }
    }
};
```

### 3.2 Dedup fast path (one comparison) + re-key

> **Resolved contradiction (F1 keeps `client_id` key; F2 re-keys):** adopt **F2's re-key** — it is the mechanism that makes reconnect-at-0 work. `Level5ShardState::client_state` (`topic.h:886`, keyed by `size_t`) and its sibling maps (`hold_buffer` `:888`, `clients_with_held_batches` `:890`, `client_emitted_tracker`, `commit_order_last_seq_`) re-key from `client_id` to `session_key = (uint64(client_id)<<32) | session_epoch`. `MakeClientBrokerStreamKey` (`topic.h:68`) folds `session_epoch` in for legacy per-stream paths.

The fast path in the S2-unified classifier (§7) is then exactly:

```cpp
ClientState5& st = shard.client_state[session_key];   // new epoch → fresh {next_expected=0}
if (st.fenced) { CheckAndInsertBatchId(shard, batch_id); continue; }   // (E) drop for fenced session
if (seq < st.next_expected) { record_dup(); continue; }               // DEDUP: one comparison
if (seq == st.next_expected) { emit; st.advance_next_expected(); }     // EQ
else { hold_or_fence; }                                                // GT
```

The 1024-window bitmap (`sequencer_utils.h:28-48`) is retained only as a secondary in-window dup guard; the primary reject is the single `seq < next_expected` compare. **The fenced-session drop (E) is one bool load on the fast path** — a late/duplicate arrival for a fenced session is silently dropped, never re-ordered, never emitted (theorem: "late and duplicate arrivals are rejected").

### 3.3 SESSION-FENCING replacing the three gap-skips (exact functions)

> **Resolved contradiction (F6: "D1 = expiry site only, force-skip/scanner = D3-scoped" vs F1: "fence all three"):** **F1's scope is adopted** — fencing is the correct *terminal* action for all three classes, since dropping the suffix is the semantic in every case. F6's point stands as a *sequencing* note: under D3 (commit-driven PBR) the hold-full path becomes rare, but the fence branch is still the correct code to place there. All three sites become `state.fence()` + drop.

Add to the shard worker:

```cpp
void PurgeFencedSession(Level5ShardState& shard, size_t session_key);  // (D)
// erases hold_buffer[key], hold_buffer_size -= n, clients_with_held_batches.erase(key)

auto record_fence = [&](size_t key){ order5_session_fences_.fetch_add(1);
                                     PurgeFencedSession(shard, key); };
```

**(1) EXPIRY** `topic.cc:5984-5999`. Replace the `// skip the gap` tail (`state.next_expected = ent.seq + 1; state.mark_sequenced(ent.seq); … ready.push_back`) with:

```cpp
state.fence();
record_fence(session_key);
// DROP ent: no ready.push_back, no MarkEmitted, no CheckAndInsertBatchId.
// (slot already retired at hold-insert; PurgeFencedSession reclaims the rest of the session's holds)
```

**Gating (see §3.4):** only fence when the gap has outlived the full lease — gate on `force_expire_all_frontiers` (D2 window, `force_expire_hold_until_ns_` at `topic.h:859`), **not** the 50 ms `kForceExpireMinAgeNs` floor alone.

**(2) HOLD-BUFFER-FULL** `topic.cc:5436`, `:5538`, `:5657`, `:5772` (fires only when `hold_buffer_size >= kHoldBufferMaxEntries`=100000 **and** `deferred_level5` also full). Replace `state.next_expected = seq+1; ready.push_back(...)` with `state.fence(); record_fence(session_key);` and DROP `*p`/`*jt` (`continue`). Rationale: ring/hold saturation means this gap cannot be filled without unbounded buffering; fencing releases pressure deterministically. Under D3 this path is rare.

**(3) SCANNER TARGETED skip** `topic.cc:5342-5344`. Replace the `if (batch_seq >= next_expected) next_expected = batch_seq+1` advance with `state.fence(); record_fence(session_key);`. **CRITICAL — still push the marker:** the skip marker (`num_msg=0`, `is_held_marker` semantics) must still `ready.push_back(std::move(*skip_it))` to advance `consumed_through` (ring drain). It consumes no GOI index (W1.3). *Fence only the session state; keep the marker emission* — dropping it re-introduces the `BrokerScannerWorker5` freeze class (`code_review_w1.md:156-188`).

### 3.4 Gating vs normal retransmit (coordinate with D2)

Change the **semantics** of `force_expire_hold_until_ns_` from "skip old holds" to "fence sessions whose `next_expected` still has a gap":

```cpp
// in the expiry loop topic.cc:5940-6000:
bool should_fence = force_expire_all_frontiers && (age_ns >= kForceExpireMinAgeNs);
// A held entry whose seq == next_expected still drains normally (awaited batch, drain loops).
// Only a held entry with seq > next_expected under should_fence triggers state.fence().
```

**Steady state (no lease expiry, `GetOrder5HoldTimeoutNs()==0`, `topic.cc:87`) never fences** — the batch waits for retransmit-to-a-different-broker (client, §5) or the lease-driven NACK (D2). This preserves "safety never depends on the detector": a plain timer eventually arms the window; leases only shrink the constant. **This is the highest-risk knob** — arming too eagerly fences live-but-slow sessions. The window must be strictly the full-lease timeout; tie the exact arming to D2.

### 3.5 Purge-on-fence subsumes the W1 residual

`code_review_w1.md:189-193,211-213` reports a held entry occasionally removed without commit (~2 batches, ~0.005%). That path *is* a gap-skip/expiry emitting an entry that should not have committed. Once §3.3 (B) + `PurgeFencedSession` (D) remove all gap-skip emission, **the residual path no longer exists** — fencing subsumes it. No separate fix.

### 3.6 GC interaction

`ClientGc` (`topic.cc:5177-5211`) evicts idle sessions after `kClientTtlEpochs`. A fenced session with an empty hold buffer is GC-eligible; correct, because its HWM is already published to the head map and the CXL table (which persist independently of the shard). Shard eviction is safe.

---

## 4. SESSION_FENCED Protocol + committed-HWM (protobuf, path)

### 4.1 The HWM source is already committed truth

The head broker's per-client cumulative ACK, `per_client_ordered_` → `GetClientOrdered(client_id)` (`topic.h:418`, `topic.cc:1514-1517`, advanced only in `CommitEpoch` `:4433`), **IS** the committed HWM. No new HWM-query mechanism is needed — it folds into `SESSION_FENCED`.

- `committed_batch_seq` = `next_expected - 1` (last strictly in-order committed batch → client resubmits `batch_seq > this`).
- `committed_msg_hwm` = `per_client_ordered_[client_id]` (cumulative committed message count → lets the client map suffix to message offsets without a query; belt-and-suspenders).

Both are snapshotted together under `per_client_mu_` (`topic.cc:1515`) with `ControlBlock.epoch` (flush+fence read) so the HWM is self-consistent with the ACK stream and the fence epoch (exactly-once step 4, §6.3).

### 4.2 Head-side fence publication

```cpp
// topic.h (guarded by per_client_mu_)
absl::flat_hash_map<uint32_t, uint64_t> fenced_clients_hwm_ ABSL_GUARDED_BY(per_client_mu_);
struct FenceRecord { uint32_t session_epoch; uint16_t reason;
                     uint64_t committed_batch_seq; uint64_t committed_msg_hwm;
                     uint64_t control_epoch; bool delivered; };
bool     IsClientFenced(uint32_t client_id) const;
uint64_t GetClientFenceHwm(uint32_t client_id) const;
std::optional<FenceRecord> TakeSessionFence(uint32_t client_id);  // read-and-mark-once, analogous to GetClientOrdered
```

The shard worker publishes `cid + committed_hwm + FenceRecord` to the head map at the end of `ProcessLevel5BatchesShard`, in the same per-client flush where `per_client_ordered_` is flushed (`topic.cc:6053-6055`).

### 4.3 Wire locus — protobuf on reconnect (primary) + capability-gated in-band (fallback)

> **Resolved contradiction (F1: new `session.proto` on handshake; F3: in-band `AckControlFrame` sentinel on the ACK socket; F4: tagged ACK frame or gRPC):** **Primary = protobuf on the handshake/redirect channel** (F1's recommendation), because the fenced-but-live client learns of the fence *on reconnect* — that is the natural, additive, collision-proof locus. The in-band ACK-sentinel (F3) is rejected as primary due to the sentinel-collision risk (a legitimate cumulative ACK reaching `UINT64_MAX-1` is unbounded-in-principle) and the v0/v1 stream-desync hazard. It may be retained only as a **capability-gated** fast-path optimization for already-connected clients, never for legacy (`session_epoch=0`) clients.

New file **`src/protobuf/session.proto`** (additive + versioned):

```protobuf
syntax = "proto3";
package embarcadero;

message SessionOpen    { uint32 client_id = 1; uint32 session_epoch = 2;
                         string topic = 3; uint32 requested_ack = 4; }
message SessionOpenAck { uint64 committed_hwm = 1; uint32 status = 2; }   // status carries FENCED-on-reconnect
message SessionFenced  { uint64 client_id = 1; uint64 session_epoch = 2;
                         uint64 committed_batch_seq = 3;   // resubmit batch_seq > this
                         uint64 committed_msg_hwm  = 4;
                         uint64 control_epoch      = 5;    // staleness/anti-replay (== ControlBlock.epoch at fence)
                         uint32 reason             = 6; }  // {HOLD_EXPIRY=1, EPOCH_STALE=2, LEASE_LOST=3, DUP_PAST_EXPECTED=4}
```

**Path:**
1. A fenced-but-live client reconnects, presenting `(client_id, old_session_epoch)` in `SessionOpen`.
2. The broker checks `IsClientFenced(client_id)` / `TakeSessionFence`; if fenced (or derivable from `per_client_ordered_ + next_expected`), it replies with `SessionFenced` (or `SessionOpenAck{committed_hwm, status=FENCED}`) **before any ACK**.
3. The client reads `committed_batch_seq`, opens `(client_id, session_epoch+1)`, resubmits exactly the suffix `> committed_batch_seq` (§5).

The bare `size_t` cumulative ACK stream (`network_manager.cc:2480-2529`) is **unchanged** for normal ACKs. `AckThread` reads `IsClientFenced`/`GetClientFenceHwm` and, for a v1 (`handshake_version≥1`) session whose `AckThread` epoch matches the fenced epoch, MAY send the capability-gated frame; a v0 session **never** receives it.

> **Ordering constraint:** any in-band fence must be sent **strictly after** the last real ACK for `committed_batch_seq` on the same fd, else the client resubmits an already-committed batch (safe via dedup, but wasteful).

---

## 5. Client Library: unACKed buffer, adaptive δ, rendezvous survivor selection, resubmit-under-new-session

Today the publisher is fire-and-forget: `PublishThread` (`publisher.cc:1900-2358`) sends `BatchHeader`+payload then *immediately* `pubQue_.ReleaseBatch()` (`:2356`) — **no copy survives the socket write**, so retransmission is impossible. D1 adds four mechanisms in `publisher.{cc,h}` + `common.{cc,h}`. **D2's lease-driven NACK is the PRIMARY retransmit trigger; the δ timer is a BACKSTOP for silent loss.** Safety (no gap-skip) lives on the sequencer; the client only decides *when* and *where* to retransmit.

### 5.1 UnACKed buffer

```cpp
struct UnackedEntry {
    uint64_t batch_seq;                 // rendezvous key component
    uint32_t num_msg;                   // cumulative-ACK release math
    int      cur_broker_id;
    uint8_t  attempt;                   // Karn: no RTT sample if >0
    steady_clock::time_point first_send_ts;   // RTT sample (first attempt only)
    steady_clock::time_point last_send_ts;    // δ backstop expiry
    void*    batch_copy; size_t copy_len;     // owned copy of BatchHeader+payload
};
struct UnackedBuffer { absl::Mutex mu; std::map<uint64_t, UnackedEntry> outstanding; uint64_t released_through{0}; };
```

**Lifecycle:**
- **Buffer-on-send** (replace `ReleaseBatch` at `publisher.cc:2356`): copy `sizeof(BatchHeader)+len` into a bounded pooled DRAM buffer (BDP-sized: append-p99.9 × offered rate), insert the entry, *then* `ReleaseBatch` the QueueBuffer slot (so the hot pool is not held hostage). The copy is mandatory — the slot is reused immediately (mirrors server-side `HoldBatchMetadata` copies).
- **Release-on-ACK** (at `publisher.cc:1847-1853` where increments fold): pop entries with `batch_seq ≤ frontier`, free their copies. **Preferred (per §4.1):** the fence/ACK carries the per-session committed HWM as a `batch_seq`, so release is an O(k) prefix pop.
- **Bound:** cap by count/bytes; when full, `PublishThread` backpressures (spin/yield) — the client mirror of D3's emergent flow control. No per-session credit window.

### 5.2 Adaptive δ (TCP-RTO style)

```cpp
struct DeltaEstimator { double srtt_ms{0}, rttvar_ms{0}; bool seeded{false};
                        std::atomic<double> delta_ms{kDeltaFloorMs}; };
// kDeltaFloorMs ≈ 5 (W1.1 append/ack p99.9 = 1.68 ms + headroom), kDeltaCapMs ≈ 50, α=1/8, β=1/4 (RFC6298)
```

- **Sample source:** on release of a first-attempt batch, `R = now − first_send_ts`. **Karn:** never sample when `attempt>0`. Update co-located with `ProcessPublishAckLatency` (`publisher.cc:1849`) — no new syscalls.
- `delta_ms = clamp(srtt + 4·rttvar, floor, cap)`.
- **New `RetransmitThread`** (started in `Init()` near `publisher.cc:758`): every `min(δ/2, 2ms)` scans `UnackedBuffer`; for any entry past `last_send_ts + backoff(δ, attempt)` (exponential backoff `δ·2^attempt`) not superseded by a D2 NACK, triggers retransmit. This is the silent-loss backstop only; the D2 NACK path calls the same routine immediately (~lease time ≈ 1 ms ≪ δ).
- **δ floor rationale (W1.1):** the 733 ms e2e tail is *subscriber-side* and does **not** gate δ; δ is set from append/ack p99.9=1.68 ms only. δ set too low → spurious steady-state retransmits (the W1.1 coupling warning).

### 5.3 Rendezvous survivor selection

```cpp
// common.{cc,h} — reusable
int RendezvousBroker(uint32_t client_id, uint64_t batch_seq,
                     const std::vector<int>& survivors, int exclude_broker);
// score_b = splitmix64(mix(client_id, batch_seq, b)); return argmax over b != exclude_broker
```

Reuse the same splitmix64 finalizer as ORDER=5 home selection (`publisher.cc:323-327`). Deterministic (a racing duplicate lands consistently), spreads consecutive batches of one client to *different* survivors (herd spreading), and always excludes the failed broker. **Replaces** the ad-hoc round-robin reroute at `publisher.cc:2287` and generalizes it from "batch-in-hand" to "any unACKed batch". Retransmit: rewrite `batch_header->broker_id = nb`, re-send `batch_copy`, `++attempt`, `last_send_ts=now`, **no RTT resample** (Karn). **Invariant:** maintain ≥2 live broker sockets whenever the cluster has ≥2 brokers (shared with D2). If survivors shrink to 0, fall back to stall→fence (bounded by δ), **never gap-skip**.

### 5.4 Resubmit-under-new-session on SESSION_FENCED

On `SessionFenced{committed_batch_seq}`:
1. Stop treating the current session as live; `session_epoch_++`; re-handshake warm connections under the new epoch.
2. From `UnackedBuffer`, release entries `≤ committed_batch_seq` (committed, exactly-once done); take exactly the suffix `> committed_batch_seq`.
3. Re-stamp each suffix entry: new contiguous `batch_seq'` from 0 under `session_epoch'` **preserving submission order** (map old→new so ACK reconciliation stays correct), rewrite the copy's `BatchHeader` (`client_id` unchanged, `session_epoch'`, `batch_seq'`), retransmit via rendezvous.

This is the **only** place the client resets `batch_seq`. Normal retransmit (§5.1-5.3) keeps `(client_id, session_epoch, batch_seq)` stable so the sequencer's `seq < next_expected` dedup drops duplicates — the client never causes reorder.

> **Resolved (F4 open-Q on session_epoch minting):** minted **client-local monotonic** for v1 (simplest, no round-trip on open); a server-assigned epoch in `SessionOpenAck` is a future option if the session table needs authoritative generation numbers.

---

## 6. Failover / Durability (D3 interplay) + D4 spatial-guard & stale-CV (Track 02)

### 6.1 Durability split

| Plane | State |
|---|---|
| **CXL (durable, single-writer, monotonic, flushed)** | `ControlBlock.{epoch, sequencer_id, committed_seq}`; GOI entries (`client_id, client_seq, broker_id, total_order, epoch_sequenced`); PBR `BatchHeader`; `TInode.offsets[b].{ordered, batch_headers_consumed_through}`; CV; **new `SessionEntry` table** (§2.3) |
| **DRAM (volatile, reconstructible)** | hold buffer (`HoldEntry5`/`HoldBatchMetadata`), `per_client_ordered_`, in-memory `ClientState5` (`next_expected`, `fenced`, window), completed-ranges ring, `fenced_clients_hwm_` |

### 6.2 D3 (commit-driven PBR) — the reconstruction prerequisite

Today PBR reclamation is **scan-driven** (W1.4): `AdvanceConsumedThroughForProcessedSlots` (`topic.cc:4479`) advances `batch_headers_consumed_through` for **all** processed slots including held/skipped, so a held-but-uncommitted batch is **not reconstructible after failover**. D3 changes this:

- In `CommitEpoch`, **do not** advance `batch_headers_consumed_through` over `is_held_marker` slots — build a held-slot exclusion set and skip them in the tinode write-back (`topic.cc:4485-4489`). Keep advancing committed + dedup-dropped slots for ring-drain liveness.
- Move `InvalidateOrder5HeldSlot` (writes `flags=kBatchHeaderFlagRetired`, `topic.cc:173`) from **hold-insert time** to **drain/commit time**, so a held slot stays a *valid published slot* (readable by the scanner via `next_expected` and by `S_new` recovery). `kBatchHeaderFlagRetired` is set only at commit.
- Delete per-session credit windows; backpressure emerges from ring fill. **Assert `session_lease < PBR_fill_time` at config load** — else a fenced-timescale stall couples other sessions on the ring (the D3 fairness coupling).

**`RecoverSequencer5State()`** (new, near `topic.cc:3597-3606`):
1. Read `ControlBlock.committed_seq`; scan `GOI[0..committed_seq]` to rebuild per-client committed HWM (`max client_seq`) and `per_client_ordered_`. Authoritative ACKed==committed frontier; never re-ACK below it.
2. Read the CXL `SessionEntry` table → per-session `next_expected` + `fenced`; **min governs** on disagreement with the GOI scan (conservative).
3. Rescan each PBR ring from the per-broker committed frontier; re-classify resident slots vs reconstructed `next_expected` (`==`→ready, `>`→re-hold, `<`→dedup). Because D3 keeps uncommitted held slots resident, the hold buffer is **fully reconstructible** and **no held batch is lost**.

**Purge-on-fence is safe across failover:** a fenced suffix is not resurrected because `S_new` rescans from the committed frontier and the `SessionEntry` records `FENCED`/`committed_hwm` → `S_new` rejects the fenced session's stale-epoch resubmission (D7).

### 6.3 D4 spatial-guard duplicate at S_new (Track-02 Scenario A)

Scenario: after suspected `S_old` failure, a client rendezvous-retransmits batch `n` to a different broker; `S_new` is elected (`epoch++`); the retransmit arrives while `committed_seq` is catching up (guard window). **Three composed rejections** (primitives exist; D4 residual = one audited guard in the S2-unified `classify()`):

1. **Dedup (primary):** `S_new` reconstructs `next_expected` from GOI + `SessionEntry`. `n.client_seq < next_expected` ⇒ already committed ⇒ rejected as late, never a fresh commit. Model **dedup alone as sufficient for safety.**
2. **committed_seq monotonicity:** a duplicate cannot lower `committed_seq`; it yields at most a stale/overlapping `CompletedRange` popped by `CommittedSeqUpdaterThread` (`topic.cc:3132-3136`). Guard input never regresses.
3. **Wrap-fence (liveness-only):** `n.epoch_created < ControlBlock.epoch` (minted under `S_old`); GOI reclamation is gated by the wrap-fence (D2/D6); `epoch_sequenced` on the GOI entry lets readers discard cross-epoch stragglers.

**D4 residual code:** in `classify()`, before assigning a GOI index to a from-scanner batch, assert `(a) epoch_created not below the active epoch floor OR route-to-dedup`, and `(b) client_seq >= reconstructed next_expected`. Add `order5_spatial_guard_rejects_`. One comparison on the fast path, no new atomics. Because dropping a fenced suffix consumes **no** GOI index (W1.3), `committed_seq` stays monotonic and the completed-range tiling stays contiguous.

### 6.4 Stale-CV ACK relay after election (Track-02 Scenario B, D2)

**Gap confirmed:** the only epoch validation (`CheckEpochOnce`/`RefreshBrokerEpochFromCXL`, `topic.cc:1931-1937`) is on the **ingest** path (`network_manager.cc:944`). The **ACK-relay** path (`AckThread`, `network_manager.cc:2330-2360`) reads `GetClientOrdered` with **no** control-block epoch check → a post-election stale sequencer could relay an ACK for an entry a later epoch discards.

**Fix:** stamp each `per_client_ordered_` advance with its producing epoch (`cached_epoch_` at `CommitEpoch`); add `Topic::AckRelayEpochValid()` (reads `ControlBlock.epoch` on the cached cadence, `flush+load_fence` per `kEpochCheckInterval`). In `AckThread`, before each ACK1 advance (`:2330/:2337/:2353`), refuse to advance an ACK whose backing frontier was produced under `epoch < ControlBlock.epoch` — **withhold** (client δ/lease-NACK retransmits) rather than confirm an entry that never becomes visible. Preserves "ACKed iff committed". Cost: one cached-epoch compare + one CXL read per interval (δ single-digit ms makes cadenced read acceptable).

### 6.5 Track-02 TLA⁺ deliverables (coordinate-before-finalizing gate)

Two models must be finalized **before** the D3 / D4-residual / D2-ACK-guard code merges:

- **Scenario A:** State `{epoch, committedSeq, seqId}`, per-client `nextExpected/committedHWM`, durable GOI, inflight msgs. Actions `CommitBatch` (only if `s=nextExpected`), `SequencerFail` (`epoch++`, Recover from `GOI[0..committedSeq]`), `Retransmit` (arrives `epochCreated < epoch`), `ClassifyAtSnew` (dup→reject / fence→reject / eq→commit). **Invariants:** `committedSeq` monotone; `NoDuplicateCommit`; `AckedIffCommitted`. Dedup alone sufficient; wrap-fence liveness-only.
- **Scenario B:** State `{epoch}`, per-client `orderedFrontier[c]` stamped `producingEpoch`, `brokerRelayedAck[c]`. Unguarded `RelayAck` must **violate** `AckedImpliesEventuallyVisible`; guarded `RelayAck` (relay only if `producingEpoch >= controlBlock.epoch`) must **satisfy** it.

---

## 7. S2 Refactor (landing prerequisite) + Implementation Sequencing

The seq-classification decision tree is **duplicated across 4 inner loops** in `ProcessLevel5BatchesShard` (`topic.cc:5255-6230`), all under `true_client_chain = UsesTrueClientChainOrdering(order_)` (`:5258`). For ORDER=5 only L1/L3 execute:

| Loop | Site | Key |
|---|---|---|
| L1 fast/true_chain | `topic.cc:5393` (`for (PendingBatch5* p : ordered)`) | client_id |
| L2 fast/legacy | `topic.cc:5500` (`for (PendingBatch5* p : vec)`) | stream_key |
| L3 general/true_chain | `topic.cc:5612` (`for (auto jt = it; jt != group_end; ++jt)`) | client_id |
| L4 general/legacy | `topic.cc:5723` (`for (auto jt = bit; jt != broker_end; ++jt)`) | stream_key |

**S2 = extract one classifier** so D1 lands in one place instead of four.

```cpp
// topic.h (private Topic members)
enum class L5Class { kDupTerminal, kEmittedTerminal, kEmit, kLate, kHoldFull, kHold, kFenced };
static L5Class ClassifyLevel5(const ClientState5& st, const ClientEmitTracker& em,
                              uint64_t seq, size_t hold_buffer_size);   // pure, no side effects
void ApplyLevel5Record(Level5ShardState& shard, ClientState5& st, ClientEmitTracker& em,
                       uint64_t key, bool true_client_chain, PendingBatch5* p,
                       std::vector<PendingBatch5>& ready,
                       absl::flat_hash_map<uint32_t,uint64_t>& terminalized_delta);
```

`ClassifyLevel5` encodes the tree order **exactly**: `(fenced→kFenced)`, `(is_duplicate&&IsEmitted→kDupTerminal)`, `(IsEmitted→kEmittedTerminal)`, `(seq==next_expected→kEmit)`, `(seq<next_expected→kLate)`, `(hold full→kHoldFull)`, `else kHold`. `ApplyLevel5Record` reproduces each current body **verbatim**, branching on `true_client_chain` at the two divergence points (kEmittedTerminal re-emit, kLate drop-vs-emit) — **do NOT flatten the divergent arms** (that would silently change ORDER=4). Preserve the C3 meta-first/move-last order in every hold-insert body (`5460/5562/5683/5798`).

**Behavior-preservation for ORDER=5:** only L1/L3 run; both feed the same body against the same `(state,emitted)` for the same `client_id` with the same tree order → emitted `ready`, `hold_buffer`, `ClientState5`/`ClientEmitTracker`, CV accumulation, and every `order5_*` counter are byte-identical. The W1.2 gate (`EMBAR_ASSERT_COMMIT_ORDER=1`, 0 violations) is preserved by construction.

### Sequencing (commits)

| Commit | Content | Scope | Gate |
|---|---|---|---|
| **A. S2 extract** | `ClassifyLevel5` + `ApplyLevel5Record`; 4 call sites delegate; seeds left in place | Track-01 only (topic.cc/.h) | Re-run W1.2 (`EMBAR_ASSERT_COMMIT_ORDER=1`) → 0 violations, byte-identical ORDER=5 |
| **B. Seed-unify (C5)** | legacy seeds `topic.cc:5498`/`:5720` → 0 | Track-01, legacy-only (ORDER=5 no-op) | Keep separate from A so drift is isolable |
| **C. Re-key to session_key** | `client_state` + siblings re-keyed to `(client_id<<32|session_epoch)` (§3.2); fields on `OptimizedClientState` (§3.1) | Track-01 | Re-run W1.2 |
| **D. D1 sequencer** | fence the 3 skip sites (§3.3), `PurgeFencedSession`, fenced drop-guard, `fenced_clients_hwm_` publication | Track-01 | broker-kill smoke test |
| **E. Protobuf** | `session.proto` (§4), `SessionEntry` CXL region (§2.3), `RecoverSequencer5State`, ACK-relay epoch guard | Track-01 + Track-04 (CXL layout) + D7 (replication) | **Track-02 TLA⁺ models finalized first** |
| **F. Client** | unACKed buffer, δ, rendezvous, resubmit-under-new-session (§5) | Track-01 client | end-to-end broker-kill |

**Hard ordering:** S2 (A) **before** the re-key (C) — the re-key becomes a one-site change instead of four. D4-residual + D2-ACK-guard **after** Track-02 models them. The re-key (C) must precede D1 (D) because fencing keys on `session_key`.

---

## 8. New / Changed Files & Protobufs

**New files:**
- `src/protobuf/session.proto` — `SessionOpen`, `SessionOpenAck`, `SessionFenced` (additive + versioned).

**Changed — sequencer/CXL:**
- `src/embarlet/sequencer_utils.h:18` — `OptimizedClientState`: add `fenced`, `committed_hwm`, `fence_epoch`, `session_epoch`, `fence()`.
- `src/embarlet/topic.h` — re-key `client_state`(`:886`)/`hold_buffer`(`:888`)/`clients_with_held_batches`(`:890`)/`client_emitted_tracker`/`commit_order_last_seq_` to `session_key`; `MakeClientBrokerStreamKey`(`:68`) folds `session_epoch`; add `L5Class` enum + `ClassifyLevel5`/`ApplyLevel5Record` decls; `order5_session_fences_` counter; `fenced_clients_hwm_` + `FenceRecord` (guarded by `per_client_mu_` `:691`); `IsClientFenced`/`GetClientFenceHwm`/`TakeSessionFence`/`AckRelayEpochValid`/`RecoverSequencer5State` decls; `SessionEntry` accessor/allocator.
- `src/embarlet/topic.cc` — S2 extract at L1-L4 (`:5393/:5500/:5612/:5723`); seed-unify `:5498`/`:5720`; fence the 3 skip sites (expiry `:5985`, hold-full `:5436/:5538/:5657/:5772`, scanner `:5342-5344`); `PurgeFencedSession`; fenced drop-guard in classify; publish fenced HWM in per-client flush (`:6053`); mirror `expected_seq`/`committed_hwm` into `SessionEntry` in `CommitEpoch` (`:4431-4433`, per-epoch batched); D3 held-slot exclusion in `AdvanceConsumedThroughForProcessedSlots` (`:4479/:4485-4489`); move `InvalidateOrder5HeldSlot` (`:173`) to drain/commit; `RecoverSequencer5State`; `AckRelayEpochValid` + epoch-stamped `per_client_ordered_`; D4 spatial-guard reject + `order5_spatial_guard_rejects_`.
- `src/cxl_manager/cxl_datastructure.h:415` — `_pad0` → `uint16_t session_epoch` + `static_assert(offsetof(BatchHeader, session_epoch) < 64)`; add `struct alignas(64) SessionEntry` + `kSessionTableBase`/`kMaxSessions`; document held-slot non-reclamation on `batch_headers_consumed_through`(`:316`).
- `src/cxl_manager/cxl_manager.{cc,h}` — map/init the session-table region; wire into chain replication (D7).

**Changed — network:**
- `src/network_manager/network_manager.cc` — `AckThread` ACK-relay epoch guard before each ACK1 advance (`:2330/:2337/:2353`); capability-gated `SessionFenced` delivery reading `IsClientFenced`/`GetClientFenceHwm`; reconnect fence delivery in `HandlePublishRequest` (`:783`); copy `session_epoch` into ingest slots.
- `src/network_manager/network_manager.h:33` — `EmbarcaderoReq` **unchanged** (session identity rides `session.proto`).

**Changed — client:**
- `src/client/publisher.{h,cc}` — `session_epoch_` (`std::atomic<uint32_t>`), `UnackedBuffer`/`UnackedEntry`, `DeltaEstimator`, `RetransmitThread`, retained-copy pool; buffer-on-send (replace `ReleaseBatch` `:2356`); release-on-ACK + RTT sample + δ (`:1847-1853`); replace round-robin reroute (`:2287`) with `RendezvousBroker`; `SessionFenced` handling; stamp `BatchHeader.session_epoch` (`:2131`).
- `src/client/common.{h,cc}` — `RendezvousBroker(client_id, batch_seq, survivors, exclude)`.
- `src/client/subscriber.*` — read durable committed HWM per session (D5 default-safe).

**Protobufs:** `session.proto` only (new). No existing message changed; the cumulative-ACK `size_t` wire is untouched.

---

## 9. Open Questions for the Human

1. **Fence trigger predicate (highest risk).** Confirm: fence *only* on `force_expire_all_frontiers` (full lease elapsed = client dead/partitioned); steady-state (`GetOrder5HoldTimeoutNs()==0`) never fences. Should an operator-set finite `GetOrder5HoldTimeoutNs` also fence (vs. its current gap-skip)? Recommend: any expiry that *would* have gap-skipped now fences. **Must coordinate exact lease→window arming with D2** — arming too eagerly fences live-but-slow sessions.
2. **D1 anchor scope.** F6 argues D1 = expiry site (`:5985`) only, with hold-full/scanner skips being D3-scoped. This spec fences **all three** (fence is the correct terminal action everywhere). Confirm you want the hold-full (`:5436/:5538/:5657/:5772`) and scanner (`:5342`) sites fenced in commit D, or deferred to D3.
3. **`session_epoch` width.** 16 bits in `BatchHeader` (reuse `_pad0`) with wrap-detection against the CXL stored epoch — sufficient, or steal a second 16 bits for a full 32-bit in-header epoch?
4. **Session-table granularity & placement.** Per-topic (256 KB × topics in the BLog tail) vs a single global table? Confirm Track-04 (CXL accessor layer) accepts the additive `kSessionTableBase` region and that D7 replicates it.
5. **SessionEntry slot reclamation.** Open-addressed probe with never-freed slots fills 4096 under long runs. When is a fenced/dead session's slot freed (GC frontier, ties to D6)?
6. **Fenced shard entry retention.** Retain a fenced shard entry past `kClientTtlEpochs` to answer very-late reconnects locally, or always defer to the head `fenced_clients_hwm_` map? Recommend the head map is authoritative.
7. **committed_msg_hwm necessity.** Given `committed_batch_seq`, is `committed_msg_hwm` needed, or can the client reconstruct message offsets from its own unACKed buffer? Recommend keeping it (belt-and-suspenders; lets the client validate its buffer).
8. **Track-02 invariant shapes.** Confirm the exact invariant names/shapes for Scenarios A & B (§6.5) before finalizing the D3 / D4-residual / D2-ACK-guard code (coordinate-before-finalizing gate). Also confirm the minting-epoch field the guards read (`epoch_created` vs `epoch_sequenced`), and the GOI-scan-vs-session-table `min` rule on committed-HWM disagreement.
9. **C2 co-design.** `CommittedSeqUpdaterThread` pending-unboundedness on a permanent GOI gap (`code_review_w1.md:79-100`) intersects D3/D4; advancing `committed_seq` past an unwritten GOI index needs a GOI-poisoning convention. Confirm this is co-designed, not landed independently.

---

*Verification note: all `topic.cc` line anchors (`5342/5436/5538/5657/5772/5985` skips; `5393/5500/5612/5723` classifier loops; `5940-6000` expiry loop; `1514-1517` GetClientOrdered; `4433/6053` per_client_ordered flush) and struct anchors (`sequencer_utils.h:18`, `topic.h:95/886/888/890/859`, `cxl_datastructure.h:83/394/397/414/415/440-446/712-713`) confirmed against the working tree. F1/F3 cited stale pre-fix numbers (e.g. 5928/5946); F6's post-fix numbers are authoritative and used throughout.*
