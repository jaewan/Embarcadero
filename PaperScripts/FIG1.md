# Fig. 1 — Throughput regimes and mixed-client ceiling

## Claim

At a fixed two-remote-publisher operating point, identify which resource binds
throughput as the completion contract moves from dual-NVMe RF2, to DRAM-copy
RF2, to replication-off ACK1; then add a mixed N=4 point to expose local-ingress
headroom. Fair baselines use the same replica sink and ACK metric. LazyLog
appears only as a faithful pre-binding durable-ACK reference; Embarcadero O0
appears only as an unordered ceiling reference.

## Fixed knobs (do not change between trials)

| Knob | Value | Why |
|------|-------|-----|
| Brokers | 4 | Paper topology |
| RF / ACK | 2/2 or 0/1 | Regime-defining completion contract |
| Embar order | 5; O0 reference | O0 is an unordered ablation only |
| Message size | 4096 B | Paper Fig1 draft |
| Bytes aggregate | 10 GiB | Divided evenly across publishers |
| Publish batch | 2048 KB | `client.yaml` design point |
| Threads/broker | 6 | Matched across Embar + baselines |
| Epoch µs | 500 | ORDER=5 remote design point |
| Disk dirs | `.Replication/disk0` + `/mnt/nvme0/replication/disk1` | Dual NVMe |
| Runtime | `throughput` | Not latency/linger |
| CXL size | 256 GiB | 64 GiB default fails 4-broker segment preflight |

## Client roster

N=2: `c4,c3`, both remote 100 GbE publishers pinned to NUMA node 1.

## Figure layout

One grouped chart uses a common linear axis and four regimes: NVMe RF2/ACK2,
DRAM-copy RF2/ACK2, replication-off ACK1 at N=2, and a replication-off mixed
N=4 ceiling with three remote publishers plus one local publisher. Bars are medians and whiskers are
min--max over all three accepted trials; no performance filtering is allowed.
LazyLog is hatched to denote its weaker pre-binding ACK. Embarcadero O0 is
hatched separately and must not be described as Scalog-equivalent.

LazyLog is **excluded from the sink panel by default** (`SKIP_LAZYLOG=1`): faithful
ACK BW is metadata-bound (`AppendToAll` + sidecar fdatasync), so disk↔mem is not
a fair data-sink A/B.

- **disk** — `disk-durable` + dual NVMe; Scalog gets `--replicate_to_disk` and
  **amortized** `fdatasync` (`EMBARCADERO_CHAIN_SYNC_BYTES`, default 256 MiB)
- **mem** — per-source DRAM rings, full-range CXL invalidate, no media sync;
  claim *DRAM replica completion* / `ack2_minimum_memory_copy_replica_prefix`
  (isolates coordination from NVMe; not a media-durable ACK2 claim)
- **Cross-system note:** Scalog ORDER=1 parallel RF vs Embar ORDER=5 serialized
  chain is a protocol difference, not a sink mismatch

## Metric

- **Summed publisher ACK GB/s** = sum of each synchronized publisher's
  end-to-end acknowledged-byte rate (`bandwidth_sum_gbps`).
- Also record overlap and send-done as diagnostics, but do not mix them into the
  bar chart.
- Send-done scaling ≠ ACK scaling (publishers pipeline ahead of ACKs).

## Appendable results

```
data/paper_eval/fig1/<CAMPAIGN_ID>/results.csv
```

Re-running with the same `CAMPAIGN_ID` **appends** trials. Plot refreshes after
each cell. Optional: `TARGET_TRIALS=K` skips cells with ≥ K `ok` rows.

Scalog/LazyLog rows were **deleted** from the campaign CSV on 2026-07-15
(backup: `results.csv.bak_pre_scalog_lazylog_purge`).

## How to run

```bash
# Full Fig1 (1 trial); Scalog/LazyLog included after sink fix
NUM_TRIALS=1 bash PaperScripts/run_fig1_throughput_scaling.sh

# Embar-only if desired
ONLY_CELLS=fig1_embar_o5_disk_n1,fig1_embar_o5_disk_n2,fig1_embar_o5_disk_n3,fig1_embar_o5_disk_n4,fig1_embar_o5_mem_n1,fig1_embar_o5_mem_n2,fig1_embar_o5_mem_n3,fig1_embar_o5_mem_n4 \
  NUM_TRIALS=1 bash PaperScripts/run_fig1_throughput_scaling.sh

# Replot
python3 PaperScripts/plot_fig1_throughput_scaling.py \
  --csv data/paper_eval/fig1/fig1_rf2_ack2_scaling/results.csv \
  --pdf data/paper_eval/fig1/fig1_rf2_ack2_scaling/fig1_throughput_scaling.pdf
```

### Matched ordering-path ablation

`PaperScripts/run_fig1_path_decomp.sh` includes the two paper-scale
replication-off modes and an opt-in semantic control:

