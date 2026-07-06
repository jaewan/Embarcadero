# Distributed KV Store on Embarcadero Shared Log — SOSP Evaluation Agent Prompt

## Role and Objective

You are a senior systems researcher implementing a **distributed key-value store** on top of the **Embarcadero shared log** for evaluation in a **SOSP 2026 submission**. Your goal is to design, implement, and benchmark a log-backed KV store that demonstrates Embarcadero's advantages over SCALOG, CORFU, and LazyLog as a shared log backbone.

This is a **publication-critical** artifact. Every design decision must be justified against what top-venue shared log papers (Tango SOSP'13, vCorfu TOCS'17, FuzzyLog SOSP'17, SCALOG NSDI'20, Delos OSDI'20, Boki SOSP'21) report. The evaluation must be reproducible, fair, and scientifically rigorous.

---

## Context: The Embarcadero System

Embarcadero is a CXL-based shared log system with multiple sequencer backends:

| Sequencer | Order Level | Description |
|-----------|-------------|-------------|
| EMBARCADERO | ORDER=5 | Native epoch-based strong ordering with CompletionVector + GOI |
| EMBARCADERO | ORDER=0 | No ordering (unordered delivery) |
| SCALOG | ORDER=1 | Scalog-style periodic global cuts from local cuts |
| CORFU | ORDER=2 | Corfu-style chain replication sequencing |
| LAZYLOG | ORDER=2 | LazyLog-style lazy ordering |

All sequencers share the same CXL memory, broker infrastructure, and client API (`Publisher`/`Subscriber`). Replication factor (RF=1 or RF=2) controls durability.

**Client API surface** (in `src/client/`):
```cpp
// Publisher: append entries to the shared log
Publisher(topic, head_addr, port, num_threads, msg_size, queue_size, order, seq_type);
publisher->Init(ack_level);           // ack: 0=fire-forget, 1=ordered, 2=durable
publisher->Publish(data, size);       // async append
publisher->Poll(n);                   // wait for n acks

// Subscriber: consume entries in total order
Subscriber(head_addr, port, topic, measure_latency, order_level);
void* msg = subscriber->Consume();    // blocking; returns MessageHeader*
// MessageHeader contains: client_id, client_order, total_order, size
```

---

## Existing PoC Code

A proof-of-concept exists at `bench/kv_store/`. Read these files completely before starting:

- **`distributed_kv_store.h`** — `DistributedKVStore` class: SMR-based KV using Publisher/Subscriber. `ShardedKVStore` (64 shards, `absl::flat_hash_map`, reader-writer locks). `LogEntry` serialization with OpType enum (PUT, DELETE, MULTI_PUT, MULTI_GET, transactions).
- **`distributed_kv_store.cc`** — Implementation: `logConsumer()` thread consumes from Subscriber and applies to local state. `processLogEntryFromRawBuffer()` parses and applies. `put()`/`multiPut()` publish to log. `waitForSyncWithLog()` for read-your-writes.
- **`main.cc`** — `KVStoreBenchmark` class: multi-put and multi-get benchmarks with varying batch sizes, CSV output, latency percentiles.
- **`CMakeLists.txt`** — Build config linking against gRPC, protobuf, folly, abseil, glog, yaml-cpp.

**PoC limitations** (must be addressed):
1. Single-node only (one KV instance, one broker connection)
2. No YCSB workload support
3. No multi-client coordination or contention benchmarks
4. No snapshotting or recovery
5. No comparison across sequencer backends
6. Hardcoded `NUM_MAX_BROKERS` assumption
7. Transaction support is stubbed out
8. No throughput/latency decomposition by phase

---

## Development Environment

