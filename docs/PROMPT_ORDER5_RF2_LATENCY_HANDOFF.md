# ORDER=5 RF=2 Latency Handoff Prompt

You are taking over a production-grade distributed-systems debugging and benchmarking effort in the `Embarcadero` repo at `/home/domin/Embarcadero`.

Your job is not shallow triage. Your job is to finish the remaining blocker, recover trustworthy end-to-end performance, and leave publication-quality artifacts.

## Mission

Finish the final blocked publication cell:

- System: `Embarcadero`
- Ordering: `ORDER=5`
- Replication factor: `RF=2`
- Ack policy: `ACK=2`
- Benchmark: latency

Everything else in the publication matrix is already either curated or known-good enough for comparison:

- Throughput matrix is complete and curated.
- Other latency cells are already clean.
- The last unresolved cell is `Embarcadero ORDER=5 RF=2 ACK=2 latency`.

You must preserve artifacts, avoid hand-wavy conclusions, and either:

1. fix the blocker and finish the publication latency matrix, or
2. prove the remaining root cause with exact preserved evidence.

## What Embarcadero Is

Embarcadero is a high-performance brokered pub/sub log built around CXL-shared memory and explicit broker-side ordering/replication logic. It is intended to beat Corfu, especially for the target publication configurations, by:

- keeping hot-path sequencing and data movement broker-local,
- using shared-memory metadata structures in CXL for low-overhead coordination,
- avoiding the heavier remote-log/sequencer bottlenecks of Corfu in the common case,
- exploiting batching, broker-local durability tracking, and topology-aware placement.

Publication expectation:

- Embarcadero should substantially outperform Corfu in throughput.
- Embarcadero should also produce defensible latency results for the publication matrix.
- If Embarcadero loses badly or becomes fragile in the target cell, assume there is still a real system bug until proven otherwise.

## High-Level Implementation Model

Important components:

- `src/embarlet/topic.cc`, `src/embarlet/topic.h`
  - broker-side topic state
  - ORDER=5 epoch sequencer path
  - completion-vector publication
  - export logic for subscribers / latency ACK visibility

- `src/disk_manager/chain_replication.cc`
  - ORDER=5 RF=2 replication tail progression
  - GOI token handoff
  - durable ACK2 frontier publication

- `src/disk_manager/disk_manager.cc`
  - replication batch discovery from PBR / broker log

- `src/network_manager/network_manager.cc`
  - broker networking and control path interactions

- `src/client/publisher.cc`, `src/client/publisher.h`
  - latency publisher, ACK wait, cluster-status subscription, diagnostics

Relevant shared state concepts:

- GOI: global ordered index entries committed by the sequencer
- PBR: per-broker replication/export ordering index
- CV: completion vector; contains sequencer and durable completion frontiers
- `sequencer_logical_offset`: ACK1/logical frontier
- `completed_logical_offset`: durable ACK2 frontier
- `completed_pbr_head`: export/recovery visibility frontier

For replicated topics:

- the replication tail should own the durable `completed_*` frontier
- the sequencer should not fake durable progress
- monotonicity and ownership of shared CXL metadata matter a lot

## Current Proven Findings

Do not restart from scratch. These findings are already established:

1. Throughput topology bugs were fixed earlier by exporting a routable Corfu sequencer IP to remote clients.
2. `run_latency.sh` was improved to preserve per-trial stdout/stderr and broker logs.
3. There were real ORDER=5 latency tail bugs, not just harness artifacts.
4. A CV monotonicity bug was fixed:
   - sequencer-side CV flush helpers were changed from plain stores to max-only CAS.
5. GOI refresh ordering was hardened:
   - `RefreshGOIEntry()` and `RefreshGOIToken()` now use `CXL::full_fence()` after flush.
6. Control-block refresh ordering in chain replication was also hardened with `full_fence()`.
7. Strong-order replication no longer skips across PBR gaps in `disk_manager.cc`.
8. A replication-thread drain-on-stop improvement was added in `chain_replication.cc`.
9. A real CV ownership bug was identified and patched:
   - for replicated topics, the sequencer was incorrectly writing `completed_pbr_head` and, in logical-only paths, `completed_logical_offset`
   - this raced the tail on the same CXL line and produced mixed states like newer `pbr_head` with older durable logical frontier
   - that ownership fix is in the current worktree / checkpoint commit

## What The Latest Results Mean

Two important result roots:

- `fix21`:
  - `/home/domin/Embarcadero/data/publication/latency/20260328_moscxl_publication_reviewed_fix21`
  - old failure shape persists: almost all ACKs arrive, but one source path still stalls late

- `fix22`:
  - `/home/domin/Embarcadero/data/publication/latency/20260328_moscxl_publication_reviewed_fix22`
  - after removing sequencer writes to replicated `completed_*`, the run no longer shows the old near-complete RF=2 result
  - instead it plateaus much earlier, around:
    - `B0=356720`
    - `B1=23145`
    - `B2=175468`
    - `B3=128719`
  - this is actually useful: it means the previous "almost complete" RF=2 behavior was partly being propped up by incorrect sequencer-owned durable CV publication
  - the true remaining bug is still in the actual RF=2 tail durability path

