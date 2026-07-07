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
