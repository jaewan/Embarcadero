#!/usr/bin/env python3
"""Run an owned, bounded DRAM smoke cluster. Python standard library only."""

import argparse
import csv
from dataclasses import dataclass
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
GIB = 1024 ** 3
MIB = 1024 ** 2
REGION_BYTES = 64 * GIB
CXL_BASE_ADDR = 0x400000000000
SEGMENT_BYTES = 256 * MIB
PAYLOAD_BYTES = 32 * MIB
MESSAGE_BYTES = 4096
CONTROL_PORT = 12140
DATA_PORT = 1214


class RunError(RuntimeError):
    pass


def cpu_set(value):
    result = set()
    if not value.strip():
        return result
    for part in value.strip().split(","):
        bounds = part.split("-")
        if len(bounds) == 1:
            result.add(int(bounds[0]))
        elif len(bounds) == 2 and int(bounds[0]) <= int(bounds[1]):
            result.update(range(int(bounds[0]), int(bounds[1]) + 1))
        else:
            raise ValueError("invalid CPU/node range: " + part)
    return result


def read_optional(path):
    try:
        return Path(path).read_text().strip()
    except (OSError, UnicodeError):
        return None


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")
    temporary.replace(path)


def binary_digest(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(MIB), b""):
            digest.update(block)
    return digest.hexdigest()


def verify_executed_binary(child, declared_path, expected_digest, evidence):
    """Check the executable inode mapped by an owned child, retaining failures."""
    executable = Path(f"/proc/{child.pid}/exe")
    evidence.update(pid=child.pid, declared_path=str(declared_path),
                    declared_sha256=expected_digest, observed_path=os.readlink(executable),
                    observed_sha256=binary_digest(executable), observed_monotonic=time.monotonic())
    if (evidence["observed_path"] != str(declared_path.resolve()) or
            evidence["observed_sha256"] != expected_digest):
        raise RunError("executed binary differs from preflight: " + str(declared_path))


def build_provenance(build_dir):
    cache = read_optional(build_dir / "CMakeCache.txt")
    selected = {}
    if cache is not None:
        for line in cache.splitlines():
            match = re.fullmatch(r"([^:#]+):[^=]+=(.*)", line)
            if match and (match[1] in {"CMAKE_BUILD_TYPE", "CMAKE_CXX_COMPILER",
                                      "EMBARCADERO_NATIVE_ARCH", "EMBARCADERO_SANITIZER"} or
                          match[1].startswith("CMAKE_CXX_FLAGS")):
                selected[match[1]] = match[2]
    commands = build_dir / "compile_commands.json"
    return {"cmake_cache": selected, "cache_available": cache is not None,
            "compile_commands_sha256": binary_digest(commands) if commands.is_file() else None,
            "comparison_requirement": "match compiler, build type, ISA, sanitizer, and observed PBR reservation mode"}


def source_provenance(directory):
    """Describe the runner tree; this does not attest which sources built binaries."""
    commands = {}
    for field, command in (("git_revision", ["git", "rev-parse", "HEAD"]),
                           ("git_status", ["git", "status", "--porcelain"])):
        try:
            result = subprocess.run(command, cwd=directory, capture_output=True,
                                    text=True, timeout=10)
            commands[field] = {"exit_code": result.returncode,
                               "stdout": result.stdout.strip(), "stderr": result.stderr.strip()}
        except (OSError, subprocess.TimeoutExpired) as error:
            commands[field] = {"exit_code": None, "stdout": "", "stderr": str(error)}
    revision, status = commands["git_revision"], commands["git_status"]
    if (revision["exit_code"] != 0 or status["exit_code"] != 0 or
            not re.fullmatch(r"[0-9a-fA-F]{40}|[0-9a-fA-F]{64}", revision["stdout"])):
        label = ("unavailable: no verified Git revision/status; a source archive or non-Git tree "
                 "requires separate archive attestation")
    elif status["stdout"]:
        label = ("incomplete: working tree is dirty; binary hashes identify executed artifacts, "
                 "not reproducible source")
    else:
        label = "clean revision"
    return {"git_revision": revision["stdout"], "git_status": status["stdout"],
            "git_commands": commands, "source_directory": str(directory),
            "source_provenance": label,
            "source_provenance_scope": "runner source tree only; compiled source association requires build evidence"}