| Cell | Contract | Purpose |
|---|---|---|
| `v0_order0_ack1_rf0` | no global order | unordered-ingest upper bound |
| `v05_order5_nofifo_ack1_rf0` | global order, session FIFO deliberately bypassed | isolates global-order publication |
| `v1_order5_ack1_rf0` | global order plus session FIFO | adds predecessor checks and hold enforcement |

The middle cell sets
`EMBARCADERO_ORDER5_BYPASS_SESSION_FIFO_ABLATION=1`. It is deliberately invalid
`[ORDER5_SESSION_FIFO_ABLATION]`. Both `ORDER=5` cells enable
`EMBAR_ORDER5_COMMIT_PROFILE=1`, which reports batches, messages, hold depth,
and time in GOI, export, metadata, completion-vector, and held-slot phases.
Because both `ORDER=5` modes retain grouping, within-round sorting, session
state reconstruction, allocation, and reclamation, their difference measures
the incremental checks and hold enforcement—not every CPU cycle attributable
to session-aware processing. The bypass is not enabled in paper-scale
performance campaigns: at large natural reorder depth, the present prototype
does not yet separate classified from published session state. The fail-closed
runner rejects such a short ACK frontier, so no V0.5 throughput is citable.

Run a smoke test before the paper-scale campaign:

```bash
bash PaperScripts/run_fig1_path_decomp.sh --smoke
```

Before any clean performance campaign, run the forced cross-seal semantic gate.
It must report normal `ORDER=5` as valid and the bypass as invalid specifically
because of `session_fifo_apply_order`:

```bash
PaperScripts/run_ordering_ablation_semantic_test.sh
```

Run the defensible V0-versus-V1 paper-scale comparison:

```bash
# Target only the mixed local/remote ceiling; keep this on a clean commit.
campaign="fig1_ordering_ablation_n4_$(git rev-parse --short=8 HEAD)_$(date -u +%Y%m%dT%H%M%SZ)"
CAMPAIGN_ID="$campaign" \
OUT_ROOT="data/paper_eval/fig1/$campaign" \
N_VALUES=4 RUN_REPLICATION_VARIANTS=0 RUN_SESSION_ABLATION=0 \
bash PaperScripts/run_fig1_path_decomp.sh

column -s, -t \
  "data/paper_eval/fig1/$campaign/ordering_ablation_n4_summary.csv"
```

The runner already saves the required summary CSV and per-load manifest,
constructs a campaign manifest containing archived-log hashes, writes
`SHA256SUMS`, and exits nonzero on any missing cell, incomplete trial set,
configuration mismatch, forbidden mapping/sink fallback, or summary failure.

## Caveats (read before citing numbers)

### A. Scalog / LazyLog RF2 sink mismatch (**fixed; old rows purged**)

**Was:** harness labeled disk/mem via `CHAIN_REPLICATION_SINK`, but Scalog/LazyLog
ignored it and defaulted to `replicate_to_memory` → replica files under `/tmp/`
with `fdatasync` for *both* labels. Embar alone used real dual-NVMe vs mem-copy.

**Fix (script + code):**
1. `run_multiclient.sh` / `broker_lifecycle.sh`: Scalog/LazyLog RF>1 +
   `disk-durable` ⇒ `--replicate_to_disk` + require replica dirs; mem sink leaves
   default memory; missing sink env fail-closes.
2. `scalog_replication_manager.cc`: `log_to_memory` ⇒ DRAM ring copy, **no**
   media `fdatasync` (claim `replicated_ack_emulated`).

**Action:** rebuild `embarlet`, re-run Scalog/LazyLog cells (CSV purged
2026-07-15; backup `results.csv.bak_pre_scalog_lazylog_purge`). Do **not** cite
pre-fix Scalog disk ≫ Embar disk.

### A2. Scalog mem vs Embar mem (2026-07-16)

Scalog mem BW can exceed Embar O5 mem and look “weird” (N2>N1, sub-second
overlap). Causes:

1. **Protocol (legitimate):** Scalog RF2 is parallel `min(replication_done)` +
   ORDER=1; Embar O5 is GOI-gated **serialized chain** + CV. Not a pure sink A/B.
2. **CXL invalidate (unfair, fixed):** Scalog only flushed the first cacheline
   before copy; Embar full-range invalidates. Scalog now calls
   `invalidate_cache_range_for_read` before CXL→DRAM/disk copy.
3. **Per-source mem rings (fixed):** RF2 primary+replica no longer share one
   unlocked ring.
4. **Disk sync amortization (fixed):** Scalog CXL path now uses Embar’s
   `EMBARCADERO_CHAIN_SYNC_BYTES` / interval instead of per-chunk fdatasync.
5. **Claim bug (fixed):** `ACK_DURABILITY_CONTRACT` for Scalog/LazyLog mem was
   still `...media_durable...`; now `...memory_copy...` when sink=memory-copy.

Scalog **mem** CSV rows purged 2026-07-16 (`results.csv.bak_pre_scalog_mem_purge`);
re-run after rebuild before citing.

