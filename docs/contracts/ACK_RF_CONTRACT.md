# ACK / Replication Factor Contract

This document is the authoritative publish-completion contract for Embarcadero.

## Replication factor

`replication_factor` **includes** the CXL primary:

| RF | Meaning |
|----|---------|
| 0  | Invalid / unused for ACK2. Treat as invalid with ACK=2. |
| 1  | Local CXL primary only. No remote durable replica. |
| 2  | CXL primary + one media-durable disk replica. |
| N  | CXL primary + (N-1) media-durable disk replicas. |

Until multi-host work lands, guarantees are scoped to **process failure** on the single `moscxl` host. Dual NVMe protects a disk copy, not the host or power domain.

## CXL persistence claim boundary

CXL memory is treated as **volatile** unless the deployment explicitly provisions
DAX/persistent CXL and records that mode in provenance. For ACK2, durability is
defined by the `RF-1` media-durable disk replicas (`fdatasync`/equivalent), not by
CXL residency. After process restart, ordered/durable frontiers must be
reconstructible from GOI + disk replicas.

## ACK levels

| ACK | Guarantee |
|-----|-----------|
| 0 | Fire-and-forget. No broker completion guarantee. |
| 1 | Payload is visible in CXL and the ORDER=5 GOI commit is visible. Independent of RF. |
| 2 | Valid only for `RF >= 2`. Advances only after all required `RF-1` disk replicas are media-durable (`fdatasync`/equivalent). |

## Fail-closed policy

- Reject `ack_level` outside `{0,1,2}`.
- Reject `ACK=2 && RF<2` at client init and broker topic create.
- Do not silently promote RF.
- Handshake paths may refuse `ack=2` when the topic RF is below 2.

Validation lives in [`src/common/ack_rf_policy.h`](../../src/common/ack_rf_policy.h) and must be called from:

- broker topic create (`topic_manager.cc`)
- client startup (`client/main.cc`)
- client `CreateNewTopic` (`client/common.cc`)
