# OSDI/SOSP-Quality Implementation and Ablation Checklist

This document defines how to **confirm** that the Embarcadero sequencer implementation and ablation study meet top-tier systems conference standards (OSDI, SOSP, NSDI, SIGCOMM). Use it as a gate before submitting and for artifact evaluation preparation.

---

## Part I: Implementation Quality (Did We Build It Right?)

### 1.1 Spec Compliance and Correctness

| Check | Status / Action | Reference |
|-------|-----------------|-----------|
| **Order levels match design** | Order 0, 1, 2, 5 semantics and data structures align with `docs/EMBARCADERO_DEFINITIVE_DESIGN.md` §2–§3. | Design §2.4, §3.2, §3.3 |
| **Epoch-batched sequencing** | One atomic per epoch (not per batch) in PER_EPOCH_ATOMIC mode; epoch seal → sequence → commit pipeline. | Design §3.2 |
| **Level 5 invariants** | Per-client FIFO: if client sends M1 before M2 (client_seq), then M1.global_seq < M2.global_seq or M1 declared lost (hold timeout). | Design §3.2, README “Order 5 semantics” |
| **Deduplication** | No false positives (batch_id); retries/late arrivals rejected; duplicates counted. | Deduplicator, add_to_hold paths |
| **Validation** | `validate_ordering()` passes on every run when `validate=true`: monotonic global_seq, per-client order for L5. | `validate_ordering_reason()` |
| **Single-thread vs scatter-gather parity** | L5 semantics (hc, next_expected, gaps_skipped, dedup) identical; client_gc_shard and hc sync in place. | age_hold_shard, process_level5_shard, client_gc_shard |

**Confirmation:** Run `./sequencer5_benchmark --test` and full suite with `--validity=algo`; all runs must be valid and validation must pass.

### 1.2 Concurrency and Memory Safety

| Check | Status / Action | Notes |
|-------|-----------------|-------|
| **No data races** | Build with ThreadSanitizer (`-fsanitize=thread`), run ablation suite; zero races. | `build_asan/` exists; add TSAN build target. |
| **No undefined behavior** | Build with UBSan (`-fsanitize=undefined`); run tests. | Optional but recommended. |
| **Cache-line discipline** | Shared structures (PBR, GOI, shard watermarks) aligned/padded; no false sharing. | Code style rule; pahole check. |

### 1.3 Documented Limitations and Scope

| Item | Document where | Reviewer-facing |
|------|----------------|-----------------|
| **In-memory only** | README, ABLATION_RESULTS.md | “Ablation measures sequencer logic; CXL/network in §6.” |
| **No recovery/failover** | README “Scope and limitations” | Not evaluated in this benchmark. |
| **PBR full / saturation** | README “Valid experiments” | PBR_full &lt; 0.1% for headline; warn otherwise. |
| **Latency scope** | README “Latency 0.3–1 μs” | In-benchmark inject→commit only; not e2e. |

**Confirmation:** README and ABLATION_RESULTS.md clearly state what is in scope and what is not; no overclaim.

---

## Part II: Ablation Study Quality (Did We Measure It Right?)

### 2.1 Experimental Design

| Principle | Requirement | Your setup |
|-----------|-------------|------------|
| **Single varying factor** | Each ablation varies one dimension (e.g. per-batch vs per-epoch; L0 vs L5%). | Ablation 1: mode only; Levels: L5 ratio only. |
| **Controlled confounds** | Same brokers, producers, duration, PBR size, validity mode. | Use `--clean` or `--paper` for fixed config. |
| **Paired comparisons** | For A vs B, pair baseline and treatment (e.g. same run order, same machine). | `run_ablation1_paired()`: baseline then optimized per iteration. |
| **Warm-up** | Discard first run(s) to avoid cold cache / CPU ramp. | Warm-up run before measurement runs. |

### 2.2 Statistical Rigor

| Check | OSDI/SOSP expectation | Current / Action |
|-------|------------------------|-------------------|
| **Sample size** | Typically n ≥ 5; for confidence intervals n ≥ 10 preferred. | 5 runs default; document “median [min, max]” and optionally 95% CI. |
| **Report variance** | Median or mean ± std/CI; never only a single run for claims. | Median, [min, max], ±stddev in output. |
| **Speedup distribution** | Report per-pair speedup (e.g. median [min, max] of ratios), not only point estimate. | Ablation1Result: speedup as StatResult (median, min, max). |
| **Validity gating** | Exclude invalid runs (PBR saturated, loss, steady-state violation). | `[INVALID RUN]` / `[INVALID COMPARISON]`; report valid/total. |
| **No p-hacking** | Predefine primary metrics and validity criteria; don’t cherry-pick runs. | `--validity=algo|max|stress` and PBR threshold fixed. |

**Confirmation:** All reported numbers come from runs that pass validity; figure captions state n, metric, and validity mode.

### 2.3 Reproducibility

