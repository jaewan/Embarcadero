# PROMPT — feed to moscxl claude-code (Track-05 RDMA write-bug fix, with commit)

```
You are a coding agent on moscxl. Fix the Track-05 RDMA one-sided WRITE bug in rdma_bench, then COMMIT
the fix. The FABRIC IS ALREADY EXONERATED (stock ib_write_bw passes broker<->c1) — the bug is in OUR
code. Testbed is idle.

WORKING DIR: ~/Embarcadero-sessions/s2-work  (git repo; branch `s2` @ 9901b93 has src/rdma_transport/).
First:  git switch -c rdma-write-fix s2
Do ALL work here. NEVER touch ~/Embarcadero (live tree) or other ~/Embarcadero-sessions/* dirs.

READ FIRST (authoritative — has the full root-cause, the exoneration, the ranked leads, and the debug
procedure): docs/experiments/RDMA_WRITE_BUG_TRACK05_BRIEF.md.
Summary: `rdma_bench --op=write` (broker<->c1, 64KiB) -> IBV_WC_REM_INV_REQ_ERR (status 9) on the
first write; `--op=read` (56Gb/s) and `--op=fetchadd` (1.51Mops) PASS on the same QP/MR. Stock
ib_write_bw PASSES the same hosts/gid -> the defect is in rdma_bench's runtime path/params, NOT the
verbs setup (MR/QP flags + PostWrite WR already verified correct). Top lead (H1): the client never
checks `size` vs the peer's advertised `remote.region.len` (rdma_bench_main.cc:88-92) -> an
out-of-bounds RETH gives REM_INV_REQ.

BUILD (STANDALONE — src/rdma_transport has its own CMakeLists, needs only libibverbs-dev; no main-tree
dep; do it outside the flock lock):
  cd ~/Embarcadero-sessions/s2-work/src/rdma_transport
  cmake -S . -B build_rdma && cmake --build build_rdma -j
  (c1 lacks cmake -> build there via the g++ one-liner documented in handoff-05 if you need the client
   binary local to c1; or run the client from broker's build over the fabric.)

FABRIC PRECHECK: the RoCE config is non-persistent — c1 may have lost its RoCEv2 IPv4 GID on reboot.
Verify c1 has a RoCEv2 IPv4 GID (the binary auto-scans with gid=-1); if absent, re-apply c1's recipe
from sessions/rdma_fabric_setup.md (c1 has sudo). Confirm with `ib_write_bw` broker<->c1 as the control.

DEBUG (under flock; log START/END to ~/Embarcadero-sessions/activity.log; kill strays by explicit PID
— NEVER `pkill -f`; a client that dies before OOB connect must not strand the server on the lock):
  1. Reproduce with control: `--op=write --size=4096` with matched server `--bytes >= size`. If 4KiB
     PASSES but 64KiB fails -> size/bounds in our code; if 4KiB also fails -> write-path defect.
  2. Run write vs read with IDENTICAL params + verbose CQ logging; capture the exact WC and the exact
     raddr/rkey/size/region.len on both sides.
  3. Apply the H1 defensive bounds guard (correct regardless): in rdma_bench_main.cc (~:89)
     `if (op != "fetchadd" && size > remote.region.len) { fprintf(stderr,"size %u > remote %u\n",size,remote.region.len); return 1; }`
  4. If it still fails at matched small size with valid bounds, bisect our QP/MR/AH setup against stock
     ib_write_bw (perftest source).
  Fix the actual defect; verify `--op=write` PASSES broker<->c1 at 4KiB AND 64KiB with real bandwidth.

COMMIT (branch rdma-write-fix; --no-verify --no-gpg-sign):
  - the rdma_bench fix (+ the bounds guard), and
  - a short "## Fix" section appended to docs/experiments/RDMA_WRITE_BUG_TRACK05_BRIEF.md: root cause
    confirmed, the change, and the passing write bandwidth.
  Trailer: Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>

RECONCILE:  git bundle create /tmp/rdma_write_fix.bundle s2..rdma-write-fix
REPORT: root cause, the fix, passing write bandwidth (4KiB + 64KiB), commit SHA, bundle path. Testbed
left clean (lock free, no strays, no stranded server). Do NOT push.
```
