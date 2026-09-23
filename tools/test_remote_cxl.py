#!/usr/bin/env python3
"""Remote profile preflight and validation; never connects to a remote host."""

from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from unittest import mock

import dev_cluster as dev
import remote_cxl as remote


def publisher(host, gib=1):
    return f"{host},0,{gib},/opt/embarcadero/bin/throughput_test"


class RemoteProfileTests(unittest.TestCase):
    def options(self, *arguments):
        return remote.parse_args([
            "--publisher", publisher("c1"), "--publisher", publisher("c3"),
            "--brokers", "4", "--physical-cxl", "--head-addr", "10.10.10.10", *arguments])

    def test_requires_explicit_safe_topology_and_bounded_work(self):
        options, remaining, local = self.options("--threads-per-broker", "6")
        self.assertEqual(len(options.publisher), 2)
        self.assertEqual(local.brokers, 4)
        self.assertTrue(local.physical_cxl)
        self.assertEqual(options.threads_per_broker, 6)
        for args in (
            ["--publisher", publisher("c1"), "--brokers", "4", "--physical-cxl", "--head-addr", "10.10.10.10"],
            ["--publisher", publisher("c1"), "--publisher", publisher("c1"), "--brokers", "4", "--physical-cxl", "--head-addr", "10.10.10.10"],
            ["--publisher", publisher("c1", 16), "--publisher", publisher("c3", 16), "--brokers", "4", "--physical-cxl", "--head-addr", "10.10.10.10"],
            ["--publisher", publisher("c1"), "--publisher", publisher("c3"), "--brokers", "3", "--physical-cxl", "--head-addr", "10.10.10.10"],
            ["--publisher", publisher("c1"), "--publisher", publisher("c3"), "--brokers", "4", "--head-addr", "10.10.10.10"],
            ["--publisher", "c1;touch /tmp/unsafe,0,1,/bin/true", "--publisher", publisher("c3"), "--brokers", "4", "--physical-cxl", "--head-addr", "10.10.10.10"],
        ):
            with self.subTest(args=args), mock.patch("sys.stderr"), self.assertRaises(SystemExit):
                remote.parse_args(args)

    def test_commands_preserve_per_host_binary_and_node(self):
        options, _, local = self.options("--library", "c1=/opt/lib/libglog.so.1", "--hugetlb")
        workload = remote.RemotePublishers(options, local)
        workload.directories = {"c1": "/tmp/embarcadero-cxl-1002-abcdefgh",
                                "c3": "/tmp/embarcadero-cxl-1002-ijklmnop"}
        c1 = workload.launch(options.publisher[0])
        c3 = workload.launch(options.publisher[1])
        self.assertIn("--cpunodebind=0", c1[-1])
        self.assertIn("EMBAR_USE_HUGETLB=1", c1[-1])
        self.assertIn("EMBARCADERO_PUSH_READY_FILE=", c3[-1])
        self.assertIn("--head_addr 10.10.10.10", c3[-1])
        self.assertNotIn("rm -rf", c1[-1])

    def test_exact_ack_zero_fence_and_all_routes_required(self):
        options, _, local = self.options()
        workload = remote.RemotePublishers(options, local)
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for host, identity in (("c1", 71), ("c3", 72)):
                count = dev.GIB // 4096
                (base / f"remote-{host}.log").write_text(
                    f"[ACK_VERIFY] normalized_received={count} raw_received={count} target={count} 100%\n"
                    f"[ORDER5_ROUTING] client_id={identity} retransmit_attempts=0 session_fenced_observed=0 "
                    f"session_rto_min_ms=60000 broker0_msgs={count//4} broker1_msgs={count//4} "
                    f"broker2_msgs={count//4} broker3_msgs={count//4}\n"
                    "Push-ready barrier: go_ns=1000000000\n"
                    "Publisher push start (wall ns): 1000000000\nPublish test completed in 1.00 seconds\n")
            self.assertEqual(workload.validate(base)["total_payload_bytes"], 2 * dev.GIB)
            path = base / "remote-c1.log"
            path.write_text(path.read_text().replace("session_fenced_observed=0", "session_fenced_observed=1"))
            with self.assertRaises(dev.RunError):
                workload.validate(base)
            path.write_text(path.read_text().replace("session_fenced_observed=1", "session_fenced_observed=0")
                            .replace("client_id=71", "client_id=72"))
            with self.assertRaisesRegex(dev.RunError, "sessions collided"):
                workload.validate(base)

    def test_hugetlb_request_requires_observed_mapping(self):
        options, _, local = self.options("--hugetlb")
        workload = remote.RemotePublishers(options, local)
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            for host in ("c1", "c3"):
                (base / f"remote-{host}.log").write_text(
                    f"[ACK_VERIFY] normalized_received={dev.GIB//4096} raw_received=0 "
                    f"target={dev.GIB//4096} 100%\n"
                    f"[ORDER5_ROUTING] client_id=1 retransmit_attempts=0 session_fenced_observed=0 "
                    f"session_rto_min_ms=60000 broker0_msgs={dev.GIB//4096}\n")
            with self.assertRaisesRegex(dev.RunError, "HugeTLB mapping was not confirmed"):
                workload.validate(base)

    def test_cleanup_refuses_unowned_path_and_waits_for_exact_child(self):
        options, _, local = self.options()
        workload = remote.RemotePublishers(options, local)
        p = options.publisher[0]
        workload.directories[p.host] = "/tmp/unrelated"
        with self.assertRaisesRegex(dev.RunError, "refusing cleanup"):
            workload.cleanup(p)
        directory = f"/tmp/embarcadero-cxl-{os.getuid()}-abcdefgh"
        workload.directories[p.host] = directory
        with mock.patch.object(remote, "ssh", return_value="") as transport:
            workload.cleanup(p)
        script = transport.call_args.args[1]
        self.assertIn("readlink /proc/$pid/exe", script)
        self.assertIn("kill -TERM $pid", script)
        self.assertIn("kill -KILL $pid", script)
        self.assertIn("rm -rf -- " + directory, script)
        self.assertEqual(subprocess.run(["sh", "-n"], input=script, text=True,
                                        capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
