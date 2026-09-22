#!/usr/bin/env python3
"""Optional owned DRAM tests of real ingress rejection and blocked shutdown.

Requires an explicitly fault-enabled build. No wire layouts are encoded here:
production_fault_driver uses the production C++ headers and generated protobufs.
"""

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import dev_cluster as dev
from fault_control import FaultControl

CORE_CASES = ("repeated_rejected_connections", "control", "fragmented_open", "truncated_control", "mismatched_client",
         "incomplete_payload", "malformed_body", "shutdown_partial_handshake",
         "shutdown_ack_connect", "shutdown_queue", "shutdown_partial_control", "shutdown_partial_body", "fence_before_commit", "commit_before_fence",
         "fence_empty_prefix", "ack_publication_lag", "shutdown_replication_token", "session_capacity",
         "goi_capacity", "blog_capacity", "rollover_retention")
SESSION_CASES = {"fence_before_commit": 8, "commit_before_fence": 8, "fence_empty_prefix": 2}
STORAGE_CASES = ("goi_capacity", "blog_capacity", "rollover_retention")
CLIENT_EXTENSIONS = ("ack_hwm_withheld", "session_reopen_resubmit")
CASES = CORE_CASES + CLIENT_EXTENSIONS
REAL_CLIENT_CASES = ("ack_publication_lag",) + CLIENT_EXTENSIONS
ACTIVE_TIMEOUT_SECONDS = 60
PAYLOAD_UPPER_BOUNDS = {
    "repeated_rejected_connections": 24576,
    "control": 24576, "fragmented_open": 24576, "truncated_control": 24576,
    "mismatched_client": 24576, "incomplete_payload": 32768, "malformed_body": 32768,
    "shutdown_partial_control": 0, "shutdown_partial_body": 8192,
    "shutdown_partial_handshake": 0, "shutdown_ack_connect": 0, "shutdown_queue": 0,
    "fence_before_commit": 49152, "commit_before_fence": 40960, "fence_empty_prefix": 16384,
    "ack_publication_lag": 8192, "ack_hwm_withheld": 8192, "session_reopen_resubmit": 65536,
    "shutdown_replication_token": 8192, "session_capacity": 16384, "goi_capacity": 24576,
    "blog_capacity": 263454720, "rollover_retention": 670433280,
}
HOOKS = {
    "fragmented_open": ("ingress.session_prefix.partial",),
    "shutdown_partial_handshake": ("ingress.handshake.partial",),
    "shutdown_partial_control": ("ingress.session_prefix.partial",),
    "shutdown_partial_body": ("ingress.payload.partial",),
    "shutdown_ack_connect": ("ack.connect.wait",),
    "shutdown_queue": ("queue.worker_paused", "queue.push.blocked"),
}


class ActiveDeadline:
    """Interrupt blocking Python controller waits; owned cleanup remains separate."""
    def __init__(self, seconds):
        if signal.getitimer(signal.ITIMER_REAL)[0] != 0:
            raise dev.RunError("fault runner cannot replace an existing real-time deadline")
        self.previous_handler = signal.getsignal(signal.SIGALRM)
        self.deadline = time.monotonic() + seconds
        self.closed = False

        def expired(_number, _frame):
            raise dev.RunError(f"fault case exceeded its {seconds}-second active deadline")

        signal.signal(signal.SIGALRM, expired)
        signal.setitimer(signal.ITIMER_REAL, seconds)

    def close(self):
        if not self.closed:
            signal.setitimer(signal.ITIMER_REAL, 0)
            signal.signal(signal.SIGALRM, self.previous_handler)
            self.closed = True


