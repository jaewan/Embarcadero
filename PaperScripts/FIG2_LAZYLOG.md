# LazyLog Faithful-Append Experiment Plan (FIG2_LAZYLOG)

## 1. Scientific question

Does Embarcadero ORDER=5 (ordered + durable) match or exceed faithful LazyLog's
pre-binding ACK throughput (durable, not yet ordered) at matched RF=2?

This directly validates the paper's central claim: Embarcadero reaches
**ordered + durable** ACK in the same throughput class that LazyLog uses for
**durable-only** ACK.  The FIFO and per-session-prefix claims are architectural
(not measurable by throughput alone) and belong in the design and
failure-recovery sections, not this figure.

---

## 2. What "faithful LazyLog" means here

**ACK path:** `AppendToAll` fan-out to RF metadata replicas (each does `fdatasync`
on its sidecar) **AND** `min(replication_done)` across RF data replicas.  Global
binding is **not** consulted — see `src/common/lazylog_append_contract.h`,
`CanCompleteLazyLogAppend`.

**Required gate:** `EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS` must be set with
exactly RF comma-separated `host:port` entries.  `REQUIRE_FAITHFUL_LAZYLOG=1`
is passed by the script to fail-close if the env var is missing.

**Binary:** `build/bin/lazylog_metadata_replica` must be running at each endpoint
before brokers start (run_fig1_throughput_scaling.sh handles this automatically
via `start_lazylog_metadata`).

---

## 3. What is NOT being measured here

| Excluded | Reason |
|---|---|
| Ordered-delivery throughput (binding-gated) | Binding is background; subscribers gate on it, not the append ACK |
| Disk vs mem data-sink delta for LazyLog | Expected ~flat — metadata `fdatasync` dominates both |
| Per-client FIFO | Architectural claim; LazyLog has none; Embarcadero has it |
| Failure recovery | Separate figure (Fig 3 / Sec 7.3) |
| RF=1 LazyLog without metadata endpoints | Silently binding-gated ACK — not faithful; see Section 10 |

---

## 4. Series to measure

### Panel A — Throughput vs N clients (main result)

| Legend label | System | ACK semantics | RF | Sink |
|---|---|---|---|---|
| Embar O5 disk | Embarcadero | ordered + durable (fdatasync) | 2 | disk-durable |
| Embar O5 mem | Embarcadero | ordered + replicated (mem-copy) | 2 | memory-copy |
| LazyLog faithful | LazyLog | durable pre-binding (metadata fdatasync + data-replica done) | 2 | disk |

**One LazyLog series only (not two):** faithful ACK is inherently disk-bound via
metadata `fdatasync`, so there is no meaningful "LazyLog faithful mem" variant —
the disk↔mem delta will be flat.

Independent variable: N ∈ {1, 2, 3} remote clients (c4, c3, c1) + N=4 with one
local client.

### Panel B — Latency vs load (optional, follow-up)

LazyLog faithful ACK P50/P99 vs Embar O5 at controlled single-client load
(100, 250, 500, 1000 MB/s). Shows whether Embar's ordered+durable latency
tracks LazyLog's durable-only latency or exceeds it — and at what load point.

---

## 5. Harness configuration

```bash
# Kill any stale metadata replicas first
pkill -x lazylog_metadata_replica 2>/dev/null; sleep 1

# Sidecar directories
mkdir -p .Replication/lazylog_meta/a .Replication/lazylog_meta/b

# Start two metadata replicas (done automatically by start_lazylog_metadata in script)
setsid build/bin/lazylog_metadata_replica \
  --listen 0.0.0.0:50081 \
  --sidecar .Replication/lazylog_meta/a/metadata.sidecar \
  >lazylog_meta_a.log 2>&1 </dev/null &

setsid build/bin/lazylog_metadata_replica \
  --listen 0.0.0.0:50082 \
  --sidecar .Replication/lazylog_meta/b/metadata.sidecar \
  >lazylog_meta_b.log 2>&1 </dev/null &

sleep 2
nc -z 10.10.10.10 50081 && echo "50081 OK" || echo "50081 FAIL"
nc -z 10.10.10.10 50082 && echo "50082 OK" || echo "50082 FAIL"

# Run faithful LazyLog cells only (preflight: single N=1 trial first)
NUM_TRIALS=1 \
SKIP_LAZYLOG=0 \
SKIP_MEM=1 \
SKIP_BASELINES=0 \
SKIP_SCALOG_LAZYLOG=0 \
ONLY_CELLS=fig1_lazylog_o2_disk_n1 \
  bash PaperScripts/run_fig1_throughput_scaling.sh

# Full run (3 trials, all N values, LazyLog disk only)
NUM_TRIALS=3 \
TARGET_TRIALS=3 \
SKIP_LAZYLOG=0 \
SKIP_MEM=1 \
SKIP_BASELINES=0 \
SKIP_SCALOG_LAZYLOG=0 \
  bash PaperScripts/run_fig1_throughput_scaling.sh
```

