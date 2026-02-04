# Design Note: Sequencer-Only Head Node (Option C.2)

**Date:** 2026-01-31  
**Goal:** Eliminate B0 same-process writer/reader by making the head node sequencer-only (no data ingestion). See LOCKFREE_BATCH_HEADER_RING_DESIGN.md §9.

---

## 1. Key Design Decisions

### 1.1 Data Broker Set

**Decision:** Explicit configuration, not implicit.

```yaml
# config/embarcadero.yaml
cluster:
  sequencer_broker_id: 0          # Which broker ID runs sequencer
  data_broker_ids: [1, 2, 3]      # Which broker IDs accept client writes
```

- `sequencer_broker_id` identifies which node runs the sequencer (and no data path).
- `data_broker_ids` is the set of brokers that have batch header rings and accept publishes.
- These two sets are disjoint by design.

### 1.2 CXL Layout Allocation

**Current behavior:** Each broker that creates/joins a topic allocates its own segment + batch header ring and writes `tinode_->offsets[broker_id_]` (TopicManager::CreateNewTopicInternal → GetNewSegment, GetNewBatchHeaderLog, InitializeTInodeOffsets).

**Proposed behavior (minimal change):** When the node is sequencer-only (`is_sequencer_node` and `broker_id_ == 0`), skip allocation and skip initializing `offsets[0]` during topic creation. Data brokers 1, 2, 3 continue to allocate their own rings and write `offsets[1]`, `[2]`, `[3]` as today.

- **Implication:** `tinode_->offsets[0]` remains zeroed/unused on the sequencer node. Code that accesses `offsets[broker_id]` for scanning must only use `broker_id ∈ data_broker_ids`.
- **Alternative (larger change):** Sequencer pre-allocates layout for all `data_broker_ids` and writes their offsets; data brokers then only map CXL and use pre-filled offsets. This requires a new “sequencer allocates, data broker maps” flow; the minimal change above avoids that.

### 1.3 Broker ID 0 on Sequencer Node

The sequencer process still has `broker_id_ == 0`, but:

| Component | Behavior on Sequencer Node |
|-----------|-----------------------------|
| NetworkManager | Not started (no client connections) |
| CXLAllocationWorker | Not started |
| BrokerScannerWorker5[0] | Not started (no B0 ring to scan) |
| BrokerScannerWorker5[1,2,3] | Started (scan remote rings) |
| Topic::GetCXLBuffer for B0 | Never called (no ingest path) |

**Audit required:** Search for `broker_id == 0` or `broker_id_` usage in topic.cc (GetCXLBuffer, Sequencer5), network_manager.cc (startup, ingest paths), cxl_manager.cc (layout allocation). Ensure no path on the sequencer node allocates or reads B0’s ring.

### 1.4 Heartbeat / Discovery Contract

Sequencer node participates in heartbeat but advertises:

```protobuf
message HeartbeatResponse {
  int32 broker_id = 1;
  bool is_sequencer = 2;       // true for sequencer-only node
  bool accepts_publishes = 3;  // false for sequencer-only node
  // ...
}
```

**Client behavior:**

- `GetRegisteredBrokerSet()` returns {0, 1, 2, 3} (full cluster membership).
- Client filters to only connect to nodes where `accepts_publishes == true`.
- Scanner set on sequencer = `data_broker_ids` from config (not full membership).

### 1.5 Startup Sequence

**Sequencer Node (broker_id=0, is_sequencer=true):**

1. Load config (`data_broker_ids = [1,2,3]`).
2. Create topic without allocating B0 ring (skip GetNewSegment/GetNewBatchHeaderLog and InitializeTInodeOffsets for broker 0).
3. Start heartbeat (advertise `is_sequencer=true`, `accepts_publishes=false`).
4. Wait for data brokers to register (optional; scanners wait on `offsets[broker_id].log_offset != 0` anyway).
5. Start Sequencer5 with scanners for `data_broker_ids` only (from config).

**Data Broker (broker_id=1, is_sequencer=false):**

1. Load config.
2. Map CXL layout (shared with sequencer).
3. Create/join topic: allocate own segment + batch header ring, write `offsets[broker_id_]`.
4. Start heartbeat (advertise `is_sequencer=false`, `accepts_publishes=true`).
5. Start NetworkManager (accept client connections).

---

## 2. Implementation Checklist

### PR1 (Config + Topic Creation) - COMPLETE
- [x] Add `is_sequencer_node` / `data_broker_ids` / `sequencer_broker_id` to config.
  - Added `Cluster` section in `src/common/configuration.h` with ConfigValue wrappers
  - Added YAML parsing in `src/common/configuration.cc`
  - Environment variable overrides: `EMBARCADERO_IS_SEQUENCER_NODE`, `EMBARCADERO_SEQUENCER_BROKER_ID`