class DriverControl:
    def __init__(self, artifact):
        self.parent, self.child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
        self.artifact = artifact
        self.events = []

    def wait(self, expected, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.parent.settimeout(max(0.001, deadline - time.monotonic()))
            packet = self.parent.recv(512)
            if not packet:
                raise dev.RunError("native driver disconnected before " + expected)
            text = packet.decode("ascii")
            self.events.append({"monotonic": time.monotonic(), "packet": text})
            dev.write_json(self.artifact, self.events)
            if text == "STAGE " + expected:
                return
        raise dev.RunError("native driver did not reach " + expected)

    def proceed(self):
        self.parent.sendall(b"CONTINUE")

    def close(self):
        self.parent.close()
        self.child.close()


def verify_executed_binary(process, declared_path, expected_digest, evidence):
    """Bind a manifest digest to the executable inode actually mapped by a child.

    A build can replace the pathname between preflight and exec. Reading the
    procfs link at a controlled barrier checks that child's executable instead.
    """
    executable = Path(f"/proc/{process.pid}/exe")
    evidence.update(pid=process.pid, declared_path=str(declared_path),
                    declared_sha256=expected_digest, observed_path=os.readlink(executable),
                    observed_sha256=dev.binary_digest(executable),
                    observed_monotonic=time.monotonic())
    if evidence["observed_sha256"] != expected_digest:
        raise dev.RunError(f"executed binary differs from preflight: {declared_path}; "
                           f"expected {expected_digest}, observed {evidence['observed_sha256']}")


def validate_result(run_dir, case):
    if case == "ack_hwm_withheld":
        text = (run_dir / "driver.log").read_text(errors="replace")
        # Poll's timeout branch returns before its later shortfall diagnostic.
        # Require this exact real timeout, not any failure/shortfall marker.
        failures = re.findall(r"\[Publisher ACK Timeout\]: Waited (\d+) seconds for ACKs, "
            r"normalized_received=(\d+) raw_received=(\d+) out of (\d+) \(timeout=(\d+)s\)", text)
        if len(failures) != 1 or tuple(map(int, failures[0])) != (2, 0, 0, 2, 2):
            raise dev.RunError("withheld HWM did not produce the exact two-second ACK timeout")
        if "[ACK_VERIFY]" in text or "[ORDERED_DELIVERY_AUDIT] status=passed" in text:
            raise dev.RunError("client falsely completed while the authoritative HWM was withheld")
        return {"expected_exit_code": 1, "ack_timeout_seconds": 2,
                "scope": "real publisher Poll times out with the authoritative two-message HWM withheld; no successful delivery audit"}
    if case == "session_reopen_resubmit":
        text = (run_dir / "driver.log").read_text(errors="replace")
        audits = re.findall(r"\[ORDERED_DELIVERY_AUDIT\] status=passed messages=(\d+) expected=(\d+) "
            r"payload_bytes=(\d+) duplicates=(\d+) parse_errors=(\d+) export_gaps=(\d+) indexed_payload=1\b", text)
        if len(audits) != 1 or tuple(map(int, audits[0])) != (4, 4, 16384, 0, 0, 0):
            raise dev.RunError("reopened publisher did not deliver exactly four original indexed application messages")
        resubmits = re.findall(r"\[SESSION_REOPEN_RESUBMIT\] old_epoch=1 new_epoch=2 committed_batch_seq=\d+ "
                              r"suffix_batches=(\d+) requeued_pool_batches=(\d+) direct_resubmit_batches=(\d+)", text)
        if (len(resubmits) != 1 or not 0 < int(resubmits[0][0]) <= 4 or
            int(resubmits[0][1]) + int(resubmits[0][2]) != int(resubmits[0][0]) or
            text.count("[SESSION_FENCED_OBSERVED]") != 1):
            raise dev.RunError("publisher did not perform exactly one complete epoch1-to-2 suffix resubmission")
        acks = re.findall(r"\[ACK_VERIFY\] normalized_received=(\d+) raw_received=(\d+) target=(\d+) 100%", text)
        if len(acks) != 1 or int(acks[0][0]) != 4 or int(acks[0][2]) != 4:
            raise dev.RunError("reopened publisher did not complete its authoritative four-message target")
        if re.search(r"retransmit_attempts=[1-9]|\[ORDERED_DELIVERY_AUDIT\] status=failed", text):
            raise dev.RunError("session resubmission retried unexpectedly or failed audit")
        return {"audited_messages": 4, "payload_bytes": 16384, "session_rollovers": 1,
                "scope": "real publisher recovers an empty fenced prefix by reopening epoch2 and resubmitting the original indexed application suffix exactly once"}
    if case == "ack_publication_lag":
        text = (run_dir / "driver.log").read_text(errors="replace")
        audits = re.findall(r"\[ORDERED_DELIVERY_AUDIT\] status=passed messages=(\d+) expected=(\d+) "
            r"payload_bytes=(\d+) duplicates=(\d+) parse_errors=(\d+) export_gaps=(\d+) indexed_payload=1\b", text)
        if len(audits) != 1 or tuple(map(int, audits[0])) != (2, 2, 8192, 0, 0, 0):
            raise dev.RunError("real publisher/subscriber did not audit exactly two indexed messages")
        ack = re.findall(r"\[ACK_VERIFY\] normalized_received=(\d+) raw_received=(\d+) target=(\d+) 100%", text)
        if len(ack) != 1 or int(ack[0][0]) != 2 or int(ack[0][2]) != 2 or int(ack[0][1]) >= 2:
            raise dev.RunError("publisher Poll did not record the controlled authoritative/raw ACK split")
        if re.search(r"retransmit_attempts=[1-9]|session_fenced_observed=[1-9]|status=failed", text):
            raise dev.RunError("ACK publication case retried, fenced, or failed")
        return {"audited_messages": 2, "payload_bytes": 8192,
                "scope": "real publisher Poll snapshot with authoritative HWM complete while raw count lags; eventual ledger retirement and indexed delivery"}
    text = (run_dir / "driver.log").read_text(errors="replace")
    expected = (f"[FAULT_RESULT] status=passed case={case} messages=0 exact_prefix=1 false_ack=0 peers_closed=1"
                if case.startswith("shutdown_") else
                f"[FAULT_RESULT] status=passed case={case} messages=6 payload_bytes=24576 "
                "exact_prefix=1 false_ack=0 recovery_control=1")
    if case in SESSION_CASES:
        expected = (f"[FAULT_RESULT] status=passed case={case} messages={SESSION_CASES[case]} "
                    "exact_prefix=1 false_ack=0 fenced_open=1 recovery_control=1")
    if case == "shutdown_replication_token":
        expected = (f"[FAULT_RESULT] status=passed case={case} messages=2 goi=0 sink=memory-copy "
                    "ack_claim=replicated_ack_emulated missing_token_shutdown=1 false_ack=0 false_tail_completion=0")
    if case == "session_capacity":
        expected = (f"[FAULT_RESULT] status=passed case={case} messages=4 sessions=4096 distinct_keys=4096 "
                    "last_slot_contenders=4 overflow_rejected=1 committed_reopen=1 exact_replay=1 false_ack=0")
    if case == "goi_capacity":
        expected = (f"[FAULT_RESULT] status=passed case={case} messages=4 exact_prefix=1 false_ack=0 "
                    "admission_rejected=1 replay=1")
    if case in ("blog_capacity", "rollover_retention"):
        counts = (64320, 263454720, 1, 2) if case == "blog_capacity" else (163680, 670433280, 3, 5)
        expected = (f"[FAULT_RESULT] status=passed case={case} messages={counts[0]} payload_bytes={counts[1]} "
                    f"exact_prefix=1 false_ack=0 retained_payload=1 segments={counts[2]} pbr_laps={counts[3]}")
    if text.count(expected) != 1 or "[FAULT_RESULT] status=failed" in text:
        raise dev.RunError("native production oracle did not pass exactly once; see driver.log")
    if case == "shutdown_replication_token":
        return {"marker": expected, "scope": "two ordered payloads remain intact while absent tail token keeps ACK2 and completion at zero; memory-copy is not media durability"}
    return {"marker": expected, "scope": "exact native GOI/CV/session prefix, ACK boundary, indexed payload, and recovery control"
            if not case.startswith("shutdown_") else "zero committed prefix, peer closure, normal broker exit"}


def arm_broker_hooks(control, case):
    if case == "session_reopen_resubmit":
        return {"old_writer": control.arm("ingress.after_blog_reserve", epoch=1, batch=0),
                "expiry": control.arm("classification.before_expiry_sweep", epoch=1, batch=0, value=1000000000000)}
    if case == "goi_capacity":
        return {"capacity": control.arm("capacity.commit_rejected")}
    if case in ("blog_capacity", "rollover_retention"):
        armed = {"export": control.arm("export.after_select", batch=0)}
        if case == "blog_capacity":
            armed["allocate"] = control.arm("storage.before_rollover_allocate", batch=0, value=1)
        else:
            armed["writer"] = control.arm("ingress.after_blog_reserve", client=1003, epoch=1, batch=0)
        return armed
    if case in SESSION_CASES:
        empty = case == "fence_empty_prefix"
        return {"expiry": control.arm("classification.before_expiry_sweep", client=1001, epoch=1,
                                     batch=0 if empty else 3, value=1000000000000),
                "scanner": control.arm("scanner.after_collect", client=1002 if case == "fence_before_commit" else 1001,
                    epoch=1, batch=1 if case == "fence_before_commit" or empty else 4)}
    return {name: control.arm(name) for name in HOOKS.get(case, ())}


def session_schedule(control, driver, armed, case):
    driver.wait("session_prefix_verified")
    seal = control.arm("epoch.before_seal")
    seal_hit = control.hit(seal)
    driver.proceed()
    driver.wait("session_fault_sent")
    scanner_hit = control.hit(armed["scanner"])
    if scanner_hit["detail0"] != seal_hit["batch"]:
        raise dev.RunError("selected ingress did not enter the deliberately held epoch")
    control.release(armed["scanner"])
    control.release(seal)
    expiry_hit = control.hit(armed["expiry"])
    if expiry_hit["detail0"] != (1 if case == "fence_before_commit" else 0):
        raise dev.RunError("classification did not reach the required ready/gap schedule")
    control.release(armed["expiry"])
    return {"seal": seal_hit, "scanner": scanner_hit, "expiry": expiry_hit,
            "expiry_advance_ns": 1000000000000}


def ack_schedule(control, ready=True):
    if ready:
        control.ready(20)
    armed = {name: control.arm(name, batch=2) for name in
             ("ack.after_authoritative_hwm", "poll.after_ack_snapshot", "ack.after_retirement")}
    control.start()
    hwm = control.hit(armed["ack.after_authoritative_hwm"])
    snapshot = control.hit(armed["poll.after_ack_snapshot"])
    if hwm["detail0"] >= 2 or hwm["detail1"] <= 0 or snapshot["detail0"] >= 2 or snapshot["detail1"] != 2:
        raise dev.RunError("ACK barriers did not capture raw/ledger lag behind the authoritative HWM")
    # Hold Poll's success snapshot so the client cannot begin destruction while
    # the ACK worker is still paused. Its captured raw value remains diagnostic.
    control.release(armed["ack.after_authoritative_hwm"])
    retirement = control.hit(armed["ack.after_retirement"])
    if retirement["detail0"] != 0 or retirement["detail1"] != 0:
        raise dev.RunError("ACK retirement barrier retained completed messages")
    control.release(armed["ack.after_retirement"])
    control.release(armed["poll.after_ack_snapshot"])
    return {"authoritative_hwm": hwm, "poll_snapshot": snapshot, "retirement": retirement}


def withheld_ack_schedule(control, observe=None):
    control.ready(20)
    if observe:
        observe()
    identity = control.arm("ack.before_authoritative_hwm", batch=2)
    control.start()
    hit = control.hit(identity)
    if hit["detail0"] != 0 or hit["detail1"] != 0:
        raise dev.RunError("negative ACK control did not stop the first authoritative publication")
    # Intentionally remain held: the actual two-second Poll timeout must fail.
    return {"withheld_hwm": hit, "expected_client_exit": 1}


def reopen_schedule(broker_control, client_control, armed, observe=None):
    writer = broker_control.hit(armed["old_writer"], timeout=20)
    expiry = broker_control.hit(armed["expiry"], timeout=20)
    if expiry["client"] != writer["client"] or expiry["detail0"] != 0 or expiry["detail1"] == 0:
        raise dev.RunError("real client did not expose the selected empty-prefix gap")
    broker_control.release(armed["expiry"])
    client_control.ready(20)
    identity = client_control.arm("session.before_reopen", client=writer["client"], epoch=1, batch=2)
    client_control.start()
    reopen = client_control.hit(identity)
    if reopen["detail0"] != 0 or reopen["detail1"] != 0:
        raise dev.RunError("client reopen hook did not observe the expected empty committed prefix")
    if observe:
        observe()
    broker_control.release(armed["old_writer"])
    client_control.release(identity)
    return {"old_writer": writer, "expiry": expiry, "reopen": reopen, "expiry_advance_ns": 1000000000000}


def replication_schedule(controls, arms, processes, driver):
    driver.wait("replication_batch_sent")
    predecessor = controls[1].hit(arms[1]["replication"])
    successor = controls[2].hit(arms[2]["replication"])
    for hit in (predecessor, successor):
        if (hit["detail0"], hit["detail1"]) != (0, 1):
            raise dev.RunError("replication barrier did not observe GOI0 at token1")
    driver.proceed()
    driver.wait("replication_wait_verified")
    started = time.monotonic()
    os.killpg(processes[2].pid, signal.SIGTERM)
    if processes[2].wait(timeout=15) != 0:
        raise dev.RunError("replication successor did not exit normally from the real predecessor wait")
    successor_shutdown = time.monotonic() - started
    driver.proceed()
    driver.wait("replication_successor_stopped_verified")
    controls[1].release(arms[1]["replication"])
    remaining_deadline = time.monotonic() + 15
    for process in reversed(processes[:2]):
        os.killpg(process.pid, signal.SIGTERM)
    for process in processes[:2]:
        if process.wait(timeout=max(0, remaining_deadline - time.monotonic())) != 0:
            raise dev.RunError("remaining replication broker did not exit normally")
    driver.proceed()
    return {"predecessor": predecessor, "successor": successor,
            "successor_shutdown_seconds": successor_shutdown}


def storage_schedule(control, driver, armed, case, geometry):
    if case == "goi_capacity":
        driver.wait("capacity_fault_sent")
        hit = control.hit(armed["capacity"])
        if (hit["detail0"], hit["detail1"]) != (2, 4):
            raise dev.RunError("GOI failure changed reservation counters or targeted the wrong prefix")
        control.release(armed["capacity"])
        driver.proceed()
        return {"capacity": hit, "logical_goi_capacity": 2}
    export = control.hit(armed["export"])
    if export["batch"] != 0 or export["detail1"] != 480 * geometry["message_wire_bytes"]:
        raise dev.RunError("stalled reader did not retain the first native large batch")
    if case == "blog_capacity":
        driver.wait("allocation_fault_sent", timeout=30)
        allocation = control.hit(armed["allocate"])
        if allocation["detail1"] <= allocation["detail0"]:
            raise dev.RunError("rollover allocation barrier did not identify the old cursor")
        control.release(armed["allocate"])
        driver.wait("storage_rejection_verified")
        control.release(armed["export"])
        driver.proceed()
        return {"stalled_export": export, "allocation_rejected": allocation}
    driver.wait("writer_header_sent", timeout=30)
    writer = control.hit(armed["writer"])
    if writer["detail1"] != 480 * geometry["message_wire_bytes"]:
        raise dev.RunError("late writer did not reserve the exact native large batch")
    driver.proceed()  # Main publisher advances while the late receive remains paused.
    driver.wait("storage_rollovers_done", timeout=30)
    control.release(armed["writer"])
    driver.proceed()
    driver.wait("late_writer_done")
    control.release(armed["export"])
    driver.proceed()
    return {"stalled_export": export, "late_writer": writer}


def require_fault_build(build):
    cache = dev.read_optional(build / "CMakeCache.txt") or ""
    if not re.search(r"^EMBARCADERO_ENABLE_FAULT_INJECTION:BOOL=ON$", cache, re.MULTILINE):
        raise dev.RunError("configure a separate build with -DEMBARCADERO_ENABLE_FAULT_INJECTION=ON")


def memory_preflight(hardware, brokers=1):
    result = dev.memory_preflight(hardware, brokers)
    if shutil.disk_usage("/dev/shm").free < dev.REGION_BYTES + 8 * dev.GIB:
        raise dev.RunError("fault profile requires 72 GiB free in /dev/shm")
    if hardware["nodes"]["0"]["free_bytes"] < 6 * dev.GIB:
        raise dev.RunError("fault profile requires 6 GiB client-node memory headroom")
    return result


def observe_thread_syscall(pid, tid, numbers, *, different_futex=None, futex_timed=None, timeout=1.0):
    """Observe the reached hook's Linux x86-64 thread, never just RELEASED."""
    if os.uname().machine != "x86_64":
        raise dev.RunError("native wait oracle requires Linux x86-64 syscall numbers")
    if tid <= 0:
        raise dev.RunError("missing native fault thread identity")
    path = Path(f"/proc/{pid}/task/{tid}/syscall")
    deadline = time.monotonic() + timeout
    while True:
        try:
            raw = path.read_text().strip()
        except OSError as error:
            raise dev.RunError(f"cannot observe fault thread syscall: {error}") from error
        fields = raw.split()
        if len(fields) == 9:
            try:
                number = int(fields[0], 0)
                args = [int(value, 0) for value in fields[1:7]]
            except ValueError as error:
                raise dev.RunError("malformed native syscall snapshot") from error
            if number in numbers and (different_futex is None or args[0] != different_futex):
                # For futex observations require condition-variable WAIT_BITSET,
                # not transient reacquisition of the controller's mutex (WAIT).
                if number != 202 or ((args[1] & 0x7f) == 9 and
                        (futex_timed is None or bool(args[3]) == futex_timed)):
                    return {"pid": pid, "tid": tid, "syscall": number, "arguments": args,
                            "raw": raw, "monotonic": time.monotonic()}
        if time.monotonic() >= deadline:
            raise dev.RunError(f"fault thread {tid} did not enter expected syscall {sorted(numbers)}")
        time.sleep(0.0005)


def observe_hit(controller, identity, case):
    hit = controller.hit(identity)
    if case in ("fragmented_open", "shutdown_partial_control") and (hit["detail0"], hit["detail1"]) != (1, 4):
        raise dev.RunError("fragment hook did not observe the one-byte control prefix")
    if case == "shutdown_partial_handshake" and (hit["detail0"], hit["detail1"]) != (8, 64):
        raise dev.RunError("handshake hook did not observe the exact truncated native handshake")
    if case == "shutdown_partial_body" and not (hit["detail0"] == 1 and hit["detail1"] > 1):
        raise dev.RunError("payload hook did not observe a one-byte incomplete body")
    if case == "shutdown_queue" and (hit["detail0"], hit["detail1"]) != (2, 2):
        raise dev.RunError("queue hook did not observe a full two-entry queue")
    return hit


def run_case(args, case, hardware):
    replication = case == "shutdown_replication_token"
    brokers = 3 if replication else 1
    run_dir = Path(tempfile.mkdtemp(prefix=f"embarcadero-fault-{os.getuid()}-", dir=args.run_root)).resolve()
    print(f"{case}: {run_dir}", flush=True)
    shm_path = Path("/dev/shm") / run_dir.name
    shm_name = "/" + run_dir.name
    manifest = {"schema": 1, "profile": "production-fault-dram", "case": case, "status": "preflight",
                "backend": "dram-emulation", "cxl_evidence": False, "performance_evidence": False,
                "region_bytes": dev.REGION_BYTES, "segment_bytes": dev.SEGMENT_BYTES,
                "application_payload_upper_bound_bytes": PAYLOAD_UPPER_BOUNDS[case],
                "payload_bound_scope": "publisher ingress application payload attempts, including rejected and resubmitted bytes; excludes subscriber replay and protocol headers",
                "brokers": brokers,
                "order": 5, "ack": 2 if replication else 1, "replication_factor": 3 if replication else 0,
                "persistence": "none", "replication_sink": "memory-copy",
                "expected_hooks": HOOKS.get(case, ()), "shared_memory": str(shm_path),
                "started_wall": time.time(), "started_monotonic": time.monotonic(),
                "hardware": hardware, "shutdown_deadline_seconds": 15,
                "active_deadline_seconds": ACTIVE_TIMEOUT_SECONDS,
                "limitations": ["local DRAM correctness experiment; no CXL, persistence, or performance claim",
                                "fixed ports serialize per user; unrelated launchers do not honor this lock"]}
    owned = control = driver_control = client_control = active_timer = None
    controls = []
    processes = []
    arms = []
    identity = None
    markers = []
    passed = False
    if case == "session_reopen_resubmit":
        manifest["unique_application_payload_bytes"] = 16384
        manifest["transmitted_payload_bound_reason"] = "four unique messages; exactly one bounded suffix resubmission, zero timer retransmissions; 64 KiB conservative cap"
    try:
        build = args.build_dir.resolve()
        require_fault_build(build)
        broker = build / "bin/embarlet"
        driver = build / "bin" / ("throughput_test" if case in REAL_CLIENT_CASES else "production_fault_driver")
        for binary in (broker, driver):
            if not os.access(binary, os.X_OK):
                raise dev.RunError("missing executable: " + str(binary))
        numactl = shutil.which("numactl")
        if not numactl:
            raise dev.RunError("numactl is required")
        manifest["memory"] = memory_preflight(hardware, brokers)
        dev.check_ports([dev.CONTROL_PORT] + list(range(dev.DATA_PORT, dev.DATA_PORT + brokers)))
        config = dev.effective_config(brokers)
        geometry = None
        if case == "shutdown_queue":
            config["embarcadero"]["network"]["io_threads"] = 1
        if case == "ack_hwm_withheld":
            config["client"]["runtime"]["ack_timeout_sec_throughput"] = 2
        if case == "session_reopen_resubmit":
            config["embarcadero"]["storage"]["batch_size"] = 8192
            config["client"]["publisher"]["batch_size_kb"] = 8
            config["client"]["publisher"]["threads_per_broker"] = 2
        if case in ("blog_capacity", "rollover_retention"):
            query = subprocess.run([str(driver), "--print-fault-geometry"], capture_output=True,
                                   text=True, timeout=5, check=True)
            geometry = json.loads(query.stdout)
            if any(type(geometry.get(key)) is not int or geometry[key] <= 0 for key in
                   ("batch_header_bytes", "message_payload_bytes", "message_wire_bytes")):
                raise dev.RunError("invalid native fault geometry")
            if geometry["message_payload_bytes"] != 4096 or 480 * geometry["message_wire_bytes"] > 2 * dev.MIB:
                raise dev.RunError("native storage test geometry exceeds the bounded profile")
            config["embarcadero"]["storage"]["batch_headers_size"] = 64 * geometry["batch_header_bytes"]
            manifest["native_fault_geometry"] = geometry
        config_path = run_dir / "effective-config.yaml"
        dev.write_json(config_path, config)
        env, selected, removed = dev.child_environment(shm_name, brokers)
        if replication:
            env["EMBARCADERO_REPLICATION_FACTOR"] = selected["EMBARCADERO_REPLICATION_FACTOR"] = "3"
        if case == "ack_hwm_withheld":
            env["EMBARCADERO_ACK_TIMEOUT_SEC"] = selected["EMBARCADERO_ACK_TIMEOUT_SEC"] = "2"
        manifest.update(environment=selected, ignored_environment_names=removed)
        manifest["layout"] = dev.layout_preflight(broker, config_path, env)
        manifest["binary_sha256"] = {str(path): dev.binary_digest(path) for path in (broker, driver)}
        manifest["config_sha256"] = dev.binary_digest(config_path)
        manifest["build"] = dev.build_provenance(build)
        manifest["fault_injection_build"] = True
        manifest["source_revision"] = subprocess.run(["git", "rev-parse", "HEAD"], cwd=ROOT,
            capture_output=True, text=True, timeout=5).stdout.strip()
        manifest["source_status"] = subprocess.run(["git", "status", "--porcelain"], cwd=ROOT,
            capture_output=True, text=True, timeout=5).stdout.strip()
        manifest["source_provenance"] = "binary/config hashes identify executed artifacts; a dirty source tree requires a separately archived source snapshot for reproduction"
        def binding(node):
            return [numactl, "--physcpubind=" + ",".join(map(str, hardware["nodes"][str(node)]["cpus"])),
                    f"--membind={node}"]
        broker_commands = [binding(1) + [str(broker), "--emul", "--config", str(config_path)] +
            (["--head"] if broker_id == 0 else ["--follower", f"127.0.0.1:{dev.CONTROL_PORT}"])
            for broker_id in range(brokers)]
        driver_command = binding(0) + [str(driver), "--config", str(config_path), "--shm-name", shm_name,
                                       "--case", case]
        if case in REAL_CLIENT_CASES:
            driver_command = binding(0) + [str(driver), "--config", str(config_path), "--head_addr", "127.0.0.1",
                "-t", "1", "-o", "5", "-a", "1", "-r", "0", "-n", "2" if case == "session_reopen_resubmit" else "1",
                "-m", "4096", "-s", "16384" if case == "session_reopen_resubmit" else "8192",
                "--sequencer", "EMBARCADERO"]
        manifest.update(broker_commands=broker_commands, driver_command=driver_command)
        if args.dry_run:
            manifest["status"] = "dry-run"
            passed = True
        else:
            fd = os.open(shm_path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            info = os.fstat(fd)
            identity = info.st_dev, info.st_ino
            os.close(fd)
            owned = dev.OwnedProcesses(run_dir, env, 15)
            owned.start_wall = time.time()
            startup_deadline = time.monotonic() + args.startup_timeout
            manifest["pids"], manifest["fault_environment"], manifest["broker_placement"] = {}, {}, {}
            manifest["executed_binaries"] = {}

            def capture_binary_identity(name, child, binary):
                evidence = manifest["executed_binaries"].setdefault(name, {})
                verify_executed_binary(child, binary, manifest["binary_sha256"][str(binary)], evidence)

            for broker_id, command in enumerate(broker_commands):
                broker_control = FaultControl(run_dir / f"broker-{broker_id}-fault-events.json")
                controls.append(broker_control)
                broker_env = broker_control.environment()
                if case == "shutdown_queue":
                    broker_env["EMBARCADERO_FAULT_QUEUE_CAPACITY"] = "2"
                if case in SESSION_CASES:
                    broker_env["EMBARCADERO_FAULT_DISABLE_FAST_SEAL"] = "1"
                if case == "goi_capacity":
                    broker_env["EMBARCADERO_FAULT_GOI_CAPACITY"] = "2"
                name = f"broker-{broker_id}"
                manifest["fault_environment"][name] = broker_env
                process = owned.start(name, command, environment=broker_env, pass_fds=broker_control.pass_fds)
                processes.append(process)
                broker_control.child_started()
                markers.append(Path(f"/tmp/embarlet_{process.pid}_ready"))
                manifest["pids"][name] = process.pid
                dev.write_json(run_dir / "manifest.json", manifest)
                # Worker startup initializes the controller before any topic work.
                broker_control.ready(max(0.001, startup_deadline - time.monotonic()))
                if replication and broker_id:
                    hook = "replication.before_token_advance" if broker_id == 1 else "replication.wait_predecessor"
                    armed = {"replication": broker_control.arm(hook, client=1001, epoch=1, batch=0)}
                else:
                    armed = arm_broker_hooks(broker_control, case)
                arms.append(armed)
                broker_control.start()
                if case == "shutdown_queue":
                    manifest["worker_pause"] = broker_control.hit(armed["queue.worker_paused"])
                dev.wait_ready(process, owned, startup_deadline)
                capture_binary_identity(name, process, broker)
                manifest["broker_placement"][name] = dev.placement_snapshot(process, hardware["nodes"]["1"]["cpus"],
                    1, run_dir, name, shm_name if broker_id == 0 else None)
                if not manifest["broker_placement"][name].get("thread_cpu_masks"):
                    raise dev.RunError(name + " startup placement was not observed")
            control, process, armed = controls[0], processes[0], arms[0]
            active_timer = ActiveDeadline(ACTIVE_TIMEOUT_SECONDS)
            active_deadline = active_timer.deadline
            manifest["active_started_monotonic"] = active_deadline - ACTIVE_TIMEOUT_SECONDS
            if case in REAL_CLIENT_CASES:
                client_control = FaultControl(run_dir / "client-fault-events.json")
                manifest["client_fault_environment"] = client_control.environment()
                workload = owned.start("driver", driver_command, environment=client_control.environment(),
                                       pass_fds=client_control.pass_fds)
                client_control.child_started()
            else:
                driver_control = DriverControl(run_dir / "driver-events.json")
                driver_command += ["--controller-fd", str(driver_control.child.fileno())]
                workload = owned.start("driver", driver_command, pass_fds=(driver_control.child.fileno(),))
                driver_control.child.close()
            manifest["pids"]["driver"] = workload.pid
            manifest["status"] = "running"
            dev.write_json(run_dir / "manifest.json", manifest)
            def capture_driver_placement():
                capture_binary_identity("driver", workload, driver)
                manifest["driver_placement"] = dev.placement_snapshot(workload, hardware["nodes"]["0"]["cpus"],
                    0, run_dir, "driver")
            if case == "ack_publication_lag":
                # Record placement while the first ACK hook is waiting for START.
                client_control.ready(20)
                capture_driver_placement()
                manifest["fault_hits"] = ack_schedule(client_control, ready=False)
            elif case == "ack_hwm_withheld":
                manifest["fault_hits"] = withheld_ack_schedule(client_control, capture_driver_placement)
            elif case == "session_reopen_resubmit":
                manifest["fault_hits"] = reopen_schedule(control, client_control, armed, capture_driver_placement)
            else:
                driver_control.wait("driver_ready")
                capture_driver_placement()
                if not manifest["driver_placement"].get("thread_cpu_masks"):
                    raise dev.RunError("native driver placement was not observed at its startup barrier")
                driver_control.proceed()
            if not manifest["driver_placement"].get("thread_cpu_masks"):
                raise dev.RunError("driver placement was not observed at its controlled barrier")
            if case in STORAGE_CASES:
                manifest["fault_hits"] = storage_schedule(control, driver_control, armed, case, geometry)
            elif replication:
                manifest["fault_hits"] = replication_schedule(controls, arms, processes, driver_control)
            elif case in SESSION_CASES:
                manifest["fault_hits"] = session_schedule(control, driver_control, armed, case)
            elif case == "repeated_rejected_connections":
                driver_control.wait("fd_baseline")
                before = sorted(os.listdir(f"/proc/{process.pid}/fd"))
                driver_control.proceed()
                driver_control.wait("fd_completed")
                deadline = time.monotonic() + 2
                while True:
                    after = sorted(os.listdir(f"/proc/{process.pid}/fd"))
                    if len(after) <= len(before):
                        break
                    if time.monotonic() >= deadline:
                        raise dev.RunError("broker descriptors grew after 128 rejected connections")
                    time.sleep(0.02)
                manifest["descriptor_inventory"] = {"before": before, "after": after,
                    "rejected_connections": 128, "no_count_growth": True}
                driver_control.proceed()
            elif case == "fragmented_open":
                driver_control.wait("fragment_sent")
                hook_id = armed["ingress.session_prefix.partial"]
                manifest["fault_hit"] = observe_hit(control, hook_id, case)
                control.release(hook_id)
                driver_control.proceed()
            elif case.startswith("shutdown_"):
                driver_control.wait("shutdown_sockets_open")
                name = HOOKS[case][-1]
                manifest["fault_hit"] = observe_hit(control, armed[name], case)
                if case in ("shutdown_queue", "shutdown_partial_control", "shutdown_partial_body"):
                    tid = manifest["fault_hit"]["tid"]
                    before = observe_thread_syscall(process.pid, tid, {202}, futex_timed=True)
                    manifest["native_wait_evidence"] = {"paused_hook": before}
                    control.release(armed[name])
                    expected = {"shutdown_queue": {202}, "shutdown_partial_control": {35, 230},
                                "shutdown_partial_body": {45}}[case]
                    # The queue consumer remains paused. A different condition-
                    # variable futex with no timeout distinguishes the actual queue
                    # wait from both group words of the controller timed wait.
                    after = observe_thread_syscall(process.pid, tid, expected,
                        different_futex=before["arguments"][0] if case == "shutdown_queue" else None,
                        futex_timed=False if case == "shutdown_queue" else None)
                    manifest["native_wait_evidence"]["production_wait"] = after
                manifest["shutdown_signal_monotonic"] = time.monotonic()
                os.killpg(process.pid, signal.SIGTERM)
            deadline = active_deadline
            while workload.poll() is None:
                if not case.startswith("shutdown_"):
                    owned.check_brokers()
                if time.monotonic() >= deadline:
                    raise dev.RunError("native protocol driver exceeded its bounded deadline")
                if sum(path.stat().st_size for path in run_dir.glob("*.log")) > 64 * dev.MIB:
                    raise dev.RunError("fault case exceeded its 64 MiB log budget")
                time.sleep(0.02)
            expected_client_exit = 1 if case == "ack_hwm_withheld" else 0
            if workload.returncode != expected_client_exit:
                raise dev.RunError(f"native driver exited with {workload.returncode}; see driver.log")
            manifest["oracle"] = validate_result(run_dir, case)
            if case.startswith("shutdown_") and not replication:
                process.wait(timeout=max(0, 15 - (time.monotonic() - manifest["shutdown_signal_monotonic"])))
                manifest["shutdown_elapsed_seconds"] = time.monotonic() - manifest["shutdown_signal_monotonic"]
            elif not replication:
                owned.check_brokers()
            passed = True
    except Exception as error:
        manifest["error"] = f"{type(error).__name__}: {error}"
        print(f"{case}: {manifest['error']}", file=sys.stderr)
    finally:
        # Cancel before cleanup: controller cancellation and child teardown have
        # their own bounds and must not be interrupted by the workload timer.
        if active_timer:
            active_timer.close()
            manifest["active_ended_monotonic"] = time.monotonic()
        cleanup_handlers = {number: signal.signal(number, signal.SIG_IGN)
                            for number in (signal.SIGINT, signal.SIGTERM)}
        if owned:
            # Establish normal teardown intent before cancelling test barriers.
            # Do not wait here: a queue hook can hold a lock needed by shutdown.
            owned.request_stop()
        if client_control:
            client_control.close()
        for broker_control in controls:
            broker_control.close()  # Cancel hooks before owned cleanup, including failure paths.
        if driver_control:
            driver_control.close()
        if owned:
            try:
                owned.close()
                manifest["forced_shutdown"] = owned.forced
                manifest["exit_codes"] = {name: child.returncode for name, child in owned.children}
                if owned.forced or any(child.returncode != (1 if name == "driver" and case == "ack_hwm_withheld" else 0)
                                       for name, child in owned.children):
                    passed = False
                    manifest["cleanup_error"] = "forced termination or nonzero child exit"
            except Exception as error:
                passed = False
                manifest["cleanup_error"] = str(error)
        if identity and (owned is None or all(child.poll() is not None for _, child in owned.children)):
            try:
                info = shm_path.lstat()
                if (info.st_dev, info.st_ino) == identity:
                    shm_path.unlink()
            except FileNotFoundError:
                pass
        for marker in markers:
            try:
                info = marker.lstat()
                if stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_mtime >= owned.start_wall:
                    marker.unlink()
            except FileNotFoundError:
                pass
        manifest["shared_memory_removed"] = not os.path.lexists(shm_path)
        if not manifest["shared_memory_removed"]:
            passed = False
        manifest["status"] = "dry-run" if args.dry_run and passed else "passed" if passed else "failed"
        manifest["ended_monotonic"] = time.monotonic()
        dev.write_json(run_dir / "manifest.json", manifest)
        for number, handler in cleanup_handlers.items():
            signal.signal(number, handler)
    return passed, run_dir / "manifest.json"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build/debug-faults")
    parser.add_argument("--run-root", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--case", choices=("all",) + CASES, default="all")
    parser.add_argument("--startup-timeout", type=float, default=120)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not 1 <= args.startup_timeout <= 120:
        parser.error("startup timeout must be in 1..120 seconds")
    old_handlers = {}
    try:
        for number in (signal.SIGINT, signal.SIGTERM):
            old_handlers[number] = signal.signal(number, dev.interrupted)
        hardware = dev.topology()
        with dev.ClusterLock():
            for case in CASES if args.case == "all" else (args.case,):
                passed, _ = run_case(args, case, hardware)
                if not passed:
                    return 1
        return 0
    except (dev.RunError, OSError) as error:
        print("production faults: " + str(error), file=sys.stderr)
        return 1
    finally:
        for number, handler in old_handlers.items():
            signal.signal(number, handler)


if __name__ == "__main__":
    sys.exit(main())