### Phase 1: Development on c1
- **Server**: c1 (development machine)
- Build: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && make -C build -j$(nproc)`
- Single-broker testing: run `embarlet` locally, then `kv_store_bench`
- Focus: correctness, API design, YCSB integration

### Phase 2: Full evaluation on moscxl
- **Server**: moscxl (CXL-equipped, 4-broker production setup)
- Multi-broker, multi-client evaluation
- All four sequencer backends (EMBARCADERO, SCALOG, CORFU, LAZYLOG)
- RF=1 and RF=2 configurations
- Latency stats require `cmake -DCOLLECT_LATENCY_STATS=ON`

### Branch
Create and work on branch: `kv-store-sosp-eval`
```bash
cd /home/domin/Embarcadero
git checkout -b kv-store-sosp-eval
```

---

## Design Specification

### Architecture: Log-Backed State Machine Replication

Every KV store replica is a deterministic state machine:
1. **Writes**: Client serializes operation → publishes to shared log → waits for ACK
2. **Apply**: Each replica subscribes to the log → applies entries in total order → updates local state
3. **Reads**: Served from local materialized state (after read-your-writes barrier)

This is the standard architecture used by Tango, SCALOG, Delos, and Boki.

### What to Implement

#### 1. YCSB Workload Engine (`bench/kv_store/ycsb_workload.h/.cc`)

Implement the 6 standard YCSB workloads:

| Workload | Reads | Updates | Inserts | Read-Modify-Write | Scan |
|----------|-------|---------|---------|-------------------|------|
| A (Update heavy) | 50% | 50% | — | — | — |
| B (Read mostly) | 95% | 5% | — | — | — |
| C (Read only) | 100% | — | — | — | — |
| D (Read latest) | 95% | — | 5% | — | — |
| E (Short ranges) | — | — | 5% | — | 95% |
| F (Read-modify-write) | 50% | — | — | 50% | — |

Requirements:
- Zipfian and uniform key distributions (Zipfian theta=0.99 is standard)
- Configurable record count (default: 1M records), field size (default: 100B value), operation count
- Pre-load phase (insert all records) separate from run phase
- Deterministic seed for reproducibility

**Do NOT use an external YCSB binary.** Implement a native C++ workload generator that produces the operation stream directly. This avoids JVM overhead and measures the KV store without benchmark-tool artifacts.

#### 2. Multi-Client KV Store (`bench/kv_store/distributed_kv_store.{h,cc}`)

Extend the existing PoC to support:
- **N independent KV store replicas** (each with its own Publisher + Subscriber), running as separate processes or threads
- **Configurable sequencer backend** via `--sequencer` flag
- **Configurable ACK level** via `--ack` flag
- **Configurable replication factor** via `--rf` flag
- **Client-side batching**: Group multiple YCSB operations into a single log append for throughput
- **Read-your-writes consistency**: After a write, the same client must see its own write before returning reads

#### 3. Benchmark Harness (`bench/kv_store/kv_bench_main.cc`)

Command-line interface:
```bash
./kv_store_bench \
  --sequencer EMBARCADERO \      # EMBARCADERO | SCALOG | CORFU | LAZYLOG
  --order 5 \                    # 0 | 1 | 2 | 5
  --ack 2 \                      # 0 | 1 | 2
  --rf 1 \                       # 1 | 2
  --workload A \                 # A | B | C | D | E | F
  --record_count 1000000 \       # Number of KV records
  --operation_count 10000000 \   # Number of operations in run phase
  --value_size 100 \             # Value size in bytes
  --num_clients 1 \              # Number of KV client replicas
  --batch_size 1 \               # Operations per log append
  --distribution zipfian \       # zipfian | uniform
  --broker_ip 10.10.10.10 \     # Broker address
  --warmup_ops 100000 \          # Warmup operations (not measured)
  --output_dir ./results/ \      # Output directory
  --run_id auto                  # Auto-generated or manual run ID