The script already sets `REQUIRE_FAITHFUL_LAZYLOG=1` and
`EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS=$LAZYLOG_RF2_METADATA_ENDPOINTS`
for all LazyLog cells (run_fig1_throughput_scaling.sh:500-508).

> ⚠️ **Testbed fault-domain limitation:** both metadata replicas run on the same
> machine (moscxl, ports 50081 and 50082).  The
> `lazylog_metadata_replica_contract.md` requires distinct failure domains; this
> deployment violates that requirement.  Consequence: a single broker-host crash
> loses both replicas — the RF=2 durability claim does not hold across host
> failures on this testbed.  Label all results as "co-located RF=2 (single
> host)" and do not claim cross-host durability.  For publication, replicas must
> be placed on separate machines (e.g., c4 and c3 via SSH forwarding).

---

## 6. Expected outcomes and interpretation

### Expected: LazyLog faithful throughput — measure, don't predict

`AppendToAll` blocks the **network-receive thread** until every metadata replica
returns success after `fdatasync`.  This is the structural bottleneck.

**Throughput ceiling formula** (per receive thread, single broker):

```
ceiling = CLIENT_PUB_BATCH_KB / (gRPC_RTT_to_replica + fdatasync_latency)
```

On this testbed (loopback gRPC, same-machine replicas):
- gRPC loopback RTT ≈ 50–100 µs
- NVMe `fdatasync` for a small sidecar append ≈ **50–200 µs** (not 1 ms — NVMe,
  not spinning disk; `fdatasync` on a small append is a journal flush, not a data
  flush of the full payload)
- With 2 MB batches: ceiling ≈ 2 MB / (100 + 150) µs ≈ **8 GB/s per thread**

However, 4 broker processes share the NVMe. Concurrent `fdatasync` calls serialize
on the journal: effective `fdatasync` latency rises 4× → ceiling ≈ 2 GB/s per broker.

**Important testbed caveat:** both metadata replicas are on the **same machine**
(ports 50081, 50082 on 10.10.10.10). The `AppendToAll` fan-out is therefore not
truly parallel — both `fdatasync` calls compete for the same NVMe journal.
For a different-machine RF=2 deployment, the `fdatasync` calls would overlap fully.

**Do not hard-code an expected range.** Measure and report empirically.
Annotate the result with: "testbed: both metadata replicas co-located on broker host."

### Expected: Embar O5 disk >> LazyLog faithful

This is the main result. Embar's replica `fdatasync` runs on the replica-pull
thread (parallel to ordering), not inline on the network-receive thread.
Embar's sequencer assigns global order without a coordinator round-trip.
LazyLog's metadata `fdatasync` IS on the inline receive thread.

### Expected: disk↔mem delta for LazyLog ≈ 0

Metadata `fdatasync` dominates the ACK latency regardless of data sink.
A flat LazyLog faithful series is **correct behavior**, not a broken measurement.
This is why LazyLog goes in a separate panel, not in the disk/mem A/B panel.

### If LazyLog faithful >> 3 GB/s (unexpected)

- Verify `EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS` is actually being consumed:
  check broker logs for "LazyLog metadata replication enabled with 2 replicas"
- Verify replicas are doing `fdatasync`: check sidecar file size grows per run
- Verify `AppendToAll` is not returning before replicas ack:
  set `VLOG=1` and check `LazyLog metadata append descriptor` log lines

### If LazyLog faithful < 200 MB/s (unexpectedly low)

- Metadata sidecar and data replica may share the same NVMe device, causing
  `fdatasync` contention
- Fix: point metadata sidecar to the second NVMe:
  `--sidecar /mnt/nvme0/lazylog_meta/a/metadata.sidecar`
- Or: run metadata replicas with `O_DSYNC` (modify `AppendRecord` to open with
  `O_DSYNC | O_CREAT | O_RDWR` — skips explicit `fdatasync`)

