# Documentation

Design, evaluation, and operational documentation for Embarcadero.

## Start here

- [Build and dependency bootstrap](development-build.md)
- [Supported development commands](development-commands.md) and [local DRAM setup](development-dram.md)
- [Supported modes and limits](support-matrix.md)
- [Production component boundaries](architecture/refactoring-boundaries.md)
- [Test inventory and fault campaign](../test/README.md)
- [Refactoring completion ledger](reviews/2026-09-22-refactoring-completion-plan.md) and [verification evidence](reviews/2026-09-22-completion-results.md)
- [Matched performance protocol](performance-pilot.md)

The supported local workflow uses explicit DRAM emulation with a 64 GiB region,
NUMA node 1 for brokers, and node 0 for clients. Historical research documents
below describe earlier layouts and experiments; use the current support and
development guides for executable commands and implementation limits.

## Layout

| Directory | Contents |
|-----------|----------|
| `architecture/` | Current production component boundaries and invariants. |
| `reviews/` | Refactoring plans, review findings, fault specifications, and qualification results. |
| `design/` | Research design and historical specifications, including `EMBARCADERO_DEFINITIVE_DESIGN.md`, layout v2, Order-5 optimization, and Scalog progression. Earlier layout documents are not the current shared-region ABI. |
| `baselines/` | Analysis of baseline systems (`SCALOG_LIMITATION.md`). |
| `experiments/` | Evaluation methodology, checklists, and audits — latency experiment checklist, latency-vs-load, publication evaluation audit, buffer-scheme assessment, hold-timeout experiment. |
| `operations/` | Runbooks & configuration reference — `configuration.md`, Corfu sequencer C3 runbook, `history-purge-runbook.md` (git history maintenance). |
| `agent-prompts/` | Archived AI-agent task prompts kept for provenance (KV store, Scalog/Embarcadero, Order-5 handoffs). |
| `memory-bank/` | Working technical context and active-work notes. |
| `perf/` | Performance analysis notes. |
| `context/` | Generated codebase map (`codebase_map.xml`) — regenerate after structural changes. |

## Paper and historical design

- **Architecture:** [`design/EMBARCADERO_DEFINITIVE_DESIGN.md`](design/EMBARCADERO_DEFINITIVE_DESIGN.md)
- **CXL memory layout:** [`design/CXL_MEMORY_LAYOUT_v2.md`](design/CXL_MEMORY_LAYOUT_v2.md)
- **Configuration:** [`operations/configuration.md`](operations/configuration.md)
- **Paper:** [current text](../Paper/Text/) and [previous SOSP draft](../Paper/SOSP_Text/) (LaTeX sources; managed separately).