### A3. LazyLog is metadata-bound (not a Fig1 sink series)

Faithful LazyLog ACK waits on **metadata `AppendToAll`** (per-batch, RF metadata
replicas with sidecar `fdatasync`) before the data-plane `min(replication_done)`
clamp — so disk↔mem data-sink deltas will not show in ACK BW.

Fixes applied: channel/stub reuse; contract renamed to
`ack2_metadata_append_plus_minimum_*_replica_prefix`. Fig1 defaults
`SKIP_LAZYLOG=1`. Re-include only as a separate “faithful append” row, not as a
disk/mem sink companion.

**Full experiment plan: `PaperScripts/FIG2_LAZYLOG.md`.**

Preflight cell (run this first to confirm the faithful path is live):
```bash
NUM_TRIALS=1 SKIP_LAZYLOG=0 SKIP_MEM=1 SKIP_BASELINES=0 SKIP_SCALOG_LAZYLOG=0 \
  ONLY_CELLS=fig1_lazylog_o2_disk_n1 \
  bash PaperScripts/run_fig1_throughput_scaling.sh
```
Check broker log for “LazyLog metadata replication enabled with 2 replicas”
and the result CSV for `status=ok` and `overlap_gbps > 0`.

### B. Overlap vs Bandwidth vs Send-done

| Metric | Meaning |
|--------|---------|
| Send-done | Bytes pushed; can scale with N while ACK is flat |
| Bandwidth | E2E incl. post-send `ack_wait` (honest bulk drain) |
| Overlap | Concurrent ACK progress; **noisy if window ≪ 10 s** |

Embar mem: Send-done scales (8→15→19→30 GB/s); ACK aggregate stays ~6–8 GB/s
because N=1 is already near the ACK/CXL-replica ceiling — N=2 cannot double.
N=4 overlap spikes (e.g. 8.84) with sub-second windows are **not** trustworthy;
use TOTAL Bandwidth (~8 GB/s).

Scalog overlap that exceeds `10 GiB / window` is invalid (burst artifact).

### C. RF2 DRAM ACK “only ~6–8 GB/s” is not a fake path

DRAM-replica ACK2 still does CXL write (ingest) + CXL read (replica) + DRAM ring
copy. Measured CXL ~21 GB/s/dir; concurrent R+W ≈ ~10 GB/s/dir. Plus 100 G NIC
and flushes. Embar DRAM ACK ~6–8 GB/s is near that envelope; label it
**DRAM replica completion**, not media-durable ACK2.

### D. Long `ack_wait` ≠ per-message latency

Bulk TP blasts ~10 GiB at NIC rate then drains ACKs. Disk ACK ~1.3 GB/s ⇒
~seconds of backlog. That is **pipeline depth / rate mismatch**, not O5 adding
multi-second latency per append. Embar O5+**mem** drains in ~0.3 s — O5 ACK path
is fine. Paper latency curves are a different experiment (controlled load).

### E. Embar disk vs “who has better disks”

Embar disk uses amortized `fdatasync` (256 MB). Scalog CXL path uses
**per-chunk** `fdatasync` when writing a real fd — once fairly wired to NVMe,
Scalog may be **slower**, not faster. Do not interpret purged Scalog-disk
numbers as a design win for Scalog.

### F. Client binaries and CXL size

- Sync `throughput_test` md5 across moscxl/c4/c3/c1 before a paper pass.
- `EMBARCADERO_CXL_SIZE=274877906944` (256 GiB) required for 4×8 GiB segments.

### G. Corfu

Corfu RF2 disk has seen ACK timeout / shortfall; treat incomplete Corfu cells
as non-plot until stable. Token-before-write protocol tax remains.

## Correctness checklist

1. RF=2 ACK=2 for every plotted series.
2. Embar sink env isolation (disk clears `INMEM*`; mem sets both).
3. **Scalog disk/mem** — rebuilt `embarlet` with per-source rings + amortized
   sync; fail-closed if RF>1 without `CHAIN_REPLICATION_SINK`. LazyLog off by
   default (`SKIP_LAZYLOG=1`).
4. Matched threads / batch / msg size.
5. N=4 is `local` NUMA 0.
6. Dual NVMe for Embar disk (`/mnt/nvme0` second dir).
7. No nested flock around `run_multiclient`.
8. Client binaries synced; CXL size 256 GiB.
9. Prefer Bandwidth when overlap window warned.
10. Clean git tag for publication freeze.

## Review log

| Pass | Result |
|------|--------|
| R1–R3 | Draft, CSV quoting, contracts, local NUMA=0 |
| CXL size | Default 64 GiB aborted; set 256 GiB |
| Client sync | Redeployed matching `throughput_test` mid-campaign |
| Scalog/LazyLog audit | Sink mismatch found; CSV purged; **harness+mem-copy fixed** |
| Metric caveats | Documented overlap/Bandwidth/Send-done + ack_wait ≠ latency |