def effective_config(brokers):
    config = json.loads((ROOT / "config/dev-dram-local.json").read_text())
    config["embarcadero"]["broker"]["max_brokers"] = brokers
    config["embarcadero"]["cluster"]["data_broker_ids"] = list(range(brokers))
    return config


def child_environment(shm_name, brokers):
    # Historical experiment switches must not silently change this profile.
    removed = sorted(name for name in os.environ if
                     name.startswith(("EMBAR", "SCALOG", "CORFU", "LAZYLOG")) or
                     name == "NUM_BROKERS")
    env = {name: value for name, value in os.environ.items() if name not in removed}
    selected = {
        "EMBARCADERO_CXL_SHM_NAME": shm_name,
        # Every broker must share one virtual base while shared raw pointers
        # remain in the ABI. Independent fallbacks can diverge under PIE ASLR.
        "EMBARCADERO_CXL_BASE_ADDR": hex(CXL_BASE_ADDR),
        "EMBARCADERO_HEAD_ADDR": "127.0.0.1",
        "EMBARCADERO_REQUIRED_CXL_SEGMENTS": str(brokers),
        "EMBARCADERO_CXL_ZERO_MODE": "full",
        "EMBARCADERO_CXL_MAP_POPULATE": "1",
        "EMBARCADERO_REPLICATION_FACTOR": "0",
        "EMBARCADERO_CHAIN_REPLICATION_SINK": "memory-copy",
        "EMBARCADERO_RUNTIME_MODE": "throughput",
        "EMBARCADERO_SESSION_RTO_MIN_MS": "60000",
        "EMBARCADERO_ACK_TIMEOUT_SEC": "20",
        "EMBARCADERO_E2E_TIMEOUT_SEC": "20",
        "EMBARCADERO_E2E_AUDIT_MODE": "stream",
        "EMBARCADERO_SUBSCRIBER_RETAINED_BYTES": str(256 * MIB),
        "EMBARCADERO_SUBSCRIBER_MAX_MESSAGES": "262144",
        "EMBARCADERO_SUB_CONNECTIONS": "1",
        "EMBARCADERO_QUEUE_POOL_MAX_BYTES": str(512 * MIB),
        "EMBAR_USE_HUGETLB": "0",
        "EMBAR_VALIDATE_ORDER": "1",
        "NUM_BROKERS": str(brokers),
    }
    env.update(selected)
    return env, selected, removed


def topology(cxl_node=None):
    allowed = os.sched_getaffinity(0)
    result = {"online_nodes": read_optional("/sys/devices/system/node/online"),
              "allowed_cpus": sorted(allowed), "nodes": {}}
    for node in ((0, 1, cxl_node) if cxl_node is not None else (0, 1)):
        base = Path(f"/sys/devices/system/node/node{node}")
        cpus = read_optional(base / "cpulist")
        memory = read_optional(base / "meminfo")
        if cpus is None or memory is None:
            raise RunError(f"dev-dram-local requires NUMA node {node}")
        usable = sorted(cpu_set(cpus) & allowed)
        if not usable and node != cxl_node:
            raise RunError(f"current CPU affinity excludes NUMA node {node}")
        if node == cxl_node and usable:
            raise RunError(f"CXL NUMA node {node} must be memory-only")
        match = re.search(r"MemFree:\s+(\d+) kB", memory)
        result["nodes"][str(node)] = {"cpus": usable,
            "free_bytes": int(match[1]) * 1024 if match else 0,
            "distance": read_optional(base / "distance")}
    result["thp_enabled"] = read_optional("/sys/kernel/mm/transparent_hugepage/enabled")
    result["tmpfs_thp"] = read_optional("/sys/kernel/mm/transparent_hugepage/shmem_enabled")
    result["numa_balancing"] = read_optional("/proc/sys/kernel/numa_balancing")
    result["kernel"] = os.uname().release
    return result


