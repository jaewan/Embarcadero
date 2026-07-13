# Embarcadero Agent Handoff — moscxl, 2026-07-11

You are a systems engineering agent working on **Embarcadero**: a distributed,
totally-ordered pub/sub shared log built over CXL disaggregated memory, targeting
OSDI'27/SOSP'27 resubmission. This document is your ramp-up guide.

## Working directory

All work happens in **`~/Embarcadero`** (git, branch `main`, commit `2a1ac51b`).  
**Never touch `~/Embarcadero` clones you didn't create.**  
Never `pkill -f`. Kill only by explicit PID.  
Commit with `--no-verify --no-gpg-sign`. Trailer: `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>`.

## Files to read first (ramp-up order)

```
Paper/improvement_plan.md          # The full OSDI'27 revision plan — the bible
Paper/Text/Sec1_Introduction.tex   # Current paper intro (post-rewrite)
Paper/Text/Abstract2.tex           # Current abstract
Paper/Text/Sec7_Evaluation.tex     # Eval section — numbers and figures
docs/handoff/DELIVERY_CEILING_PROFILING_BRIEF.md  # Delivery performance context
docs/experiments/delivery_slo_measurement.md      # Latest latency measurements
docs/baselines/fairness_appendix.md               # CXL-Corfu/Scalog/LazyLog porting rules
docs/design/ORDER5_EXPORT_GAP_REPORTING.md        # O5-1 ring-lap design
```

## Cluster topology

| Node | IP (10G fabric) | NIC NUMA | PCIe | Role |
|---|---|---|---|---|
| **moscxl (broker)** | 10.10.10.10 | NUMA 1 (NIC) | ×16 | Brokers + sequencer + CXL DIMM |
| **c4** | 10.10.10.12 | NUMA 1 | ×16 | **Primary publisher** (NUMA-local, original topology) |
| **c3** | 10.10.10.181 | NUMA 1 | ×16 full | Secondary publisher (12.4 GB/s TCP) |
| **c1** | 10.10.10.11 | NUMA 0 | ×8 downgraded | Third publisher — pin to NUMA 0 |
| **c2** | 192.168.60.x | — | 1G only | Management only, excluded from eval |

All clients have passwordless sudo. SSH from broker to c1/c3/c4 is passwordless.

CXL DIMM: 256 GB on NUMA node 2 (CPU-less). Hugepages: 6500×2MB = 13 GB.

## Current experiment status

### What completed and is valid
- **E2 throughput N=1** (all systems, epoch_us=500, NUMA 1 for c1 — slightly suboptimal):
  - Embarcadero ORDER=5 RF=0: **3.33 GB/s** median (c1, NUMA-mismatched)
  - CORFU RF=0: **2.35 GB/s** | SCALOG RF=0: **3.07 GB/s** | LAZYLOG RF=0: **3.33 GB/s**
- **E2 throughput N=2** (c1+c3):
  - Embarcadero ORDER=5: **6.66 GB/s** | CORFU: **4.07 GB/s** | LAZYLOG: **6.60 GB/s**
- **E3 latency** (Embarcadero ORDER=5 + linger, RF=0):
  - Append P50 = **251 µs** at 100 MB/s, **305 µs** at 2000 MB/s (sub-ms across full range!)
  - This is the paper's headline latency (was 632 µs pre-linger)

### Overnight run that may be completing now
Check: `tail -20 /tmp/overnight_final_complete_*.log | tail -1 | xargs cat`  
It covers: E2 (Parts A), E3 SLO curves (Part B), E9 RF sensitivity (Part C), E8 overhead (Part D).  
Run was started with c1 pinned to NUMA 1 (old incorrect pin). Results still valid but slightly below ideal.

### NUMA retest needed (high priority)
After overnight completes, run the targeted retest with correct NUMA pinning:
```bash
flock -n /tmp/embarcadero_run_multiclient.lock echo FREE  # verify no other run
bash scripts/cluster_setup.sh  # ensures c4 is synced
SKIP_BASELINES=0 NUM_TRIALS=4 bash scripts/run_overnight_eval.sh
```
c4 (NUMA 1, NUMA-local) is now `CLIENT_HOSTS_REMOTE` default — this should recover
throughput closer to the original 5.17 GB/s. Expected: **4-5 GB/s N=1** with c4,
**~14-16 GB/s N=2** with c4+c3.

## Known NUMA fix (just applied)

The original paper used c4 (NUMA 1 NIC) as primary client. Our evals used c1 (NUMA 0 NIC)
but pinned to NUMA 1 → cross-NUMA DMA → throughput capped at 3.33 GB/s instead of ~5 GB/s.

Fixed in `scripts/run_multiclient.sh` and `scripts/run_overnight_eval.sh`:
- c1 now pinned to NUMA 0 (matches its NIC)
- c4 now configured on 10.10.10.12 and is the default primary client
- `CLIENT_NUMAS` builder: c1→0, all others→1

## Remaining work for OSDI'27

### CRITICAL (paper cannot be submitted without)

**1. Re-run E2/E3 with c4 as primary client (NUMA-local)**
Expected to recover N=1 throughput toward 5+ GB/s and restore comparison ratios.
Command: `bash scripts/run_overnight_eval.sh` (c4 is now default CLIENT_HOSTS_REMOTE)

