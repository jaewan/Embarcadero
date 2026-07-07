# RDMA WRITE bug — empirically-grounded finding + Track-05 debug brief

**Symptom:** `rdma_bench --op=write` (moscxl↔c1, 64 KiB) → `IBV_WC_REM_INV_REQ_ERR` (status 9) on the
first write, QP dies. `--op=read` (56 Gb/s) and `--op=fetchadd` (1.51 Mops) PASS on the same QP/MR.

## DECISIVE result (2026-07-06) — it is OUR code, not the fabric
Stock perftest between the **same hosts, same device, same gid** (RoCEv2 gid3, `mlx5_0`, 64 KiB):
- `ib_read_bw` moscxl←c1 → **PASS, 6697 MiB/s** (control)
- **`ib_write_bw` moscxl←c1 → PASS, 6442 MiB/s**

**⇒ The fabric/NIC/RoCEv2 config handles inbound RDMA WRITE correctly.** This **refutes** both prior
guesses: (1) the first agent's "server MR missing `IBV_ACCESS_REMOTE_WRITE`" (flags are present,
`rdma_common.cc:91-93,123-124`), and (2) the root-cause workflow's synthesized "multi-packet
responder/fabric issue" (empirically false — stock write works). **The bug is in `rdma_bench`.**

## What is already verified CORRECT (static, 6 adversarial hypotheses — so look elsewhere)
`PostWrite` (`rdma_common.cc:207-219`) is byte-identical to the passing `PostRead` except
`opcode=IBV_WR_RDMA_WRITE`; correct `wr.wr.rdma` union arm; `num_sge=1 ≤ max_send_sge`; **no**
`IBV_SEND_INLINE`; signaled-only. Same `raddr/rkey/size` as read (`rdma_bench_main.cc:88-93`). MR +
QP have `REMOTE_WRITE`. PSNs matched. Same binary source both roles. So the bug is **not** in those.

## Leads (empirically-grounded — for the focused rdma_bench debug)
Since stock write works but ours fails with the WR construction verified, the defect is in `rdma_bench`'s
*runtime path/params*, most likely one of:
1. **Param/bounds in the FAILING run (top lead, H1):** the client never checks `size` vs the peer's
   advertised `remote.region.len` (`rdma_bench_main.cc:88-92`). If the failing WRITE run used a server
   `--bytes` < client `--size` (or a different remote region than the read run), an out-of-bounds RETH
   → `REM_INV_REQ`. **Confirm the exact `--bytes`/`--size` of the failing run.** (READ passing at 64 KiB
   only rules this out if the read run used the *same* server region as the write run.)
2. **op-dispatch / completion handling for write vs read** in `rdma_bench_main.cc` — a write-specific
   branch (buffer prep, CQ poll, inflight accounting) that static reading of `PostWrite` in isolation
   didn't cover.
3. **Delta vs stock ib_write_bw:** ours fails where stock passes → diff our QP/MR/AH setup against
   what `ib_write_bw` does (e.g. RNR/timeout, `rdma_cm` vs manual, AH grh fields).

## Debug procedure (Track-05, on the testbed)
1. **Reproduce with control:** `rdma_bench --op=write` with **`--size=4096`** (single MTU packet) AND
   matched server `--bytes ≥ size`. If 4 KiB write PASSES but 64 KiB fails → size/bounds/segmentation
   in our code; if 4 KiB also fails → write-path defect independent of size.
2. Run `--op=write` and `--op=read` **with identical params + verbose CQ logging**; capture the exact
   WC and the exact `raddr/rkey/size/region.len` on both sides for each.
3. Add the **defensive guard** (correct regardless, H1): `rdma_bench_main.cc:~89`
   `if (op != "fetchadd" && size > remote.region.len) { fprintf(stderr,"size %u > remote %u\n",size,remote.region.len); return 1; }`
4. If ours still fails at matched small size with valid bounds, bisect the QP/MR setup against stock
   `ib_write_bw` (source is in perftest).

## Status / ownership
- **Fabric: EXONERATED** (stock write works). Do NOT chase PFC/ECN/DSCP/firmware.
- **Bug: Track-05 `rdma_bench` code.** Non-blocking for the *rest* of the RDMA variant work (read +
  atomic paths — the reviewer-D naive-token repro — already work); blocks the WRITE-based W5 path.
- **Lesson:** static root-cause over-concluded "not a code bug"; a 2-minute `ib_write_bw` control
  flipped it. For hardware/verbs bugs, run the stock-tool control early.

## Fix (2026-07-07, branch `rdma-write-fix`)

**Reproduction status first (honesty):** with byte-identical binaries/params/GIDs to the failing
run, `--op=write` now PASSES at both 4 KiB (46.0 Gb/s) and 64 KiB (53.7 Gb/s ≈ the 54 Gb/s
`ib_write_bw` fabric reference). The original WC9 is **environment-sensitive and was not
reproducible today**. The debug therefore pivoted to the brief's lead #3 (delta vs stock) and
closed the mechanisms that fit the evidence.

**Root cause (latent defect, closed):** `ConnectRcQp` set `path_mtu =` **each side's own**
`active_mtu` with **no negotiation** (`rdma_common.cc`, RTR block). Stock perftest negotiates
`min(local, remote)` — the one setup-path delta vs `ib_write_bw`. Under an active-MTU divergence
between the two ports (the fabric MTU config is runtime/non-persistent, and the GID-index drift
5→3 on c1 proves link events occurred in the failure window), the requester segments WRITEs at a
PMTU the responder's strict inbound FIRST/MIDDLE length check rejects → `IBV_WC_REM_INV_REQ_ERR`
on the first write — while READ (requester-side response check is laxer) and single-packet
atomics keep passing. That asymmetric pass/fail pattern is exactly the observed symptom.

**Changes:**
1. `rdma_common.h/.cc` — `QpEndpoint` now carries `mtu` (active MTU enum, exchanged OOB);
   `ConnectRcQp` sets `path_mtu = min(local active, remote active)` and logs when it clamps.
   `mtu==0` (legacy peer) falls back to the old behavior. NOTE: `QpEndpoint` is a raw OOB wire
   struct — rebuild both ends together.
2. `rdma_bench_main.cc` — H1 defensive bounds guard: `--size` of 0 or > the peer's advertised
   `region.len` fails fast with the actual numbers instead of a cryptic WC9.
3. `rdma_common.cc PollCq` — WC errors now print status string, opcode, wr_id, vendor_err, and
   qp_num, so any future one-off self-documents.

**Verification (moscxl server ↔ c1 client, RoCEv2 gid auto-scan, both ends rebuilt):**
| Case | Result |
|---|---|
| write 4 KiB | PASS — 1.40 Mops/s, **46.01 Gb/s** |
| write 64 KiB | PASS — **53.65 Gb/s** (fabric ceiling ~54 Gb/s per ib_write_bw) |
| read 64 KiB (regression) | PASS — 56.18 Gb/s |
| fetchadd (regression) | PASS — 1.99 Mops/s |
| guard: size 64 KiB > server region 4 KiB | clean error, rc=1 (no WC9) |

**Residual:** the exact environmental trigger of 2026-07-06 was not captured live (it predates
the enriched WC logging). If WC9 recurs, the new diagnostics + negotiated PMTU log line will
pin it in one run. Recommend also making the fabric MTU/GID config persistent per
`sessions/rdma_fabric_setup.md` "Make it durable".