def memory_preflight(hardware, brokers, cxl_node=None):
    required = REGION_BYTES + (2 * brokers + 2) * GIB
    available = shutil.disk_usage("/dev/shm").free
    if available < REGION_BYTES + GIB:
        raise RunError("/dev/shm needs 65 GiB free for the 64 GiB region and headroom")
    if hardware["nodes"]["1"]["free_bytes"] < (2 * brokers * GIB if cxl_node is not None else REGION_BYTES + 2 * brokers * GIB):
        raise RunError("NUMA node 1 has insufficient free memory for this cluster")
    if cxl_node is not None and hardware["nodes"][str(cxl_node)]["free_bytes"] < REGION_BYTES + GIB:
        raise RunError(f"CXL NUMA node {cxl_node} needs 64 GiB plus headroom")
    if hardware["nodes"]["0"]["free_bytes"] < 2 * GIB:
        raise RunError("NUMA node 0 needs at least 2 GiB free for the client")
    status = Path("/proc/self/status").read_text()
    allowed = re.search(r"^Mems_allowed_list:\s*(.*)$", status, re.MULTILINE)
    required_nodes = {0, 1} | ({cxl_node} if cxl_node is not None else set())
    if allowed is None or not required_nodes.issubset(cpu_set(allowed[1])):
        raise RunError("process/cgroup memory policy excludes a required NUMA node")
    soft, _ = resource.getrlimit(resource.RLIMIT_AS)
    if soft != resource.RLIM_INFINITY and soft < REGION_BYTES + 2 * GIB:
        raise RunError("RLIMIT_AS cannot accommodate one broker's shared mapping")
    # Respect all ancestors: the process's leaf can say 'max' under a limited slice.
    limits = []
    cgroup = Path("/proc/self/cgroup").read_text()
    unified = next((line[3:] for line in cgroup.splitlines() if line.startswith("0::")), None)
    if unified is None:
        raise RunError("memory preflight currently requires cgroup v2")
    root = Path("/sys/fs/cgroup")
    current = root / unified.lstrip("/")
    while True:
        maximum = read_optional(current / "memory.max")
        usage = read_optional(current / "memory.current")
        if maximum and maximum != "max" and usage:
            remaining = max(0, int(maximum) - int(usage))
            limits.append({"path": str(current), "remaining_bytes": remaining})
            if remaining < required:
                raise RunError(f"cgroup memory budget is too small: {current}")
        if current == root:
            break
        current = current.parent
    return {"shm_available_bytes": available, "additional_memory_budget_bytes": required,
            "cgroup_limits": limits, "rlimit_as": soft}