---

## 7. Claim this measurement supports

> Embarcadero ORDER=5 delivers messages that are both globally ordered and
> media-durable at the same aggregate throughput that LazyLog's pre-binding
> append contract achieves for durability alone — without global order.

This makes the ordering-is-not-the-bottleneck argument empirical:
- (ordered + durable) throughput ≥ (durable-only) throughput
- LazyLog's bottleneck is per-batch inline metadata `fdatasync`; Embar moves
  replication off the receive path

---

## 8. Publication treatment

LazyLog faithful results go in a **separate panel** from the Fig1 disk/mem A/B.

**Series label:** "LazyLog pre-binding ACK (durable, unordered)"

**Embar comparison label:** "Embarcadero ordered + durable ACK₂ (disk)"

**Caption note (mandatory):**
"LazyLog ACK is pre-binding: the batch is media-durable but has no global
sequence number yet. Binding assigns total order asynchronously; subscribers
gate on the binding frontier. Embarcadero ACK₂ is both ordered and durable."

**Do NOT include** in the disk/mem sink panel (flat line looks broken).
**Do NOT include** any binding-gated LazyLog result (that is not LazyLog's number).
**Do NOT** label faithful LazyLog as "ACK₂" without the pre-binding qualifier
(ACK₂ in the paper means ordered+durable for all other systems).

---

## 9. Prerequisites checklist

Before running:

- [ ] Build `embarlet` and `lazylog_metadata_replica` after double-credit fix commit
- [ ] Confirm `build/bin/lazylog_metadata_replica` exists on moscxl
- [ ] Preflight: `nc -z 10.10.10.10 50081 && nc -z 10.10.10.10 50082`
- [ ] Sidecar dirs exist and writable: `.Replication/lazylog_meta/a` and `b`
- [ ] Run single N=1 preflight cell; verify `overlap_gbps > 0` and `status=ok`
- [ ] Confirm broker logs show "LazyLog metadata replication enabled with 2 replicas"
- [ ] Sync `throughput_test` binary to all client machines (c1–c4)
- [ ] Confirm no stale metadata replica processes: `pgrep lazylog_metadata_replica`

---

## 10. Follow-up: RF=1 faithful LazyLog (separate experiment)

> ⚠️ **Critical: RF=1 without a metadata endpoint is NOT faithful LazyLog.**
>
> `SupportsPerClientAppendAckLevel1()` returns `false` when
> `lazylog_metadata_replica_client_ == nullptr` (no metadata endpoints configured).
> The ACK then falls through to `SupportsPerClientAckLevel1()` which is `true` for
> `LAZYLOG && order==kOrderTotal` — this engages the **binding-gated** ordered path
> (`ApplyGlobalBinding` → `tinode->ordered`), NOT the pre-binding faithful path.
>
> Any RF=1 LazyLog run without `EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS` set is
> silently binding-gated and must not be labeled or compared as faithful LazyLog.

To run a genuine RF=1 faithful ablation, you must set one metadata endpoint:

```bash
LAZYLOG_RF2_METADATA_ENDPOINTS=10.10.10.10:50081   # one endpoint for RF=1
REPLICATION_FACTOR=1
ACK=1
```

`Topic::Topic` validates `metadata_endpoints.size() == replication_factor` (both
= 1 here), so one endpoint is correct.  `start_lazylog_metadata` starts two
replicas by default; for RF=1 only the port-50081 replica is used.

The faithful RF=1 ACK reduces to:
- `AppendToAll` to 1 metadata replica (1 `fdatasync` per batch)
- `min(replication_done)` across RF=1 = self only (trivially satisfied on CXL write)

This isolates metadata coordinator cost from data replication cost — useful ablation
for understanding whether throughput is limited by metadata `fdatasync` alone or by
both metadata and data replication together.  Not part of the main paper result.

---

## 11. Review log

| Date | Note |
|---|---|
| 2026-07-15 | Faithful path implemented; channel/stub reuse fixed; experiment plan written |
| 2026-07-15 | Double-credit fix for SCALOG/LAZYLOG DrainDurableBatches committed to broker |
| 2026-07-16 | Post-review fixes: ACK2 canary scoped to ORDER=0 only; LazyLog batch_complete CXL flush added; LazyLog mem guard added to fig1 script; fdatasync estimate corrected; fault-domain caveat added; RF=1 silent fallback documented |
