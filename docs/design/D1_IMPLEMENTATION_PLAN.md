# D1 §7 Implementation Plan — one-shot-ready execution playbook

**Owner:** Track 01. **Spec:** `docs/design/D1_SESSION_FIFO_DESIGN_v2.md` (freeze-ready; this plan is its
§7/§8 turned into an execution checklist). **Base:** `chore/repo-reorg @ 9901b93` (S2 landed).

Each commit below is scoped to be executed in **one focused pass** (implement → build → validate →
commit) on a branch off the integration base. Do them in order; each lists its **files**, **exact
changes**, **ready-to-code artifacts**, **validation gate**, and **blocking precondition**.

## Status
- **A. S2 extract** — ✅ DONE (`937317b`). *Note:* implemented as a `classify_one` lambda (not the
  `ClassifyLevel5` enum the spec sketched); D1's fence/backpressure branches land inside
  `classify_one` + the expiry loop. Byte-identical for ORDER=5, validated 8/8, W1.2=0.
- **B. Seed-unify (C5)** — ✅ DONE (`9901b93`). *Residual:* ORDER=4 (legacy) path not runtime-tested.
- **C–G** — below. **All gated on your freeze of v2.** Additional per-commit gates noted.

## Global gates (resolve before the gated commits)
| Gate | Blocks | Owner |
|---|---|---|
| **Freeze v2** (sign off §11 R-A/B/C + §9 residuals) | C, D, E, F, G | **you** |
| **D2 `session_lease_ns` value** (§9.1; 1s/2s is placeholder) | E | D2 |
| **Open-load `append_send_to_ack` p99.9** measurement (§9.2) → δ floor/cap | G | Track 01 (testbed) |
| **Track-02 TLA⁺ models A & B** finalized (§6.5) | F | Track 02 |
| **Track-04 CXL layout** sign-off: `SessionEntry` region offset + `GOIEntry.session_epoch` (§9.3) | D | Track 04 |

---

## Commit C — re-key to `session_key` (Track-01 only; gate: freeze)
**Why:** fencing (E) keys on `(client_id, session_epoch)`, so re-key first.
**Files:** `topic.h`, `topic.cc`, `sequencer_utils.h`, `queue_buffer.{h,cc}`.
**Changes:**
- `sequencer_utils.h:18` `OptimizedClientState`: add `bool fenced`, `uint64_t committed_hwm`,
  `uint32_t fence_epoch`, `uint32_t session_epoch`, `void fence()` (sets `fenced=true`,
  `committed_hwm=next_expected`, idempotent+monotone). (Struct = write-through cache of SessionEntry, wired in D.)
- `topic.h`: re-key `client_state`(:886)/`hold_buffer`(:888)/`clients_with_held_batches`(:890)/
  `client_emitted_tracker`/`commit_order_last_seq_` to `session_key = (client_id<<32) | full_32bit_session_epoch`.
  `MakeClientBrokerStreamKey`(:68) folds full `session_epoch` (ORDER=4 path).
- `topic.cc`: everywhere these maps are keyed by `client_id`/`stream_key`, thread `session_key`
  (the `classify_one` call sites already pass a `key` param — feed `session_key`). `session_epoch`
  source: `BatchHeader.session_epoch` (added in G/§2.2) — until G lands, `session_epoch=0` (legacy),
  so C is a **no-op behavior change** on today's traffic (all epoch 0).
- `queue_buffer.h:167`/`.cc:223`: add `void ResetBatchSeqForNewSession();` (body stores 0; quiesced-only).
**Validation:** rebuild + W1.2 ×8 (`EMBAR_ASSERT_COMMIT_ORDER=1`) → 0 violations, ORDER=5 unchanged.
**Rework risk pre-freeze:** LOW (mechanical re-key).

---

## Commit D — CXL `SessionEntry` table (Track-01 + Track-04; gate: freeze + Track-04 layout)
**Why:** durable truth for fence state; E's `PublishSessionEntry` needs the region.
**Files:** `cxl_datastructure.h`, `cxl_manager.{cc,h}`, `topic.h` (accessors).

