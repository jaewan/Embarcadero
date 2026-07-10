# BLOCKER-1 verification rerun (review fix, 2026-07-08)

Demonstrates the fixed `rdma_recovery` (`--pbr-slots`/`--seam-final-seq` programmatic ring-seam
exclusion, `--dump-file`/`--verify-only` to defer scoring until the memserver's own end-of-run
sentinel dump is available) producing a clean, machine-checked PASS on a fresh backlogged-kill
trial with a real ring seam, replacing the original "1 mismatch = PASS by hand" framing for
trial 4 of `leg1_item1_backlogged_kill/`.

Same topology/params as `leg1_item1_backlogged_kill/` (broker0=moscxl, broker1=c1, memserver=c3,
200000-slot PBR ring, unthrottled load, kill broker0 at t=4s). Two-pass recovery per trial:
1. **Move** (`recovery_move.txt`): immediately after detection, `rdma_recovery --dump-file=...`
   reads 40,960,000 bytes (10000 slots) from broker1's replica and writes them to c3's spare
   region, ALSO saving a local copy — no verify yet (the seam value isn't known until the
   memserver's own end-of-run dump, which happens after this step).
2. **Verify** (`recovery_verify.txt`): after all processes exit, `final_global_seq_broker0` is
   parsed from `ms.txt`'s `sentinel[broker=0]=<value>` line, then
   `rdma_recovery --verify-only=<dump> --pbr-slots=200000 --seam-final-seq=<value>` re-scores the
   dumped bytes with NO RDMA involved, deriving the seam's exact physical slot programmatically
   (`seam_final_seq % pbr_slots`) rather than a human picking it from the FAIL output.

| Trial | Detected via | `final_global_seq_broker0` | `excluded_seam_slot` (= final_global_seq % 200000) | Result |
|---|---|---|---|---|
| 1 | LEASE_TIMEOUT (6.978s) | 3,847,044 | 47,044 | `bad_slots=0 PASS`, `recovery_verify_rc=0` |
| 2 | LEASE_TIMEOUT (6.986s) | 3,882,621 | 82,621 | `bad_slots=0 PASS`, `recovery_verify_rc=0` |
| 3 | RDMA_ERROR/kPeerDown (8.264s) | 3,016,480 | 16,480 | `bad_slots=0 PASS`, `recovery_verify_rc=0` |

All three excluded-seam-slot values are distinct and derived from that trial's own real final
sequence number (not a hardcoded or copy-pasted constant) — confirming the exclusion is genuinely
programmatic. `recovery_verify_rc=0` in all three trials is the machine-checked pass criterion (no
manual override). This is a **structural / low-8-bit sequential-pattern check** (see
`../../w5_phase3b_results.md` item 2), not full byte-for-byte identity verification.

`recovered.bin`/local dump files are deleted by the rerun script immediately after scoring (they
are transient recovery payloads, not measurement artifacts worth archiving) — `du -sh` on this
directory is small (~104K after truncating broker1's known ENOSPC replication-QP log spam, same
pattern as `leg1_item1_backlogged_kill/`).
