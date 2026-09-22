#!/usr/bin/env python3
"""Runner lifecycle tests use fake executables; no 64 GiB mapping or brokers."""

import contextlib
import io
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import dev_cluster as runner


class RunnerTests(unittest.TestCase):
    audit = ("[ORDERED_DELIVERY_AUDIT] status=passed messages=8192 expected=8192 "
             "payload_bytes=33554432 duplicates=0 parse_errors=0 export_gaps=0 indexed_payload=1")

    def setUp(self):
        # Tests exercise real flock semantics on a private inode. A live cluster
        # must never block this fake-process suite, or share its serialization.
        context = contextlib.ExitStack()
        self.addCleanup(context.close)
        self.lock_directory = Path(context.enter_context(tempfile.TemporaryDirectory()))
        private_tempfile = mock.Mock(wraps=tempfile)
        private_tempfile.gettempdir.return_value = str(self.lock_directory)
        context.enter_context(mock.patch.object(runner, "tempfile", private_tempfile))

    def test_smoke_requires_exact_audited_delivery(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            (base / "throughput_benchmark_summary.csv").write_text(
                "total_message_size_bytes,publish_goodput_mbps,e2e_goodput_mbps\n33554432,1,1\n")
            for log in ("Order Level 5 check PASSED", self.audit.replace("messages=8192", "messages=8191"),
                        self.audit.replace("payload_bytes=33554432", "payload_bytes=33550336"),
                        self.audit.replace("duplicates=0", "duplicates=1"),
                        self.audit.replace("parse_errors=0", "parse_errors=1"),
                        self.audit.replace("export_gaps=0", "export_gaps=1"),
                        self.audit.replace("indexed_payload=1", "indexed_payload=0"),
                        self.audit + "\nretransmit_attempts=1", self.audit + "\n" + self.audit):
                with self.subTest(log=log):
                    (base / "client.log").write_text(log)
                    with self.assertRaises(runner.RunError):
                        runner.validate_smoke(base)
            (base / "client.log").write_text(self.audit)
            self.assertEqual(runner.validate_smoke(base)["audited_messages"], 8192)

    def test_cpu_ranges(self):
        self.assertEqual(runner.cpu_set("0-2,8,10-11"), {0, 1, 2, 8, 10, 11})
        with self.assertRaises(ValueError):
            runner.cpu_set("4-1")

    def test_profile_and_environment_are_explicit(self):
        config = runner.effective_config(3)
        self.assertEqual(config["embarcadero"]["cxl"]["size"], 64 * runner.GIB)
        self.assertEqual(config["embarcadero"]["storage"]["segment_size"], 256 * runner.MIB)
        self.assertEqual(config["embarcadero"]["cluster"]["data_broker_ids"], [0, 1, 2])
        with mock.patch.dict(os.environ, {"EMBARCADERO_CXL_ZERO_MODE": "none",
                                         "EMBARCADERO_CXL_SIZE": "1024",
                                         "EMBARCADERO_CXL_BASE_ADDR": "0x600000000000",
                                         "EMBAR_FAULT_INJECT": "1"}):
            env, selected, removed = runner.child_environment("/owned-test", 3)
        self.assertEqual(env["EMBARCADERO_CXL_ZERO_MODE"], "full")
        self.assertEqual(env["EMBARCADERO_CXL_SHM_NAME"], "/owned-test")
        self.assertEqual(env["EMBARCADERO_CXL_BASE_ADDR"], "0x400000000000")
        self.assertEqual(selected["EMBARCADERO_CXL_BASE_ADDR"], env["EMBARCADERO_CXL_BASE_ADDR"])
        self.assertIn("EMBARCADERO_CXL_BASE_ADDR", removed)
        self.assertNotIn("EMBARCADERO_CXL_SIZE", env)
        self.assertNotIn("EMBAR_FAULT_INJECT", env)
        self.assertIn("EMBAR_FAULT_INJECT", removed)
        self.assertEqual(selected["NUM_BROKERS"], "3")

    def test_occupied_port_rejected_without_touching_listener(self):
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            listener.listen()
            port = listener.getsockname()[1]
            with self.assertRaisesRegex(runner.RunError, "occupied"):
                runner.check_ports([port])
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                pass

    def test_lock_excludes_second_run(self):
        with runner.ClusterLock() as held:
            self.assertEqual(held.path.parent, self.lock_directory)
            with self.assertRaisesRegex(runner.RunError, "another dev cluster"):
                with runner.ClusterLock():
                    self.fail("second runner acquired lock")

    def test_cleanup_only_owned_group_and_escalates(self):
        unrelated = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"],
                                     start_new_session=True)
        try:
            with tempfile.TemporaryDirectory() as directory:
                owned = runner.OwnedProcesses(Path(directory), os.environ.copy(), 0.1)
                child = owned.start("stubborn", [sys.executable, "-c",
                    "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print('ready',flush=True); time.sleep(60)"])
                deadline = time.monotonic() + 3
                while "ready" not in (Path(directory) / "stubborn.log").read_text():
                    if time.monotonic() >= deadline:
                        self.fail("fake child did not start")
                    time.sleep(0.01)
                owned.close()
                owned.close()  # idempotent, so a finalizer cannot signal a reused PID.
                self.assertIsNotNone(child.poll())
                self.assertEqual(owned.forced, ["stubborn"])
                self.assertIsNone(unrelated.poll())
        finally:
            unrelated.terminate()
            unrelated.wait(timeout=3)

    def test_split_cleanup_signals_once_and_prevents_late_children(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            owned = runner.OwnedProcesses(base, os.environ.copy(), 2)
            child = owned.start("graceful", [sys.executable, "-c",
                "import signal,time,sys;signal.signal(signal.SIGTERM,lambda *_:sys.exit(0));print('ready',flush=True);time.sleep(20)"])
            deadline = time.monotonic() + 3
            while "ready" not in (base / "graceful.log").read_text():
                if time.monotonic() >= deadline:
                    self.fail("fake child did not become ready")
                time.sleep(0.01)
            with mock.patch.object(runner.os, "killpg", wraps=os.killpg) as kill:
                owned.request_stop()
                owned.request_stop()
                with self.assertRaises(runner.RunError):
                    owned.start("late", [sys.executable, "-c", "pass"])
                owned.close()
                self.assertEqual([call for call in kill.call_args_list if call.args[1] == signal.SIGTERM],
                                 [mock.call(child.pid, signal.SIGTERM)])
            self.assertEqual(child.returncode, 0)
            self.assertFalse(owned.forced)

    def test_shell_dev_route_validates_before_legacy_checks_and_lock(self):
        with tempfile.TemporaryDirectory() as directory:
            lock = Path(directory) / "legacy.lock"
            env = {**os.environ, "EMBARCADERO_MEMORY_BACKEND": "invalid-legacy-value",
                   "RUN_LOCK_FILE": str(lock)}
            script = runner.ROOT / "scripts/run_multiclient.sh"
            help_result = subprocess.run([str(script), "--dev-dram", "--help"], env=env,
                capture_output=True, text=True, timeout=5)
            self.assertEqual(help_result.returncode, 0, help_result.stderr)
            self.assertIn("owned, bounded DRAM", help_result.stdout)
            rejected = subprocess.run([str(script), "--dev-dram", "--not-a-supported-option"], env=env,
                capture_output=True, text=True, timeout=5)
            self.assertEqual(rejected.returncode, 2)
            self.assertIn("unrecognized arguments", rejected.stderr)
            self.assertFalse(lock.exists())

    def fake_build(self, directory):
        build = directory / "build"
        binaries = build / "bin"
        binaries.mkdir(parents=True)
        broker = binaries / "embarlet"
        broker.write_text("#!/usr/bin/env python3\n" + """
import json, os, pathlib, sys, time
if '--print-layout' in sys.argv:
    print(json.dumps({'region_bytes': 64 * 1024**3, 'metadata_bytes': 33 * 1024**3,
                      'payload_bytes': 31 * 1024**3, 'segment_size': 256 * 1024**2,
                      'segment_count': 124}))
    sys.exit(0)
assert '--emul' in sys.argv
assert os.environ['EMBARCADERO_CXL_SHM_NAME'].startswith('/embarcadero-dev-')
assert os.environ['EMBARCADERO_CXL_BASE_ADDR'] == '0x400000000000'
pathlib.Path('/tmp/embarlet_%s_ready' % os.getpid()).write_text('ready\\n')
time.sleep(60)
""")
        client = binaries / "throughput_test"
        client.write_text("#!/usr/bin/env python3\n" + """
import pathlib
pathlib.Path('throughput_benchmark_summary.csv').write_text(
    'total_message_size_bytes,publish_goodput_mbps,e2e_goodput_mbps\\n33554432,1,1\\n')
print('[ORDERED_DELIVERY_AUDIT] status=passed messages=8192 expected=8192 payload_bytes=33554432 duplicates=0 parse_errors=0 export_gaps=0 indexed_payload=1')
""")
        numactl = directory / "fake-numactl"
        numactl.write_text("#!/usr/bin/env python3\nimport os,sys\nos.execv(sys.argv[3], sys.argv[3:])\n")
        for path in (broker, client, numactl):
            path.chmod(0o700)
        return build, numactl

    @contextlib.contextmanager
    def fake_host(self, numactl):
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(runner, "topology", return_value={
                "nodes": {"0": {"cpus": [0]}, "1": {"cpus": [1]}}}))
            stack.enter_context(mock.patch.object(runner, "memory_preflight", return_value={}))
            stack.enter_context(mock.patch.object(runner, "placement_snapshot", return_value={}))
            stack.enter_context(mock.patch.object(runner, "check_ports"))
            stack.enter_context(mock.patch.object(runner.shutil, "which", return_value=str(numactl)))
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            yield

    def test_dry_run_records_all_emulated_commands_without_region(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            build, numactl = self.fake_build(base)
            with self.fake_host(numactl):
                code = runner.main(["--build-dir", str(build), "--run-root", str(base), "--brokers", "3", "--dry-run"])
            self.assertEqual(code, 0)
            path = next(base.glob("embarcadero-dev-*/manifest.json"))
            manifest = json.loads(path.read_text())
            self.assertEqual(manifest["status"], "dry-run")
            self.assertEqual(len(manifest["broker_commands"]), 3)
            for command in manifest["broker_commands"]:
                self.assertIn("--emul", command)
                self.assertIn("--membind=1", command)
            self.assertIn("--membind=0", manifest["client_command"])
            self.assertFalse(Path(manifest["shared_memory"]).exists())

    def test_legacy_profile_records_same_command_and_environment_it_executes(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            build, numactl = self.fake_build(base)
            profile = runner.SmokeProfile("legacy-test", 0, False, lambda _: {})
            with self.fake_host(numactl):
                code = runner.main(["--build-dir", str(build), "--run-root", str(base), "--dry-run"], profile=profile)
            self.assertEqual(code, 0)
            manifest = json.loads(next(base.glob("embarcadero-dev-*/manifest.json")).read_text())
            self.assertEqual(manifest["profile"], "legacy-test")
            self.assertFalse(manifest["audit_enabled"])
            self.assertEqual(manifest["order"], 0)
            self.assertEqual(manifest["client_command"][manifest["client_command"].index("-o") + 1], "0")
            self.assertEqual(manifest["environment"]["EMBAR_VALIDATE_ORDER"], "0")

    def test_automatic_mapping_dry_run_removes_inherited_fixed_override(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            build, numactl = self.fake_build(base)
            with self.fake_host(numactl), mock.patch.dict(os.environ, {"EMBARCADERO_CXL_BASE_ADDR": "0x600000000000"}):
                code = runner.main(["--build-dir", str(build), "--run-root", str(base), "--brokers", "3",
                                    "--automatic-mapping", "--dry-run"])
            self.assertEqual(code, 0)
            manifest = json.loads(next(base.glob("embarcadero-dev-*/manifest.json")).read_text())
            self.assertNotIn("EMBARCADERO_CXL_BASE_ADDR", manifest["environment"])
            self.assertIn("automatic", manifest["mapping_policy"])

    def test_fake_success_cleans_region_and_owned_processes(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            build, numactl = self.fake_build(base)
            with self.fake_host(numactl):
                code = runner.main(["--build-dir", str(build), "--run-root", str(base), "--brokers", "3"])
            path = next(base.glob("embarcadero-dev-*/manifest.json"))
            manifest = json.loads(path.read_text())
            self.assertEqual(code, 0, manifest)
            self.assertEqual(manifest["status"], "passed")
            self.assertFalse(Path(manifest["shared_memory"]).exists())
            for pid in manifest["pids"].values():
                self.assertFalse(Path(f"/proc/{pid}").exists())
                self.assertFalse(Path(f"/tmp/embarlet_{pid}_ready").exists())

    def test_startup_failure_keeps_lock_until_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            build, numactl = self.fake_build(base)
            original = runner.OwnedProcesses.close
            close_checked = []
            def close_with_lock_check(owned):
                with self.assertRaises(runner.RunError):
                    with runner.ClusterLock():
                        pass
                close_checked.append(True)
                return original(owned)
            with self.fake_host(numactl), mock.patch.object(runner, "wait_ready", side_effect=runner.RunError("injected timeout")), \
                    mock.patch.object(runner.OwnedProcesses, "close", close_with_lock_check):
                code = runner.main(["--build-dir", str(build), "--run-root", str(base)])
            self.assertEqual(code, 1)
            self.assertTrue(close_checked)
            manifest = json.loads(next(base.glob("embarcadero-dev-*/manifest.json")).read_text())
            self.assertFalse(Path(manifest["shared_memory"]).exists())
            self.assertIn("injected timeout", manifest["error"])

    def test_cleanup_failure_cannot_report_success(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            build, numactl = self.fake_build(base)
            original = runner.OwnedProcesses.close
            def failing_close(owned):
                original(owned)
                raise OSError("injected cleanup error")
            with self.fake_host(numactl), mock.patch.object(runner.OwnedProcesses, "close", failing_close):
                code = runner.main(["--build-dir", str(build), "--run-root", str(base)])
            manifest = json.loads(next(base.glob("embarcadero-dev-*/manifest.json")).read_text())
            self.assertEqual(code, 1)
            self.assertEqual(manifest["status"], "failed")
            self.assertIn("injected cleanup error", manifest["cleanup_error"])
            self.assertFalse(Path(manifest["shared_memory"]).exists())


if __name__ == "__main__":
    unittest.main()
