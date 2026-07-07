# Implement D1 (Session-Based FIFO with Fencing) on the Embarcadero Testbed

## 1. Mission & What D1 Is

You are implementing **D1 — session-based FIFO ordering with fencing**, the Embarcadero paper's #1 contribution, now **design-frozen** (do not relitigate frozen decisions). D1 gives Embarcadero an *exactly-once, gap-free, duplicate-free per-session ordering guarantee that survives broker failover*. Today, when a batch goes missing at the head of a client's stream, the sequencer does one of three **gap-skips** (silently drops the missing slot and moves on), which breaks FIFO. D1 replaces those gap-skips with **session fencing**: a session `S = (client_id, session_epoch)` is fenced iff a gap at its next-expected sequence outlives a full *session lease*; a fenced session commits nothing beyond its committed high-water-mark, and the client re-opens a new session generation that resubmits exactly the un-committed suffix. The boxed guarantee: for any session, batches committed to the GOI (keyed by one monotone per-session-global `batch_seq`) form a **gap-free, duplicate-free prefix** of the client's submission stream; late/duplicate/cross-broker-retransmit arrivals are **rejected, never reordered, never re-emitted**; the union over the generation chain `S1 ⊂ S2 ⊂ …` is exactly-once across fencing. This is a **multi-commit** effort (commits C→G, hard-ordered). You work **incrementally**: build + run the ORDER=5 gate after **each** commit, and commit per the plan. You have NO prior context — read the ramp-up list first.

## 2. Fresh-Clone + Build Setup

