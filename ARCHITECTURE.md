# Architecture

A short map of the system. For the authoritative design see
[`docs/design/EMBARCADERO_DEFINITIVE_DESIGN.md`](docs/design/EMBARCADERO_DEFINITIVE_DESIGN.md)
and the memory layout in
[`docs/design/CXL_MEMORY_LAYOUT_v2.md`](docs/design/CXL_MEMORY_LAYOUT_v2.md).

## Core idea

Embarcadero decouples payload ingestion from ordering (write-before-order), but makes the
ordering coordination **memory-local** over CXL instead of network-bound:

1. Clients send messages to stateless **log servers** (brokers).
2. A broker appends the payload to its **per-server log in CXL shared memory** and publishes a
   small (single cache-line) **metadata record**, also in CXL.
3. A **sequencer** polls all servers' metadata via memory loads, assigns global sequence numbers,
   and records decisions in a shared index — using a bounded **hold buffer** to preserve
   per-client FIFO / ack ordering without sitting on the write path.
4. Correctness on non-coherent CXL 2.0 relies on single-writer ownership, monotonic updates,
   host-local cache-line flushes, and poll-based state transitions.

## Modules (`src/`)

| Module | Responsibility |
|--------|----------------|
| `embarlet/` | The broker process (`embarlet`): topics, segments, buffers, replication orchestration, heartbeat, GOI recovery. |
| `cxl_manager/` | CXL shared-memory coordination and the sequencer implementations (Embarcadero Order-5, plus Scalog / LazyLog / Corfu baselines, each with local + global variants). |
| `disk_manager/` | Durability & replication (chain replication; per-baseline replication managers/clients). |
| `network_manager/` | Client↔broker and broker↔broker networking. |
| `client/` | Pub/sub client library and the `throughput_test` / benchmark drivers. |
| `common/` | Configuration (YAML), performance utilities, order-level definitions, compatibility shims. |
| `protobuf/` + `cmake/` | gRPC/Protobuf schemas for sequencing & replication, and the CMake rules that compile them. |

## Ordering levels

`ORDER=0` (unordered fast path) … `ORDER=5` (Embarcadero's shard-worker total-ordering with
hold buffer). Baselines (Scalog, LazyLog, Corfu) are selected via `SEQUENCER=` in the run
scripts. See the design doc for the per-level protocol.

## Data flow (append)

```
client → broker (per-server log in CXL) → publishes 64B metadata in CXL
                                             ↓ (memory poll)
                                         sequencer → global order + shared index
                                             ↓
                                         replication (disk_manager) → ack to client
```

## Build targets

The broker (`embarlet`) and the standalone global sequencers (`scalog_global_sequencer`,
`lazylog_global_sequencer`, `corfu_global_sequencer`) build from `src/`. Clients/benchmarks
(`throughput_test`, `benchmarks/kv_store/kv_ycsb_bench`) link the client sources. See the root
`CMakeLists.txt` and `src/CMakeLists.txt`.
