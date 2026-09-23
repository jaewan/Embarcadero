#!/usr/bin/env python3
"""Owned finite real-CXL throughput profile with two or three remote publishers.

The caller supplies each host's executable and NIC-local NUMA node. This
runner changes no remote network, hugepage, or system-library configuration.
"""

import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import time

import dev_cluster as dev
from experiment_workload import ack_and_routing


SSH = ("ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=5")
HOST_RE = re.compile(r"[A-Za-z][A-Za-z0-9_.-]{0,62}\Z")
DIR_RE = re.compile(r"/tmp/embarcadero-cxl-[0-9]+-[A-Za-z0-9_]+\Z")
original_config = dev.effective_config


@dataclass(frozen=True)
class Publisher:
    host: str
    node: int
    payload_gib: int
    binary: str
    library: str | None = None

    @property
    def payload_bytes(self):
        return self.payload_gib * dev.GIB


def parse_publisher(value):
    """HOST,NIC_NUMA_NODE,PAYLOAD_GIB,ABSOLUTE_BINARY_PATH."""
    fields = value.split(",", 3)
    if len(fields) != 4 or not HOST_RE.fullmatch(fields[0]):
        raise argparse.ArgumentTypeError("publisher must be HOST,NODE,GIB,/absolute/binary")
    try:
        node, gib = int(fields[1]), int(fields[2])
    except ValueError as error:
        raise argparse.ArgumentTypeError("publisher node and GiB must be integers") from error
    if not 0 <= node <= 31 or not 1 <= gib <= 16 or not Path(fields[3]).is_absolute():
        raise argparse.ArgumentTypeError("publisher needs node 0..31, 1..16 GiB, absolute binary")
    return Publisher(fields[0], node, gib, fields[3])


def parse_library(value):
    host, separator, path = value.partition("=")
    if not separator or not HOST_RE.fullmatch(host) or not Path(path).is_absolute():
        raise argparse.ArgumentTypeError("library must be HOST=/absolute/library")
    return host, path


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--publisher", action="append", type=parse_publisher, required=True,
                        help="repeat HOST,NIC_NODE,PAYLOAD_GIB,/absolute/throughput_test")
    parser.add_argument("--library", action="append", type=parse_library, default=[],
                        help="optional private shared library as HOST=/absolute/library")
    parser.add_argument("--hugetlb", action="store_true", help="require remote client HugeTLB mapping")
    parser.add_argument("--threads-per-broker", type=int, default=6)
    parser.add_argument("--deadline-seconds", type=int, default=120)
    known, remaining = parser.parse_known_args(argv)
    hosts = [publisher.host for publisher in known.publisher]
    if len(hosts) not in (2, 3) or len(set(hosts)) != len(hosts):
        parser.error("provide two or three distinct publisher hosts")
    if not 1 <= known.threads_per_broker <= 8 or not 30 <= known.deadline_seconds <= 300:
        parser.error("threads must be 1..8 and deadline 30..300 seconds")
    if sum(p.payload_gib for p in known.publisher) > 16:
        parser.error("aggregate payload must fit the bounded 16 GiB profile")
    libraries = dict(known.library)
    if len(libraries) != len(known.library) or set(libraries) - set(hosts):
        parser.error("libraries must name distinct configured publisher hosts")
    known.publisher = [Publisher(p.host, p.node, p.payload_gib, p.binary, libraries.get(p.host))
                       for p in known.publisher]
    local = dev.parse_args(remaining)
    if local.brokers != 4 or not local.physical_cxl:
        parser.error("remote CXL profile requires --brokers 4 --physical-cxl")
    if local.head_addr == "127.0.0.1":
        parser.error("--head-addr must advertise the broker's remote-reachable address")
    return known, remaining, local


def ssh(host, command, timeout=20):
    try:
        result = subprocess.run([*SSH, host, command], capture_output=True, text=True, timeout=timeout)
    except subprocess.TimeoutExpired as error:
        raise dev.RunError(f"{host}: SSH command timed out") from error
    if result.returncode:
        raise dev.RunError(f"{host}: SSH command failed: {result.stderr[-1000:]}")
    return result.stdout.strip()


def remote_config(brokers):
    config = original_config(brokers)
    config["embarcadero"]["cxl"]["size"] = dev.REGION_BYTES
    config["embarcadero"]["cxl"]["emulation_size"] = dev.REGION_BYTES
    config["embarcadero"]["storage"]["segment_size"] = dev.SEGMENT_BYTES
    return config