**2. E4 failure suite — NO SCRIPTS EXIST yet**
The paper claims "per-session repair without global stall, where WBO baselines seal/cut/reconfigure"
but this has zero experimental support. Three sub-experiments needed:
- E4a: broker kill with M=4 concurrent sessions → per-session stall CDF vs. 660 ms TCP
- E4b: sequencer failover MTTR (expected <200 ms but untested)
- E4f: Scalog seal/cut stall + Corfu reconfiguration stall under same fault

Stub script: `scripts/run_failure_suite.sh` — has the manual procedures but no automation yet.
The broker-kill injection needs to be added to `run_multiclient.sh` (BROKER_KILL_AFTER_SEC env).

**3. Update abstract numbers after NUMA-correct eval**
Abstract claims: "18.2 GB/s under strong total ordering at 1.6 ms P99"
- 18.2 GB/s was from a co-located publisher (N=3 with moscxl-local). Our all-remote max is N=2 c4+c3.
  The paper's own methodology (§IV.0 rule 2) bars co-located publishers from headlines.
  Target: report N=2 all-remote number (expected ~14-16 GB/s with c4+c3 after NUMA fix).
- 1.6 ms P99 at RF=2 is unconfirmed (no `e9_latency_embar5_rf2` result yet).
  Cell is in the overnight script now — will produce the number after next run.

**4. Sequencer failover claim: remove `<200 ms` until measured**
`Paper/Text/Sec5_FaultTolerance.tex` was updated to say "sub-second, proportional to committed
batch count" but the `<200 ms` figure needs to be replaced with a measured value from E4b.

### Important (paper quality)

**5. E10 sequencer scalability ablation (properly)**
The ablation sweep in `scripts/run_throughput_ablation.sh` is a parameter sweep (epoch_us × threads)
for Embarcadero's own throughput. This is NOT E10 which requires comparing vs the batched CXL-Corfu
sequencer. A proper E10 needs the Corfu comparison at matched load.

**6. Paper machine names**
`Paper/Text/Sec7_Evaluation.tex` was already cleaned — no more `c4`, `moscxl`, `c2` etc.
But double-check when adding new eval text.

**7. YCSB E7 evaluation**
`scripts/run_ycsb_eval.sh` exists and covers workloads A-F. Run it after the main evals settle.
Binary: `build/bin/kv_ycsb_bench` (already built).

## Key scripts

```bash
bash scripts/cluster_setup.sh          # One-time: sync bins to c1/c3/c4, verify NUMA/hugepages/RDMA
bash scripts/run_overnight_eval.sh     # Full eval sweep (E2,E3,E9,E8) — nohup it
bash scripts/run_throughput_ablation.sh # Epoch×threads sweep for E10 (loopback only)
bash scripts/run_failure_suite.sh      # E4 failure suite (partially manual)
bash scripts/run_ycsb_eval.sh          # E7 YCSB applications
bash scripts/aggregate_e2e_throughput.py data/overnight_eval/RUNTAG/multiclient
bash scripts/aggregate_latency_vs_load.py data/overnight_eval/RUNTAG/latency
```

Monitor running eval: `tail -f /tmp/overnight_*.log | tail -20`

## Paper files (gitignored, local only)

```
Paper/improvement_plan.md          # Full OSDI'27 plan
Paper/Text/Abstract2.tex           # Has \todo{UPDATE after overnight eval} markers
Paper/Text/Sec7_Evaluation.tex     # Eval section (machine names cleaned)
Paper/Text/Sec5_FaultTolerance.tex # Failover MTTR claim needs measurement
```

The paper is in `~/Embarcadero/Paper/` (gitignored). Edit directly there.
Key TODOs still in the paper (grep for `\todo`):
- Abstract: verify 18.2 GB/s and 1.6 ms P99
- Sec5: confirm recovery scan latency
- Sec7 latency table: "Update P50/P99 from overnight linger run"

## System architecture (ultra-brief)

```
Publisher (c4/c1/c3) → TCP → Broker (embarlet, 4 processes on moscxl)
  → CXL NUMA node 2 (255 GB DIMM) ← Epoch Sequencer (500 µs, Sequencer5 thread)
  → CXL write: payload to Blog, metadata to PBR/GOI → ACK
  → Subscriber → ConsumeOrdered() → deliver
```

Key invariants: single-writer ownership, monotonic updates, poll-based publication,
`clwb+sfence` → sentinel-last write discipline. No cross-host atomics.

## Env vars for tuning

```bash
EMBARCADERO_RUNTIME_MODE=latency      # enables 300 µs linger (batch fills on time, not just size)
EMBAR_ORDER5_EPOCH_US=500             # epoch period (100-5000 µs; 500 is default for remote)
EMBARCADERO_CXL_ZERO_MODE=metadata    # startup: zero only metadata (~1s vs ~5s for full)
EMBARCADERO_CXL_MAP_POPULATE=0        # follower brokers: lazy mmap (avoid 77s MAP_POPULATE)
BROKER_REACHABILITY_TIMEOUT_SEC=60    # reachability probe timeout (was 20, now 60)
```

## What the paper needs to say and show (assessment)

Good: Latency (251 µs P50 flat across load), linger fix, CORFU structural analysis (token RTT),
LazyLog binding analysis, all correctness fixes (CXL-1, CXL-2, O5-1, Sequencer2, linger).

Needs work: Throughput headline (need NUMA-correct N=1/N=2 numbers), failure suite (E4 completely
missing), sequencer failover MTTR (claimed, not measured), RF=2 latency (one cell needed).

The paper's central claim — "per-session FIFO under full striping without global stall" — rests
on the failure suite that doesn't exist yet. That is the highest-priority engineering work.
