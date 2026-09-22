#!/usr/bin/env python3
"""Dispatch tests execute tiny fixture programs, never brokers or SSH."""

import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

import experiment


FAKE_RUNNER = '''import json, os, signal, sys
from pathlib import Path
signal.signal(signal.SIGTERM, lambda *_: sys.exit(19))
Path(os.environ["DISPATCH_RECORD"]).write_text(json.dumps({
    "argv": sys.argv[1:], "pid": os.getpid(), "cwd": os.getcwd(),
    "entrypoint": str(Path(__file__).relative_to(Path(os.environ["FIXTURE_ROOT"]))),
    "historical_env": os.environ.get("NUM_CLIENTS")
}))
if os.environ.get("DISPATCH_HOLD") == "1":
    while True: signal.pause()
sys.exit(int(os.environ.get("DISPATCH_EXIT", "0")))
'''


class ExperimentDispatchTests(unittest.TestCase):
    def setUp(self):
        context = tempfile.TemporaryDirectory(prefix="experiment-dispatch-")
        self.addCleanup(context.cleanup)
        self.directory = Path(context.name)
        self.repo = self.directory / "repository with spaces"
        self.cwd = self.directory / "caller with spaces"
        self.cwd.mkdir()
        self.record = self.directory / "dispatch.json"
        self.env = {**os.environ, "PYTHONDONTWRITEBYTECODE": "1",
                    "DISPATCH_RECORD": str(self.record), "FIXTURE_ROOT": str(self.repo),
                    "NUM_CLIENTS": "historical-value-preserved"}
        for relative in ("tools/experiment.py", "scripts/lib/experiment_dispatch.sh",
                         "scripts/run_experiment.sh", *experiment.LEGACY_LAUNCHERS.values()):
            destination = self.repo / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(experiment.ROOT / relative, destination)
        for relative in experiment.PROFILES.values():
            target = self.repo / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(FAKE_RUNNER)

    def run_fixture(self, command, **options):
        self.record.unlink(missing_ok=True)
        return subprocess.run(command, cwd=self.cwd, env=self.env, capture_output=True,
                              text=True, timeout=5, **options)

    def read_record(self):
        return json.loads(self.record.read_text())

    def test_all_profiles_preserve_literal_arguments_cwd_environment_and_status(self):
        arguments = ["--output", "path with spaces", "$(touch BAD)", "`touch BAD`", "--help"]
        self.env["DISPATCH_EXIT"] = "23"
        for profile, target in experiment.PROFILES.items():
            with self.subTest(profile=profile):
                result = self.run_fixture([sys.executable, str(self.repo / "tools/experiment.py"),
                                           profile, *arguments])
                self.assertEqual(result.returncode, 23, result.stderr)
                record = self.read_record()
                self.assertEqual(record["argv"], arguments)
                self.assertEqual(record["entrypoint"], target)
                self.assertEqual(record["cwd"], str(self.cwd))
                self.assertEqual(record["historical_env"], self.env["NUM_CLIENTS"])
                self.assertFalse((self.cwd / "BAD").exists())

    def test_no_profile_and_unknown_profile_never_execute(self):
        for arguments, expected in (([], 0), (["--help"], 0), (["not-a-profile"], 2),
                                    (["legacy", "../../outside"], 2), (["legacy", "--help"], 0)):
            with self.subTest(arguments=arguments):
                result = self.run_fixture([sys.executable, str(self.repo / "tools/experiment.py"), *arguments])
                self.assertEqual(result.returncode, expected)
                self.assertFalse(self.record.exists())

    def test_missing_selected_runner_fails_without_fallback(self):
        (self.repo / experiment.PROFILES["dev"]).unlink()
        result = self.run_fixture([sys.executable, str(self.repo / "tools/experiment.py"), "dev"])
        self.assertEqual(result.returncode, 2)
        self.assertFalse(self.record.exists())

    def test_all_documented_shell_launchers_dispatch_before_historical_body(self):
        profiles = {"--dev-dram": "dev", "--workload-dram": "workload", "--fault-dram": "fault", "--perf-dram": "perf",
                    "--legacy-startup": "legacy-startup"}
        for launcher in experiment.LEGACY_LAUNCHERS.values():
            for option, profile in profiles.items():
                with self.subTest(launcher=launcher, profile=profile):
                    result = self.run_fixture(["/bin/bash", str(self.repo / launcher), option,
                                               "--build-dir", "path with spaces", "--dry-run"])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(self.read_record()["entrypoint"], experiment.PROFILES[profile])
                    self.assertEqual(self.read_record()["argv"], ["--build-dir", "path with spaces", "--dry-run"])

    def test_compatibility_help_and_unknown_options_never_reach_legacy_body(self):
        for launcher in experiment.LEGACY_LAUNCHERS.values():
            for option, expected in (("--help", 0), ("--dev-drma", 2)):
                with self.subTest(launcher=launcher, option=option):
                    result = self.run_fixture(["/bin/bash", str(self.repo / launcher), option])
                    self.assertEqual(result.returncode, expected, result.stderr)
                    self.assertFalse(self.record.exists())

    def install_legacy_stub(self, relative):
        target = self.repo / relative
        # Retain the actual first-four-line guard. Replace only the historical
        # body, so no historical command can execute during this regression.
        prefix = "\n".join(target.read_text().splitlines()[:4]) + "\n"
        self.assertIn("embarcadero_dispatch_supported", prefix)
        target.write_text(prefix + 'exec python3 "$FIXTURE_ROOT/tools/dev_cluster.py" "$@"\n')

    def test_explicit_legacy_route_strips_only_its_dispatch_argument(self):
        for name, relative in experiment.LEGACY_LAUNCHERS.items():
            with self.subTest(name=name):
                self.install_legacy_stub(relative)
                result = self.run_fixture([sys.executable, str(self.repo / "tools/experiment.py"),
                                           "legacy", name, "--", "--historical-option", "value with spaces"])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.read_record()["argv"], ["--historical-option", "value with spaces"])
                self.assertIn("Historical research route", result.stderr)

    def test_original_environment_only_invocation_is_preserved(self):
        for relative in experiment.LEGACY_LAUNCHERS.values():
            with self.subTest(launcher=relative):
                self.install_legacy_stub(relative)
                result = self.run_fixture(["/bin/bash", str(self.repo / relative)])
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.read_record()["argv"], [])
                self.assertEqual(self.read_record()["historical_env"], self.env["NUM_CLIENTS"])

    def test_shell_alias_exec_preserves_pid_and_signal_delivery(self):
        self.env["DISPATCH_HOLD"] = "1"
        child = subprocess.Popen(["/bin/bash", str(self.repo / "scripts/run_experiment.sh"), "dev"],
                                 cwd=self.cwd, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            deadline = time.monotonic() + 5
            record = None
            while child.poll() is None and time.monotonic() < deadline:
                try:
                    record = self.read_record()
                    break
                except (FileNotFoundError, json.JSONDecodeError):
                    time.sleep(0.01)
            self.assertIsNotNone(record)
            # exec through shell and dispatcher keeps one PID: SIGTERM reaches
            # the runner directly, with no intermediary left to own its children.
            self.assertEqual(record["pid"], child.pid)
            child.send_signal(signal.SIGTERM)
            child.communicate(timeout=5)
            self.assertEqual(child.returncode, 19)
        finally:
            if child.poll() is None:
                child.kill()
            child.communicate(timeout=5)

    def test_real_runner_option_validation_precedes_all_launch_operations(self):
        for profile in experiment.PROFILES:
            with self.subTest(profile=profile):
                result = self.run_fixture([sys.executable, str(experiment.ROOT / "tools/experiment.py"),
                                           profile, "--definitely-invalid-launcher-option"])
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertEqual(list(self.cwd.iterdir()), [])
                self.assertFalse(self.record.exists())


if __name__ == "__main__":
    unittest.main()
