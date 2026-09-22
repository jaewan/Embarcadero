# Systems expert review of the refactoring plan

Date: 2026-09-22. Source baseline: `ae9959dd2892af821878aa42d4e2c05fcb635466`.

Three independent AI reviewers examined the plan from distributed-systems correctness, performance/NUMA, and architecture/build perspectives. The coordinating review checked their key code references, inspected local hardware, and calculated capacity scenarios. This is a source-grounded expert review, not a benchmark campaign or a claim of external human peer review. No brokers were launched, no remote hosts were accessed, and no host settings or runtime code were changed.

## Verdict

**Approve the plan with the amendments now incorporated into the [refactoring plan](2026-09-22-refactoring-plan.md).** It should materially improve correctness, reproducibility, diagnosability, and maintainability. It can preserve performance during careful extraction and offers specific optimization opportunities. Neither a speedup nor blanket nonregression is established by this review.

Treat three categories differently:

| Change | Expected benefit | Performance commitment |
|---|---|---|
| Safe runner, trustworthy builds, startup configuration/layout checks | Repeatability and fewer invalid experiments | Keep startup cost separate; no new steady-state work required |
| Ring/capacity, lifetime, validation, session/fencing fixes | Prevent corruption, permanent stalls, and false acceptance | Measure necessary safety costs; unsafe old behavior is not the contract |
| Component extraction and file organization | Local reasoning, testability, maintainability | Preserve copies, batching, ownership, and publication semantics; require measured nonregression |

The most credible gain is removing an unconditional reclamation mutex from every successful payload reservation. Repairing ring wrap can prevent a complete progress collapse. Both are stronger motivations than expecting performance from cleaner directories.

## Hardware and real data path

Observed locally during review:

- Two AMD EPYC 9754 sockets, 128 physical cores/socket, SMT2; 512 logical CPUs.
- Node 0: CPUs `0–127,256–383`; node 1: `128–255,384–511`. CPU 128 and 384 are siblings. Sample shared L3 groups are `128–135,384–391` and `136–143,392–399`.
- NUMA distance table: local 10, remote 32. These are topology distances, not a measured latency ratio.
- Nodes 0 and 1 each expose approximately 756 GiB DRAM; no node 2. `/dev/shm` has approximately 756 GiB available.
- The up 100 Gb/s interface `enp193s0f0np0` is on node 1. The up 1 Gb/s interface `enp1s0f0np0` is on node 0. Actual routing must be recorded before remote measurements.
- Both nodes report zero free 2 MiB HugeTLB pages. Anonymous THP policy is `madvise`; tmpfs THP policy is `never`; automatic NUMA balancing is enabled. These are observations, not proposed tuning changes.

The normal ORDER5 path is:

1. The client copies application data into batch-pool storage (`src/client/queue_buffer.cc:525`) and sends headers/payload using ordinary `send()` (`src/client/publisher.cc:4267`).
2. The broker reserves payload space and receives directly into it (`src/network_manager/network_manager.cc:1478`, `1591`). An optional non-temporal ingest mode instead uses staging; its setting must be explicit.
3. Only after payload reception does the broker reserve PBR metadata (`network_manager.cc:1684`, `1717`) and publish completion (`1903`).
4. ORDER5 commits batched message-order and GOI ranges (`src/embarlet/topic.cc:5186`, `5207`), then publishes the associated progress.
5. Replication behavior depends on the sink: memory-copy performs a copy, memory-accounting advances accounting without copying, and disk uses `pwrite()` plus separate `fdatasync()` work (`src/disk_manager/chain_replication.cc:443`, `474`, `565`).

With client pages on node 0 and broker pages on node 1, this remains a TCP path with kernel copying and cross-socket traffic. It is not end-to-end zero copy. Local clients avoid a physical network link but consume this host's CPU and memory/interconnect resources. Remote clients over the node-1 NIC can have different bottlenecks. The requested local layout is a sound default development control, not proof of the absolute performance ceiling.

## Thought experiments and required amendments

### 1. Safe reclamation exposes finite capacity

**Scenario:** a slow subscriber retains a descriptor into segment A while the ingress broker rolls several more segments. `MaybeGCRetiredSegments()` retains only two retired segments and frees older ones (`topic.cc:1604`); `FreeSegment()` clears the allocation bit (`cxl_manager.cc:944`). Reuse can change the bytes behind the subscriber's descriptor.

Disabling that reuse prevents this failure but eventually exhausts the finite region. A 64 GiB region reserves 32 GiB for GOI plus other metadata, leaving **less than 32 GiB** for payload. At a hypothetical **12 GiB/s** allocation rate:

| Quantity | Arithmetic result |
|---|---:|
| 32 MiB smoke transfer | Approximately 2.60 ms of payload time |
| 256 MiB segment | Approximately 20.83 ms of payload time |
| 32 GiB upper bound on payload capacity | Approximately 2.67 seconds |