- [x] Modify topic creation path: when `is_sequencer_node` and `broker_id_ == 0`, skip GetNewSegment/GetNewBatchHeaderLog and skip InitializeTInodeOffsets for broker 0.
  - Modified both `CreateNewTopicInternal` overloads in `src/embarlet/topic_manager.cc`
  - Added `is_sequencer_node_` member to `TopicManager` class
  - Topic object still created (for running Sequencer5) but with nullptr segment_metadata
- [x] Wire `is_sequencer_node` from Configuration to TopicManager in `src/embarlet/embarlet.cc`

### PR2 (Startup + Scanners) - COMPLETE
- [x] Modify Sequencer5 to spawn scanners only for `data_broker_ids` (from config), not from GetRegisteredBrokerSet (or filter: scanner set = GetRegisteredBrokerSet ∩ data_broker_ids).
  - Added `is_sequencer_only_` member to `Topic` class (detected from `segment_metadata==nullptr && broker_id==0`)
  - Modified `Topic::Topic` constructor to skip offset validation and address initialization when `is_sequencer_only_`
  - Modified `Sequencer5()` to filter out B0 from `registered_brokers` when `is_sequencer_only_`
- [x] Modify embarlet.cc startup to skip NetworkManager (and CXLAllocationWorker for B0) when `is_sequencer_node`.
  - Added `skip_networking` parameter to `NetworkManager` constructor
  - When `skip_networking=true`, skip creating listener/receiver/CXLAllocationWorker threads
  - Wired `is_sequencer_node` from config to NetworkManager in `embarlet.cc`
- [x] Audit `broker_id == 0` usage in topic.cc, cxl_manager, network_manager.
  - All paths safe: CXL init (epoch, bitmap, CompletionVector) are one-time shared resource init that B0 must do
  - `is_sequencer_only_` flag in Topic prevents accessing unallocated offsets[0]

### PR2 Test Results
Tested with `is_sequencer_node: true` config:
```
[SEQUENCER_ONLY] NetworkManager: skip_networking=true, no listener/receiver threads
Embarcadero initialized. Ready to go
```
- ✅ NetworkManager correctly skips all threads (listener, receivers, CXL workers)
- ✅ Process starts without FATAL crash
- ✅ Defense in depth guard added to EmbarcaderoGetCXLBuffer

### PR3 (Heartbeat + Client + Tests) - COMPLETE
- [x] Update heartbeat to advertise `accepts_publishes`.
  - Added `accepts_publishes` field to `BrokerInfo` message in `heartbeat.proto`
  - Added `accepts_publishes` to `NodeEntry` struct in `heartbeat.h` (both HeartBeatServiceImpl and FollowerNodeClient)
  - Added `is_sequencer_node_` member to `HeartBeatServiceImpl`
  - Updated constructor to accept `is_sequencer_node` param (wired from HeartBeatManager → embarlet.cc)
  - Head node sets `accepts_publishes=false` when `is_sequencer_node=true`, `true` otherwise
  - All registered follower (data) brokers set `accepts_publishes=true`
  - `FillClusterInfo()` includes `accepts_publishes` in BrokerInfo
  - Added `broker_info` field to `ClusterStatus` message for client filtering
  - `SubscribeToCluster()` populates `broker_info` with full broker details
- [x] Update client to filter brokers by `accepts_publishes` for publish connections.
  - Modified `Publisher::SubscribeToClusterStatus()` in `publisher.cc`
  - Client uses `broker_info` (with backward compatibility for `new_nodes`)
  - Only connects to brokers where `accepts_publishes=true`
  - Logs skipped brokers with `accepts_publishes=false` (sequencer-only nodes)
- [x] Update test scripts for new startup sequence (1 sequencer + N-1 data brokers).
  - Created `config/embarcadero_sequencer_only.yaml` with `is_sequencer_node: true`
  - Created `test/e2e/test_sequencer_only_topology.sh` E2E test
  - Test verifies: head node in sequencer-only mode, NetworkManager skipped, client filters to data brokers
  - Added to `test/e2e/run_all.sh` test suite

---

## 3. Risk: CXL Layout (Phase 3)

The CXL layout change touches the path that everything else depends on. Mitigation:

1. **Small first PR:** Config + topic creation branch only (skip B0 allocation when sequencer-only). No heartbeat/client changes yet. Validate: start sequencer-only process, create topic, confirm no B0 ring allocated and offsets[0] unused.
2. **Then:** Wire startup (skip NetworkManager when sequencer-only) and Sequencer5 scanner set.
3. **Then:** Heartbeat + client filter + test scripts.

---

## 4. References

- LOCKFREE_BATCH_HEADER_RING_DESIGN.md §9 (B0 local-ring cache coherency)
- src/embarlet/topic_manager.cc (CreateNewTopicInternal, InitializeTInodeOffsets)
- src/embarlet/topic.cc (Sequencer5, BrokerScannerWorker5)
- src/embarlet/embarlet.cc (startup)
