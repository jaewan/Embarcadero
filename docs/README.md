# Documentation

Design, evaluation, and operational documentation for Embarcadero.

## Layout

| Directory | Contents |
|-----------|----------|
| `design/` | Core system design & specs — `EMBARCADERO_DEFINITIVE_DESIGN.md` (master), `CXL_MEMORY_LAYOUT_v2.md`, Order-5 batch-ordering optimization, Scalog progression contract. |
| `baselines/` | Analysis of baseline systems (`SCALOG_LIMITATION.md`). |
| `experiments/` | Evaluation methodology, checklists, and audits — latency experiment checklist, latency-vs-load, publication evaluation audit, buffer-scheme assessment, hold-timeout experiment. |
| `operations/` | Runbooks & configuration reference — `configuration.md`, Corfu sequencer C3 runbook, `history-purge-runbook.md` (git history maintenance). |
| `agent-prompts/` | Archived AI-agent task prompts kept for provenance (KV store, Scalog/Embarcadero, Order-5 handoffs). |
| `memory-bank/` | Working technical context and active-work notes. |
| `perf/` | Performance analysis notes. |
| `context/` | Generated codebase map (`codebase_map.xml`) — regenerate after structural changes. |

## Start here

- **Architecture:** [`design/EMBARCADERO_DEFINITIVE_DESIGN.md`](design/EMBARCADERO_DEFINITIVE_DESIGN.md)
- **CXL memory layout:** [`design/CXL_MEMORY_LAYOUT_v2.md`](design/CXL_MEMORY_LAYOUT_v2.md)
- **Configuration:** [`operations/configuration.md`](operations/configuration.md)
- **Paper:** `Paper/Text/` (LaTeX source; managed separately)
