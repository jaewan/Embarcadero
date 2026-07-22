# Embarcadero session protocol — TLA⁺ specification (Track 02 / W2)

Machine-checked TLA⁺ model of the Embarcadero session protocol, covering the
adversarial cross-product the resubmission relies on (`Paper/improvement_plan.md`
→ G4 / W2). This is a shipped artifact and is cited in the paper's §5.

`Embarcadero.tla` is the single source of truth for the state machine.
`MCEmbarcadero.tla` is the TLC harness (symmetry set + a state-space bound). Each
scenario is a `.cfg` that toggles adversary/feature `CONSTANT` flags and checks
`TypeOK` + `Safety`.

## ⚠ Exactly what is machine-checked (read before citing "machine-checked in TLA⁺")

Model checking here is **safety-only** and at **small finite scope** (2 clients or
1 for the failover-heavy configs, 2 brokers, `MaxSeq`=2, ≤1 failover, ≤1 re-open).
The paper must claim **only** the properties below as machine-checked, and label
the rest as argued-in-prose or out-of-scope. This scoping is deliberate — the
mismatch between "checked" and "claimed" is the artifact's biggest integrity risk.

**Machine-checked (safety invariants, all scenarios):**

| Invariant | Guarantee clause | Design |
|---|---|---|
| `NoDupCommit` | total order; **exactly-once**, incl. across fence/re-open | dedup, D1 |
| `PerSessionPrefix` | **per-session prefix**: strictly-increasing, contiguous-from-1, never reordered | D1, G1 |
| `LateNeverReordered` | late/duplicate rejected, never reordered | D1 |
| `FencedSuffixNeverCommitted` | fenced suffix never committed (session death, not gap-skip) | D1 |
| `NoWrap` | spatial guard / wrap-fence | (kept) |
| `AckedIffCommitted` | a **live durable ACK names a committed entry**; stale-epoch ACK relays are rejected | D5, D2 |
| `ReaderAgreement` | positions reaching durability never reorder/change identity; an ACK1-only suffix may be truncated on failover | D5 (theorem clause 3) |
| stale-CV **necessity** | the ACK-relay epoch check is load-bearing (`stale_cv_bug_demo` counterexample) | D2 |

**NOT machine-checked — argued in prose or out of scope (do not claim as checked):**

- **Liveness / availability** — the stall → retransmit → fence progression and
  "the durable frontier eventually advances." Modelled structurally (the actions
  exist and are reachable) but not TLC-verified. The δ/retransmit/lease timing
  argument lives in the paper (D1/D2).
- **Over-aggression of the epoch check** — because checking is safety-only, the
  model shows the epoch check *never admits a bad ACK*; it cannot show the check
  *never wrongly blocks a legitimate re-sequenced-durable ACK* (that is a liveness
  property). The check being non-over-aggressive is argued, not checked.
- **The speculative (ACK1) delivery contract** — `ReaderAgreement` constrains
  positions that reach durability. The model permits failover to truncate an
  ACK1-only suffix and does not model an application consuming that speculative
  payload. ACK1 is not a failover-stable completion contract; speculative
  readers detect the epoch change and reread.
- **Scope bounds** — bugs that need ≥3 outstanding batches (deep multi-gap
  cascade), ≥2 failovers (a zombie surviving two epochs), or >2 brokers are out of
  the checked scope. Defensible for a model of this size; the paper says so.

## Mechanisms modelled (design cross-reference → `docs/design/protocol_spec.md`)

Sessions & per-session FIFO (D1); two-phase PBR publication + broker crash
pre/post publish (§3.1); client retransmit to a different broker (D1); sequencer
dedup; hold buffer + cascade release + assign-at-release (D4); session fencing
(D1); **exactly-once across fencing via session re-open + suffix resubmit** (D1);
spatial guard + wrap-fence; control-block epoch + sequencer failover with GOI
truncation + PBR rescan (D3/D7); lease false positive / SIGSTOP (D2);
**completion-vector ACK path with a per-signal epoch tag and the broker ACK-relay
epoch check** (D5/D2) — the zombie stale-CV advance is a real, always-enabled
action, and the epoch check on the relay path is the fix; CXL-module loss in the
ACK1→ACK2 window with a **speculative (ACK1) reader** (D5).

