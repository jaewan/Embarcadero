# Track 07 — Presentation artifacts & guarantee framing (D8, D9, G5, V.2)

**Read `sessions/00-BACKGROUND.md` first.** Track id: `07-presentation`. Branch:
`session/07-presentation`. Mostly writing (reads code/design) — **no testbed, start now.**

**Goal.** Make the added machinery read as *organized*, not *more*. Produce the compression artifacts,
each answering a named reviewer complaint: the boxed guarantee theorem (both properties), the
structure-ownership table, the **mechanism self-audit table (D9)**, the four pseudocode blocks, the
glossary + renames. A presentation score of 1 last time let the PC reject content it liked — close it.

**Plan sections:** `Paper/improvement_plan.md` → **D8, D9, G5, V.1, V.2**, plus **D1 (theorem — note
panel #11: BOTH properties + composition + divergence)** and D5 (ACK contracts). The **D9 self-audit
table is your W1 on-paper closure (~2-week deadline)** — do it first. Skim
`docs/design/EMBARCADERO_DEFINITIVE_DESIGN.md` and `docs/design/CXL_MEMORY_LAYOUT_v2.md` for facts.

## You OWN (Paper `.tex` — these files/artifacts only)
- `Paper/Text/Sec4_Design.tex` and `Paper/Text/Sec5_FaultTolerance.tex` (design/failure sections —
  where the theorem, tables, and pseudocode live).
- A new `Paper/Text/Glossary.tex`.
- The boxed **guarantee theorem** (D1), the **structure-ownership/flush table** (D8), the
  **mechanism self-audit table** (D9), the **four pseudocode blocks** (broker / sequencer / replica /
  repair loops), the **polling-economics** sentence (D8: ~5 MB/s vs 21 GB/s), and the **substrate
  table with leg-1/leg-2 columns** (V.2 — coordinate content with Track 06's comparison table).

## Do NOT touch
- `src/`, `spec/`, `scripts/`, `Paper/Text/RelatedWork.tex` / `Limitations.tex` / `UseCases.tex`
  (Track 06), `Paper/improvement_plan.md`.
- `Paper/Text/Sec1_Introduction.tex` — Track 06 may edit it; if you need an intro tweak, note it in
  handoff for the human to merge.

## Scope
- **Boxed guarantee theorem** (D1, §2) — **must state BOTH properties + composition + the one
  divergence (panel #11), not just per-session prefix:**
  > *(Total order)* the committed GOI is a single total order.
  > *(Per-session prefix)* each session's committed batches embed in that order as a submission-order
  > prefix of its stream; a batch is ACKed iff committed; late/duplicate arrivals are rejected, never
  > reordered.
  > *(Reader agreement)* speculative (ACK1) and durable-frontier readers observe prefixes of the same
  > total order; positions never reorder. **Sole divergence:** under CXL-module loss in the ACK1→ACK2
  > window, a speculative reader may have delivered a payload at position *k* that a durable reader
  > later observes as `ERR_DATA_LOSS` at *k* — *visibility* differs, order does not.
  `Hole`/`Causal_Late` cease to exist. Include the **honest availability statement** (a broker kill
  gaps essentially every striped session; the win is *duration* ≈δ — single-digit ms vs 660 ms —
  plus independent per-session recovery **and a bounded, measured reconvergence transient on
  survivors**; write it exactly that way, never "only one stream stalls").
- **ACK1/ACK2 boxed contracts** (D5), split by fault domain, µs-scale window quantified; the CXL-
  module-loss case is the theorem's *sole divergence*. Note the fenced-client recovery uses the
  **HWM folded into the `SESSION_FENCED` payload** (no separate HWM-query mechanism — D1/self-audit).
- **Structure-ownership/flush table** (D8): structure | size & sizing rule | single writer | readers
  | flush discipline | failure handling — covering Blog, PBR, GOI, CV, control block, heartbeat
  lines, session tables, subscriber cursors. Pull facts from design docs; verify sizes (Blog = ingest
  × GC horizon; PBR = (N>T) slots × 128 B; GOI 16 GB ≈ 48 s).
- **Mechanism self-audit table (D9) — the complexity budget is a TABLE, not an assertion (panel #8):**
  mechanism → the one guarantee/availability line it serves → keep/cut/demote. Label eval infra
  (RDMA variants, injection) and table-stakes (GC) as such, not as system novelty. Report net
  complexity vs v1 honestly: **−5 deleted anomaly mechanisms** (hold-timeout semantics, `Hole`,
  `Causal_Late`, per-entry expiry, idle-armed force-expiry) replaced by recognizable timers/dedup/
  fencing; **−2 audit deletions** (session HWM folded into `SESSION_FENCED`; redirect push demoted).
  Ship the table.
- **Pseudocode ×4** for the four roles (reviewer E asked explicitly).
- **Glossary**; term renames: batching **"epoch" → "round"**; unify **Broker vs Server**; neutral
  machine names **host-A/B/C** defined once. Audit every term defined before first use vs reviewer E's list.

## Coordinate
- The theorem must match Track 01's **frozen** semantics and Track 02's invariants — sync before
  finalizing wording. Use `\TODO{}` for any number that comes from evaluation.
- Renaming "epoch"→"round" is paper-only here; if it should also change in code/comments, propose it
  to Track 01 (don't edit `src/`).

## Done criteria
- Theorem box, ACK contracts, structure-ownership table, 4 pseudocode blocks, glossary all drafted
  and compiling in the owned `.tex`; term audit done. Handoff note with what's drafted, the
  epoch→round decision for code, and any intro contention with Track 06.