These are calculations, not measured speeds; real wire overhead and retry waste reduce useful capacity. Warmup, partial receives, held/unpublished batches, and retries consume storage too. The receive path can wait after payload allocation for PBR space (`network_manager.cc:1684–1730`).

**Decision:** bounded finite-transfer characterization comes first. Sustainable throughput requires completed safe reclamation and stable memory use. Containment is not a completed soak-test milestone. Verify each reference class independently prevents premature reuse.

### 2. A green smoke test misses ring wrap entirely

The configured 10 MiB PBR holds **81,920 entries of 128 bytes**. At nominal 2 MiB batches, one wrap represents approximately **160 GiB per broker**. That cannot fit in the proposed region without reuse; the 32 MiB smoke workload is only about 16 nominal batches.

The actual producer uses absolute sequences while consumed state is reduced to a modulo position (`topic.cc:2838`, `2987`, `8599`). A model that reproduces these expressions helps diagnose the problem, but does not test all synchronization in production.

**Decision:** inject ring capacities 4/8/16 into the real producer/scanner/retirement path. Require multiple generations, concurrent producers, delayed publication, held entries, and explicit progress. Keep production geometry for throughput comparisons.

### 3. An array bounds check can prevent corruption but permanently stall subscribers

**Scenario:** one GOI slot remains and a round contains two ready batches. The current code allocates message-order space at `topic.cc:5186`, then GOI space at `5207`, and writes entries at `5245`. Rejecting only the final write leaves an allocated message-order gap that no later batch fills.

**Decision:** determine an admissible batch set before irreversible message/GOI sequence allocation. Serialize admission with the true owner(s); a check followed by concurrent `fetch_add` is not sufficient. Either reject the round consistently or commit an explicitly selected valid prefix. Audit already-advanced classification/session state too.

**Gate:** capacities 1–4, multi-batch rounds and held releases, with cancellation/retry. No out-of-bounds index, orphaned total-order range, false ACK, or silently discarded ready descriptor.

### 4. Earlier PBR reservation can introduce head-of-line blocking

**Scenario:** to reduce wasted payload space, reserve the PBR slot before receiving the payload. A slow or disconnected sender now owns the next contiguous scanner position while later healthy sessions are ready.

Receive-before-PBR is deliberate in both the code (`network_manager.cc:1684`) and current paper (`Paper/Text/Sec4_Design.tex:146`). Moving it changes the ordering pipeline, not just memory management.

**Decision:** preserve receive-before-ordered-slot reservation. If adding early resource limits, use bounded admission credits without assigning scanner-visible positions. Track received, unpublished, and abandoned bytes separately. A stalled receive must not prevent unrelated ready sessions from progressing.

### 5. “Owned messages” can add a full extra payload copy

**Scenario:** transport extraction returns a `vector<byte>` to storage instead of receiving into the existing reservation. Every byte gets copied into BLog a second time. At high throughput this adds memory traffic without adding protocol value.

**Decision:** cross boundaries using lifetime-safe reservation handles and validated spans, retaining normal direct receive. Preserve batched atomics in commit. Do not introduce per-message heap allocation, global reference counts, virtual dispatch, or extra fences solely for architectural uniformity. Explicitly retain and label the optional non-temporal staging experiment instead of silently enabling it.

### 6. Correct validation has a real cost

Emulation disables explicit flush requirements (`cxl_manager.cc:356`). Current payload validation is coupled to the flush branch (`network_manager.cc:1843–1900`), and the ORDER0 fast path returns earlier (`1651–1684`). Fixing this correctly adds work to previously unchecked valid traffic.

**Scenario:** hold byte throughput constant but reduce message size from 4 KiB to 64 bytes. Header checks per payload byte rise sharply. An aggregate large-message benchmark can conceal this regression.

**Decision:** validate bounded framing once before publication, independently of cache mode; reuse validated information where safe. Measure receive, validation, and publication separately for small/large messages and ORDER0/ORDER5. An explained correctness cost is acceptable; disabling validation to match unsafe throughput is not.

### 7. Fencing must stay terminal without gratuitous per-batch probes

The commit guard is gated by its own initially zero rejection count (`topic.cc:5153`), while incrementing that count requires executing the guard (`5128`). The guard probes session state (`5099`); fencing and ordinary snapshot publication occur on other paths (`6863`, `5503`, `5614`).

**Scenario:** classify a batch as ready, fence its session, then commit the old ready batch and publish a normal snapshot. Test both forbidden commit and possible terminal-state overwrite. This is a source-grounded schedule to exercise, not a claimed live reproduction.

**Decision:** define commit/fence serialization and the linearization point; require nonregressing frontiers and terminal fenced generations. Optimize probes only with a demonstrated visibility invariant. Measure healthy traffic and fence churn/high table occupancy separately.

