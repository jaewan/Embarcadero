# Scalog Canonical Progression Contract

This note defines the authoritative progression and visibility contract for Scalog-in-Embarcadero so ACK, export, and replication semantics stay aligned.

## Scope

- Sequencer type: `SCALOG`
- Ordering mode: `ORDER=1` (`kOrderPerBroker`)
- ACK levels:
  - `ACK=1`: ordered visibility
  - `ACK=2`: durability over replication set

## Authoritative Frontiers (Per Broker)

- `written`:
  - Meaning: delegated/published local ingress progress (message count).
  - Writer: broker ingress/delegation path.
  - Use: producer-side local progress, local-cut input.
- `ordered`:
  - Meaning: cumulatively globally ordered and visible message count.
  - Writer: Scalog local sequencer after applying global-cut deltas.
  - Use: ordered visibility for both export and `ACK=1`.
- `replication_done[replica][source_broker]`:
  - Meaning: last replicated offset observed by each replica.
  - Use: `ACK=2` (durable frontier is min across replication set, converted to count).

## Contract Rules

1. Global cuts from the wire are cumulative and must be applied as deltas exactly once.
2. `ordered` is monotonic and advances in the same progression order consumed by export.
3. `ACK=1` for Scalog `ORDER=1` reads `ordered` (never `written`).
4. Export cannot return a batch whose end logical offset exceeds `ordered`.
5. `ACK=2` is durability-only and independent from export readiness.

## Why this matters

Without a single shared visibility frontier, benchmark comparisons become contract-mismatched:

- Export might show data that `ACK=1` would not acknowledge yet, or
- `ACK=1` could acknowledge data that is not export-visible.

Scalog now uses `ordered` as the unified visibility frontier to avoid this drift.

## Runtime Guardrails in Code

- Cumulative-to-delta global cut consumption:
  - `src/cxl_manager/scalog_local_sequencer.cc`
- Scalog `ACK=1` on ordered frontier:
  - `src/network_manager/network_manager.cc`
- Scalog export visibility guard (batch end <= ordered):
  - `src/embarlet/topic.cc`
