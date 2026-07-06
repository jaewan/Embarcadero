# Track 01 — Core protocol (the trunk)

**Read `sessions/00-BACKGROUND.md` first.** Track id: `01-core-protocol`. Branch:
`session/01-core-protocol`.

**Goal.** Land the semantic + availability fix in the sequencer/collector: session-based FIFO
(D1), CXL heartbeat leases (D2), commit-driven PBR frontier (D3), and GOI assign-at-release (D4).
This is the critical path — everything in the evaluation depends on it. You own the hot files;
other sessions must not touch them.

**Plan sections:** `Paper/improvement_plan.md` → D1–D5, W1, W3, D8. Read them in full.

## You OWN (only this track edits these)
- `src/embarlet/topic.cc`, `src/embarlet/topic.h` (epoch collector, hold buffer, CommitEpoch)
- `src/embarlet/buffer_manager.{cc,h}`, `src/embarlet/topic_manager.{cc,h}`
- `src/cxl_manager/cxl_manager.{cc,h}`, `src/cxl_manager/cxl_datastructure.h` (control block, session tables, heartbeat lines)
- `src/client/publisher.*`, `src/client/subscriber.*`, `src/client/common.*` (sessions, adaptive-δ retransmit, warm connections, `SESSION_FENCED`+HWM-in-payload recovery)
- Session/lease protobufs under `src/protobuf/` (new messages only)

## Do NOT touch
- Baseline sequencers (`*scalog*`, `*lazylog*`, `*corfu*`) — Track 03 owns baseline adaptation.
- The CXL accessor interposition layer — Track 04.
- Any `Paper/Text/*.tex`.

## Step 1 — W1 verification checklist (BEFORE writing new code)
Do these first and record findings in `docs/experiments/w1_findings.md` (new). They size the rest.
1. **Coupled δ / E2E-tail item (v4/W1.1, panel #5):** δ (the retransmit constant) and the E2E tail
   are **one problem** — δ cannot be set until append P99.9-under-load is known, else clients
   spuriously retransmit in steady state. E2E P99 ≈ 86.7 ms vs append P50 ≈ 745 µs (100× gap); KV
   path hits ~1.0 Mops/s, so it's likely subscriber poll-scheduling/batching. Diagnose (and fix or
   fully explain) **before writing a word**. Instrument the subscriber path in
   `src/client/subscriber.*` and the export path in `topic.cc`.
2. **In-order ACK invariant:** confirm the sequencer commits per-session batches strictly in order
   and ACKs only on GOI commit, across epoch-collector seals. If violated, D1 suffix-fencing needs
   collector-layer rework (this is a Week-2 gate — flag the human immediately if broken).
3. **GOI assign-at-release feasibility** in the triple-buffered collector (D4).
4. **PBR frontier semantics:** scan-driven or commit-driven today? (epoch-collection path in
   `topic.cc`). Determines the size of the D3 change.

## Step 2 — Implement (D1–D4), after W1
- **D1 sessions:** `(client_id, session_epoch)`; per-session batch seq; sequencer `expected_seq`
  per session; dedup (`seq < expected` rejected — one comparison on the fast path); commit only in
  order, never past a gap; **hold-expiry ⇒ session fencing, never gap-skip**. Client lib buffers
  unACKed batches and retransmits batch *n* to a **different** broker with **adaptive δ**
  (TCP-RTO-style: δ = SRTT + 4·RTTVAR on observed ACK latency), the **lease-driven NACK (D2) as the
  primary trigger** and the timer as a **backstop for silent loss**. **Survivor selection by
  rendezvous hashing on `(client_id, batch_seq)`** to spread the retransmit herd (panel #5).
  **Fold the session committed-HWM into the `SESSION_FENCED` payload** (self-audit deletion, panel
  #8) — the fence response carries the HWM so a fenced-but-live client resubmits exactly the
  uncommitted suffix under a new session; **no separate HWM-query mechanism.**
- **D2 leases:** per-broker monotonic heartbeat line every ~100 µs; sequencer suspects after k
  misses (~1 ms) and drives the D1 NACK; broker-status epoch in control block; client keeps ≥2 warm
  connections. **Redirect-notice push is a DEMOTED optimization** — the adaptive timer already gives
  correctness; implement it only if time allows. Principle, **scoped to brokers** (panel #6):
  *detection is a performance hint, never a safety input.* (Sequencer false-positive election is
  separately safe via epoch tags + spatial guard; only GOI-reclamation *liveness* rides the slow
  out-of-band fence.)
  - **NEW correctness requirement — stale-CV ACK-relay epoch check (D2/v4):** brokers must
    **validate the active control-block epoch on the ACK-relay path** before advancing any client
    ACK. Otherwise an `S_old` that advances CV entries post-election could make a broker relay ACK1
    for an entry that never becomes visible — violating the theorem's "ACKed iff committed." This
    interleaving is in the W2 TLA⁺ list; coordinate with Track 02.
- **D3 frontier:** make PBR reclamation **commit-driven** (slot freed at commit, not scan), so held
  batches are reconstructible from PBRs after failover. **Delete per-session credit windows** —
  backpressure emerges from ring fill. Enforce provisioning inequality **session lease < PBR fill
  time**.
- **D4 assign-at-release:** exclude held batches from the epoch `fetch_add`; assign GOI seq at
  release/commit; keep `committed_seq` monotonic for the spatial guard.

**Fast path must not regress** — capture a before/after microbenchmark (feeds E8). Two TLA⁺
scenarios are where bugs will live — **model them before you finalize the code** (coordinate with
Track 02): (a) a retransmitted duplicate arriving at S_new inside the spatial-guard window (D4);
(b) the stale-CV ACK relay after election (D2).

## Build & test (see BACKGROUND for the protocol)
- Build in your own remote tree (`-j 64`, cached gRPC). Target `embarlet` + `throughput_test`.
- Running the cluster / e2e / microbenchmarks: **take the `flock` lock** (exclusive testbed).
- Regression: run the existing publication harness (`scripts/`) under the lock; ≥3 trials.

## Done criteria
- W1 findings documented; any invariant violation escalated to the human.
- D1–D4 + the stale-CV ACK-relay epoch check implemented; `embarlet` + client build clean on
  `broker`; fast-path microbenchmark shows no regression; a broker-kill smoke test shows per-session
  stall ≈δ (single-digit ms, not 660 ms).
- Handoff note with staged pathspec + commit message(s), and the **frozen session/lease design**
  that Tracks 02 and 05 depend on.