Session OPEN also needs authoritative capacity before success: 4,096 generation entries exist (`cxl_datastructure.h:225`), table allocation can fail (`topic.cc:391`), publication can silently return (`444`), and OPEN currently returns from a snapshot path (`network_manager.cc:1133`). Test the last-slot race, repeated OPEN after a lost reply, and the 4,097th generation while admitted sessions continue progressing.

### 8. Containment can remove a lock, but rollover needs a coherent state transition

Every successful `TryReserveBLogSpaceFailClosed()` calls GC (`topic.cc:2819`), which takes `segment_rollover_mu_` even when the retired list is empty (`1609`). Eight receiving threads can contend on that lock without any rollover. Removing unnecessary GC work is a concrete optimization candidate.

However, `current_segment_` is an ordinary pointer (`topic.h:896`), written during rollover (`topic.cc:1591`) and read outside that mutex (`2795`). Merely deleting GC does not fix how segment identity and allocation cursor are observed together.

**Decision:** contain reclamation promptly, prove race-free reservation/rollover state, and move eventual GC to frontier/rollover events or amortized work. Test concurrent boundary reservations and measure 1/2/4/8 ingress-thread reservation cost. Do not claim a speedup without contention/throughput evidence.

### 9. Configuration and build changes can invalidate the comparison

`GetNewSegment()` stores layout geometry in process statics (`cxl_manager.cc:784–818`). Two small-capacity fixtures in one process can inherit the first fixture's geometry. A mutex around every formerly racy config getter would fix one race while adding hot-path contention.

The build currently forces `-O3` and enables `-march=native` (`CMakeLists.txt:16`, `src/CMakeLists.txt:20`). GTest discovery can silently omit most tests (`test/CMakeLists.txt:56`, `659`). Some integration tests construct their own GOI state rather than invoking production commit (`test/phase2_integration_test.cc:122`).

**Decision:** immutable per-region geometry and primitive cached settings; required test dependency/inventory checks; truthful production-versus-model coverage. Compare identical Release/ISA flags. Split startup validation of the existing layout from region-header migration so urgent containment is not blocked by ABI work.

Existing `SegmentManager`/`BufferManager` code is not the production path listed in `src/CMakeLists.txt:49`. Inventory and retire or repair those attempts deliberately; do not trust their names during extraction. Baseline-generated dependencies also reach the client (`src/CMakeLists.txt:169`); a minimal client build is required before claiming optional baselines.

### 10. The client or socket can hide a broker regression

**Scenario:** refactoring makes broker processing 10% slower but the local generator was already the bottleneck. End-to-end throughput is unchanged. Conversely, a different client page mode or ACK retention policy changes throughput while broker code is unchanged.

Client allocation tries HugeTLB, hugetlbfs, and then anonymous THP advice (`src/client/common.cc:406–480`). Current default queue geometry can allocate roughly 512 MiB even for a 32 MiB workload (`queue_buffer.cc:171–179`). ACK1 typically pins pool slots; disk ACK2 normally retains owned retransmission copies (`publisher.cc:713–728`). Memory-accounting and memory-copy sinks have different work despite similar reported copy-byte accounting (`chain_replication.cc:449`).

**Decision:** record actual page backing, pool slots/bytes, retention, credit, RTO, sink, CPU/core budgets, and achieved offered load. Preserve these across revisions. Use fixed total broker CPU budget for scaling, with fixed-per-broker-budget experiments labeled separately. Capture softirq placement and routing diagnostically without hidden system tuning.

## Measurement contract

1. **Now:** correctness-qualified, capacity-bounded finite transfers on isolated DRAM; one and three brokers; ORDER5/ACK1 first. Include cleanup and payload/order audits.
2. **During fixes:** production component measurements for reservation, parser, ring, commit, and client retention; reduced geometry only for boundary tests, not as a silent performance optimization.
3. **After safe reuse:** sustained end-to-end tests spanning repeated segment/PBR wraps, bounded memory, stalled-consumer recovery, and separately qualified disk/memory sinks.

Use at least five interleaved paired repetitions with matched compiler, flags, topology, page mode, workload and sink. Compare p99 at matched offered load; assess saturation throughput separately. Supplement acknowledged throughput with cycles/batch, scanner progress, lock/pool/queue waits, faults and retransmissions. Expensive profiling belongs in separate diagnostic runs. The proposed 5% throughput and 10% p99 thresholds remain investigation triggers calibrated to variability, not proof of equivalence.

The revised plan therefore makes a defensible commitment: **eliminate known unsafe behavior, preserve the existing efficient data path during extraction, and accept performance claims only with matched evidence.** DRAM development proceeds now; real CXL publication behavior and final CXL performance remain deferred hardware gates.
