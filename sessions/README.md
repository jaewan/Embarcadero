# sessions/ — parallel work briefs

Each file is a self-contained brief for one parallel Claude Code session working on the Embarcadero
resubmission (`Paper/improvement_plan.md`, **v4**). Target venue is a *schedule* decision:
**SOSP'27 is the realistic primary; OSDI'27 only if the schedule lands** — neither is gated on
multi-host CXL. The thesis is the **conjunction** (failure independence ∧ no-shared-endpoint),
proved on owned RDMA hardware (W5-A/W5-B).

**Every session starts by reading [`00-BACKGROUND.md`](00-BACKGROUND.md)** (shared context, repo
layout, remote build/test protocol on `ssh broker`, and the `flock` testbed lock), then its own
track file.

## Tracks

| File | Track | Starts | Owns (conflict domain) |
|------|-------|--------|------------------------|
| [`01-core-protocol.md`](01-core-protocol.md) | Core protocol: W1→W3, D1–D4 (sessions, leases, frontier, assign-at-release) | now (critical path) | `src/embarlet/topic.*`, `src/cxl_manager/*`, `src/client/*` |
| [`02-tla-spec.md`](02-tla-spec.md) | TLA⁺ spec (W2) | now | `spec/` |
| [`03-baseline-ports.md`](03-baseline-ports.md) | Baseline CXL-transport ports (W4) | now | CXL mailbox lib, baseline sequencers, `docs/baselines/` |
| [`04-fault-injection.md`](04-fault-injection.md) | Non-coherence fault-injection harness (W6) | now | fault-inj layer, `docs/experiments/fault_injection.md` |
| [`05-rdma-variant.md`](05-rdma-variant.md) | Embarcadero-on-RDMA — **two variants W5-A/W5-B** (conjunction proof, E4e) | after 01 design frozen | `src/rdma_transport/` |
| [`06-related-work-positioning.md`](06-related-work-positioning.md) | Related work + positioning (Part I: conjunction thesis, 2-claim novelty) | now | `Paper/Text/RelatedWork.tex`, Limitations/UseCases |
| [`07-presentation-artifacts.md`](07-presentation-artifacts.md) | Theorem (both properties) + tables + pseudocode + glossary (D8, D9, G5, V.2) | now | `Paper/Text/Sec4_Design.tex`, `Sec5_FaultTolerance.tex`, Glossary |

## How to spawn a session
Open a new session at the repo root and give it:

> Read `sessions/00-BACKGROUND.md`, then `sessions/0N-<track>.md`, and execute that track. Work only
> on your track's owned files. Build/test on `ssh broker` per the background protocol; take the
> `flock` lock for exclusive testbed runs. Do not commit unless I ask — leave a handoff note.

## First deliverables: W1's seven on-paper closures (~2-week deadline, no code)
v4/§W1 items 5–11 are the highest-leverage work and are already folded into the writing/spec tracks —
each treats its closure as its **first** output: (5) conjunction rewrite + (7) 2-claim novelty →
Track 06; (6) two-variant W5 spec → Track 05; (9) porting rule → Track 03; (10) D9 self-audit table →
Track 07; (11) stale-CV TLA⁺ scenario → Track 02; (8) SLO-curve + per-baseline knee predictions →
predictions file (below).

## Not covered here (human tasks)
- **W7 multi-host CXL hardware pursuit** — outreach/logistics (XConn, Samsung, SK hynix, PolarDB
  authors, CXL consortium). **v4: this is UPSIDE, not a gate** — no track blocks on it. The week-8
  decision chooses *which paper exists* (full CXL-thesis vs. narrower substrate-comparative, SOSP'27),
  not whether work proceeds. Not a coding session — track separately.
- **IV.1 pre-registered predictions** — commit the predictions file *before* any headline experiment
  (E1/E2). v4: predict **throughput compression (~1.2–1.6×)** AND **per-baseline knee locations**;
  headline is a **throughput-vs-SLO curve across 1–100 ms**, not a single threshold. Do it up front
  (with Track 01 or yourself).
- **Evaluation matrix E1–E10** — starts once Track 01 lands + predictions are registered. E4e (the
  W5-A/W5-B conjunction contrast) is the money figure.
