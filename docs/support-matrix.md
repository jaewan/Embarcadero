# Implemented modes and qualification limits

Admission validation is a configuration contract, not a correctness or performance certification. The [matched DRAM campaign](reviews/2026-09-22-followup-results.md) covers finite ORDER5/ACK1/RF0 transfers with one client and one or three local brokers; one-broker ACK throughput remains statistically inconclusive. The [real-CXL campaign](reviews/2026-09-23-cxl-before-after.md) additionally covers finite four-broker ORDER5/ACK1/RF0 transfers and a scoped local throughput comparison. Other implemented modes below retain their research interfaces; they have not inherited that qualification.

## Ordering and acknowledgement

| Sequencer | Admitted order | ACK semantics and limits |
|---|---|---|
| Embarcadero | 0 | ACK1 tracks per-client receive/write progress; no total ordering. The subscriber ring can overrun and skip old batches. ACK2 requires RF≥2 and the legacy replication path; its memory sink is emulation, not media durability. |
| Embarcadero | 2, 4 | Implemented research ordering paths. Their ACK attribution, multiclient behavior, subscriber retention, and fencing are not qualified by the ORDER5 tests. |
| Embarcadero | 5 | ACK1 tracks the head's per-client ordered frontier. ACK2 requires RF≥2 and the configured replication completion path. Neither means subscriber delivery has completed. |
| Scalog | 1 | Baseline local-sequencer path. ACK2 additionally requires `SCALOG_CXL_MODE=1` and RF≥2. Its replication configuration and durability require separate validation. |
| Corfu, LazyLog | 2 | Baseline-specific ordering and durable sidecar paths. ACK2 requires RF≥2; ordered/append progress is distinct from media durability. |
| `KAFKA` enum | 0, 2, 4, 5 | Historical in-tree delegation ablation, not Apache Kafka. Retained for research; no general ACK or interoperability guarantee. |

ACK0 is fire-and-forget in every mode: it provides no broker-completion guarantee. ACK levels outside 0–2, negative RF, ACK2 with RF below 2, unknown sequencers, Embarcadero ORDER1/3, Scalog orders other than 1, and Corfu/LazyLog orders other than 2 are rejected. RF includes the primary; RF1 alone cannot satisfy ACK2.

Create-topic retries must match the existing order, RF, sequencer, ACK level, and inode-replication setting. Changing ACK level requires a fresh topic/region because initialization selects ACK-dependent paths. Topic creation does not provide an in-place mode upgrade.

## Replication topology and sinks

Replicated topic admission requires **every configured broker** to be live and publishable, with exactly the contiguous IDs `0..broker.max_brokers-1`. Configure that capacity to the intended cluster size. RF cannot exceed it. Partial snapshots, duplicate/out-of-range identities, and membership holes are rejected before topic allocation or publication. This prevents different brokers from constructing different modulo replication sets. Dynamic replicated membership and automatic reconstruction after broker loss are unsupported; these admission checks are not failover fencing.

For Embarcadero's ordinary ORDER5 replication path, RF≥2 requires a running chain whose RF and broker count match the admitted topic and topology. The `EMBARCADERO_REPLICATION_FACTOR` startup setting must agree with the topic RF. A running chain with a conflicting topic RF is rejected rather than silently changing the requested guarantee. Explicit `EMBARCADERO_UNIFIED_REPLICATION_PATH` remains a research alternative and requires its own validation. Non-ORDER5 legacy replication does not require a GOI chain; its admitted topology is passed unchanged to the worker setup, never silently clamped.

| ORDER5 chain sink | Completion claim |
|---|---|
| `disk-durable` | Successful payload write and media sync precede the replica completion token. The label is `media_durable`; it does not establish recovery correctness or independent host failure domains. |
| `memory-copy` | Copies to a bounded DRAM ring and advances emulated replica completion: `replicated_ack_emulated`. Ring overwrite is not durable retention. |
| `memory-accounting` | Accounts for bytes without retaining a payload replica: `replicated_ack_emulated`. This is a protocol/performance ablation. |

The legacy `--replicate_to_disk` switch and ORDER5 chain sink selection are different controls. Without `--replicate_to_disk`, legacy replication can report completion after a memory copy; do not label that result media-durable. The chain's explicit sink setting controls its own behavior. This matrix preserves those research modes rather than silently upgrading their guarantees.

## Evidence and remaining gates

`support_contract_test` exercises the same scalar, membership, and chain-admission helpers used by `TopicManager`/`DiskManager`, including partial-membership rejection. It is not a linked broker or failure-recovery test. Existing ACK policy, durable frontier, replication topology, sidecar, and process-restart fixtures cover narrower contracts. The disk/memory-copy ACK2 scripts are executable test specifications, not evidence that every listed configuration has passed.

The owner-selected release contract is bounded primary storage: retain data and stop safely at capacity. Primary BLog allocation is finite and non-recycling. The qualified ORDER5 path retains its BLog/GOI prefix and fails closed at exhaustion; it does not evict replayable history to accommodate slow subscribers. This does not turn the ORDER0 subscriber ring or the overwriteable memory-copy replica sink into retained replay stores. Those research-mode limits remain as listed above. Real CXL visibility, independent-host durability, failure/recovery behavior, and all unqualified mode combinations require their own recorded experiments.

The paper's full durable ACK additionally requires replicated order/session/control metadata and a certified prefix across independent CXL modules. Payload `fsync` and completion-token ordering alone do not provide that recovery contract. Current order-visible delivery remains speculative across sequencer replacement; see [paper/implementation boundaries](reviews/2026-09-22-completion-results.md). Restored physical CXL node-2 placement and finite throughput results do not upgrade these failure or durability guarantees; see the [release checkpoint](reviews/2026-09-24-release-checkpoint.md).