Interpretation:

- `fix21` exposed a mixed frontier symptom.
- `fix22` exposes the real tail-owned ACK2 progression path.
- The system is now more correct, but still incomplete.

## Most Relevant Artifact Evidence

Key latency roots:

- `/home/domin/Embarcadero/data/publication/latency/20260328_moscxl_publication_reviewed_fix20`
- `/home/domin/Embarcadero/data/publication/latency/20260328_moscxl_publication_reviewed_fix21`
- `/home/domin/Embarcadero/data/publication/latency/20260328_moscxl_publication_reviewed_fix22`

Key examples:

- `fix20` mixed-frontier evidence:
  - sequencer-side B3 reaches full frontier in:
    - `.../fix20/.../broker_0.log`
  - exporter remains stuck with older durable logical frontier in:
    - `.../fix20/.../broker_1.log`

- `fix21` timeout summary:
  - `.../fix21/.../run.log`

- `fix22` timeout summary:
  - `.../fix22/.../run.log`

Throughput curated summary:

- `/home/domin/Embarcadero/data/publication/throughput/20260328_moscxl_publication_reviewed_fix8/throughput_summary.csv`

## Publication Topology

Main host:

- broker host for publication runs: `moscxl`
- brokers pinned to NUMA node 1 on `moscxl`

Throughput client placement:

- 1 client: `c4`
- 2 clients: `c4`, local `moscxl` on NUMA node 0
- 3 clients: `c4`, `moscxl`, `c3`

Corfu:

- global sequencer: `c1`

Latency topology:

- publisher: `c4`
- broker host: `moscxl`
- Corfu sequencer: `c1`

## How To Sync And Run Multi-Node Experiments

Common repo root:

- `/home/domin/Embarcadero`

Useful scripts:

- `scripts/run_latency.sh`
- `scripts/run_multiclient.sh`
- `scripts/publication/run_latency_cell.sh`
- `scripts/publication/run_throughput_cell.sh`

Typical publication latency cell command:

```bash
env TAG=20260328_moscxl_publication_reviewed_fixXX \
    SYSTEM=embarcadero \
    ORDER=5 \
    SEQUENCER=EMBARCADERO \
    REPLICATION_FACTOR=2 \
    NUM_TRIALS=1 \
    RUN_ID=run_full_latency_order5_rf2_fixXX \
    EMBARCADERO_ORDER5_TRACE=1 \
    EMBARCADERO_ORDER5_PHASE_DIAG=1 \
    EMBAR_TOPIC_DIAGNOSTICS=1 \
    bash scripts/publication/run_latency_cell.sh
```

Typical publication throughput cell command pattern:

```bash
env TAG=... \
    SYSTEM=embarcadero \
    ORDER=... \
    SEQUENCER=EMBARCADERO \
    REPLICATION_FACTOR=... \
    NUM_CLIENTS=... \
    RUN_ID=... \
    bash scripts/publication/run_throughput_cell.sh
```

If you need multi-node sync, use repo-local scripts if they exist in the current checkpoint:

- `run_clients.sh`
- `run_servers.sh`
- `sync_clocks.sh`

Before large reruns:

1. verify the remote binaries/scripts are in sync
2. verify the cluster-status stream shows all brokers as `accepts_publishes=true`
3. preserve every run under a fresh append-only `data/publication/.../fixXX`

## Constraints

- Do not use `ORDER=4` for publication.
- Never overwrite or collapse failed runs.
- Preserve every failure as an inspectable artifact.
- Each run root must retain:
  - exact command
  - git commit
  - datetime
  - topology/env
  - raw stdout/stderr
  - parsed summary CSV/TSV

## Current Code Areas To Inspect Next

Primary focus:

- `src/disk_manager/chain_replication.cc`
- `src/disk_manager/disk_manager.cc`
- `src/embarlet/topic.cc`

Secondary:

- `src/network_manager/network_manager.cc`
- `src/client/publisher.cc`

Suggested next debugging direction:

1. treat `fix22` as the truthful baseline for RF=2 ACK2 durability
2. inspect why tail-owned durable ACK2 frontiers stall early across all sources
3. determine whether the stall is in:
   - GOI commit visibility
   - replication enqueue
   - write-worker completion
   - token handoff
   - final tail `UpdateCompletionVector()`
4. prefer targeted diagnostics over speculative rewrites
5. once you have a fix, rerun under a fresh append-only tag and compare against `fix21` and `fix22`

## What Success Looks Like

Best case:

- full throughput matrix remains clean
- final latency cell becomes clean
- final curated latency summary is regenerated under a fresh tag

Acceptable fallback only if necessary:

- publication latency still blocked, but the exact root cause is proven with preserved artifacts and no ambiguity

## Work Style

- Think like an owner.
- Do not erase evidence.
- Do not paper over failures with retries.
- Use the latest preserved artifacts first.
- Every claim must cite exact files.
- Every code change must explain why the old behavior was wrong and why the new behavior is correct.

