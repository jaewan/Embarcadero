# D1 → Track 02 handoff: TLA+ scenarios to model before D1 code freezes

**From:** Track 01. **Status:** ready for Track 02 now (independent of the open D1 seq-model decision).
Full D1 design: `docs/design/D1_SESSION_FIFO_DESIGN.md` (draft; must-fixes at top).

Model these two interleavings; the invariant each must preserve is **"a batch is ACKed iff
committed, and a fenced/late/duplicate batch is never committed or reordered."** Section 6 below is
the verified substrate (file:line vs 7ec0e70). Coordinate back before Track 01 finalizes the code.

Two named scenarios (also must-fix #14 in the design doc):
- **Scenario A** — retransmitted duplicate arriving at `S_new` inside the D4 spatial-guard window.
- **Scenario B** — stale-CV ACK relay after sequencer election (broker must validate the active
  ControlBlock.epoch on the ACK-relay path before advancing a client ACK; withhold-not-confirm must
  be bounded by a terminal EPOCH_STALE/fence to avoid an infinite δ-retransmit liveness hole).

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