class RemotePublishers:
    def __init__(self, options, local):
        self.publishers = options.publisher
        self.options = options
        self.local = local
        self.directories = {}
        self.hashes = {}
        self.evidence = {}
        self.children = {}
        self.launched = set()

    def prepare(self, config, env, selected, manifest, _hardware, common, _run_dir):
        if common.brokers != 4 or not common.physical_cxl:
            raise dev.RunError("four real-CXL brokers required")
        config["embarcadero"]["network"]["io_threads"] = max(16, len(self.publishers) * self.options.threads_per_broker + 6)
        selected.update(EMBAR_VALIDATE_ORDER="0", EMBARCADERO_SESSION_RTO_MIN_MS="60000",
                        EMBARCADERO_ACK_TIMEOUT_SEC="120", EMBARCADERO_E2E_TIMEOUT_SEC="120")
        env.update(selected)
        manifest.update(profile="cxl-remote-order5-ack1-rf0", client_deadline_seconds=self.options.deadline_seconds,
                        application_payload_bytes=sum(p.payload_bytes for p in self.publishers),
                        initial_payload_reservation_upper_bound_bytes=(
                            sum(p.payload_bytes for p in self.publishers) // 4096 * (4096 + 128)
                            + 2 * dev.MIB * common.brokers * len(self.publishers) * self.options.threads_per_broker),
                        message_bytes=4096, workload={
                            "publisher_hosts": [p.host for p in self.publishers],
                            "payload_bytes_per_host": {p.host: p.payload_bytes for p in self.publishers},
                            "threads_per_broker": self.options.threads_per_broker,
                            "broker_receive_threads": config["embarcadero"]["network"]["io_threads"],
                            "batch_bytes": 2 * dev.MIB, "pool_cap_bytes": 2 * dev.GIB,
                            "order": 5, "ack": 1, "replication_factor": 0,
                            "validation_scope": "exact per-client ACK, all-broker routing, zero retry/fence; no subscriber audit",
                            "timing_scope": "client clocks and 0.01-second durations; approximate cross-host completion"})
        manifest["limitations"] = ["Finite bounded prototype; no paper-topology or subscriber-delivery claim",
                                   "Remote NIC/MTU/HugeTLB settings are inspected externally and never changed here"]

    def commands(self, _binding, _binary, config, run_dir):
        self.config, self.run_dir = config, run_dir
        plans = []
        for p in self.publishers:
            digest = ssh(p.host, "sha256sum " + shlex.quote(p.binary)).split()[0]
            if not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise dev.RunError(p.host + ": invalid executable digest")
            self.hashes[p.host] = digest
            plans.append({"host": p.host, "numa_node": p.node, "payload_bytes": p.payload_bytes,
                          "binary": p.binary, "binary_sha256": digest, "library": p.library})
        return plans

    def setup(self, p):
        directory = ssh(p.host, f"mktemp -d /tmp/embarcadero-cxl-{os.getuid()}-XXXXXXXX")
        if not DIR_RE.fullmatch(directory):
            raise dev.RunError(p.host + ": unexpected private directory")
        self.directories[p.host] = directory
        quoted = shlex.quote(directory)
        ssh(p.host, "cp -- " + shlex.quote(p.binary) + " " + quoted + "/throughput_test")
        library_digest = None
        if p.library:
            library_digest = ssh(p.host, "sha256sum " + shlex.quote(p.library)).split()[0]
            ssh(p.host, "cp -- " + shlex.quote(p.library) + " " + quoted + "/")
        copied = ssh(p.host, "sha256sum " + quoted + "/throughput_test")
        if copied.split()[0] != self.hashes[p.host]:
            raise dev.RunError(p.host + ": copied client digest changed")
        result = subprocess.run(["scp", "-q", str(self.config), f"{p.host}:{directory}/config.json"],
                                capture_output=True, text=True, timeout=20)
        if result.returncode:
            raise dev.RunError(p.host + ": configuration copy failed: " + result.stderr[-500:])
        local_config_hash = hashlib.sha256(self.config.read_bytes()).hexdigest()
        remote_config_hash = ssh(p.host, "sha256sum " + quoted + "/config.json").split()[0]
        if remote_config_hash != local_config_hash:
            raise dev.RunError(p.host + ": copied configuration digest changed")
        if p.library:
            copied_library_hash = ssh(p.host, "sha256sum " +
                                      shlex.quote(directory + "/" + Path(p.library).name)).split()[0]
            if copied_library_hash != library_digest:
                raise dev.RunError(p.host + ": copied library digest changed")
        linkage = ssh(p.host, f"LD_LIBRARY_PATH={quoted} ldd {quoted}/throughput_test")
        if "not found" in linkage:
            raise dev.RunError(p.host + ": missing client shared-library dependency")
        placement = ssh(p.host, "numactl --hardware; ip route get " + shlex.quote(self.local.head_addr))
        self.evidence[p.host] = {"directory": directory, "source_binary_sha256": self.hashes[p.host],
                                 "config_sha256": remote_config_hash,
                                 "library_sha256": library_digest, "ldd": linkage, "topology_and_route": placement}

    def launch(self, p):
        directory = self.directories[p.host]
        quoted = shlex.quote(directory)
        command = ["numactl", f"--cpunodebind={p.node}", f"--membind={p.node}", "./throughput_test",
                   "--config", "config.json", "--head_addr", self.local.head_addr,
                   "-t", "5", "-o", "5", "-a", "1", "-r", "0", "-n", str(self.options.threads_per_broker),
                   "-m", "4096", "-s", str(p.payload_bytes), "--sequencer", "EMBARCADERO"]
        variables = {"LD_LIBRARY_PATH": directory, "EMBARCADERO_RUNTIME_MODE": "throughput",
                     "EMBARCADERO_SESSION_RTO_MIN_MS": "60000", "EMBARCADERO_ACK_TIMEOUT_SEC": "120",
                     "EMBARCADERO_E2E_TIMEOUT_SEC": "120", "EMBARCADERO_QUEUE_POOL_MAX_BYTES": str(2 * dev.GIB),
                     "EMBARCADERO_PUBLISH_BROKER_ALLOWLIST": "0,1,2,3", "EMBAR_VALIDATE_ORDER": "0",
                     "EMBAR_USE_HUGETLB": "1" if self.options.hugetlb else "0",
                     "EMBARCADERO_PUSH_READY_FILE": directory + "/ready",
                     "EMBARCADERO_PUSH_GO_FILE": directory + "/go"}
        environment = " ".join(f"{key}={shlex.quote(value)}" for key, value in variables.items())
        shell = (f"cd {quoted} && ( {environment} {' '.join(map(shlex.quote, command))} "
                 "& child=$!; echo $child > client.pid; wait $child; rc=$?; date +%s%N > end.ns; exit $rc )")
        return [*SSH, p.host, shell]

    def verify(self, p):
        directory = self.directories[p.host]
        value = ssh(p.host, "cat " + shlex.quote(directory + "/client.pid"))
        if not value.isdecimal():
            raise dev.RunError(p.host + ": invalid remote PID")
        pid = int(value)
        info = ssh(p.host, f"readlink /proc/{pid}/exe; sha256sum /proc/{pid}/exe; "
                   f"grep '^Cpus_allowed_list:' /proc/{pid}/status")
        lines = info.splitlines()
        if len(lines) != 3 or lines[0] != directory + "/throughput_test" or lines[1].split()[0] != self.hashes[p.host]:
            raise dev.RunError(p.host + ": executing an unexpected client binary")
        if p.library:
            mapping = ssh(p.host, f"grep -F {shlex.quote(directory + '/' + Path(p.library).name)} "
                          f"/proc/{pid}/maps | head -1")
            if not mapping:
                raise dev.RunError(p.host + ": private library was not mapped")
            self.evidence[p.host]["library_mapping"] = mapping
        self.evidence[p.host].update(pid=pid, executed_sha256=lines[1].split()[0], cpu_mask=lines[2])

    def collect(self, p):
        directory = self.directories[p.host]
        target = self.run_dir / ("remote-" + p.host)
        target.mkdir()
        for name in ("client.pid", "ready", "go", "end.ns"):
            result = subprocess.run(["scp", "-q", f"{p.host}:{directory}/{name}", str(target / name)],
                                    capture_output=True, text=True, timeout=20)
            if result.returncode:
                raise dev.RunError(p.host + ": missing remote artifact " + name)
        files = ssh(p.host, "find " + shlex.quote(directory) + " -maxdepth 2 -type f -printf '%P\\n'")
        (target / "files.txt").write_text(files + "\n")

    def cleanup(self, p):
        directory = self.directories.get(p.host)
        if not directory:
            return
        quoted = shlex.quote(directory)
        if p.host in self.launched:
            # An SSH transport may fail between process launch and PID-file
            # publication. In that window we cannot prove what to terminate;
            # retain the private directory for inspection instead of erasing it.
            for _ in range(20):
                if ssh(p.host, f"if test -f {quoted}/client.pid; then echo ready; fi") == "ready":
                    break
                time.sleep(0.1)
            else:
                raise dev.RunError(p.host + ": launched client has no owned PID file; retained " + directory)
        script = (f"if test -f {quoted}/client.pid; then pid=$(cat {quoted}/client.pid); "
                  "case $pid in *[!0-9]*|'') exit 2;; esac; "
                  "if test -e /proc/$pid/exe; then "
                  "actual=$(readlink /proc/$pid/exe); "
                  f"test \"$actual\" = {shlex.quote(directory + '/throughput_test')} || exit 3; "
                  "kill -TERM $pid; fi; fi; "
                  f"rm -rf -- {quoted}")
        ssh(p.host, script, timeout=20)

    def execute(self, owned, manifest, _hardware, run_dir):
        try:
            for p in self.publishers:
                self.setup(p)
            for p in self.publishers:
                self.children[p.host] = owned.start("remote-" + p.host, self.launch(p))
                self.launched.add(p.host)
            deadline = time.monotonic() + self.options.deadline_seconds
            while time.monotonic() < deadline:
                owned.check_brokers()
                if any(child.poll() is not None for child in self.children.values()):
                    raise dev.RunError("remote client exited before push-ready barrier")
                ready = [p for p in self.publishers if ssh(p.host, "if test -f " +
                         shlex.quote(self.directories[p.host] + "/ready") + "; then echo ready; fi") == "ready"]
                if len(ready) == len(self.publishers):
                    break
                time.sleep(0.1)
            else:
                raise dev.RunError("remote publishers did not become ready")
            for p in self.publishers:
                self.verify(p)
            go_ns = time.time_ns() + 2_000_000_000
            for p in self.publishers:
                ssh(p.host, f"printf '%s\\n' {go_ns} > " + shlex.quote(self.directories[p.host] + "/go"))
            manifest.update(status="running", remote_evidence=self.evidence, remote_go_wall_ns=go_ns)
            dev.write_json(run_dir / "manifest.json", manifest)
            deadline = time.monotonic() + self.options.deadline_seconds
            while time.monotonic() < deadline:
                owned.check_brokers()
                if all(child.poll() is not None for child in self.children.values()):
                    break
                time.sleep(0.1)
            else:
                raise dev.RunError("remote publisher deadline exceeded")
            if any(child.returncode != 0 for child in self.children.values()):
                raise dev.RunError("remote publisher failed; inspect remote logs")
            for p in self.publishers:
                self.collect(p)
        finally:
            manifest["remote_evidence"] = self.evidence
            manifest["remote_cleanup"] = {}
            for p in self.publishers:
                if p.host in self.directories:
                    try:
                        self.cleanup(p)
                        manifest["remote_cleanup"][p.host] = "removed owned directory"
                    except (OSError, dev.RunError) as error:
                        manifest["remote_cleanup"][p.host] = "FAILED: " + str(error)
            dev.write_json(run_dir / "manifest.json", manifest)

    def validate(self, run_dir):
        rows = []
        for p in self.publishers:
            log = (run_dir / ("remote-" + p.host + ".log")).read_text(errors="replace")
            result = ack_and_routing(log, p.payload_bytes // 4096, range(4), 60000)
            starts = re.findall(r"Publisher push start \(wall ns\): (\d+)", log)
            durations = re.findall(r"Publish test completed in ([0-9.]+) seconds", log)
            if len(starts) != 1 or len(durations) != 1 or float(durations[0]) <= 0:
                raise dev.RunError(p.host + ": missing one complete timing record")
            result.update(host=p.host, start_wall_ns=int(starts[0]), duration_seconds=float(durations[0]))
            rows.append(result)
        routed = {broker: sum(row["sent_by_broker"].get(broker, 0) for row in rows) for broker in range(4)}
        if any(count <= 0 for count in routed.values()):
            raise dev.RunError("not every broker received publisher traffic")
        start = min(row["start_wall_ns"] for row in rows)
        end = max(row["start_wall_ns"] + round(row["duration_seconds"] * 1e9) for row in rows)
        seconds = (end - start) / 1e9
        return {"clients": rows, "sent_by_broker": routed,
                "total_payload_bytes": sum(p.payload_bytes for p in self.publishers),
                "complete_transfer_seconds_approx": seconds,
                "ack_completion_decimal_gb_s_approx": sum(p.payload_bytes for p in self.publishers) / seconds / 1e9,
                "metric_limit": "client durations have 0.01-second precision and cross-host wall clocks are required"}

    def check_final_artifacts(self, run_dir):
        cleanup = json.loads((run_dir / "manifest.json").read_text()).get("remote_cleanup", {})
        if cleanup and (len(cleanup) != len(self.publishers) or
                        any(value != "removed owned directory" for value in cleanup.values())):
            raise dev.RunError("remote owned cleanup was incomplete")


def main(argv=None):
    options, remaining, local = parse_args(argv)
    dev.REGION_BYTES = 96 * dev.GIB
    dev.SEGMENT_BYTES = 8 * dev.GIB
    dev.effective_config = remote_config
    workload = RemotePublishers(options, local)
    return dev.main(remaining, profile=dev.SmokeProfile(
        name="cxl-remote-order5-ack1-rf0", audit_enabled=False,
        validator=workload.validate, workload=workload))


if __name__ == "__main__":
    sys.exit(main())