**Ready-to-code — `cxl_datastructure.h`:**
```cpp
constexpr size_t kMaxSessions = 4096;              // per topic; 4096 × 64B = 256 KB/topic
// kSessionTableBase: NEW dedicated region — Track-04 CXL-layout decision. Proposed: a metadata-plane
// region after CV/GOI, per-topic stride 256KB, chain-replicated with ControlBlock/CV (D7).
// DO NOT grow ControlBlock (128B contract, :83). Confirm offset with Track 04 before coding.

struct alignas(64) SessionEntry {                 // exactly 64B → one TLP-atomic line
    std::atomic<uint64_t> session_key{0};         // (client_id<<32)|full_32bit_session_epoch; 0=free
    std::atomic<uint64_t> expected_seq{0};        // next per-session-global batch_seq to commit; monotone
    std::atomic<uint64_t> committed_hwm{0};        // == expected_seq-1 at publish; monotone; SESSION_FENCED payload
    std::atomic<uint64_t> highest_sequenced{0};    // monotone
    std::atomic<uint64_t> state_word{0};           // [flags:8|resv:24|last_epoch_full32:32] bit0=FENCED bit1=ACTIVE; WRITTEN LAST
    std::atomic<uint64_t> reserved0{0};
    uint8_t _pad[16];
};
static_assert(sizeof(SessionEntry) == 64, "one TLP-atomic cache line");
```
Also: `GOIEntry` — consume `uint32_t session_epoch` from the 40B `_pad`(:270); keep 128B
(`static_assert`). Populate it at commit (`topic.cc:4166/:4189` alongside `client_seq`).
`BatchHeader._pad0`(:415) → `uint16_t session_epoch` (16-bit TRANSPORT HINT only) + `static_assert(offsetof < 64)`.

