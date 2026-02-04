# Diagnostic Runbook: Recv Bottleneck

**Purpose:** Step-by-step order to run recv-bottleneck diagnostics. Use this while a throughput test is active (brokers and client running).

See also: `docs/CRITICAL_ASSESSMENT_RESPONSE.md`, `docs/RECV_OPTIMIZATION_METHODS.md`.

---

## Order of operations

### 1. Run throughput test (with new instrumentation)

**Option A — one command (recommended):** Run throughput test and diagnostics together so the diagnostic script runs while brokers are alive:

```bash
cd /home/domin/Embarcadero
./scripts/run_throughput_with_diagnostics.sh
```

This starts the throughput test, waits ~28s, runs `diagnose_recv_bottleneck.sh` (TCP window, strace, mpstat), then waits for the test to finish. Override: `DIAG_WAIT_SEC=25 THROUGHPUT_LOG=/tmp/my.log ./scripts/run_throughput_with_diagnostics.sh`

**Option B — two terminals:** Run the test, then run the diagnostic script manually within ~25–30s:

```bash
cd /home/domin/Embarcadero
./scripts/run_throughput.sh 2>&1 | tee /tmp/throughput_run.log
```

Or with kernel buffer tuning (requires sudo once):

```bash
sudo ./scripts/tune_kernel_buffers.sh
EMBARCADERO_TUNE_KERNEL_BUFFERS=1 ./scripts/run_throughput.sh 2>&1 | tee /tmp/throughput_run.log
```

### 2. While test is running — run diagnostic script

In another terminal:

```bash
cd /home/domin/Embarcadero
./scripts/diagnose_recv_bottleneck.sh
```

This script:

- Finds broker PIDs (or use `BROKER_PID=1234 ./scripts/diagnose_recv_bottleneck.sh`)
- Runs **ss -ti** for port 1214 (TCP window: rcv_space, snd_wnd, wscale)
- Runs **strace -c -p &lt;broker_pid&gt;** for 10s (syscall count — is recv() dominating?)
- Runs **mpstat -P ALL 1 5** (CPU vs I/O wait)

Override broker port or duration:

```bash
BROKER_PORT=1214 STRACE_SEC=15 MPSTAT_SEC=10 ./scripts/diagnose_recv_bottleneck.sh
```

### 3. After test — inspect logs

**Publisher profile** (in throughput_test stdout, e.g. /tmp/throughput_run.log):

```bash
grep "\[PublisherPipelineProfile\]" /tmp/throughput_run.log
```

**Broker profile and DrainPayloadToBuffer:** When using `run_throughput_with_diagnostics.sh`, broker output is often interleaved in the same throughput log. Otherwise check broker stderr / GLOG (e.g. `build/bin` cwd):

```bash
grep -E "\[PublishPipelineProfile\]|\[DrainPayloadToBuffer\]" /tmp/throughput_run.log
# If not found, try: build/bin/*.log or wherever GLOG writes
```

Check:

- **DrainPayloadToBuffer %** — Fresh wall-clock % (we re-enabled one timestamp per completed batch). High % can mean (A) recv syscall overhead or (B) recv waiting for data.
- **Avg recv calls per batch** — If ~5–10, healthy; if 20+, kernel may be returning small chunks (tune SO_RCVBUF / net.core.rmem_max).
- **Avg bytes per recv** — total_bytes_drained / recv_calls. If low (e.g. ~84 KB), kernel/TCP window is limiting chunk size.

### 4. Manual diagnostics (if script is not enough)

**TCP window (during test):**

```bash
ss -ti state established | grep -E ":1214" -A 5
# Or: ss -ti | grep -E "rcv_space|snd_wnd|wscale"
```

**Syscall count (during test, pick one broker PID):**

```bash
# Get broker PID: pgrep -f "embarlet.*--config"
strace -c -p <broker_pid>   # Run 10s, then Ctrl+C — summary shows % per syscall
```

**CPU vs wait (during test):**

```bash
mpstat -P ALL 1   # Run a few seconds, Ctrl+C
```

**Network utilization (optional):**

```bash
sar -n DEV 1 10   # Or: iftop
```

---

## How to interpret

| Observation | Likely cause | Next step |
|-------------|--------------|-----------|
| DrainPayloadToBuffer 97%+, CPU low, recv calls/batch ~5–10 | recv() **waiting** for data (slow sender / TCP window) | Check publisher, TCP window (ss -ti), kernel buffers (getsockopt) |
| DrainPayloadToBuffer 97%+, CPU high, recv() 80%+ of syscalls | recv() **syscall** overhead | Consider io_uring, GRO, coalescing |
| Avg bytes per recv ~84 KB or small | Kernel/TCP returning small chunks | Ensure SO_RCVBUF and net.core.rmem_max are applied (scripts/tune_kernel_buffers.sh) |
| rcv_space / snd_wnd small in ss -ti | TCP window not scaling | Kernel tune, check sender drain rate |

---

## Accepted-socket buffer fix (2026-02)

If **rcv_space** on broker payload connections (port 1214) is ~43 KB in `ss -ti`, the broker’s *accepted* sockets were using the kernel default receive buffer. We now set **SO_RCVBUF** and **SO_SNDBUF** (32 MB) on each accepted payload socket in `NetworkManager::MainThread()` right after `accept()` (see `SetAcceptedSocketBuffers()` in `src/network_manager/network_manager.cc`).

**After this fix:**

1. Rebuild and run the throughput test (same env: ORDER=0, ACK=1, TEST_TYPE=5, 10GB, 1024 B, 4 brokers).
2. Re-check **rcv_space** with `ss -ti` during or after the test — expect large values (e.g. ~32 MB) on broker payload connections, not ~43 KB.
3. If bandwidth improves (e.g. toward 1–2 GB/s), the small TCP window was the main bottleneck; see `docs/DIAGNOSTIC_RUN_RESULTS.md`.

**Kernel tuning** (`sudo ./scripts/tune_kernel_buffers.sh`) is still required so the kernel allows large buffers; the code fix applies those buffers to *accepted* connections.

---

## Quick reference: script and logs

| What | Command / location |
|------|--------------------|
| Diagnostic script | `./scripts/diagnose_recv_bottleneck.sh` |
| Broker profile (every 10s) | grep `[PublishPipelineProfile]` in run log |
| Recv calls + avg bytes per recv | grep `[DrainPayloadToBuffer]` in run log |
| Kernel buffer tune | `sudo ./scripts/tune_kernel_buffers.sh` |
| Accepted-socket buffers (broker) | `SetAcceptedSocketBuffers()` in `network_manager.cc` — re-check rcv_space after fix |
| Diagnostic run results | `docs/DIAGNOSTIC_RUN_RESULTS.md` |
