# Protocol spec cross-reference: TLA⁺ ↔ design (D1–D8)

Bridges the machine-checked TLA⁺ model in [`spec/`](../../spec/) to the definitive
design (`EMBARCADERO_DEFINITIVE_DESIGN.md`) and the resubmission plan
(`../../Paper/improvement_plan.md`, D1–D8). The model is **safety-only** at small
finite scope; `spec/README.md` states EXACTLY which guarantee clauses are
machine-checked vs. argued in prose. Read that boundary before citing.

## State variables → design

| TLA⁺ variable | Design object | Section |
|---|---|---|
| `clientNext`, `retxUsed` | client library: in-order submission + bounded retransmit | D1 |
| `net` | in-flight batches on the wire (TCP), lost on broker death | D1, §3.1 |
| `pbrReserved` / `pbr` | two-phase PBR: Phase-1 reserved slot vs Phase-2 written entry | §3.1, §2.4 |
| `collectCursor` | sequencer scan cursor over PBR (commit-driven, rescannable) | D3, §3.3 |
| `expectedSeq` | per-session sequencer table (`expected_seq` dedup frontier) | D1, D7, §3.2 |
| `hold` | hold buffer (volatile, reconstructible from PBR) | D3, §4.3 |
| `goi` (`[client,cseq,epoch,lost]`) | Global Order Index — the single total order | §2.4, §3.3 |
| `sessionFenced`, `fenceHWM` | session fencing + HWM folded into `SESSION_FENCED` | D1, G1 |
| `sessEpoch` | session generation, bumped on re-open (exactly-once) | D1 |
| `replicationFloor` | durable (ACK2) frontier, tracked by the spatial guard | D5, §3.3 |
| `cbEpoch` | control-block epoch (bumped on failover; zombie fencing) | D2, §4.2 |
| `cvSig` (`[client,cseq,epoch]`) | completion-vector advance signals, epoch-tagged | D5, D2, §3.4 |
| `acked` | client-visible ACKs relayed over the CV | D5, §3.4 |
| `orphaned` | GOI entries truncated by a failover — the stale-CV attack surface | D2, §4.2.3 |
| `specSnap` | what a speculative (ACK1) reader delivered, position by position | D5 |
| `crashed` / `paused` | broker host death / SIGSTOP lease false positive | §4.1, D2 |

## Actions → design mechanism

| TLA⁺ action | Design mechanism | Section |
|---|---|---|
| `ClientSubmit`, `ClientRetransmit` | sessions; retransmit batch *n* to a **different** broker | D1 |
| `BrokerReserve` → `BrokerWrite` | two-phase publish: reserve slot, then write + release fence | §3.1 |
| `BrokerCrash` | host death: reserved-unwritten dies (pre-PBR), written survives (post-PBR) | §3.1, §4.1 |
| `BrokerPause` / `BrokerResume` | lease false positive (SIGSTOP); recovery via retransmit + dedup | D2 |
| `SeqCollectDiscard` | dedup: late/duplicate/fenced → discard, never reorder | D1, §3.2 |
| `SeqCollectCommit` | in-order commit (`cseq=expected`) → assign global order | D1, D4 |
| `SeqCollectHold` | out-of-order (`cseq>expected`) → hold buffer | §4.3 |
| `SeqReleaseHeld` | cascade release; **assign-at-release** | D4 |
| `Fence` | hold expiry / client crash ⇒ session fence, never gap-skip | D1, G1 |
| `Reopen` | fenced-but-live client re-opens + resubmits suffix (exactly-once) | D1 |
| `Replicate` | chain replication advances the durable frontier (ACK2) | D5, §3.4 |
| `CVAdvance` | tail replica advances the CV for a durable+live batch (current epoch) | D5, §3.4 |
| `StaleCVAdvance` | zombie `S_old` advances the CV under its OLD epoch — a real action | **D2, §4.2.3** |
| `AckRelay` | broker relays a client ACK; **epoch check** on the relay path filters stale CV | **D2, D5** |
| `SpecRead` | speculative (ACK1) reader delivering ahead of durability | D5 |
| `ModuleLoss` | CXL-module loss in the ACK1→ACK2 window | D5 |
| `SequencerFailover` | new epoch: GOI truncation + PBR rescan + table rebuild | D3, D7, §4.3.1 |

## Invariants → guarantee theorem

- **Total order + exactly-once** — `goi` is a single sequence; `NoDupCommit`
  forbids double commit (also the exactly-once guarantee across fence/re-open).
- **Per-session prefix** — `PerSessionPrefix` (+ `LateNeverReordered` for the
  "late/duplicate rejected, never reordered" clause).
- **ACKed iff committed** — `AckedIffCommitted`; the clause the **stale-CV** bug
  (D2) attacks. The mechanism is faithful: the zombie `StaleCVAdvance` always may
  write a stale-epoch signal; the broker `AckRelay` epoch check refuses it.
  `stale_cv_bug_demo.cfg` (check off) is the necessity counterexample.
- **Fencing** — `FencedSuffixNeverCommitted`.
- **Reader agreement / sole divergence (D5 clause 3)** — `ReaderAgreement`: any
  speculatively-read position that reaches durability agrees on identity
  (client,cseq); the only permitted divergence is payload visibility (`lost`).
  Speculative reads in a failover-truncated zone are epoch-scoped (re-read).
- **Spatial guard / wrap-fence** — `NoWrap`.

## The newly-found bug (D2 / W2 #5), in spec terms

After `SequencerFailover` the new epoch truncates non-durable GOI entries (they
enter `orphaned`; PBR slots survive). `StaleCVAdvance` (zombie `S_old`) advances
the CV for such an entry under the old epoch. Without the broker's control-block
epoch check on `AckRelay`, the broker relays an ACK for a batch that never becomes
visible under the current epoch — violating `AckedIffCommitted`.
`stale_cv_bug_demo.cfg` (fix off) produces the counterexample; `stale_cv_ack_relay.cfg`
(fix on) shows the check restores safety. This is the D2 requirement Track 01 must
implement on the `src/embarlet` ACK-relay path.
