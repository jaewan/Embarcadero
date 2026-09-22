"""Fault harness unit checks. Fake processes never allocate the production region."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

import dev_cluster as dev

spec = importlib.util.spec_from_file_location("run_production_faults", dev.ROOT / "test/integration/run_production_faults.py")
faults = importlib.util.module_from_spec(spec)
spec.loader.exec_module(faults)


class ProductionFaultHarnessTests(unittest.TestCase):
    def test_inherited_fd_and_environment_are_child_scoped(self):
        with tempfile.TemporaryDirectory() as directory:
            # Keep both endpoints explicitly owned through process creation.
            parent, child = socket.socketpair(socket.AF_UNIX, socket.SOCK_SEQPACKET)
            try:
                owned = dev.OwnedProcesses(Path(directory), {"PATH": os.environ["PATH"], "SHARED": "unchanged"}, 1)
                process = owned.start("controlled", [sys.executable, "-c",
                    "import os,socket; s=socket.socket(fileno=int(os.environ['CONTROL_FD'])); "
                    "s.send(os.environ['SHARED'].encode()); s.close()"],
                    environment={"CONTROL_FD": str(child.fileno()), "SHARED": "child-only"}, pass_fds=(child.fileno(),))
                child.close()
                parent.settimeout(3)
                self.assertEqual(parent.recv(128), b"child-only")
                self.assertEqual(process.wait(timeout=3), 0)
                self.assertEqual(owned.env["SHARED"], "unchanged")
                self.assertNotIn("CONTROL_FD", owned.env)
                owned.close()
                self.assertFalse(owned.forced)
            finally:
                child.close()
                parent.close()

    def test_oracle_requires_exact_case_and_success_marker(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            good = "[FAULT_RESULT] status=passed case=incomplete_payload messages=6 payload_bytes=24576 exact_prefix=1 false_ack=0 recovery_control=1"
            for invalid in (good.replace("messages=6", "messages=5"), good.replace("false_ack=0", "false_ack=1"),
                            good.replace("incomplete_payload", "control"), good + "\n" + good,
                            good + "\n[FAULT_RESULT] status=failed reason=cleanup"):
                (base / "driver.log").write_text(invalid)
                with self.assertRaises(dev.RunError):
                    faults.validate_result(base, "incomplete_payload")
            (base / "driver.log").write_text(good)
            self.assertIn("exact native", faults.validate_result(base, "incomplete_payload")["scope"])

    def test_fault_build_and_barrier_evidence_are_mandatory(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            with self.assertRaises(dev.RunError):
                faults.require_fault_build(base)
            (base / "CMakeCache.txt").write_text("EMBARCADERO_ENABLE_FAULT_INJECTION:BOOL=OFF\n")
            with self.assertRaises(dev.RunError):
                faults.require_fault_build(base)
            (base / "CMakeCache.txt").write_text("EMBARCADERO_ENABLE_FAULT_INJECTION:BOOL=ON\n")
            faults.require_fault_build(base)
        controller = mock.Mock()
        controller.hit.return_value = {"detail0": 1, "detail1": 2}
        with self.assertRaises(dev.RunError):
            faults.observe_hit(controller, 1, "shutdown_queue")
        controller.hit.return_value = {"detail0": 2, "detail1": 2}
        self.assertEqual(faults.observe_hit(controller, 1, "shutdown_queue")["detail0"], 2)

    def test_ack_oracle_accepts_lag_only_with_complete_authoritative_and_delivery_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            valid = ("[ACK_VERIFY] normalized_received=2 raw_received=0 target=2 100%\n"
                     "[ORDERED_DELIVERY_AUDIT] status=passed messages=2 expected=2 payload_bytes=8192 "
                     "duplicates=0 parse_errors=0 export_gaps=0 indexed_payload=1\n")
            for invalid in (valid.replace("normalized_received=2", "normalized_received=1"),
                            valid.replace("messages=2", "messages=1"),
                            valid.replace("raw_received=0", "raw_received=2"),
                            valid + "retransmit_attempts=1\n"):
                (base / "driver.log").write_text(invalid)
                with self.assertRaises(dev.RunError):
                    faults.validate_result(base, "ack_publication_lag")
            (base / "driver.log").write_text(valid)
            self.assertEqual(faults.validate_result(base, "ack_publication_lag")["audited_messages"], 2)

    def test_negative_ack_control_requires_specific_failure_and_no_success(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            failure = ("[Publisher ACK Failure]: Did not receive ACKs for all messages. "
                       "normalized_received=0 raw_received=0 target=2 short=2\n")
            for invalid in ("segmentation fault", failure.replace("target=2", "target=1"),
                            failure + "[ACK_VERIFY] normalized_received=2\n",
                            failure + "[ORDERED_DELIVERY_AUDIT] status=passed\n"):
                (base / "driver.log").write_text(invalid)
                with self.assertRaises(dev.RunError):
                    faults.validate_result(base, "ack_hwm_withheld")
            (base / "driver.log").write_text(failure)
            self.assertEqual(faults.validate_result(base, "ack_hwm_withheld")["expected_exit_code"], 1)

    def test_real_reopen_requires_exact_original_payload_and_one_complete_suffix(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            valid = ("[SESSION_FENCED_OBSERVED] broker_id=0\n"
                     "[SESSION_REOPEN_RESUBMIT] old_epoch=1 new_epoch=2 committed_batch_seq=0 "
                     "suffix_batches=4 requeued_pool_batches=4 direct_resubmit_batches=0\n"
                     "[ACK_VERIFY] normalized_received=4 raw_received=3 target=4 100%\n"
                     "[ORDERED_DELIVERY_AUDIT] status=passed messages=4 expected=4 payload_bytes=16384 "
                     "duplicates=0 parse_errors=0 export_gaps=0 indexed_payload=1\n")
            for invalid in (valid.replace("requeued_pool_batches=4", "requeued_pool_batches=3"),
                            valid.replace("duplicates=0", "duplicates=1"),
                            valid + "[SESSION_FENCED_OBSERVED] broker_id=0\n"):
                (base / "driver.log").write_text(invalid)
                with self.assertRaises(dev.RunError):
                    faults.validate_result(base, "session_reopen_resubmit")
            (base / "driver.log").write_text(valid)
            self.assertEqual(faults.validate_result(base, "session_reopen_resubmit")["audited_messages"], 4)

    def test_ack_schedule_holds_poll_until_ledger_retirement(self):
        controller = mock.Mock()
        controller.arm.side_effect = [1, 2, 3]
        controller.hit.side_effect = [{"detail0": 0, "detail1": 8192},
                                      {"detail0": 0, "detail1": 2},
                                      {"detail0": 0, "detail1": 0}]
        faults.ack_schedule(controller)
        self.assertEqual(controller.release.call_args_list, [mock.call(1), mock.call(3), mock.call(2)])
        controller.reset_mock(side_effect=True)
        controller.arm.side_effect = [1, 2, 3]
        controller.hit.side_effect = [{"detail0": 0, "detail1": 8192},
                                      {"detail0": 0, "detail1": 2},
                                      {"detail0": 8192, "detail1": 1}]
        with self.assertRaisesRegex(dev.RunError, "retained"):
            faults.ack_schedule(controller)
        self.assertEqual(controller.release.call_args_list, [mock.call(1)])

    def test_session_schedule_rejects_selected_ingress_in_another_epoch(self):
        controller, driver = mock.Mock(), mock.Mock()
        controller.arm.return_value = 3
        controller.hit.side_effect = [{"batch": 10}, {"detail0": 11}]
        with self.assertRaisesRegex(dev.RunError, "held epoch"):
            faults.session_schedule(controller, driver, {"scanner": 1, "expiry": 2}, "fence_before_commit")
        controller.release.assert_not_called()

    def fake_build(self, base, failed_driver=False):
        build = base / "build"
        binaries = build / "bin"
        binaries.mkdir(parents=True)
        (build / "CMakeCache.txt").write_text("EMBARCADERO_ENABLE_FAULT_INJECTION:BOOL=ON\n")
        broker = binaries / "embarlet"
        broker.write_text("#!/usr/bin/env python3\n" + '''
import json,os,pathlib,signal,socket,sys,time
if '--print-layout' in sys.argv:
 print(json.dumps({'region_bytes':64*1024**3,'metadata_bytes':33*1024**3,'payload_bytes':31*1024**3,'segment_size':256*1024**2,'segment_count':124}))
 sys.exit(0)
assert '--emul' in sys.argv
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
control=socket.socket(fileno=int(os.environ['EMBARCADERO_FAULT_CONTROL_FD']))
control.send(('READY '+os.environ['EMBARCADERO_FAULT_CONTROL_TOKEN']).encode())
assert control.recv(512)==b'START'
control.send(b'STARTED')
pathlib.Path('/tmp/embarlet_%s_ready'%os.getpid()).write_text('ready')
while True: time.sleep(.1)
''')
        driver = binaries / "production_fault_driver"
        driver.write_text("#!/usr/bin/env python3\nimport socket,sys\ns=socket.socket(fileno=int(sys.argv[sys.argv.index('--controller-fd')+1]));s.send(b'STAGE driver_ready');assert s.recv(512)==b'CONTINUE'\n" + ("import sys;sys.exit(7)\n" if failed_driver else
            "print('[FAULT_RESULT] status=passed case=control messages=6 payload_bytes=24576 exact_prefix=1 false_ack=0 recovery_control=1')\n"))
        numactl = base / "numactl"
        numactl.write_text("#!/usr/bin/env python3\nimport os,sys\nos.execv(sys.argv[3],sys.argv[3:])\n")
        for path in (broker, driver, numactl):
            path.chmod(0o700)
        return build, numactl

    def fake_run(self, failed=False, dry=False):
        with tempfile.TemporaryDirectory() as directory, contextlib.ExitStack() as stack:
            base = Path(directory)
            build, numactl = self.fake_build(base, failed)
            private_tempfile = mock.Mock(wraps=tempfile)
            private_tempfile.gettempdir.return_value = str(base)
            stack.enter_context(mock.patch.object(dev, "tempfile", private_tempfile))
            stack.enter_context(mock.patch.object(dev, "topology", return_value={"nodes": {"0": {"cpus": [0]}, "1": {"cpus": [1]}}}))
            stack.enter_context(mock.patch.object(faults, "memory_preflight", return_value={}))
            stack.enter_context(mock.patch.object(dev, "placement_snapshot", return_value={"thread_cpu_masks": {"1": "0"}}))
            stack.enter_context(mock.patch.object(dev, "check_ports"))
            stack.enter_context(mock.patch.object(faults.shutil, "which", return_value=str(numactl)))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            args = ["--build-dir", str(build), "--run-root", str(base), "--case", "control"]
            result = faults.main(args + (["--dry-run"] if dry else []))
            manifest = json.loads(next(base.glob("embarcadero-fault-*/manifest.json")).read_text())
            self.assertTrue(manifest["shared_memory_removed"])
            for pid in manifest.get("pids", {}).values():
                self.assertFalse(Path(f"/proc/{pid}").exists())
                self.assertFalse(Path(f"/tmp/embarlet_{pid}_ready").exists())
            return result, manifest

    def test_fake_lifecycle_retains_failures_and_cleans_owned_resources(self):
        code, manifest = self.fake_run(failed=True)
        self.assertEqual(code, 1)
        self.assertEqual(manifest["status"], "failed")
        self.assertEqual(manifest["exit_codes"]["driver"], 7)
        self.assertFalse(manifest["forced_shutdown"])

    def test_fake_lifecycle_success_requires_graceful_exit(self):
        code, manifest = self.fake_run()
        self.assertEqual(code, 0, manifest)
        self.assertEqual(manifest["exit_codes"], {"broker-0": 0, "driver": 0})
        self.assertFalse(manifest["forced_shutdown"])

    def test_dry_run_creates_neither_children_nor_region(self):
        code, manifest = self.fake_run(dry=True)
        self.assertEqual(code, 0)
        self.assertEqual(manifest["status"], "dry-run")
        self.assertNotIn("pids", manifest)


if __name__ == "__main__":
    unittest.main()
