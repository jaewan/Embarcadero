# Fig 1 — Append throughput scaling (RF2 / ACK2)

## Claim

Under matched RF2+ACK2 durability, Embarcadero ORDER=5 scales with remote
publishers until the broker NIC / CXL-replica path saturates; a 4th **local**
publisher can lift throughput past the remote NIC knee (CXL headroom). Fair
baselines require the **same replica sink wiring** (real dual-NVMe disk vs
true memory-copy).

## Fixed knobs (do not change between trials)

| Knob | Value | Why |
|------|-------|-----|
| Brokers | 4 | Paper topology |
| RF / ACK | 2 / 2 | Media-durable contract (`ack_rf_policy`) |
| Embar order | 5 only | Fig1 is the strong-order curve (O0/O5 → Table 1) |
| Message size | 4096 B | Paper Fig1 draft |
| Bytes / client | 10 GiB | Steady window; not microbench |
| Publish batch | 2048 KB | `client.yaml` design point |
| Threads/broker | 6 | Matched across Embar + baselines |
| Epoch µs | 500 | ORDER=5 remote design point |
| Disk dirs | `.Replication/disk0` + `/mnt/nvme0/replication/disk1` | Dual NVMe |
| Runtime | `throughput` | Not latency/linger |
| CXL size | 256 GiB | 64 GiB default fails 4-broker segment preflight |

## Client roster (independent variable = N)

| N | Hosts | Role |
|---|-------|------|
| 1 | `c4` | Primary remote (NUMA 1, 100G) |
| 2 | `c4,c3` | Two full-PCIe remotes |
| 3 | `c4,c3,c1` | NIC saturation / plateau |
| 4 | `c4,c3,c1,local` | 3 remote + 1 local (CXL headroom) |

NUMA pins: c4→1, c3→1, c1→0, local→0 (broker node).

## Figure layout

Panels (a--b) plot Embar O5 and Scalog O1 across {disk, mem} with **matched
data sinks**. Panel (c) plots the validated V0--V4 Embarcadero path
decomposition at N=2 from `fig1_path_decomp/path_ablation_summary.csv`.
Individual accepted trials remain visible behind median lines; no
performance-based outlier filtering is allowed.

The N=4 point changes both the client roster and the reported metric. It is
therefore drawn as an open `4*` ceiling marker connected by a dashed segment,
not as a continuation of the N=1--3 remote overlap-throughput curve.

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

- **Overlap GB/s** = Σ(Δ`Cum_Ack_Bytes` / shared window) — ACK-paced (ACK2 here).
- Also record Bandwidth (e2e incl. `ack_wait`) and Send-done (excludes Poll).
- Prefer Bandwidth when overlap window ≪ 10 s (`MIN_OVERLAP_MS` warnings).
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