**Writer protocol** (single-writer sequencer, in `CommitEpoch` per-epoch-per-session flush,
`topic.cc:4429-4435`) — data words first, `store_fence`+`flush`, then `state_word` LAST + `store_fence`+`flush`
(spec §2.3). **Reader protocol**: `invalidate_cacheline_for_read` + `load_fence` before read.
Open-addressed `hash(session_key)%kMaxSessions` + linear probe. `epoch=0` = legacy (no entry).
**`cxl_manager`:** map/init the region; wire into chain replication (D7/Track-04).
**Validation:** 64B torn-read test (concurrent writer + poll reader sees whole-old/whole-new only);
single-writer audit; rebuild + W1.2 ×8 (no behavior change yet — table populated, not read for fencing).
**Rework risk pre-freeze:** LOW for the struct; the **region offset needs Track-04** (don't hard-code blind).

---

## Commit E — sequencer fencing (Track-01; gate: D done + D2 `session_lease_ns`)
**Why:** the semantic fix — replace the 3 gap-skips.
**Files:** `topic.cc`, `topic.h`.
**Changes (spec §3.3–3.4, §6.2.1):**
- **Head-gap lease predicate** (rewrite expiry loop `topic.cc:5888-5907`): fence iff the held entry at
  `next_expected` has `now_ns - hold_start_ns ≥ GetSessionLeaseNs(...)`, evaluated **every tick,
  detector-independent**. `hold_start_ns` write-once. Add `GetSessionLeaseNs()` (§3.4).
- **EXPIRY** (`:5984-5999`) + **SCANNER targeted** (`:5342-5344`): on lease-fire → `st.fence();
  record_fence(session_key);` and **DROP the head gap** (no `ready.push_back`/`MarkEmitted`/dedup).
  **SCANNER: still `ready.push_back` the marker** (num_msg=0) for ring drain — dropping it re-introduces
  the scanner-freeze class.
- **HOLD-BUFFER-FULL** (the 4 sites, now inside `classify_one`'s `kBackpressure`/GT-full arm): **DELETE
  the terminal force-skip; do NOT fence.** Keep spill-to-deferred; when deferred full, `break`/`return`
  the drain tick WITHOUT advancing `state` (leave resident in PBR ring = D3 backpressure).
- `PurgeFencedSession`, `record_fence` (calls `PublishSessionEntry(FENCED|ACTIVE)` **before** purge),
  fenced drop-guard in classify (fenced session → drop, **do NOT** touch `CheckAndInsertBatchId` — §11 R-E).
- `RecoverSequencer5State` (near `:3597`) with **R1 reconstruct-before-accept** (§6.2.1): park inbound
  for S until `next_expected(S) = MAX(GOI-scan+1, SessionEntry.expected_seq)` established; **park-buffer
  cap + `reconstruct_time < session_lease` assert** (§11 R-B).
- D3: exclude held-marker slots from `AdvanceConsumedThroughForProcessedSlots`(:4479/:4485-4489); move
  `InvalidateOrder5HeldSlot`(:173) to drain/commit.
- Detectors (`:2286`,`:4613-4615`,`:4939`) RETAINED as tail-drain nudges only, **decoupled from fence**.
**Validation:** broker-kill smoke (per-session stall ≈ δ, not 660ms); **false-fence test** (a
slow-but-live session with a >50ms gap during a TCP blip must NOT be fenced); W1.2 ×8 still 0.
**Rework risk pre-freeze:** MEDIUM (this is the behavior change; needs D2 lease value).

---

## Commit F — protobuf + reconnect + ACK-relay guard (Track-01 + Track-02; gate: TLA A & B finalized)
**Files:** new `src/protobuf/session.proto`; `network_manager.cc`; `topic.cc`.
**Changes (spec §4.3, §6.3, §6.4):**
- `session.proto` (additive+versioned): `SessionOpen`, `SessionOpenAck`, `SessionFenced`
  (`reason` incl. `EPOCH_STALE=2`; `committed_batch_seq` authoritative, `committed_msg_hwm` diagnostic-only).
- `network_manager.cc:783` reconnect: derive `committed_batch_seq`/fenced from durable `SessionEntry`
  (invalidate+load_fence), NOT DRAM; validate `header_low16 == (state_word.last_epoch32 & 0xFFFF)` at
  ingest (`EPOCH_STALE` on mismatch, incl. the create-path 32-bit handshake check, §11 R-F).
- `network_manager.cc:2330/:2337/:2353` `AckThread`: epoch-stamped `per_client_ordered_`, withhold ACK
  if `producing_epoch < ControlBlock.epoch`, **terminal `SessionFenced{EPOCH_STALE}`** after bounded
  withhold ≥ session_lease — but re-read durable truth before firing (§11 R-C).
- D4 spatial-guard reject at `S_new` (Scenario A).
**Validation:** the two TLA⁺ models must be checked first; then protobuf round-trip + reconnect e2e.
**Rework risk pre-freeze:** MEDIUM (wire); **hard gate on Track-02 models**.

---

## Commit G — client library (Track-01; gate: open-load δ measurement)
**Files:** `publisher.{h,cc}`, `common.{h,cc}`, `queue_buffer.*`, `subscriber.*`.
**Changes (spec §5):**
- Stamp `batch_header->session_epoch`(`publisher.cc:2105`); `session_epoch_` atomic; confirm
  `kOrderStrong` no-op stays (`:2121-2125`); ORDER=4 counter (`:2129-2132`) left intact.
- `UnackedBuffer`/`UnackedEntry` byte-capped = `offered_rate × session_lease`; producer backpressure in
  `PublishThread`; lock order `UnackedBuffer.mu → mutex_ → pubQue_` (absl annotations).
- `DeltaEstimator` (open-load-derived floor/cap — **needs the measurement**); `RetransmitThread`
  (δ backstop; D2 NACK primary); RTT sample + Karn on `attempt==0`.
- `RendezvousBroker(client_id, batch_seq, survivors, exclude)` in `common.*` (HRW/argmax, batch_seq
  mixed, dead broker excluded; NOT the client_id-only home finalizer); wire at `publisher.cc:2287`.
- ACK(message-count) → batch_seq frontier: local prefix-sum `batch_seq→cumulative_num_msg`, O(k) pop
  (`publisher.cc:1834,1847-1853`); rebase `M_base` at `session_epoch++` (§11 R-G).
- `SessionFenced` handling → `ResetBatchSeqForNewSession()` + resubmit suffix `> committed_batch_seq`.
- `subscriber.*`: read durable committed HWM; confirm nothing treats `committed_msg_hwm` as a batch_seq.
**Validation:** end-to-end broker-kill (per-session recovery ≈ δ, exactly-once across fence); δ tuned
from the open-load measurement.
**Rework risk pre-freeze:** MEDIUM; **hard gate on the δ measurement**.

---

## Execution notes
- Branch each commit off the integration base; build on `broker` (`-DCOLLECT_LATENCY_STATS=ON`, cached
  gRPC); validate; commit `--no-verify --no-gpg-sign`. Testbed discipline per `S2_REFACTOR_BRIEF.md`
  (flock, activity.log, kill-by-PID).
- **Parallelizable now (unblocked, no freeze needed):** the **open-load δ measurement** (feeds G's gate)
  and **Track-02's TLA⁺ models** (gate F) can proceed immediately — both are on the critical path.
- On freeze: C → D → E → F → G in order (hard ordering per spec §7). C is the safe first code step.