**Do NOT work in `~/Embarcadero` (the human's live tree) or any other `~/Embarcadero-sessions/*` dir.** Set up a fresh clone:

```bash
git clone https://github.com/jaewan/Embarcadero.git ~/Embarcadero-sessions/d1-impl
cd ~/Embarcadero-sessions/d1-impl
git checkout main            # must be at 2ab8d48
git rev-parse HEAD           # confirm 2ab8d48
git switch -c d1-impl
```

**Build (run OUTSIDE the flock lock — see §7):**

```bash
cmake -S . -B build \
  -DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero-main-ab/build/_deps/grpc-src \
  -DCOLLECT_LATENCY_STATS=ON && \
cmake --build build -j64
```

- The cached gRPC dir (`-DFETCHCONTENT_SOURCE_DIR_GRPC=…`) avoids re-fetching gRPC — do not omit it.
- `-DCOLLECT_LATENCY_STATS=ON` is **REQUIRED** — it emits the stage-latency CSVs (`pub_latency_stats.csv` / `stage_latency_summary.csv`) used for all latency validation.
- The build binary you run is `broker` / `embarlet`. Rebuild after every commit's changes before running the gate.

## 3. Ramp-Up Reading List (read IN THIS ORDER before touching code)

1. `docs/design/D1_SESSION_FIFO_DESIGN_v2.md` — **the frozen design (v2 FREEZE-READY + §11 R-A..R-H).** The authoritative spec: the guarantee theorem, the 4 lemmas, the core mechanisms (per-session-global `batch_seq`, `expected_seq` dedup, hold-expiry→fencing, fence predicate §3.4, rendezvous), the §5.2 adaptive-δ estimator, §7 sequencing A–G, §9 residuals, and the §11 code-constraining resolutions R-A..R-H. **§11 overrides earlier prose where it conflicts (esp. R-A).**
2. `docs/design/D1_IMPLEMENTATION_PLAN.md` — the commit-by-commit plan (C→D→E→F→G): exact files, functions, line anchors, gates, and rework-risk per commit.
3. `docs/design/D1_TLA_SCENARIOS_FOR_TRACK02.md` — the TLA+ gating scenarios for commit F (Scenario A `dup_in_guard_window`, Scenario B `stale_cv_ack_relay` + its bug-demo twin) and the "coordinate-before-finalizing" gate §6.5.
4. `spec/README.md` + `spec/results/SUMMARY.md` — "TLA+ first, code second" gate; the machine-checked safety invariants and their PASS results; the checked-vs-argued boundary.
5. `docs/experiments/w1_findings.md` + `docs/experiments/code_review_w1.md` — the ORDER=5 scanner-freeze history, the pass bar, and the fast-path no-regression evidence.
6. `docs/experiments/DELTA_MEASUREMENT_BRIEF.md` + `docs/experiments/delta_measurement_results.md` — the open-load δ measurement (gate for G); frozen constants `kDeltaFloorMs=1.7`, `kDeltaCapMs=12`.

After reading, skim the code loci in §6 below so you know where each change lands.

## 4. Incremental Commit Plan (C → D → E → F → G — HARD ORDER, no reordering)

**Ordering rationale (spec §7):** A (S2 extract) and B (seed-unify) are ALREADY DONE upstream. C must land before E because fencing keys on `session_key = (client_id, session_epoch)` — re-key first. D must land before E because E's `PublishSessionEntry` needs the SessionEntry CXL region. E is gated on D done + the D2 `session_lease_ns` value. F must land after the Track-02 TLA+ models A & B are finalized and passing. G is last, gated on the open-load δ measurement. **Branch each commit's work on `d1-impl`; build + run the ORDER=5 gate + commit `--no-verify --no-gpg-sign` after each.**

Until G stamps `BatchHeader.session_epoch`, **`session_epoch=0` (legacy) everywhere**, so C, D behave as **no-op behavior changes on current traffic** — legacy epoch32==0 skips all fence logic. Do not expect C/D to change runtime behavior; the gate is "byte-identical ORDER=5, 0 violations."

---

### COMMIT C — Re-key to `session_key` (mechanical; risk LOW)

**Purpose:** fencing (E) keys on `(client_id, session_epoch)`, so re-key first.
**Files:** `sequencer_utils.h`, `topic.h`, `topic.cc`, `queue_buffer.{h,cc}`.
**Changes:**
- (a) `sequencer_utils.h:18` `OptimizedClientState`: add `bool fenced`, `uint64_t committed_hwm`, `uint32_t fence_epoch`, `uint32_t session_epoch`, and `void fence()`. `fence()` must be **idempotent + monotone**: `if(!fenced){fenced=true; committed_hwm=next_expected;}` — never un-fence, never lower `committed_hwm`. The struct is a **write-through cache** of `SessionEntry` (actual CXL wiring lands in D, not C).
- (b) `topic.h`: re-key `client_state(:886)`, `hold_buffer(:888)`, `clients_with_held_batches(:890)`, `client_emitted_tracker`, `commit_order_last_seq_` to `session_key = (uint64_t(client_id)<<32) | full_32bit_session_epoch`. `MakeClientBrokerStreamKey(:68)` folds the full session_epoch (ORDER=4 path).
- (c) `topic.cc`: thread `session_key` through all maps keyed by `client_id`/`stream_key`. `classify_one` call sites already pass a `key` param — feed `session_key`. `session_epoch` source = `BatchHeader.session_epoch` (added in G/§2.2); until G lands `session_epoch=0`.
- (d) `queue_buffer.h:167` / `queue_buffer.cc:223`: add `void ResetBatchSeqForNewSession();` (body stores `0`; **quiesced-only, single-producer**).

**Invariant served:** session identity + write-through fence cache; L2 monotonicity.
**Gate:** rebuild + ORDER=5 W1.2 x8 with `EMBAR_ASSERT_COMMIT_ORDER=1` → **0 violations, ORDER=5 byte-identical.**

---

### COMMIT D — CXL SessionEntry table (risk LOW for struct; region offset gated on Track-04)

**Purpose:** durable truth for fence state; E's `PublishSessionEntry` needs this region.
**Files:** `cxl_datastructure.h`, `cxl_manager.{cc,h}`, `topic.h` (accessors).
**Changes:**
- Add **verbatim** to `cxl_datastructure.h` (~lines 53–68):
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
- **`kSessionTableBase` is a NEW dedicated region** (Track-04 decision). **DO NOT grow `ControlBlock`** (128B contract, `cxl_datastructure.h:83`). **STOP and confirm the region offset with Track-04 before coding it — do NOT hard-code blind.**
- `GOIEntry`: consume `uint32_t session_epoch` from the 40B `_pad(:270)`, keep 128B (`static_assert`), populate at commit (`topic.cc:4166` / `:4189` alongside `client_seq`).
- `BatchHeader._pad0(:415)` → `uint16_t session_epoch` (**16-bit TRANSPORT HINT only**) + `static_assert(offsetof(BatchHeader, session_epoch) < 64)`. SessionEntry/GOIEntry carry the **full 32-bit** epoch; BatchHeader is a hint only.
- **Writer protocol (spec §2.3):** single-writer sequencer in `CommitEpoch` per-epoch-per-session flush (`topic.cc:4429–4435`): **data words first** (store relaxed) → `store_fence`+`flush_cacheline`+`store_fence` → **`state_word` LAST** (`store(release)`) → `store_fence`+`flush`+`store_fence`.
- **Reader protocol:** `invalidate_cacheline_for_read` + `load_fence` before read; `state_word.load(acquire)`; `committed_hwm.load(acquire)`.
- Open-addressed `hash(session_key)%kMaxSessions` + linear probe; `epoch=0` = legacy (no entry). `cxl_manager`: map/init the region; wire into chain replication (D7/Track-04).

**Invariants served:** durable fence truth; L2 monotonicity; torn-read freedom.
**Gate:** 64B torn-read test (concurrent writer + poll reader sees whole-old / whole-new only); single-writer audit; rebuild + ORDER=5 W1.2 x8 (**no behavior change yet** — table populated, not read for fencing). Keep `SessionEntry` **exactly 64B** (`alignas(64)`, `_pad[16]` balance) and `GOIEntry`/`BatchHeader`/`ControlBlock` at their existing sizes.

---

### COMMIT E — Sequencer fencing (THE behavior change; risk MEDIUM; gated on D done + D2 lease value)

**Purpose:** the semantic fix — replace the 3 gap-skips with fencing.
**Files:** `topic.cc`, `topic.h`.
**Changes (spec §3.3–3.4, §6.2.1, and §11 R-A/R-B/R-E):**
- (a) **Head-gap lease predicate — §11 R-A CRITICAL.** The §3.4 `find(next_expected)` resident-slot predicate **NEVER fires** on the canonical lost-batch case (nothing resident). **Do NOT implement the literal §3.4 code.** Instead use the R-A **explicit per-session gap clock**: store `gap_since_ns` in DRAM `ClientState5`; stamp it the **first** time a batch arrives at `seq > next_expected` while `next_expected` is unfilled (write-once; clear when `next_expected` advances; on failover set = `SteadyNowNs()`). **Fence iff `now_ns - gap_since_ns >= GetSessionLeaseNs(...)`, evaluated EVERY tick, detector-independent.** Rewrite the expiry loop at `topic.cc:5888-5907`. Add `GetSessionLeaseNs()` (§3.4): `default_ms = replicated ? 2000 : 1000` (**PLACEHOLDER pending D2 — STOP if D2 hasn't supplied the value**); env `EMBARCADERO_SESSION_LEASE_MS` overrides. **DELETE** the `kForceExpireMinAgeNs=50ms` floor from the fence decision (it false-fences live clients). `GetOrder5HoldTimeoutNs()` stays `0` and is NOT the fence clock.
- (b) **EXPIRY (`:5984-5999`) + SCANNER targeted (`:5342-5344`):** on lease-fire → `st.fence()`; `record_fence(session_key)`; **DROP the head gap** (no `ready.push_back`, no `MarkEmitted`, no dedup). **EXCEPTION:** the SCANNER path **MUST STILL `ready.push_back` the skip marker (`num_msg=0`)** for ring drain — dropping it re-introduces the `BrokerScannerWorker5` freeze.
- (c) **HOLD-BUFFER-FULL (4 sites: `:5436,:5538,:5657,:5772`, now inside `classify_one` kBackpressure / GT-full arm):** **DELETE the terminal force-skip; do NOT fence** (v1's `state.fence()` here is REVERSED). Keep spill-to-deferred; when deferred is full, **break/return the drain tick WITHOUT advancing state** (leave resident in the PBR ring = D3 backpressure).
- (d) `PurgeFencedSession`; `record_fence` calls `PublishSessionEntry(FENCED|ACTIVE)` **DURABLE BEFORE** purge. **Fenced drop-guard** in classify (fenced session → drop, and **do NOT touch `CheckAndInsertBatchId`** — §11 R-E; it churns the shared dedup ring).
- (e) `RecoverSequencer5State` (near `topic.cc:3597`) with **R1 reconstruct-before-accept** (§6.2.1): park inbound for `S` until `next_expected(S) = MAX(GOI-scan+1, SessionEntry.expected_seq)`. **§11 R-B:** add a **park-buffer cap** (like `kHoldBufferMaxEntries`) with NACK/backpressure on overflow (**never drop**), plus a config assertion `recovery_reconstruct_time < session_lease`.
- (f) **D3:** exclude held-marker slots from `AdvanceConsumedThroughForProcessedSlots(:4479/:4485-4489)`; move `InvalidateOrder5HeldSlot(:173)` to drain/commit.
- (g) **Detectors (`:2286,:4613-4615,:4939`) RETAINED as tail-drain nudges ONLY** — decoupled from fence. They must NOT trigger a fence.

**Invariants served:** L1 (sole dedup), L2 (monotone), L3 (reconstruct-before-accept), `FencedSuffixNeverCommitted`, `PerSessionPrefix`, `LateNeverReordered`.
**Gate:** broker-kill smoke (per-session stall ≈ δ, not 660ms); false-fence test (a slow-but-live session with a >50ms gap during a TCP blip **must NOT be fenced**); ORDER=5 W1.2 x8 still **0 violations**.

---

### COMMIT F — protobuf + reconnect + ACK-relay guard (risk MEDIUM; HARD-GATED on Track-02 TLA+ models A & B)

> **DO NOT start F code/e2e until Track-02's TLA+ Scenarios A & B are finalized AND passing (see §5). "TLA+ first, code second."** Verify the spec passes first (`bash spec/run_all.sh`), then land the code.

**Files:** new `src/protobuf/session.proto`; `network_manager.cc`; `topic.cc`.
**Changes (spec §4.3, §6.3, §6.4, §11 R-C/R-D/R-F/R-G):**
- (a) `session.proto` (additive+versioned): `SessionOpen`, `SessionOpenAck{committed_hwm,status,assigned_session_epoch}`, `SessionFenced{client_id,session_epoch,committed_batch_seq(field 3, AUTHORITATIVE resubmit key),committed_msg_hwm(field 4, DIAGNOSTIC ONLY),control_epoch,reason}`; `reason {HOLD_EXPIRY=1, EPOCH_STALE=2, LEASE_LOST=3, DUP_PAST_EXPECTED=4}`. **`committed_msg_hwm` is NEVER a batch_seq / release axis.**
- (b) `network_manager.cc:783` reconnect: derive `committed_batch_seq`/`fenced` from the **durable `SessionEntry`** (`invalidate`+`load_fence`), **NOT DRAM**. **§11 R-D:** reconnect-answer `committed_hwm = MAX(GOI-derived, SessionEntry)` (SessionEntry lags GOI up to one epoch with lazy flush). Validate `header_low16 == (state_word.last_epoch32 & 0xFFFF)` at ingest (`EPOCH_STALE` on mismatch; **§11 R-F:** on the CREATE path validate the **full 32-bit** handshake epoch, not just the 16-bit hint).
- (c) `network_manager.cc:2330/:2337/:2353` AckThread: **epoch-stamped** `per_client_ordered_`; **WITHHOLD** any ACK whose backing frontier's `producing_epoch < ControlBlock.epoch` (let the client δ/lease-NACK retransmit — do NOT confirm); fire a **terminal `SessionFenced{EPOCH_STALE}`** after a bounded withhold `>= session_lease` (bounds the liveness hole). **§11 R-C:** before firing the terminal fence, **re-read the durable `SessionEntry`/GOI committed truth** — only escalate if genuinely uncommitted under `ControlBlock.epoch`. Add `Topic::AckRelayEpochValid()` (reads `ControlBlock.epoch` on `kEpochCheckInterval` cadence) and `cached_epoch_` stamped at `CommitEpoch`.
- (d) **D4 spatial-guard reject at `S_new` (Scenario A):** in `classify()`, before assigning a GOI index to a from-scanner batch, assert (a) `epoch_created` not below the active epoch floor OR route-to-dedup, and (b) `client_seq >= reconstructed next_expected`; add counter `order5_spatial_guard_rejects_`. **Dropping a fenced suffix consumes NO GOI index** so `committedSeq` stays monotone. **§11 R-G:** at `session_epoch++`, snapshot per-`client_id` cumulative ACK count as `M_base`; compute the new-generation frontier from `M - M_base`.

**Invariants served:** `AckedIffCommitted`, `NoDupCommit`, `NoWrap`, L4 (ACK epoch validity).
**Gate:** the **two TLA+ models checked FIRST** (§5); then protobuf round-trip + reconnect e2e; ORDER=5 W1.2 x8 still 0.

---

### COMMIT G — client library (risk MEDIUM; HARD-GATED on the open-load δ measurement)

> **DO NOT tune δ until the open-load `append_send_to_ack` p99.9 measurement gate is CLOSED (see `docs/experiments/delta_measurement_results.md`). Frozen constants: `kDeltaFloorMs=1.7`, `kDeltaCapMs=12.0` — do not change these.**

**Files:** `publisher.{h,cc}`, `common.{h,cc}`, `queue_buffer.*`, `subscriber.*`.
**Changes (spec §5, §11 R-G):**
- (a) Stamp `batch_header->session_epoch` at `publisher.cc:2105` (`= static_cast<uint16_t>(session_epoch_.load(relaxed))`); `session_epoch_` atomic; **confirm `kOrderStrong` no-op stays** (`:2121-2125` — batch_seq NOT reassigned); ORDER=4 counter (`:2129-2132`, `order5_batch_seq_per_broker_` at `publisher.h:194`) **left intact**.
- (b) `UnackedBuffer`/`UnackedEntry` byte-capped = `offered_rate_bytes_per_sec * session_lease_seconds`; producer backpressure in `PublishThread`; **strict lock order `UnackedBuffer.mu → mutex_ → pubQue_`** (absl annotations where possible; QueueBuffer mutexes are `std::mutex` → **manual audit**; never hold `UnackedBuffer.mu` across a send or a `mutex_` acquisition).
- (c) `DeltaEstimator { double srtt_ms{0}, rttvar_ms{0}; bool seeded{false}; std::atomic<double> delta_ms{kDeltaFloorMs}; }`; `delta_ms = clamp(srtt + 4*rttvar, kDeltaFloorMs=1.7, kDeltaCapMs=12.0)`, α=1/8, β=1/4 (RFC6298). `RetransmitThread` (started near `publisher.cc:758`; every `min(δ/2, 2ms)`; `backoff(δ,attempt)=δ*2^attempt`; **silent-loss BACKSTOP only** — D2 lease-NACK is PRIMARY). **Karn:** sample `R = now - first_send_ts` **ONLY** when `attempt==0`; **NEVER** when `attempt>0` (`publisher.cc:1834`/`:1847-1853`). This keeps srtt/rttvar biased low so δ ≈ floor in normal operation — intended.
- (d) `RendezvousBroker(uint32_t client_id, uint64_t batch_seq, const std::vector<int>& survivors, int exclude_broker)` in `common.*`: `score_b = splitmix64_finalize(mix(client_id, batch_seq, b))` for `b in survivors, b != exclude_broker`; return `argmax_b`. **HRW/argmax mixing batch_seq, excluding the dead broker.** **DO NOT reuse** the ORDER=5 home finalizer (`publisher.cc:323-327/:330`) — it hashes `client_id` ALONE then `% ALL brokers` (herd collision bug). Snapshot the survivor set ONCE under `mutex_`. Replaces the round-robin reroute at `publisher.cc:2287`. Maintain ≥2 live broker sockets when the cluster has ≥2 brokers.
- (e) ACK(message-count) → batch_seq frontier: local prefix-sum `batch_seq → cumulative_num_msg`, O(k) pop (`publisher.cc:1834,1847-1853`); **§11 R-G:** rebase `M_base` at `session_epoch++`.
- (f) `SessionFenced` handling → `ResetBatchSeqForNewSession()` + resubmit the suffix `> committed_batch_seq`.
- (g) `subscriber.*`: read the durable committed HWM; **confirm nothing treats `committed_msg_hwm` as a batch_seq** (grep audit).

**Invariants served:** L1, L4, `AckedIffCommitted`, exactly-once across fence.
**Gate:** end-to-end broker-kill (per-session recovery ≈ δ, **exactly-once across fence**); δ tuned from the open-load measurement; ORDER=5 W1.2 x8 still 0.

## 5. Correctness Invariants (never break) + the TLA Gate for Commit F

**Machine-checked SAFETY invariants (TLC-verified; code must never violate):**
- `NoDupCommit` — total order + exactly-once, incl. across fence/re-open.
- `PerSessionPrefix` — strictly-increasing, contiguous-from-1, never reordered.
- `LateNeverReordered` — a late batch is never reordered.
- `FencedSuffixNeverCommitted` — session death, not gap-skip.
- `NoWrap` — spatial/wrap-fence.
- `AckedIffCommitted` — committed + durable + live (the stale-CV target).
- `ReaderAgreement` — durable-reaching positions never reorder/change identity; sole divergence is payload visibility ("lost").

One-line invariant the two F scenarios preserve (verbatim): **"a batch is ACKed iff committed, and a fenced/late/duplicate batch is never committed or reordered."**

**Runtime invariant (checked every trial):** per-session in-order commit — `client_seq` strictly greater than the last committed `client_seq` per `client_id`. `EMBAR_ASSERT_COMMIT_ORDER=1` turns this into a hard CHECK/abort. Deliberate gap-skips advance monotonically so they do NOT trip it; only a true reorder does.

**The Commit-F TLA gate (HARD — do before F code/e2e):**
- **Scenario A** = `spec/dup_in_guard_window.cfg` — a retransmitted dup arriving at `S_new` inside the D4 spatial-guard window. Must PASS preserving `committedSeq` monotone, `NoDuplicateCommit`, `AckedIffCommitted`. Design conclusion baked in: **dedup alone is sufficient for safety; wrap-fence is liveness-only** (code must NOT rely on wrap-fence for safety; dropping a fenced suffix consumes NO GOI index).
- **Scenario B** = `spec/stale_cv_ack_relay.cfg` (fix ON) — stale-CV ACK relay after election WITH the epoch-check fix. Must PASS (satisfy `AckedIffCommitted`).
- **Scenario B twin** = `spec/stale_cv_bug_demo.cfg` (fix OFF) — **EXPECTED `AckedIffCommitted` VIOLATION** (~1,524 states, depth 11). This is INTENDED and documented — it proves the ACK-relay epoch check is load-bearing. **Do NOT "fix" it. If it ever stops violating, THAT is the real problem — STOP and report.**

**Re-run the model checker (needs OpenJDK 21; no CXL/ports → no flock):**
```bash
bash spec/run_all.sh    # 12 cfgs in parallel, each own -metadir + java.io.tmpdir, -deadlock, prints ALL_DONE
```
Single scenario:
```bash
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC -deadlock -workers 16 \
  -metadir states/stale_cv_ack_relay -config spec/stale_cv_ack_relay.cfg spec/MCEmbarcadero.tla
```
`spec/Embarcadero.tla` is the single source of truth; `spec/MCEmbarcadero.tla` is the TLC harness. **Re-run after ANY design-affecting change.** Any counterexample is handed to the parent — **NEVER "fix" the spec unilaterally.**

**Do NOT claim as machine-checked:** liveness/availability (stall→retransmit→fence, "durable frontier eventually advances"), the epoch check being non-over-aggressive, end-to-end speculative payload delivery, or bugs needing ≥3 outstanding batches / ≥2 failovers / >2 brokers. These are argued-in-prose only.

## 6. Implementation Map (concrete file:function anchors)

**CXL data structures — `src/cxl_manager/cxl_datastructure.h`:**
- `ControlBlock` (128B @ 0x0) `:83-176`; `committed_seq` monotone `:159-162`. **DO NOT grow ControlBlock.**
- `CompletionVectorEntry` (128B @ 0x1000) `:202-221`.
- `GOIEntry` (128B @ 0x2000) `:240-273`; `client_seq` `:261`; `_pad` `:270` → add `uint32_t session_epoch`.
- `BatchHeader` (128B) `:397-441`; `batch_seq` `:407`; `pbr_absolute_index` `:421`; `_pad0` `:415` → `uint16_t session_epoch`.
- `kCompletionVectorOffset=0x1000`, `kGOIOffset=0x2000` `:712-713`.
- **New:** `SessionEntry` + `kMaxSessions=4096` (verbatim in Commit D); `kSessionTableBase` (Track-04 offset).

**Sequencer — `src/embarlet/topic.{h,cc}`:**
- `HoldBatchMetadata`/`PendingBatch5` `topic.h:50-84` (`batch_seq` @ `:52`/`:67`, `client_id` @ `:66`).
- `Level5ShardState` `topic.h:878-905` (`client_state:886`, `client_emitted_tracker:887`, `hold_buffer:888`, `clients_with_held_batches:890`).
- `AdvanceConsumedThroughForProcessedSlots` `topic.h:958-963`.
- `ControlBlock.epoch++` at `topic.cc:97`; `ShouldAssertCommitOrder` `:77-85`.
- `CommitEpoch` `:4079` (`global_seq_.fetch_add:4093`, `global_batch_seq_.fetch_add:4112`, GOI write loop `:4149-4195` with `client_seq` @ `:4166`/`:4189`, W1.2 check `:4196-4215`, per-client flush `:4429-4435`).
- `EpochSequencerThread` `:4495`; `classify_one` `:5408-5507`.
- Fence/expiry loci: expiry rewrite `:5888-5907`; EXPIRY skip `:5984-5999`; SCANNER targeted `:5342-5344`; HOLD-FULL skips `:5436/:5538/:5657/:5772`; detectors `:2286,:4613-4615,:4939`; `InvalidateOrder5HeldSlot` `:173`; D3 `:4479/:4485-4489`.
- `RecoverSequencer5State` near `:3597-3606`.
- `batch_id = (broker_id<<48)|pbr_absolute_index` `:1714`.

**Sequencer utils — `src/embarlet/sequencer_utils.h`:** `OptimizedClientState` `:18`; `FastDeduplicator` `RING_SIZE=1<<20`, `TABLE_SIZE=1<<21` `:125-127`, `check_and_insert :146` (best-effort same-broker LRU only — **NON-authoritative**; the SOLE authoritative dedup is `seq < next_expected`).

**Network — `src/network_manager/network_manager.cc`:** reconnect `:783`; ingest `:944`; AckThread ACK-relay `:2330-2360` (advance sites `:2330/:2337/:2353`; non-head sentinel `(size_t)-1` `:2333-2358`); `GetOffsetToAckFast` `:1748-1871`; `GetOffsetToAck` `:1873-2014` (ACK1 `:1895-1936`, ACK2 `:1940-2013`, `completed_logical_offset` `:1960`).

**Client — `src/client/publisher.{h,cc}`:** `Publisher` `publisher.h:18-363`; `client_order_ :207`; `ack_received_ :281`; `order5_batch_seq_per_broker_ :194` (ORDER=4 — LEAVE INTACT); home finalizer `:323-327/:330` (DO NOT reuse for rendezvous); stamp session_epoch `:2105`; kOrderStrong no-op `:2121-2125`; release fold `:1834/:1847-1853`; reroute → RendezvousBroker `:2287`; RetransmitThread start `:758`.

**Queue buffer — `src/client/queue_buffer.{h,cc}`:** `batch_seq_` `queue_buffer.h:167`, stamped `queue_buffer.cc:223` (`h->batch_seq = batch_seq_.fetch_add(1)`); add `ResetBatchSeqForNewSession()` `:223`.

**FIXED DECISIONS (do not relitigate):** ONE monotone per-session-global `batch_seq` per session = the existing seal counter `QueueBuffer::batch_seq_` (NO new client counter); `committed_hwm`, resubmit suffix, dedup, `next_expected` all live in this seq space. `batch_id` dedup is NON-authoritative (cross-broker retransmit gets a new `batch_id`). `state_word` WRITTEN LAST. `ResetBatchSeqForNewSession` is single-producer, quiesced-only, SESSION_FENCED-only.

## 7. Testbed Discipline (VERBATIM — follow exactly)

- Take the lock with `flock` on `~/Embarcadero-sessions/testbed.lock`; log START/END to `~/Embarcadero-sessions/activity.log`.
- **COMPILE OUTSIDE the lock — hold the lock ONLY for runs.**
- **NEVER `pkill -f throughput_test/embarlet` (or any `pkill -f`)** — it matches your own shell argv and self-kills. **Kill strays by explicit PID only.**
- After runs, verify the lock is free AND no stray processes / stranded server remain before releasing.
- Do NOT touch `~/Embarcadero` (live tree) or other `~/Embarcadero-sessions/*` dirs — work only in `~/Embarcadero-sessions/d1-impl` on branch `d1-impl`.

**ORDER=5 gate / pass bar (run after EVERY commit):**
- Repro: **`ORDER=5` + `ACK=1` + `THREADS_PER_BROKER=4`** loopback, multiple trials. **4 threads are REQUIRED** to exercise out-of-order arrival → hold → invalidated slot (single-thread and ORDER=0 CANNOT reproduce the scanner-freeze).
- Set `EMBAR_ASSERT_COMMIT_ORDER=1` (hard CHECK/abort on true reorder). Teardown prints `[W1.2_SUMMARY] ... order5_commit_order_violations=N`.
- **Pass bar:** ORDER=5 passes **7/8+** with `EMBAR_ASSERT_COMMIT_ORDER=1` and **0 commit-order violations** (11.46–12.11 GB/s). The known ~2-batch (~0.005%) hold-buffer shortfall is pre-existing (why 9/10 not 10/10), subsumed by D1 — not a new regression.
- **Fast-path no-regression:** loopback baseline Order0 **12.46 GB/s** / Order5 **12.28 GB/s** is the no-regression bar. C and D must be **byte-identical for ORDER=5**. E/F/G must stay at/above baseline on the fast (`==next_expected` emit) path.

## 8. Commit / Push / Bundle + Report-Back

- Commit per commit-plan step with **scoped pathspec** (only the files you own for that commit — NOT the staged repo-reorg):
```bash
git commit --no-verify --no-gpg-sign <owned files...>
```
  Trailer **verbatim** at the end of every commit message:
```
Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```
- `--no-verify --no-gpg-sign` is REQUIRED — gpg signing hangs and the pre-commit hook is interactive.
- **Reconciliation (NEVER push to main):** push the branch to origin and create a review bundle:
```bash
git push -u origin d1-impl
git bundle create /tmp/d1_impl.bundle origin/main..d1-impl
```
- **Report back** (as your final text output — do NOT write a report .md file): which commits (C–G) landed vs. blocked and why; per-commit gate results (ORDER=5 pass ratio, `order5_commit_order_violations`, byte-identical/GB-s numbers); for F, the `spec/run_all.sh` result (A PASS, B PASS, bug-demo VIOLATION as expected); the branch name, the origin push confirmation, and the bundle path `/tmp/d1_impl.bundle`; any STOP-and-report items from §9.

## 9. STOP and Report (do NOT proceed / do NOT guess) if:

- **Track-04 has NOT signed off the `kSessionTableBase` region offset** (or `GOIEntry.session_epoch` layout) — Commit D. Do NOT hard-code the offset blind.
- **D2 has NOT supplied `session_lease_ns`** — Commit E is blocked (placeholder is 1000/2000ms; D2 must set `session_lease_ms > kDeltaCapMs` i.e. `> 12ms`, and satisfy `kDeltaCapMs < session_lease_ms < PBR_fill_time_ms` AND `session_lease_ns >= open-load append/ack p99.9`).
- **Track-02 TLA+ Scenarios A & B are NOT finalized/passing** (`bash spec/run_all.sh`) — Commit F is hard-blocked. "TLA+ first, code second."
- **The open-load δ measurement gate is NOT closed** — Commit G's δ tuning is blocked (constants stay `kDeltaFloorMs=1.7`, `kDeltaCapMs=12`).
- **`spec/stale_cv_bug_demo.cfg` STOPS violating `AckedIffCommitted`** — that means a real safety regression, not a fix.
- **Any `EMBAR_ASSERT_COMMIT_ORDER=1` violation appears**, ORDER=5 drops below the 7/8 pass bar, or the fast-path regresses below the 12.28 GB/s Order5 baseline.
- **Any TLA+ counterexample appears** on Scenario A or B (fix ON) — hand it to the parent; never edit the spec unilaterally.
- **Any frozen decision would need changing** (single per-session-global batch_seq, state_word-written-last, SessionEntry=64B, 128B ControlBlock/GOIEntry/BatchHeader contracts, δ constants, the C→D→E→F→G hard order, R-A gap-clock over the §3.4 resident-slot predicate) — STOP and report the conflict rather than deviating.
