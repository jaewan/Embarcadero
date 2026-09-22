#!/usr/bin/env python3
"""No hardware allocations: test paired protocol and owned fake processes."""
import contextlib
import hashlib
import io
import tarfile
import json
import os
from pathlib import Path
import tempfile
import types
import unittest
from unittest import mock

import perf_compare as perf


class PerfTests(unittest.TestCase):
    def test_broker_protocol_preserves_serial_audit_despite_streaming_default(self):
        with mock.patch.dict(os.environ, {"EMBARCADERO_E2E_AUDIT_MODE": "stream",
                                         "EMBARCADERO_SUBSCRIBER_RETAINED_BYTES": "1"}):
            env, selected, removed = perf.environment("/owned-perf-test", 1)
        self.assertEqual(env["EMBARCADERO_E2E_AUDIT_MODE"], "serial")
        self.assertEqual(selected["EMBARCADERO_SUBSCRIBER_RETAINED_BYTES"], str(3 * perf.dev.GIB))
        self.assertEqual(selected["EMBARCADERO_SUBSCRIBER_MAX_MESSAGES"], "1048576")
        self.assertIn("EMBARCADERO_E2E_AUDIT_MODE", removed)

    def test_archived_source_binds_inventory_archive_and_actual_build_source(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            source.mkdir()
            (source / "CMakeLists.txt").write_bytes(b"project(test)\n")
            (source / "CMakeLists.txt").chmod(0o644)
            archive = root / "source.tar.gz"
            data = (source / "CMakeLists.txt").read_bytes()
            with tarfile.open(archive, "w:gz") as tar:
                member = tarfile.TarInfo("CMakeLists.txt")
                member.size, member.mode = len(data), 0o644
                tar.addfile(member, io.BytesIO(data))
            inventory = {"revision": "a" * 40, "archive_sha256": perf.dev.binary_digest(archive),
                         "files": {"CMakeLists.txt": {"sha256": hashlib.sha256(data).hexdigest(), "mode": 0o644}}}
            sidecar = archive.with_suffix(".gz.json")
            sidecar.write_text(json.dumps(inventory))
            result = perf.archived_source_snapshot(source, archive, root / "valid")
            self.assertEqual(result["provenance"], "archived dirty snapshot")
            self.assertIsNone(result["git_status"])
            self.assertEqual(result["verified_files"], 1)
            self.assertEqual(Path(result["archive"]).read_bytes(), archive.read_bytes())
            self.assertEqual(Path(result["inventory"]).read_bytes(), sidecar.read_bytes())
            extra = source / "extra.h"
            extra.write_text("// unarchived include\n")
            with self.assertRaisesRegex(perf.dev.RunError, "file inventory differs"):
                perf.archived_source_snapshot(source, archive, root / "extra-file")
            extra.unlink()
            extra.symlink_to(source / "CMakeLists.txt")
            with self.assertRaisesRegex(perf.dev.RunError, "symlink"):
                perf.archived_source_snapshot(source, archive, root / "extra-symlink")
            extra.unlink()
            (source / "CMakeLists.txt").write_bytes(b"project(changed)\n")
            with self.assertRaisesRegex(perf.dev.RunError, "CMAKE_HOME_DIRECTORY"):
                perf.archived_source_snapshot(source, archive, root / "changed")
            (source / "CMakeLists.txt").unlink()
            with self.assertRaisesRegex(perf.dev.RunError, "CMAKE_HOME_DIRECTORY"):
                perf.archived_source_snapshot(source, archive, root / "missing")
            (source / "CMakeLists.txt").write_bytes(data)
            (source / "CMakeLists.txt").chmod(0o644)
            inventory["archive_sha256"] = "0" * 64
            sidecar.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(perf.dev.RunError, "SHA256"):
                perf.archived_source_snapshot(source, archive, root / "wrong-archive")
            inventory["archive_sha256"] = perf.dev.binary_digest(archive)
            inventory["files"]["CMakeLists.txt"]["sha256"] = "0" * 64
            sidecar.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(perf.dev.RunError, "member differs"):
                perf.archived_source_snapshot(source, archive, root / "wrong-member")
            inventory["revision"] = ""
            sidecar.write_text(json.dumps(inventory))
            with self.assertRaisesRegex(perf.dev.RunError, "revision"):
                perf.archived_source_snapshot(source, archive, root / "no-revision")

    def test_compile_contract_respects_last_wins_and_architecture_order(self):
        with mock.patch.object(perf.dev, "binary_digest", return_value="compiler"):
            a = perf.compile_contract(["-O2", "-O3", "-DFOO=1", "-UFOO", "-march=native", "-mno-avx"], "c++", "macros")
            b = perf.compile_contract(["-O3", "-UFOO", "-march=native", "-mno-avx"], "c++", "macros")
            self.assertEqual(a, b)
            self.assertNotEqual(a, perf.compile_contract(["-O3", "-O2", "-UFOO", "-march=native", "-mno-avx"], "c++", "macros"))
            self.assertNotEqual(a, perf.compile_contract(["-O3", "-UFOO", "-mno-avx", "-march=native"], "c++", "macros"))
            duplicate = perf.compile_contract(["-O3", "-UFOO", "-mno-avx", "-march=native", "-mno-avx"], "c++", "macros")
            self.assertEqual(a, duplicate)

    def test_empty_or_off_node_placement_cannot_qualify(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with mock.patch.object(perf.dev, "placement_snapshot", return_value={"thread_cpu_masks": {}}):
                with self.assertRaises(perf.dev.RunError):
                    perf.observed_placement(None, [0], 0, root, "client")
            (root / "client.numa_maps").write_text("1000 bind:0 anon=10 N1=10\n")
            with mock.patch.object(perf.dev, "placement_snapshot", return_value={"thread_cpu_masks": {"1": "0"}}):
                with self.assertRaises(perf.dev.RunError):
                    perf.observed_placement(None, [0], 0, root, "client")
    def test_balanced_plan_keeps_qualification_excluded(self):
        runs = perf.run_plan(3, 6)
        self.assertEqual(len(runs), 14)
        self.assertTrue(all(not row["included"] for row in runs[:2]))
        self.assertEqual([row["execution_sequence"] for row in runs], list(range(1, 15)))
        self.assertEqual([runs[2 + 2 * i]["variant"] for i in range(6)],
                         ["baseline", "candidate"] * 3)
        self.assertEqual(len(perf.run_plan(1, 6, True)), 2)

    def test_mapping_requires_agreement_between_expected_log_and_owned_region(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = "CXL mapping successful at address: 0x400000000000\n"
            numa = "400000000000 bind:1 file=/dev/shm/owned N1=100\n"
            (root / "broker-2.log").write_text(log)
            (root / "broker-2.numa_maps").write_text(numa)
            evidence = perf.validate_mapping_base(root, "broker-2", "/owned")
            self.assertEqual(evidence["observed"], "0x400000000000")
            for bad_log, bad_numa in (
                    (log.replace("0x4000", "0x6000"), numa),
                    (log, numa.replace("400000000000", "500000000000")),
                    (log.replace("0x4000", "0x6000"), numa.replace("400000000000", "600000000000")),
                    ("", numa), (log, ""),
                    (log, numa.replace("/owned", "/other-region")),
                    (log + log, numa), (log, numa + numa)):
                with self.subTest(log=bad_log, numa=bad_numa):
                    (root / "broker-2.log").write_text(bad_log)
                    (root / "broker-2.numa_maps").write_text(bad_numa)
                    with self.assertRaisesRegex(perf.dev.RunError, "shared mapping base"):
                        perf.validate_mapping_base(root, "broker-2", "/owned")

    def test_proc_counter_delta_excludes_startup(self):
        before = {key: 100 for key in ("user_seconds", "system_seconds", "minor_faults", "major_faults", "sample_monotonic")}
        after = {key: 103 for key in before}
        self.assertEqual(set(perf.counter_delta(before, after).values()), {3})
        self.assertEqual(perf.counter_delta(None, after), {"status": "unavailable"})

    def result_fixture(self, directory, brokers=3):
        (directory / "throughput_benchmark_summary.csv").write_text(
            "total_message_size_bytes,message_size_bytes,message_count,order,ack_level,replication_factor,sequencer,num_threads_per_broker,publish_goodput_mbps,e2e_goodput_mbps\n"
            "33554432,4096,8192,5,1,0,EMBARCADERO,1,100,50\n")
        routing = " ".join(f"broker{i}_msgs={8192 // brokers + (i < 8192 % brokers)}" for i in range(brokers))
        log = ("[ACK_VERIFY] normalized_received=8192 raw_received=8192 target=8192 100%\n"
               "[ORDERED_DELIVERY_AUDIT] status=passed messages=8192 expected=8192 payload_bytes=33554432 duplicates=0 parse_errors=0 export_gaps=0 indexed_payload=1\n"
               f"[ORDER5_ROUTING] retransmit_attempts=0 session_fenced_observed=0 session_rto_min_ms=2000 {routing}\n"
               "Publisher unacked_byte_cap=1000000000 pool_bytes=536870912 unacked_retention=pool_pin\n")
        (directory / "client.log").write_text(log)
        for i in range(brokers):
            (directory / f"broker-{i}.log").write_text("PBR reservation mode=atomic128\n")
        return log

    def test_validation_requires_global_ack_exact_audit_and_all_ingresses(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = self.result_fixture(root)
            build = {"pbr_mode_from_compile": "atomic128"}
            metrics, evidence = perf.validate_result(root, 32 * perf.dev.MIB, 3, "candidate", build)
            self.assertEqual(metrics["ack_completion_mib_s"], 100)
            self.assertEqual(sum(evidence["routed_messages"].values()), 8192)
            for invalid in (log.replace("retransmit_attempts=0", "retransmit_attempts=1"),
                            log.replace("broker2_msgs=2730", "broker2_msgs=0"),
                            log.replace("session_rto_min_ms=2000", "session_rto_min_ms=60000"),
                            log.replace("indexed_payload=1", "indexed_payload=0"),
                            log + "Segment rolled\n"):
                with self.subTest(invalid=invalid):
                    (root / "client.log").write_text(invalid)
                    with self.assertRaises(perf.dev.RunError):
                        perf.validate_result(root, 32 * perf.dev.MIB, 3, "candidate", build)

    def test_order5_raw_diagnostic_can_lag_authoritative_ack_frontier(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            log = self.result_fixture(root, 1)
            build = {"pbr_mode_from_compile": "atomic128"}
            # This is the real writer interleaving: HWM CAS, unacked retirement,
            # then raw diagnostic increment. Poll can snapshot between them.
            (root / "client.log").write_text(log.replace("raw_received=8192", "raw_received=8064"))
            _, evidence = perf.validate_result(root, 32 * perf.dev.MIB, 1, "candidate", build)
            self.assertEqual(evidence["ack"]["normalized_received"], 8192)
            self.assertEqual(evidence["ack"]["raw_received_diagnostic"], 8064)
            self.assertFalse(evidence["ack"]["raw_received_is_completion_gate"])
            # A raw counter at target cannot conceal a short authoritative ACK.
            (root / "client.log").write_text(log.replace("normalized_received=8192", "normalized_received=8064"))
            with self.assertRaisesRegex(perf.dev.RunError, "ACK completion count mismatch"):
                perf.validate_result(root, 32 * perf.dev.MIB, 1, "candidate", build)

    def fake_inputs(self, root):
        (root / "runs").mkdir()
        binaries = root / "bin"
        binaries.mkdir()
        broker = binaries / "broker"
        broker.write_text("#!/usr/bin/env python3\n" + """
import os,pathlib,signal,sys,time
assert '--emul' in sys.argv
assert os.environ['EMBARCADERO_CXL_BASE_ADDR'] == '0x400000000000'
signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
print('CXL mapping successful at address: ' + os.environ['EMBARCADERO_CXL_BASE_ADDR'], flush=True)
pathlib.Path('/tmp/embarlet_%s_ready' % os.getpid()).write_text('ready\n')
print('PBR reservation mode=atomic128', flush=True)
while True: time.sleep(0.1)
""".replace("'ready\n'", "'ready\\n'"))
        client = binaries / "client"
        fixture = root / "fixture"
        fixture.mkdir()
        self.result_fixture(fixture, 1)
        client.write_text("#!/usr/bin/env python3\n" +
            "import pathlib,sys,time\ntime.sleep(0.3)\n" +
            f"pathlib.Path('throughput_benchmark_summary.csv').write_text({(fixture / 'throughput_benchmark_summary.csv').read_text()!r})\n" +
            f"print({(fixture / 'client.log').read_text()!r},flush=True)\n")
        numactl = binaries / "numactl"
        numactl.write_text("#!/usr/bin/env python3\nimport os,sys\nos.execv(sys.argv[3],sys.argv[3:])\n")
        for binary in (broker, client, numactl):
            binary.chmod(0o700)
        identity = {"broker": str(broker), "broker_sha256": perf.dev.binary_digest(broker),
                    "client": str(client), "client_sha256": perf.dev.binary_digest(client),
                    "build": {"pbr_mode_from_compile": "atomic128"}}
        args = types.SimpleNamespace(output=root, brokers=1, payload_mib=32, startup_timeout=3, dry_run=False)
        campaign = {"runs": [], "numactl": str(numactl), "layout": {}, "baseline_compatibility": {}}
        return args, campaign, {"baseline": identity, "candidate": identity}

    def execute_fake(self, root, fail_cleanup=False, dry_run=False, wrong_mapping=False):
        args, campaign, identities = self.fake_inputs(root)
        args.dry_run = dry_run
        assignment = {"client": [0], "broker-0": [1]}
        entry = perf.run_plan(1, 6)[1]
        hardware = {"nodes": {"0": {"cpus": [0]}, "1": {"cpus": [1]}}}
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(perf.dev, "topology", return_value=hardware))
            stack.enter_context(mock.patch.object(perf, "cpu_assignment", return_value=(assignment, {})))
            stack.enter_context(mock.patch.object(perf, "memory_preflight", return_value={}))
            stack.enter_context(mock.patch.object(perf.dev, "check_ports"))
            def fake_placement(child, cpus, node, run_dir, role, shm_name=None):
                if role.startswith("broker-"):
                    base = "600000000000" if wrong_mapping else "400000000000"
                    (run_dir / f"{role}.numa_maps").write_text(
                        f"{base} bind:1 file=/dev/shm/{run_dir.name} N1=100\n")
                return {"thread_cpu_masks": {"1": "0"}}
            stack.enter_context(mock.patch.object(perf, "observed_placement", side_effect=fake_placement))
            if fail_cleanup:
                close = perf.dev.OwnedProcesses.close
                def failed(owned):
                    close(owned)
                    raise OSError("injected cleanup failure")
                stack.enter_context(mock.patch.object(perf.dev.OwnedProcesses, "close", failed))
            return perf.execute_run(args, entry, campaign, perf.effective_config(1), identities, assignment)

    def test_fake_lifecycle_records_resources_and_cleans_owned_region(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute_fake(Path(directory))
            self.assertEqual(result["status"], "passed", result)
            self.assertTrue(result["shared_memory_removed"])
            self.assertTrue(all(code == 0 for code in result["exit_codes"].values()))
            self.assertEqual(result["mapping_bases"]["broker-0"]["observed"], "0x400000000000")
            self.assertEqual(result["protocol_revision"], "paired-dram-v4-explicit-serial-audit")
            self.assertIn("client_rusage", result)
            self.assertGreater(result["ended_monotonic"], result["started_monotonic"])
            for pid in result["pids"].values():
                self.assertFalse(Path(f"/proc/{pid}").exists())

    def test_cleanup_error_prevents_qualification(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute_fake(Path(directory), fail_cleanup=True)
            self.assertEqual(result["status"], "failed")
            self.assertTrue(any("cleanup failure" in reason for reason in result["failure_reasons"]))

    def test_wrong_mapping_prevents_client_start_and_still_cleans_owned_resources(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute_fake(Path(directory), wrong_mapping=True)
            self.assertEqual(result["status"], "failed")
            self.assertTrue(any("shared mapping base" in reason for reason in result["failure_reasons"]))
            self.assertNotIn("client", result["pids"])
            self.assertTrue(result["shared_memory_removed"])
            self.assertEqual(result["exit_codes"], {"broker-0": 0})

    def test_dry_run_never_creates_region_or_process(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.execute_fake(Path(directory), dry_run=True)
            self.assertEqual(result["status"], "dry-run")
            self.assertFalse(Path(result["shared_memory"]).exists())
            self.assertEqual(result["exit_codes"], {})
            self.assertNotIn("pids", result)


if __name__ == "__main__":
    unittest.main()
