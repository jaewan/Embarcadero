# Diagnostic Run Results (Phase 1–3)

**Date:** 2026-02-01  
**Test:** `ORDER=0 ACK=1 TEST_TYPE=5 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 NUM_BROKERS=4`  
**Script:** `./scripts/run_throughput_with_diagnostics.sh` → `/tmp/full_diagnostic_run.log`, `/tmp/throughput_run.log`, `/tmp/diagnose_recv_output.txt`

---

## Phase 1–2: Metrics Extracted

| Metric | Value |
|--------|--------|
| **Bandwidth** | 884.53 MB/s |
| **Publisher pipeline** | BufferWrite 7.78%, SendPayload 92.22% |
| **DrainPayloadToBuffer / recv per batch** | Not in client log (broker logs go to broker stdout; not captured in same file) |
| **TCP rcv_space (ss -ti)** | **43,690 bytes** on broker-side payload connections (receiver) |
| **strace** | `ptrace(PTRACE_SEIZE): Operation not permitted` (needs sudo) |
| **CPU (mpstat)** | **%idle 96.44** (all) — CPU almost entirely idle |

---

## Phase 3: Decision-Tree Interpretation

- **Bandwidth < 1 GB/s** ✓ (884 MB/s)
- **CPU idle > 50%** ✓ (96.44% idle)
- **rcv_space on broker payload connections** = **43,690** (~43 KB) — very small

**Conclusion:** **TCP window limited; sender/network bottleneck.** The broker’s *receive* window (rcv_space) on payload connections is ~43 KB. With kernel tuning, `net.core.rmem_max` is 128 MB, but the *accepted* sockets on the broker were never given `SO_RCVBUF`; they kept the kernel default (~43 KB). So the bottleneck is the small TCP receive window on the broker, not recv() syscall overhead or CPU.

---

## Root Cause and Fix

- **Cause:** Accepted payload sockets in `NetworkManager::MainThread()` are created by `accept(server_socket, ...)`. On Linux, the new fd does *not* inherit `SO_RCVBUF` from the listening socket. The listening socket in `MainThread()` also never had `SO_RCVBUF` set (SetupPayloadSocket exists but is not used for the listener or for accepted fds). So every payload connection used the default rcvbuf (~43 KB) → **rcv_space:43690** in `ss -ti`.
- **Fix:** Set `SO_RCVBUF` and `SO_SNDBUF` on the **accepted** fd immediately after `accept()`, before enqueueing the request. Implemented in `src/network_manager/network_manager.cc`:
  - `SetAcceptedSocketBuffers(fd)` (32 MB, same as SetupPayloadSocket).
  - Called for `req.client_socket` right after a successful `accept()`.

---

## Re-run After Fix (SetAcceptedSocketBuffers)

- **Build:** Fixed missing `SetAcceptedSocketBuffers` definition; rebuild succeeded.
- **Bandwidth:** **891.08 MB/s** (was 884.53 MB/s) — ~0.7% improvement.
- **rcv_space in ss -ti:** Some broker payload connections still show **rcv_space:43690** (rcv_ssthresh:43690); others show large windows (e.g. rcv_space:1.2MB, rcv_ssthresh:62MB). So either (1) some connections did not get the new buffer (e.g. different code path or connection type), or (2) rcv_space is the *current* free space and the buffer is full because the broker is slow to drain. If rcv_ssthresh stays 43690, the socket buffer is still default-sized for those sockets.

## Next Steps

1. **Optional:** Add a LOG after `SetAcceptedSocketBuffers(req.client_socket)` and re-run to confirm it is called for every accepted payload connection.
2. **Optional:** Re-run diagnostics (including `ss -ti` after fix) to confirm rcv_space/rcv_ssthresh on broker payload sockets; if still 43690, investigate alternate accept paths or kernel capping.
3. **Optional:** Run `sudo ./scripts/diagnose_recv_bottleneck.sh` during a test to get strace syscall breakdown (ptrace requires root).

---

## Files

- Full run log: `/tmp/full_diagnostic_run.log`
- Throughput log: `/tmp/throughput_run.log`
- Diagnostic output: `/tmp/diagnose_recv_output.txt`
- Runbook: `docs/DIAGNOSTIC_RUNBOOK_RECV.md`