| Check | Requirement | Action |
|-------|-------------|--------|
| **Exact command line** | Paper or appendix gives exact command to reproduce each figure. | Log full config in run output; `run_paper_matrix.sh` documents commands. |
| **Environment** | OS, kernel, CPU model, RAM; compiler and flags. | Add `--env` or script that prints uname, CPU, gcc -v, CXXFLAGS. |
| **Seeds** | If any randomness, seed fixed or reported. | Producer/client IDs deterministic from config; document. |
| **Artifact** | Code + scripts + small input to reproduce in ≤ 1 hour (AE guidelines). | `run_paper_matrix.sh`, README build/run; consider `reproduce.sh`. |

### 2.4 Honest Interpretation (Critical for OSDI/SOSP)

| Pitfall | Your mitigation |
|---------|------------------|
| **Claiming speedup when variance overlaps 1.0** | README: “0.98× median speedup … atomics were not the bottleneck”; claim is *simplification without penalty*, not throughput win. |
| **Comparing across validity modes** | Do not mix algo vs max vs stress in one comparison; label each figure. |
| **Ignoring invalid runs** | Report “valid/total” and drop invalid from aggregates. |
| **Overclaiming scope** | Ablation = sequencer logic only; CXL/network/replication in full evaluation. |

**Confirmation:** Paper text and figure captions match README/ABLATION_RESULTS interpretation; no overstated claims.

---

## Part III: Ablation Results That Meet the Bar

### 3.1 Minimum Reporting for Each Ablation

1. **Ablation 1 (Per-batch vs per-epoch)**  
   - Throughput: median [min, max] (and optionally 95% CI), n runs.  
   - Atomic count per run (and atomic reduction ratio).  
   - Statement: “Same throughput within variance; 87K× fewer atomics.”  
   - All runs valid (no PBR saturation for headline config).

2. **Levels (L0 vs 10% L5 vs 100% L5)**  
   - Throughput and, for L5, gaps_skipped (and optionally efficiency %).  
   - Clear label: in-memory sequencer only.

3. **Scalability (producers, brokers, τ)**  
   - Throughput vs threads/brokers/epoch_us; plateau explained (single sequencer bottleneck).  
   - Optional: scatter-gather scaling (throughput vs shards).

### 3.2 What “OSDI/SOSP Quality” Means for This Ablation

- **Correctness:** Validation passes; semantics match design; no known bugs in reported configurations.  
- **Reproducibility:** Another researcher can run the same commands and get consistent (within variance) results.  
- **Statistical honesty:** Variance and validity reported; claims limited to what the data support.  
- **Scope clarity:** Explicit that this is sequencer logic ablation; full system and baselines (Scalog, Corfu, CXL hardware) are separate.

---

## Part IV: Concrete Checklist Before Submission

### Implementation

- [ ] `./sequencer5_benchmark --test` passes (ordering + per-client).
- [ ] Full suite with `--validity=algo` (e.g. `--paper` or `--clean`): all runs valid, no validation failures.
- [ ] TSAN build and run: no data races (add CMake option if missing).
- [ ] README and ABLATION_RESULTS.md: scope and limitations clearly stated; no overclaim.

### Ablation

- [ ] Every figure/table: exact command, n runs, validity mode, metric name.
- [ ] Ablation 1: paired runs, speedup as distribution (e.g. median [min, max]); narrative = “same performance, 87K× fewer atomics.”
- [ ] Levels and scalability: variance reported; invalid runs excluded and valid/total stated.
- [ ] Paper narrative consistent with README (“simplification without penalty,” not “throughput speedup” when variance overlaps 1.0).

### Reproducibility and Artifacts

- [ ] `run_paper_matrix.sh` (or equivalent) reproduces headline numbers; log includes full config.
- [ ] README: build (cmake, make), run commands, expected runtime.
- [ ] Optional: `reproduce.sh` that prints environment (CPU, OS, compiler) and runs one key experiment. Example to append to logs:
  ```bash
  echo "=== ENV ==="; uname -a; grep -m1 "model name" /proc/cpuinfo 2>/dev/null; g++ --version 2>/dev/null | head -1
  ```
- [ ] Artifact package: code, scripts, README; no proprietary or unreleased dependencies.

### Threats to Validity (document in paper or appendix)

- **Internal:** Single machine, fixed workload; generalize to other CPUs/workloads in discussion.  
- **External:** No real CXL, no Scalog/Corfu comparison here; full evaluation is in §6.  
- **Construct:** Throughput and atomics are the right metrics for “coordination cost”; latency is in-benchmark only.

---

## Part V: Summary

**Implementation is OSDI/SOSP quality when:**  
(1) It matches the spec and passes all correctness checks, (2) it has no known concurrency bugs (TSAN-clean), and (3) limitations and scope are clearly documented.

**Ablation results are OSDI/SOSP quality when:**  
(1) Design is single-factor with paired/controlled comparisons, (2) statistics report variance and validity, (3) interpretation is honest (no overclaim), (4) results are reproducible from artifact with documented commands and environment.

Use this checklist as a gate before submission and as the basis for the Artifact Evaluation appendix (reproducibility, scope, and limitations).