## Scenario → cfg map (W2)

| # | cfg | Scenario | Sessions | Key adversaries |
|---|---|---|---|---|
| 1 | `crash_pre_post_pbr` | broker crash pre-/post-PBR | 2 | crash |
| 2 | `retransmit_race` | batch retransmitted to a different broker | 2 | crash + retransmit |
| 3 | `dup_in_guard_window` | **named:** dup at S_new inside the spatial-guard window | 1 | crash + failover, tight `GOICap` |
| 4 | `lease_false_positive` | SIGSTOP pause → dup discarded by dedup | 2 | lease pause |
| 5 | `stale_cv_ack_relay` | **PRIORITY:** stale-CV ACK relay, **with** the epoch-check fix | 1 | failover + stale-CV + fix |
| 5b | `stale_cv_bug_demo` | same **without** the fix — expected counterexample | 1 | failover + stale-CV, fix OFF |
| 6 | `client_crash_midstream` | uncommitted suffix fenced, never committed | 2 | crash → gap → fence |
| 7 | `seq_failover_open_gap` | failover during an open gap; S_new rescans | 1 | crash + failover |
| 8 | `module_loss_ack_window` | order survives, payload may not; surfaced | 1 | module loss + spec reader |
| + | `reader_agreement` | reader-agreement / sole-divergence, hard case | 1 | failover + module loss + spec reader |
| + | `exactly_once_fence` | exactly-once across fence via re-open | 1 | crash + re-open |

`core` is the no-adversary baseline. Failover-heavy configs run at **1 session**
(the checked properties are per-session; cross-session interleaving is covered by
the 2-session non-failover configs) so the state space exhausts.

## How to reproduce (on `broker`, CPU-only — no testbed lock)

TLC runs on Java (broker has OpenJDK 21). Model-checking uses no CXL/ports/cgroups,
so **no `flock` is required**. Safety-only ⇒ pass `-deadlock` (a quiescent terminal
state is expected, not a bug). Give parallel runs distinct `-metadir`s (TLC's
time-stamped metadata dir otherwise collides).

```bash
cd /path/to/Embarcadero/spec
curl -fL -o tla2tools.jar \
  https://github.com/tlaplus/tlaplus/releases/latest/download/tla2tools.jar
# one scenario:
java -XX:+UseParallelGC -cp tla2tools.jar tlc2.TLC -deadlock -workers 16 \
  -metadir states/stale_cv_ack_relay \
  -config stale_cv_ack_relay.cfg MCEmbarcadero.tla
# all scenarios (safe defaults; tune TLC_WORKERS/TLC_HEAP if desired):
bash run_all.sh
```

`run_all.sh` writes fresh output to `reproduced-results/` and state to
`states/`; it never overwrites the checked-in reference results.

## Results

Per-scenario TLC summaries are in `spec/results/<scenario>.txt`. Expected:
**all scenario cfgs pass `Safety`**; `stale_cv_bug_demo` produces the expected
`AckedIffCommitted` counterexample (documented, not a regression). See
`spec/results/SUMMARY.md` for the table.

## Findings for other tracks

- **Track 01 (core protocol):** the ACK-relay control-block **epoch check is
  load-bearing** — `stale_cv_bug_demo` shows that without it a broker relays an ACK
  for an orphaned (failover-truncated) batch, violating the live-epoch ACK rule (D2 /
  W2 #5). The check must guard the ACK-relay path in `src/embarlet` (see handoff).
- Any counterexample found here is handed to Track 01, not "fixed" unilaterally
  ("TLA⁺ first, code second").
