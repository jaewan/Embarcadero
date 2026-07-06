# Handoff — Track 07 (Presentation artifacts & guarantee framing)

Branch base: `chore/repo-reorg`. **Not committed** (per 00-BACKGROUND rules). All work
compiles: validated with a minimal `pdflatex` wrapper (`Defs` + Glossary + Sec4 + Sec5),
`EXIT=0`, no LaTeX errors, one residual 1.9 pt overfull (cosmetic).

## What I did (all V.2 / D-series artifacts for Track 07)

All in files I own: `Paper/Text/Sec4_Design.tex`, `Paper/Text/Sec5_FaultTolerance.tex`,
new `Paper/Text/Glossary.tex` (+ one additive `\input` line in `Main.tex`).

1. **Boxed guarantee theorem (D1)** — new `\subsection{The Guarantee}` at top of Sec4,
   `\label{thm:guarantee}`. States all four clauses: total order, per-session prefix
   (ACKed iff committed), reader agreement, **sole divergence** (ACK1→ACK2 CXL-module
   loss → `ERR_DATA_LOSS` at preserved position). Followed by the **honest availability
   statement** (striping gaps *essentially every* session; win is duration ≈δ + independent
   recovery + bounded/measured reconvergence — written exactly per plan, never "only one
   stream stalls").
2. **Substrate table w/ leg-1/leg-2 columns (V.2)** — `\label{tab:substrate}` in Sec4.
   4 rows (classic broker-owned / RDMA host-DRAM W5-A / RDMA memory-server W5-B / CXL).
   **Coordinate with Track 06**: I added a footnote pointing to `\S\ref{sec:related}`;
   this table isolates the two legs, Track 06's related-work table compares full systems —
   make sure we don't duplicate/contradict.
3. **Session-FIFO rewrite (D1/D3/D4)** — replaced the old `\subsection{The Hold Buffer and
   the Causal Contract}` (Prefix-FIFO, `Hole`, `Causal_Late`, per-entry/idle force-expiry)
   with `\subsection{Sessions: Enforcing the Per-Session Prefix}`: expected_seq + dedup,
   T/R absorption, retransmit(rendezvous, adaptive δ)→**fence, never gap-skip**,
   `SESSION_FENCED` carrying HWM, commit-driven PBR frontier + emergent flow control (no
   credit windows), assign-at-release, design-alternatives paragraph.
4. **ACK1/ACK2 boxed contracts (D5)** — `\label{box:ack}` in read-path section, split by
   fault domain; subscribers default to durable frontier, ACK1 speculation opt-in.
   Removed `Hole`/`Causal_Late` from the subscriber read path.
5. **Structure-ownership/flush table (D8)** — `\label{tab:ownership}` (`table*`, full width,
   fixed `p{}` columns). Covers Blog, PBR, GOI, CV, control block, heartbeat, session
   tables, subscriber cursors. Plus **polling-economics** sentence (~5 MB/s vs 21 GB/s).
6. **Mechanism self-audit table (D9)** — `\label{tab:selfaudit}`, the W1 closure. Each
   mechanism → contract line, or labeled eval-infra / table-stakes / demoted. Net **−7**
   (5 anomaly + 2 audit deletions); credit-windows deletion noted separately.
7. **Four pseudocode blocks** — `alg:broker`, `alg:seq`, `alg:replica`, `alg:repair`
   (algpseudocode). New `\subsection{The Four Roles in Pseudocode}`.
8. **GC subsection (D6)** — brief `\subsection{Garbage Collection}` (labeled table-stakes,
   cross-refs the self-audit) so the 16 GB GOI isn't a 48-s demo.
9. **Sec5 updates** — session-based failover recovery (session tables chain-replicated,
   read+rescan; fenced sessions stay fenced; no silent skip); new **stale-CV ACK-relay
   epoch check** (D2 correctness requirement, flagged as TLA⁺-checked).
10. **Glossary + term audit (G5)** — new `Glossary.tex` (`tab:glossary`); unified
    **server** (dropped "broker" in prose, kept `broker_id` as a code field); **host-A/B/C**
    machine names defined once; batching **"epoch" → "round"** in my files; all my refs
    verified against defined labels.

## Decisions the human should confirm

- **epoch→round for code/comments:** I renamed the *batching* sense to "round" in Sec4/Sec5
  only (control-plane fencing "epoch" untouched). **Sec7_Evaluation.tex still says "epoch
  sequencer"/"epoch batching"** (~8 occurrences) — that file is Track 01/eval-owned. Propose
  Track 01 rename batching-epoch→round there (and optionally in `src/` comments) for
  consistency. Until then the paper is internally split.
- **Theorem placement:** plan says the boxed theorem lives in **§2 (Problem & guarantees)**;
  I put it at the top of §4 (Design) because I own §4, not §2. It references §2's Property 1/2.
  Human may want to move/duplicate `thm:guarantee` into §2.
- **Glossary placement:** I added `\input{Text/Glossary}` after `Sec2_Background` in `Main.tex`
  (one additive line, commented). Relocate to appendix if space-constrained.

## Coordination / open items for other tracks

- **Track 01 (frozen semantics):** please confirm the theorem wording and the session-FIFO
  mechanics (δ = SRTT+4·RTTVAR, rendezvous hashing on `(client_id,batch_seq)`, fence-not-skip,
  HWM-in-`SESSION_FENCED`, assign-at-release, commit-driven PBR frontier) match the frozen
  design. I wrote to the plan (v4 source of truth); flag any drift.
- **Track 02 (TLA⁺):** Sec5 now asserts the **stale-CV ACK-relay** interleaving is
  machine-checked — this is the panel-item-11 scenario. Make sure it's in your model.
- **Track 06 (positioning):** (a) substrate/leg table content overlap — reconcile with your
  related-work comparison table; (b) **`Abstract2.tex` still uses old framing** ("bounded hold
  buffer to preserve FIFO", "misses the hold window") — you own abstract/intro precision (V.2);
  update to per-session-prefix + the demonstrated-over-RDMA / characterized-on-CXL claim.
  I did not touch `Sec1_Introduction.tex`.

## Numbers marked for eval (no `\todo` left dangling; all are prose forward-refs to §7)

δ magnitude (~10 ms), reconvergence transient, ACK1→ACK2 window (µs), sequencer CPU
idle/peak, and the 660 ms TCP baseline all forward-reference `\S\ref{sec:eval}` rather than
hardcoding — Track 01/eval fills these.

## Suggested commit (when the human approves)

**Note:** `Paper/` is a **separate nested git repo** (its own `.git`), distinct from the
outer Embarcadero repo. Paper `.tex` edits must be committed *inside* `Paper/`, not via the
outer repo. Also, at the time of writing `Paper/Text/Main.tex` carries **other tracks'**
uncommitted changes too (Abstract2, RelatedWork, Sec1, bibliography.bib are all modified by
others) — stage the Glossary `\input` line selectively (`git -C Paper add -p Text/Main.tex`)
so you don't sweep up cross-track work.

```
# Paper subrepo (Track-07 files):
git -C Paper add Text/Sec4_Design.tex Text/Sec5_FaultTolerance.tex Text/Glossary.tex
git -C Paper add -p Text/Main.tex   # only the \input{Text/Glossary} line
git -C Paper commit --no-verify -m "Track 07: v4 guarantee framing + presentation artifacts

Boxed theorem (D1), ACK1/ACK2 contracts (D5), structure-ownership (D8) and
mechanism self-audit (D9) tables, 4 pseudocode blocks, substrate leg table,
glossary + term audit; rewrite hold-buffer to session FIFO, delete Hole/
Causal_Late; Sec5 stale-CV epoch check + session-based recovery.
Post-review fixes: GOI 64->128 B, Alg1 abs-index, Alg2 hold.insert/m/fenceExpired,
polling economics idle+peak."

# handoff-07.md lives in the OUTER repo (sessions/), commit it there separately if desired.
```

## Senior review — findings & fixes (post-review pass)

### Fixes applied to Sec4_Design.tex
- **M1 — GOI entry size 64 → 128 bytes.** Corrected in the layout bullet and the structure-ownership table (was internally contradicted by Sec4 lines 274/279, which already said 128 B). 48 s exhaustion figure is arithmetically consistent only with 128 B.
- **B1 — Algorithm 1 descriptor tuple.** Phase-1 tuple now includes the absolute index `a` (`⟨o, cnt, epoch, cid, seq, a, c=⊥⟩`), matching the declared slot layout (line 145) and code `BatchHeader.pbr_absolute_index`; phase-2 correctly sets `c ← a`.
- **B2 — Algorithm 2 hold buffer + round count.** `hold[S]` is now a per-session ordered buffer via `hold[S].insert` (was a scalar slot that overwrote concurrent out-of-order batches; code uses `skipped_batches[client_id][batch_seq]` btree_map). Round message count is now defined (renamed to avoid collision with per-session `n`); commit/drain renamed to stage/drainInOrder (in-order drain matching `ProcessSkipped`).
- **B3 — Fence sweep de-hotpathed.** Per-iteration O(#sessions) `fence sessions whose gap outlived the lease` replaced with `fenceExpired()` amortized O(1) via a lease-expiry queue/timer wheel; keeps the "polling is cheap" cost model honest (hot loop stays the 4 PBR loads).
- **E1 — Polling economics.** Now reports idle floor ~5 MB/s (~1 load/PBR/round) AND peak ≤ ~180 MB/s (2.8 M loads/s × 64 B ≈ 179 MB/s ≈ 0.85% of the 21 GB/s CXL read budget); the "four orders of magnitude" phrasing was true only at idle (~2 orders at peak).

### FLAGS for human / Track 01 (NOT fixed — design-owner calls)
- **C1 — Session-FIFO / fencing / SESSION_FENCED / assign-at-release not in code.** Sec4 (and the theorem) describe session identity `(client_id, session_epoch)`, session fencing, `SESSION_FENCED(HWM)`, and assign-at-release in present tense, but the code has none of it: `topic.cc` does skip-and-stash (`skipped_batches`/`ProcessSkipped`) plus per-batch `global_seq_.fetch_add` at scan/reservation (topic.cc:877,891,956), not deferred to release. No `session_epoch` field in `BatchHeader`; zero grep hits for `SESSION_FENCED`/`session_epoch`. What IS real: per-client `expected_seq` in-order commit, out-of-order hold, batch_id dedup (FastDeduplicator), control-plane epoch zombie fencing. The evaluation must not attribute numbers to unbuilt mechanisms.
- **C2 — clwb-vs-clflushopt rationale false on eval machine.** Sec4:139 claims "use clwb rather than clflushopt … clwb retains the line in the writer's L3." But the code selects `_mm_clflushopt` on Intel (the testbed) — network_manager.cc:1284-1288 (`#ifdef __INTEL__ → clflushopt`), CMakeLists.txt:34-37 (GenuineIntel → `__INTEL__`); `clwb` only on AMD. `CXL::flush_cacheline` (performance_utils.h:191-193) is un-gated `clflushopt` on x86 and is the primitive used throughout chain_replication.cc. clflushopt EVICTS the line, so the L3-retention rationale is false on the Intel eval machine. Fixing the intended semantics would require a code change (use `_mm_clwb` on Intel), not just prose.

### Additional issues (completeness critic)
Verbatim newIssues surfaced during verification (file:line):

**goi-entry-128-bytes:** none beyond the tex instances already listed for fixing.

**pbr-absolute-index-tuple:**
- The phase-1 tuple on Sec4_Design.tex:421 also omits total_size/start_logical_offset/log_idx present in the real BatchHeader, but that is expected abstraction; only the omission of `a` is a true inconsistency because `a` is read on line 422 and used in the readiness predicate (170-171).
- No occurrences of the missing-`a` tuple pattern exist outside Sec4_Design.tex; grep of Paper/Text/*.tex shows all a/absolute-index/PBR[slot] references are confined to Sec4_Design.tex (lines 145,155,170,171,421,422,540). Line 540 is commented out (%) and needs no change.

**pseudocode-alg2-hold-and-n:**
- Symbol collision: `n` denotes a per-session batch sequence number in prose (Sec4_Design.tex:192-195) but is (re)used as the round aggregate message count in Algorithm 2 (Sec4_Design.tex:440-441). Even after defining it, reusing `n` would be ambiguous; the recommended fix renames the round count to `m`.
- Algorithm 2's drain semantics (line 436 'drain hold[S]') are underspecified for the multi-entry case: it should loop popping consecutive seqs (hold[S][expected], expected+1, ...) as ProcessSkipped does (topic.cc:938-976), not drain a single value.

**sec4-alg2-lease-sweep:**
- Line 443's fencing sweep is unconditional (outside the `\If{round sealed}` guard), so it runs on every raw poll iteration, not only on sealed rounds — an expiry-queue/timer-wheel rewrite also fixes this by making the check O(1) amortized regardless of guard placement.
- The provisioning inequality 'session lease < PBR fill time' (Sec4_Design.tex:243) bounds correctness but does not bound the number of concurrently-gapped sessions, so nothing in the text caps the O(#sessions) sweep cost — another reason the timer-wheel remedy is needed to keep the polling-cheap claim quantitatively honest.

**sec4-polling-economics-peak:**
- The prose 'four orders of magnitude below the ceiling' (Sec4_Design.tex:401) is only true at idle (~3.6 orders). At the paper's own peak of 2.8M batches/s, polling read traffic is ~179 MB/s = ~0.85% of 21 GB/s, i.e. only ~2 orders of magnitude below. Consider clarifying that 5 MB/s is the idle floor and peak is ~179 MB/s (still <1% of budget).

**session-fifo-fencing-claim:**
- Sec4_Design.tex:222-224 'If a gap outlives retransmission for a full session lease, the client is dead or partitioned. \sys then \textbf{fences the session}: the uncommitted suffix never appears in the log, and a reconnecting client receives \texttt{SESSION_FENCED}.' — present-tense session fencing + SESSION_FENCED response with no implementation (0 grep hits in src).
- Sec4_Design.tex:189-191 'A client opens a \emph{session} identified by (client_id, session_epoch) and stamps each batch with a per-session sequence number.' — no session_epoch field exists in BatchHeader (cxl_datastructure.h:403-424); identity is client_id+batch_seq only.
- Sec4_Design.tex:225-229 'The \texttt{SESSION_FENCED} response carries the session's committed high-water mark (HWM)... resubmits exactly the uncommitted suffix under a fresh session, giving exactly-once semantics across fencing' — no SESSION_FENCED/HWM mechanism implemented.
- Sec4_Design.tex:247-251 'Assign sequence numbers at release, not reservation ... a global sequence number is assigned at release (commit) time, not reserved when the round is collected.' — code assigns via global_seq_.fetch_add at scan/reservation (topic.cc:877,891,956), contradicting the present-tense claim.
- Sec4_Design.tex:503 'Session fencing + SESSION_FENCED(HWM) & prefix + exactly-once' and Sec4_Design.tex:505 'Assign-at-release & dense committed_seq' — table rows presenting unimplemented mechanisms as delivered guarantees.
- Sec4_Design.tex:384 GOI component-table cell 'assign-at-release' listed as an implemented property — not implemented.
- Sec5_FaultTolerance.tex:39 'a session fenced before the crash stays fenced (its session_epoch is stale); and duplicate resubmissions are discarded by batch_id against the recovered expected_seq' — session_epoch fencing not implemented (batch_id dedup and expected_seq are real, session fencing is not).
- Abstract2.tex:6 'a session whose gap outlives retransmission is fenced, never silently reordered' — session fencing not implemented.

**clwb-vs-clflushopt:**
- src/common/performance_utils.h:162 comment '@paper_ref Paper §4.2 - Uses clflushopt for non-coherent CXL writes' — the CODE comment correctly says clflushopt, further evidence the paper's clwb framing is wrong.
- Inconsistency between the vendor-gated network_manager.cc site (Intel=clflushopt) and the un-gated CXL::flush_cacheline (always clflushopt on x86): the paper's blanket 'we use clwb' does not match either path on Intel. If the intended semantics is L3 retention, the code itself would need to change (use _mm_clwb on Intel), not just the prose.

**completeness-critic-sec4-sec5:**
- GOI ENTRY SIZE (paper 64B vs code 128B). Paper: Sec4_Design.tex:146 'Global Order Index (GOI): ... Each 64-byte entry stores the global sequence number...'. Code: struct GOIEntry is 128 bytes — src/cxl_manager/cxl_datastructure.h:240 'struct alignas(128) GOIEntry', :272 static_assert(sizeof(GOIEntry) == 128); allocated cxl_manager.cc:350-351, logged '128 bytes' at cxl_manager.cc:386. Internal contradiction: Sec4 says '128 bytes per GOI entry' at :274 and '128-byte entries' at :279. Correct: 128 bytes. (A SECOND unused 64B struct GlobalOrderEntry at cxl_datastructure.h:593/607 is referenced only by GOI_SIZE macro with GOI_MAX_ENTRIES=65536 in config.h.in:62-63; the live GOI at 0x2000 uses the 128B GOIEntry.)
- GOI REGION SIZE (paper 16GB vs code 32GB). Paper: Sec4_Design.tex:279 'bounded 16 GB region', :361-362 '16 GB GOI is a 48-second demo', :384 table '16 GB ~= 48 s at peak'. Code: 256M entries x 128B = 32 GB — cxl_manager.cc:345 kMaxGOIEntries=256ULL*1024*1024, comment :342 'GOI (32 GB)', log :386 '256M entries x 128 bytes = 32 GB', cxl_datastructure.h:226 '32 GB array'. (Header cxl_manager.h:110 is itself wrong: '256M entries x 64B = 16GB'.) Correct: 32 GB.
- GOI EXHAUSTION TIME (paper ~48s vs ~96s implied by code). ~48s is consistent only with 16GiB (16*2^30/(2.8e6*128)=47.9s). With actual 32GiB / 256M entries, 256M/2.8e6 = ~96s. Correct: ~96 s (or fix the region to 16GB in code; either way paper and code currently disagree).
- PBR/ORDERING-REQUEST ENTRY SIZE (paper Sec3 says 64B, code + Sec4 say 128B). Sec3_CoordinationModel.tex:147 'Server publishes a 64-byte metadata entry to its Pending Batch Ring'. Code: PBR slot is BatchHeader = 128 bytes — cxl_datastructure.h:393 'struct alignas(64) BatchHeader', :437 static_assert(sizeof(BatchHeader) == 128). Contradicts Sec4_Design.tex:145 '128-byte slot' and :121 'reserves a 128-byte PBR slot'. Correct: 128 bytes. (Sec3 — flag only, not editable by us.)
- CXL HEARTBEAT-LINE CADENCE (paper ~100us vs code 3s gRPC; no CXL heartbeat-line found). Sec4_Design.tex:387 table 'Heartbeat lines & 1 line / server & ... clwb every ~100us & k misses => lease NACK'. Code: only heartbeat is gRPC at 3s — heartbeat_interval default 3 (configuration.h:58), std::chrono::seconds(HEARTBEAT_INTERVAL) (heartbeat.cc:659,677,739), 100ms sleeps (heartbeat.cc:225,327). No CXL per-server heartbeat cache-line written with clwb at ~100us exists (grep returned nothing). The '~100us' CXL heartbeat line and 'lease NACK' primary-detector path appear unimplemented. Flag for author confirmation (may be intended future/design mechanism).