class ClusterLock:
    """Serialize our fixed-port profile. Never unlink a live lock inode."""

    def __enter__(self):
        self.path = Path(tempfile.gettempdir()) / f"embarcadero-dev-{os.getuid()}.lock"
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid():
            os.close(fd)
            raise RunError("unsafe development lock file: " + str(self.path))
        self.file = os.fdopen(fd, "r+")
        try:
            fcntl.flock(self.file, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            self.file.close()
            raise RunError("another dev cluster owns the fixed-port profile") from error
        return self

    def __exit__(self, *_):
        self.file.close()


def check_ports(ports):
    for port in ports:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
            probe.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            try:
                probe.bind(("0.0.0.0", port))
            except OSError as error:
                raise RunError(f"port {port} is occupied; no processes were killed") from error


def layout_preflight(binary, config_path, env):
    result = subprocess.run([str(binary), "--config", str(config_path), "--print-layout"],
                            env=env, capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise RunError("broker --print-layout failed; rebuild this revision:\n" + result.stderr[-4000:])
    try:
        layout = json.loads(result.stdout)
        for field in ("region_bytes", "metadata_bytes", "payload_bytes", "segment_size", "segment_count"):
            if type(layout[field]) is not int or layout[field] <= 0:
                raise RunError("broker reports invalid layout field: " + field)
        if layout["region_bytes"] != REGION_BYTES or layout["segment_size"] != SEGMENT_BYTES:
            raise RunError("broker reports unexpected region or segment size")
        if layout["metadata_bytes"] + layout["payload_bytes"] > REGION_BYTES:
            raise RunError("broker reports an invalid memory layout")
        if layout["segment_count"] < int(env["EMBARCADERO_REQUIRED_CXL_SEGMENTS"]):
            raise RunError("too few payload segments for the requested brokers")
    except (ValueError, KeyError, TypeError) as error:
        raise RunError("invalid JSON from broker --print-layout") from error
    return layout


class OwnedProcesses:
    def __init__(self, run_dir, env, shutdown_timeout):
        self.run_dir = run_dir
        self.env = env
        self.shutdown_timeout = shutdown_timeout
        self.children = []
        self.forced = []
        self.closed = False
        self.stop_requested = False

    def start(self, name, command, *, environment=None, pass_fds=(), cwd=None):
        if self.stop_requested or self.closed:
            raise RunError("cannot start a child after owned cleanup begins")
        # Optional inherited controls are scoped to this child. The ordinary
        # smoke/performance callers retain their existing environment and FDs.
        child_env = self.env if environment is None else {**self.env, **environment}
        with (self.run_dir / f"{name}.log").open("wb") as log:
            child = subprocess.Popen(command, cwd=self.run_dir if cwd is None else cwd, env=child_env,
                                     stdin=subprocess.DEVNULL, stdout=log, stderr=log,
                                     start_new_session=True, pass_fds=pass_fds)
        self.children.append((name, child))
        return child

    def check_brokers(self):
        for name, child in self.children:
            if name.startswith("broker-") and child.poll() is not None:
                raise RunError(f"{name} exited with {child.returncode}; see {name}.log")

    def request_stop(self):
        """Signal once; callers may then cancel barriers before waiting."""
        if self.stop_requested:
            return
        self.stop_requested = True
        # Signal only groups created by start_new_session above, never a name match.
        for _, child in reversed(self.children):
            try:
                os.killpg(child.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def close(self):
        if self.closed:
            return
        self.request_stop()
        deadline = time.monotonic() + self.shutdown_timeout
        for name, child in reversed(self.children):
            try:
                child.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                self.forced.append(name)
                os.killpg(child.pid, signal.SIGKILL)
                child.wait(timeout=5)
        # A command can exit while one of its children remains in the owned group.
        for name, child in reversed(self.children):
            try:
                os.killpg(child.pid, 0)
            except ProcessLookupError:
                continue
            if name not in self.forced:
                self.forced.append(name)
            try:
                os.killpg(child.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
        self.closed = True


def wait_ready(child, owned, deadline):
    marker = Path(f"/tmp/embarlet_{child.pid}_ready")
    while time.monotonic() < deadline:
        owned.check_brokers()
        try:
            info = marker.stat()
            # A stale PID marker from an earlier run cannot establish readiness.
            if info.st_uid == os.getuid() and info.st_mtime >= owned.start_wall:
                return marker
        except FileNotFoundError:
            pass
        time.sleep(0.05)
    raise RunError(f"broker {child.pid} did not become ready before the startup deadline")


def placement_snapshot(child, expected_cpus, node, run_dir, name, shm_name=None):
    masks = {}
    for task in Path(f"/proc/{child.pid}/task").glob("*"):
        status = read_optional(task / "status")
        if status is None:
            continue
        match = re.search(r"^Cpus_allowed_list:\s*(.*)$", status, re.MULTILINE)
        if match:
            masks[task.name] = match[1]
            if not cpu_set(match[1]).issubset(set(expected_cpus)):
                raise RunError(f"{name} thread {task.name} escaped its CPU assignment")
    numa = read_optional(f"/proc/{child.pid}/numa_maps")
    if numa is not None:
        (run_dir / f"{name}.numa_maps").write_text(numa + "\n")
    pages = {}
    if shm_name and numa:
        for line in numa.splitlines():
            if f"file=/dev/shm/{shm_name.lstrip('/')} " in line:
                for found, count in re.findall(r"\bN(\d+)=(\d+)", line):
                    pages[found] = pages.get(found, 0) + int(count)
        if not pages or any(int(key) != node and count for key, count in pages.items()):
            raise RunError(f"{name} shared mapping is not entirely resident on NUMA node {node}: {pages}")
    return {"thread_cpu_masks": masks, "shared_mapping_pages": pages}


def validate_smoke(run_dir):
    with (run_dir / "throughput_benchmark_summary.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1:
        raise RunError("client did not write exactly one E2E result")
    row = rows[0]
    for field in ("publish_goodput_mbps", "e2e_goodput_mbps"):
        value = float(row[field])
        if not math.isfinite(value) or value <= 0:
            raise RunError("client reported unsuccessful E2E transfer: " + field)
    if int(row["total_message_size_bytes"]) != PAYLOAD_BYTES:
        raise RunError("client workload differs from the bounded smoke workload")
    log = (run_dir / "client.log").read_text(errors="replace")
    if re.search(r"DEBUG Check Failed|Order Level 5 check FAILED|Subscriber::Poll timeout|"
                 r"retransmit_attempts=[1-9]|\[ORDERED_DELIVERY_AUDIT\] status=failed", log):
        raise RunError("client ordering/completion/retransmission checks failed; see client.log")
    audits = re.findall(r"\[ORDERED_DELIVERY_AUDIT\] status=passed messages=(\d+) expected=(\d+) "
                        r"payload_bytes=(\d+) duplicates=(\d+) parse_errors=(\d+) export_gaps=(\d+) "
                        r"indexed_payload=1\b", log)
    expected = PAYLOAD_BYTES // MESSAGE_BYTES
    if len(audits) != 1 or tuple(map(int, audits[0])) != (expected, expected, PAYLOAD_BYTES, 0, 0, 0):
        raise RunError("client did not prove exact ordered delivery and payload integrity; see client.log")
    return {"result": row, "audited_messages": expected,
            "scope": "ACK1 E2E with exact message count, contiguous global order, indexed payload sequence and full payload comparison, and zero duplicate/parser/export-gap counters; separate fault tests cover fencing"}


@dataclass(frozen=True)
class SmokeProfile:
    """Internal adapter for documented tests sharing this owned lifecycle."""
    name: str = "dev-dram-local"
    order: int = 5
    audit_enabled: bool = True
    validator: object = validate_smoke
    # Optional tools-only adapter: prepare configuration/environment, describe
    # commands, and execute clients using this same owned broker/region lifecycle.
    # The standard smoke and legacy startup adapters leave this unset.
    workload: object = None

    def __post_init__(self):
        if self.order not in (0, 5) or not callable(self.validator):
            raise ValueError("unsupported smoke profile")


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build")
    parser.add_argument("--brokers", type=int, choices=(1, 3), default=1)
    parser.add_argument("--run-root", type=Path, default=Path(tempfile.gettempdir()))
    parser.add_argument("--dry-run", action="store_true", help="write manifest and config; do not spawn brokers or allocate shared memory")
    parser.add_argument("--automatic-mapping", action="store_true",
                        help="exercise production head-selected mapping and follower descriptor agreement")
    parser.add_argument("--physical-cxl", action="store_true",
                        help="bind the real-backend shared mapping to memory-only NUMA node 2")
    parser.add_argument("--startup-timeout", type=float, default=120)
    parser.add_argument("--shutdown-timeout", type=float, default=10)
    args = parser.parse_args(argv)
    if not 1 <= args.startup_timeout <= 300 or not 1 <= args.shutdown_timeout <= 60:
        parser.error("startup timeout must be 1..300 seconds and shutdown timeout 1..60 seconds")
    return args


def interrupted(number, _frame):
    raise RunError(f"interrupted by signal {number}")


def main(argv=None, *, profile=None):
    profile = profile or SmokeProfile()
    args = parse_args(argv)
    run_dir = Path(tempfile.mkdtemp(prefix=f"embarcadero-dev-{os.getuid()}-", dir=args.run_root)).resolve()
    print(f"Run artifacts: {run_dir}", flush=True)
    manifest = {"profile": profile.name, "backend": "numa2-real-cxl" if args.physical_cxl else "dram-emulation",
                "physical_cxl_requested": args.physical_cxl, "cxl_evidence": False,
                "run_dir": str(run_dir), "status": "preflight", "brokers": args.brokers,
                "order": profile.order, "audit_enabled": profile.audit_enabled,
                "ack": 1, "replication_factor": 0, "persistence": "none",
                "application_payload_bytes": PAYLOAD_BYTES, "message_bytes": MESSAGE_BYTES,
                "initial_payload_reservation_upper_bound_bytes":
                    (PAYLOAD_BYTES // MESSAGE_BYTES) * (MESSAGE_BYTES + 128) + 2 * MIB * args.brokers,
                "client_deadline_seconds": 30, "startup_timeout_seconds": args.startup_timeout,
                "shutdown_timeout_seconds": args.shutdown_timeout,
                "limitations": ["fixed-port profile serialized per user; other users/legacy launchers do not take this lock",
                                "smoke only; not sustained throughput or a physical CXL performance claim",
                                "RTO floor exceeds client deadline; disconnect retry bounds remain a runtime responsibility"]}
    owned = None
    run_lock = None
    shm_identity = None
    markers = []
    shm_path = Path("/dev/shm") / run_dir.name
    shm_name = "/" + run_dir.name
    exit_code = 1
    old_handlers = {}
    try:
        hardware = topology(2 if args.physical_cxl else None)
        manifest["hardware"] = hardware
        manifest["memory"] = memory_preflight(hardware, args.brokers, 2 if args.physical_cxl else None)
        broker = args.build_dir.resolve() / "bin/embarlet"
        client = args.build_dir.resolve() / "bin/throughput_test"
        numactl = shutil.which("numactl")
        if not numactl:
            raise RunError("numactl is required for the explicit CPU and memory policy")
        for binary in (broker, client):
            if not os.access(binary, os.X_OK):
                raise RunError("missing executable: " + str(binary))
        config = effective_config(args.brokers)
        if args.physical_cxl:
            config["embarcadero"]["cxl"]["numa_node"] = 2
            dax_path = Path(config["embarcadero"]["cxl"]["device_path"])
            if dax_path.exists():
                raise RunError("--physical-cxl owned placement check currently requires the NUMA-2 shared-memory fallback; DAX device exists: " + str(dax_path))
        config_path = run_dir / "effective-config.yaml"
        write_json(config_path, config)  # JSON is valid YAML, accepted by yaml-cpp.
        env, selected, removed = child_environment(shm_name, args.brokers)
        if args.automatic_mapping:
            env.pop("EMBARCADERO_CXL_BASE_ADDR", None)
            selected.pop("EMBARCADERO_CXL_BASE_ADDR", None)
        manifest["mapping_policy"] = "automatic head selection; followers use published descriptor" if args.automatic_mapping else "explicit common base"
        env["EMBAR_VALIDATE_ORDER"] = selected["EMBAR_VALIDATE_ORDER"] = "1" if profile.audit_enabled else "0"
        if profile.workload is not None:
            profile.workload.prepare(config, env, selected, manifest, hardware, args, run_dir)
            write_json(config_path, config)
        manifest.update(environment=selected, ignored_environment_names=removed,
                        shared_memory=str(shm_path), cleanup_targets=[str(shm_path)])
        manifest["layout"] = layout_preflight(broker, config_path, env)
        manifest["binaries"] = {str(path): binary_digest(path) for path in (broker, client)}
        manifest["build"] = build_provenance(args.build_dir.resolve())
        manifest.update(source_provenance(ROOT))
        def binding(node):
            cpus = hardware["nodes"][str(node)]["cpus"]
            memory_nodes = "1,2" if node == 1 and args.physical_cxl else str(node)
            return [numactl, "--physcpubind=" + ",".join(map(str, cpus)), f"--membind={memory_nodes}"]
        commands = []
        for broker_id in range(args.brokers):
            role = ["--head"] if broker_id == 0 else ["--follower", f"127.0.0.1:{CONTROL_PORT}"]
            commands.append(binding(1) + [str(broker)] + ([] if args.physical_cxl else ["--emul"]) + ["--config", str(config_path)] + role)
        client_command = binding(0) + [str(client), "--config", str(config_path), "--head_addr", "127.0.0.1",
            "-t", "1", "-o", str(profile.order), "-a", "1", "-r", "0", "-n", "1", "-m", str(MESSAGE_BYTES),
            "-s", str(PAYLOAD_BYTES), "--sequencer", "EMBARCADERO"]
        if profile.workload is not None:
            client_command = None
            manifest["client_commands"] = profile.workload.commands(binding(0), client, config_path, run_dir)
        ports = [CONTROL_PORT] + list(range(DATA_PORT, DATA_PORT + args.brokers))
        manifest.update(broker_commands=commands, client_command=client_command, ports=ports,
                        cpu_budget="all allowed CPUs on the designated node; shared among brokers, includes available SMT siblings")
        write_json(run_dir / "manifest.json", manifest)
        acquired_lock = ClusterLock()
        acquired_lock.__enter__()
        run_lock = acquired_lock
        check_ports(ports)
        if args.dry_run:
            manifest["status"] = "dry-run"
            exit_code = 0
        else:
            for signum in (signal.SIGINT, signal.SIGTERM):
                old_handlers[signum] = signal.getsignal(signum)
                signal.signal(signum, interrupted)
            fd = os.open(shm_path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW, 0o600)
            info = os.fstat(fd)
            shm_identity = (info.st_dev, info.st_ino)
            os.close(fd)
            owned = OwnedProcesses(run_dir, env, args.shutdown_timeout)
            owned.start_wall = time.time()
            deadline = time.monotonic() + args.startup_timeout
            manifest["placement"] = {}
            manifest["executed_broker_binaries"] = {}
            for broker_id, command in enumerate(commands):
                name = f"broker-{broker_id}"
                child = owned.start(name, command)
                markers.append(Path(f"/tmp/embarlet_{child.pid}_ready"))
                manifest["pids"] = {child_name: process.pid for child_name, process in owned.children}
                manifest["cleanup_targets"] = [str(shm_path)] + list(map(str, markers))
                write_json(run_dir / "manifest.json", manifest)
                wait_ready(child, owned, deadline)
                evidence = manifest["executed_broker_binaries"].setdefault(name, {})
                verify_executed_binary(child, broker, manifest["binaries"][str(broker)], evidence)
                manifest["placement"][name] = placement_snapshot(child,
                    hardware["nodes"]["1"]["cpus"], 2 if args.physical_cxl else 1,
                    run_dir, name, shm_name if broker_id == 0 or args.physical_cxl else None)
                if args.physical_cxl and broker_id == 0:
                    broker_log = (run_dir / f"{name}.log").read_text(errors="replace")
                    if "CXL region bound to NUMA node 2" not in broker_log:
                        raise RunError("real backend did not report successful NUMA-2 binding")
                    manifest["cxl_evidence"] = True
                    manifest["cxl_placement_scope"] = (
                        "real-backend shared mapping resident on memory-only NUMA node 2; "
                        "PCI device identity is not established by this runner")
            if args.automatic_mapping:
                manifest["mapping_bases"] = {}
                for broker_id in range(args.brokers):
                    name = f"broker-{broker_id}"
                    log = (run_dir / f"{name}.log").read_text(errors="replace")
                    logged = re.findall(r"CXL mapping successful at address:\s*(0x[0-9a-fA-F]+)\b", log)
                    observed = [line.split()[0] for line in (run_dir / f"{name}.numa_maps").read_text().splitlines()
                                if f"file=/dev/shm/{shm_name.lstrip('/')} " in line]
                    if len(logged) != 1 or len(observed) != 1 or int(logged[0], 16) != int(observed[0], 16):
                        raise RunError(name + " automatic mapping could not be verified from log and NUMA map")
                    manifest["mapping_bases"][name] = hex(int(logged[0], 16))
                if len(set(manifest["mapping_bases"].values())) != 1:
                    raise RunError("automatic follower mappings disagree with the head")
            if profile.workload is not None:
                profile.workload.execute(owned, manifest, hardware, run_dir)
            else:
                workload = owned.start("client", client_command)
                manifest["pids"] = {name: child.pid for name, child in owned.children}
                manifest["status"] = "running"
                write_json(run_dir / "manifest.json", manifest)
                deadline = time.monotonic() + 30
                placement_due = time.monotonic() + 0.25
                manifest["executed_binaries"] = {}
                while workload.poll() is None:
                    owned.check_brokers()
                    if time.monotonic() >= deadline:
                        raise RunError("client exceeded its 30-second smoke deadline")
                    if "client" not in manifest["executed_binaries"]:
                        try:
                            observed = os.readlink(f"/proc/{workload.pid}/exe")
                            # numactl can still own the process before exec.
                            if observed != str(Path(numactl).resolve()):
                                evidence = manifest["executed_binaries"].setdefault("client", {})
                                verify_executed_binary(workload, client, manifest["binaries"][str(client)], evidence)
                        except FileNotFoundError:
                            pass  # A quick exit is rejected below if identity was never observed.
                    if time.monotonic() >= placement_due:
                        manifest["placement"]["client"] = placement_snapshot(workload,
                            hardware["nodes"]["0"]["cpus"], 0, run_dir, "client")
                        placement_due = float("inf")
                    time.sleep(0.05)
                owned.check_brokers()
                if "client" not in manifest["placement"]:
                    manifest["placement"]["client"] = {
                        "status": "not observed: client exited before the first placement sample"}
                if workload.returncode:
                    raise RunError(f"client exited with {workload.returncode}; see client.log")
                if not manifest["executed_binaries"].get("client", {}).get("observed_sha256"):
                    raise RunError("client exited before executable identity could be verified")
            manifest["smoke"] = profile.validator(run_dir)
            manifest["status"] = "passed"
            exit_code = 0
        # Cleanup is inside the lock: don't admit a second run during teardown.
        if owned:
            owned.close()
            manifest["forced_shutdown"] = owned.forced
    except (RunError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        exit_code = 1
        manifest["status"] = "failed"
        manifest["error"] = str(error)
        print("dev_cluster: " + str(error), file=sys.stderr)
    finally:
        for signum, handler in old_handlers.items():
            signal.signal(signum, signal.SIG_IGN)
        if owned:
            try:
                owned.close()
                manifest["forced_shutdown"] = owned.forced
                manifest["exit_codes"] = {name: child.returncode for name, child in owned.children}
                if owned.forced and exit_code == 0:
                    manifest["status"] = "failed"
                    manifest["error"] = "forced termination was needed: " + ", ".join(owned.forced)
                    exit_code = 1
                if any(child.returncode != 0 for _, child in owned.children):
                    manifest["status"] = "failed"
                    manifest["cleanup_error"] = "nonzero child exit: " + repr(manifest["exit_codes"])
                    exit_code = 1
            except (OSError, subprocess.TimeoutExpired) as error:
                manifest["status"] = "failed"
                manifest["cleanup_error"] = str(error)
                exit_code = 1
        # Never unlink a mapping still used by an owned child, or an inode replaced by someone else.
        if shm_identity and (owned is None or all(child.poll() is not None for _, child in owned.children)):
            try:
                info = shm_path.lstat()
                if (info.st_dev, info.st_ino) == shm_identity:
                    shm_path.unlink()
            except FileNotFoundError:
                pass
        manifest["shared_memory_removed"] = not os.path.lexists(shm_path)
        if not manifest["shared_memory_removed"]:
            manifest["status"] = "failed"
            manifest["cleanup_error"] = "shared-memory path remains; owned inode was not safely removable"
            exit_code = 1
        for marker in markers:
            try:
                info = marker.lstat()
                if stat.S_ISREG(info.st_mode) and info.st_uid == os.getuid() and info.st_mtime >= owned.start_wall:
                    marker.unlink()
            except FileNotFoundError:
                pass
        for signum, handler in old_handlers.items():
            signal.signal(signum, handler)
        if run_lock is not None:
            run_lock.__exit__()
        manifest["pbr_reservation_modes"] = {
            path.stem: sorted(set(re.findall(r"PBR reservation mode=(atomic128|mutex)\b",
                                            path.read_text(errors="replace")))) or ["not observed"]
            for path in run_dir.glob("broker-*.log")}
        if profile.workload is not None:
            try:
                profile.workload.check_final_artifacts(run_dir)
            except (RunError, OSError) as error:
                manifest["status"] = "failed"
                manifest["cleanup_error"] = str(error)
                exit_code = 1
        write_json(run_dir / "manifest.json", manifest)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
