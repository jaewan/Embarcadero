# Production Runtime Hardening Notes

This document tracks the multi-host production gate for Embarcadero.

## Failure-domain abstraction

Until RDMA/network replicas land, guarantees remain **process failure** on a single
`moscxl` host. Dual NVMe protects a disk copy, not host/power domains.

Planned transport modes:

| Mode | Transport | Domain |
|------|-----------|--------|
| `local_cxl` | coherent/local CXL | single host |
| `rdma_replica` | RDMA/network | distinct host/power |

## Quorum sequencer election

Replace ad-hoc head failover with:

1. Persisted control epoch
2. Quorum vote among brokers
3. Explicit fence of the old head before promotion

## Observability checklist

- Per-stage latency histograms (`EMBARCADERO_STAGE_TRACE=1`)
- ACK1 / ACK2 lag
- Replica sync generations
- Segment / GC state
- PBR / export occupancy
- Reconnect / fence rates
- Effective config dump after YAML/env/CLI resolution
- NUMA placement, NIC counters, disk latency/errors

## Security checklist

- mTLS on data/control paths
- AuthZ for topic create / publish / admin
- Schema/version compatibility checks
- Strict fail-closed configuration validation

## Chaos matrix

Process crash, head crash, disk loss, ENOSPC, network partition, clock skew,
CXL fault injection, host power loss — each must prove no acknowledged loss.

## Current claim boundary (2026-07)

| Claim | Status |
|-------|--------|
| Process failure on single `moscxl` host | In scope for ACK2 with RF≥2 + media sync |
| Dual-NVMe disk copy | Protects disk medium, not host/power |
| Host / power-domain failure | Out of scope until `rdma_replica` + quorum |
| Quorum sequencer election | Scaffolded in docs; not implemented |
| mTLS / AuthZ | Checklist only |

Use `Embarcadero::DefaultSingleHostDomain()` and
`ClaimHostFailureTolerance()` from `src/common/failure_domain.h` so tooling
cannot silently advertise host tolerance.