```

#### 4. Evaluation Scripts (`scripts/kv_eval/`)

Create automation scripts following the pattern in `scripts/publication/`:
- `run_kv_eval.sh` — Full evaluation matrix driver
- `run_kv_cell.sh` — Single configuration cell (one sequencer × one workload × one RF)
- `generate_kv_plots.py` — Publication-quality figures

---

## Metrics to Report in the Paper

### Primary Metrics (Table/Figure for each)

#### 1. **Throughput vs. Number of Clients** (Figure)
- X-axis: number of KV clients (1, 2, 4, 8)
- Y-axis: aggregate ops/sec (thousands)
- Lines: one per sequencer (EMBARCADERO, SCALOG, CORFU, LAZYLOG)
- Fixed: YCSB-A, 1M records, 100B values, RF=1, Zipfian
- Show separately for RF=1 and RF=2

#### 2. **Latency CDF** (Figure)
- X-axis: latency (microseconds, log scale)
- Y-axis: cumulative fraction
- Lines: one per sequencer
- Fixed: YCSB-A, 1 client, 1M records, RF=1
- Include inset or separate panel for tail (P99-P99.99)

#### 3. **Throughput by Workload** (Table)
- Rows: YCSB-A through YCSB-F
- Columns: EMBARCADERO, SCALOG, CORFU, LAZYLOG
- Values: median of 3 trials, ops/sec
- Fixed: 1 client, 1M records, 100B values, RF=1, Zipfian

#### 4. **Latency Percentiles** (Table)
- Rows: P50, P95, P99, P99.9
- Columns: EMBARCADERO, SCALOG, CORFU, LAZYLOG
- Fixed: YCSB-A, 1 client, RF=1
- Separate table for RF=2

#### 5. **Throughput vs. Batch Size** (Figure)
- X-axis: batch size (1, 2, 4, 8, 16, 32, 64)
- Y-axis: ops/sec
- Lines: one per sequencer
- Shows the amortization benefit of log batching

#### 6. **Effect of Replication** (Table or Figure)
- Compare RF=1 vs RF=2 for each sequencer
- YCSB-A and YCSB-B
- Show both throughput and P99 latency

### Secondary Metrics (appendix or discussion)

#### 7. **Latency Decomposition** (Stacked bar chart)
- Break down end-to-end latency into:
  - Serialization time
  - Network send time (publish → broker)
  - Sequencing time (broker internal)
  - Replication time (RF=2 only)
  - Network receive time (broker → subscriber)
  - Deserialization + apply time
- One bar per sequencer

#### 8. **Scalability: Records vs. Throughput** (Line chart)
- X-axis: record count (100K, 1M, 10M, 100M)
- Y-axis: ops/sec for YCSB-B
- Shows whether in-memory state size affects throughput

---

## Implementation Plan

### Phase 1: Core YCSB Engine (c1, correctness)
1. Read all existing PoC code (`bench/kv_store/`) completely
2. Implement `YCSBWorkloadGenerator` class (Zipfian + uniform distributions, all 6 workloads)
3. Refactor `DistributedKVStore` for multi-sequencer support and configurable parameters
4. Add batch write support (group N operations into one `Publish()` call)
5. Write unit tests: verify correctness of apply ordering, read-your-writes, Zipfian distribution
6. Test with single broker on c1

### Phase 2: Benchmark Harness (c1)
1. Implement `kv_bench_main.cc` with full CLI
2. Add latency instrumentation: per-operation timestamps, percentile computation
3. Add throughput measurement: ops/sec with warmup exclusion
4. CSV output with all metrics columns
5. Verify results make sense with single broker on c1

### Phase 3: Multi-Client Support (c1 → moscxl)
1. Multi-process client launch (each client is a separate process)
2. Barrier synchronization: all clients start the run phase simultaneously
3. Result aggregation across clients
4. Test with 4 brokers on moscxl

### Phase 4: Full Evaluation (moscxl)
1. Run complete evaluation matrix: 4 sequencers × 6 workloads × 2 RFs × {1,2,4} clients × 3 trials
2. Generate publication figures
3. Validate consistency: every sequencer must produce identical final KV state for the same workload

---

## Critical Constraints

1. **Fairness**: All sequencers use the EXACT same KV store code, YCSB workload, and client configuration. The ONLY variable is the sequencer backend and its natural order level.

2. **No artificial handicaps**: Do not add unnecessary synchronization, artificial delays, or suboptimal code paths that penalize one system. The KV store should be optimized for the shared log API, and all systems use that same optimized path.

3. **Reproducibility**: Every run must record: git commit hash, full configuration, system state (broker count, RF, ACK level, sequencer), timestamps, and raw latency samples. Use `scripts/publication/` patterns for run archival.

4. **Statistical rigor**: Minimum 3 trials per configuration. Report median and error bars (min/max or IQR). Discard the first trial if it's an outlier due to warmup.

5. **Correct ordering semantics**: For ordered sequencers (ORDER=1,2,5), all KV replicas must converge to the same state. For ORDER=0, the KV store must still be correct (use client_order for per-client ordering or disable cross-client operations).

6. **Build compatibility**: The code must compile on both c1 (cmake 3.22) and moscxl (cmake 3.28). Do not add new external dependencies. Use existing abseil, folly, gRPC, protobuf.

7. **No file bloat**: Extend existing files where possible. The YCSB generator should be at most 2-3 new files. Do not create elaborate framework abstractions — keep it direct and readable.

---

## Files to Read Before Starting

Read these files completely and understand them before writing any code:

```
bench/kv_store/distributed_kv_store.h     # Existing KV store class, ShardedKVStore, LogEntry
bench/kv_store/distributed_kv_store.cc    # Implementation: logConsumer, processLogEntry, put/get
bench/kv_store/main.cc                    # Existing benchmark harness
bench/kv_store/CMakeLists.txt             # Build config
src/client/publisher.h                    # Publisher API
src/client/subscriber.h                   # Subscriber API
src/client/common.h                       # Topic creation, sequencer type parsing
scripts/publication/run_throughput_cell.sh # Pattern for evaluation scripts
scripts/run_multiclient.sh                # Multi-client orchestration pattern
```

## First Steps

1. `git checkout -b kv-store-sosp-eval` from current HEAD
2. Read all files listed above
3. Implement the YCSB workload generator
4. Extend the existing KV store for parameterized sequencer/ack/rf
5. Build and test on c1 with a single broker
