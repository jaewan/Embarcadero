# Track 03 — Baseline CXL-transport ports (W4)

**Read `sessions/00-BACKGROUND.md` first.** Track id: `03-baseline-ports`. Branch:
`session/03-baseline-ports`.

**Goal.** Give each baseline (Corfu, Scalog, LazyLog) the best CXL adaptation we can build, so the
fairness story is airtight and the decomposition waterfall (E1) has its middle leg. Plus a
per-baseline **fairness appendix**. Independent of Track 01 — start now.

**Plan sections:** `Paper/improvement_plan.md` → W4, E1 (waterfall), IV.0 rule 3, I.3 (concessions),
E10 (batched CXL-Corfu sequencer is the honest baseline).

## You OWN
- A new shared-memory mailbox RPC lib built from **our own** primitives (single-writer rings,
  clwb/sfence) — put it under `src/common/` or a new `src/cxl_transport/` dir (create it).
- Baseline adaptation code paths: `src/cxl_manager/scalog_*`, `src/cxl_manager/corfu_*`,
  `src/cxl_manager/lazylog_*`, and their replication clients in `src/disk_manager/*` **as needed to
  route their coordination over the CXL mailbox** (Corfu token path, Scalog cut/report path, LazyLog
  coordinator path).
- `docs/baselines/fairness_appendix.md` (new): per baseline — what we kept / changed / why.

## Do NOT touch
- `src/embarlet/topic.{cc,h}` and the epoch collector (Track 01).
- Embarcadero's own client session logic (Track 01), the fault-injection layer (Track 04), RDMA
  transport (Track 05), any `Paper/Text/*.tex`.
- If you genuinely need a shared primitive that lives in a Track-01 file, add it to a **new** file
  under `src/common/`/`src/cxl_transport/` instead of editing theirs; note the coupling in your
  handoff.

## Scope
- **Write down the ex ante porting rule FIRST** (v4/W4, panel #4 — this is your W1 on-paper closure,
  due ~2 weeks, before any port): *protocol-preserving substitution only — same messages, same state
  machine, transport swapped.* Anything that alters a baseline's serialization structure is a
  **redesign, not a port, and is forbidden.** This keeps the E1 "baseline-on-CXL" leg from being a
  free parameter that can be tuned into "Embarcadero-minus-hold-buffer," which would make the
  waterfall meaningless. Put the rule in `docs/baselines/porting_rule.md`.
- Build the CXL mailbox RPC once; port each baseline's control path onto it under the rule (Corfu
  token path, Scalog cut/report path, LazyLog coordinator path) — transport swap only.
- Reproduce the **batched CXL-Corfu sequencer** (E10's honest baseline; the per-message-mutex
  strawman is deleted). Keep it comparable to Embarcadero's sequencer.
- **Calibration (v4/W4, panel #10):** show each reimplementation-on-TCP reproduces its original
  paper's published numbers within a stated tolerance on comparable hardware → converts "trust our
  port" into "our port matches theirs." Record in the fairness appendix.
- **Baseline-author sanity check (panel #10):** draft short emails to the **LazyLog and Scalog**
  authors asking them to review the port descriptions; "baseline authors reviewed our adaptation" is
  worth an appendix sentence. (Flag to the human to send.)
- Open-source-ready: clean READMEs so the ports are a reviewable artifact.

## Build/test (BACKGROUND protocol)
- Build in `~/Embarcadero-sessions/03-baseline-ports/` (`-j 64`, cached gRPC). Targets:
  `scalog_global_sequencer`, `lazylog_global_sequencer`, `corfu_global_sequencer`, `corfu_sequencer_fifo_smoke`.
- Correctness smoke runs (`corfu_sequencer_fifo_smoke`, baseline cluster bring-up): **take the
  `flock` lock**.

## Done criteria
- `docs/baselines/porting_rule.md` written (the W1 on-paper closure) **before** any port.
- CXL mailbox lib + all three baseline control paths build clean on `broker`.
- Each baseline runs its own smoke/throughput check under the lock (≥3 trials); numbers noted for
  E1's middle leg; **calibration vs. original published numbers** recorded.
- `docs/baselines/fairness_appendix.md` complete (kept/changed/why + calibration + author-check
  status). Handoff note with staged pathspec + commit message and any primitive-coupling flags for
  Track 01.
