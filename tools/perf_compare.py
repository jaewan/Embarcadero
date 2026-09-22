#!/usr/bin/env python3
"""Matched DRAM broker pilot with one common audited client and owned cleanup."""

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import re
import resource
import shlex
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import tarfile
import time

import dev_cluster as dev

SEGMENT_BYTES = 4 * dev.GIB
BASELINE_REVISION = "ae9959dd2892af821878aa42d4e2c05fcb635466"
PROTOCOL_REVISION = "paired-dram-v3-fixed-mapping-base"
CLIENT_CPUS = list(range(0, 32))
BROKER_CPUS = [list(range(first, first + 32)) for first in (128, 160, 192)]


def fingerprint(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def compile_contract(args, compiler, macros):
    # Optimization, language and macro switches use their last occurrence;
    # architecture and -f options retain order because resets can interact.
    definitions = {}
    for arg in args:
        if arg.startswith(("-D", "-U")):
            name = arg[2:].split("=", 1)[0]
            definitions[name] = arg[:2] + arg[2:]
    def last(prefix):
        return next((arg for arg in reversed(args) if arg.startswith(prefix)), None)
    # Repeated identical switches add no information. Keep the final occurrence
    # without sorting away ordering among different architecture switches.
    architecture = list(reversed(list(dict.fromkeys(
        arg for arg in reversed(args) if arg.startswith("-m")))))
    return {"compiler_sha256": dev.binary_digest(Path(compiler)),
            "optimization": last("-O"), "standard": last("-std="),
            "debug": last("-g"), "definitions": definitions,
            "architecture": architecture,
            "other_codegen": [arg for arg in args if arg.startswith("-f") or arg == "-pthread"],
            "architecture_macros_sha256": hashlib.sha256(macros.encode()).hexdigest()}


def run_plan(brokers, pairs, qualification_only=False):
    runs = [{"brokers": brokers, "pair": None, "variant": version,
             "kind": "qualification", "included": False, "status": "planned"}
            for version in ("baseline", "candidate")]
    if not qualification_only:
        for pair in range(1, pairs + 1):
            versions = ("baseline", "candidate") if pair % 2 else ("candidate", "baseline")
            runs.extend({"brokers": brokers, "pair": pair, "variant": version,
                         "kind": "measurement", "included": True, "status": "planned"}
                        for version in versions)
    for sequence, entry in enumerate(runs, 1):
        entry["execution_sequence"] = sequence
    return runs


def cpu_assignment(hardware, brokers):
    assignment = {"client": CLIENT_CPUS, **{f"broker-{i}": BROKER_CPUS[i] for i in range(brokers)}}
    cores = {}
    physical = set()
    for role, cpus in assignment.items():
        node = "0" if role == "client" else "1"
        if not set(cpus).issubset(hardware["nodes"][node]["cpus"]):
            raise dev.RunError(f"{role} requires allowed CPUs {cpus[0]}..{cpus[-1]} on NUMA node {node}")
        for cpu in cpus:
            base = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology")
            socket_id = dev.read_optional(base / "physical_package_id")
            core_id = dev.read_optional(base / "core_id")
            siblings = dev.read_optional(base / "thread_siblings_list")
            if None in (socket_id, core_id, siblings):
                raise dev.RunError(f"cannot establish physical-core identity for CPU {cpu}")
            identity = (socket_id, core_id)
            if identity in physical:
                raise dev.RunError("requested CPU sets overlap physical cores through SMT siblings")
            physical.add(identity)
            cores[str(cpu)] = {"socket": socket_id, "core": core_id, "siblings": siblings}
    return assignment, cores


def effective_config(brokers):
    config = dev.effective_config(brokers)
    config["embarcadero"]["storage"]["segment_size"] = SEGMENT_BYTES
    runtime = config["client"]["runtime"]
    runtime["session_rto_min_ms_throughput"] = 2000
    runtime["ack_timeout_sec_throughput"] = 60
    return config


def environment(shm_name, brokers):
    env, selected, removed = dev.child_environment(shm_name, brokers)
    updates = {"EMBARCADERO_SESSION_RTO_MIN_MS": "2000", "EMBARCADERO_ACK_TIMEOUT_SEC": "60",
               "EMBARCADERO_E2E_TIMEOUT_SEC": "60"}
    env.update(updates)
    selected.update(updates)
    return env, selected, removed


def memory_preflight(hardware, brokers, payload_bytes):
    # Audit retains message payload until after ACK completion. Include payload,
    # wire overhead, publisher pool, subscriber buffers, and allocation headroom.
    client_budget = max(6 * dev.GIB, 2 * payload_bytes + 2 * dev.GIB)
    budget = dev.memory_preflight(hardware, brokers)
    extra = client_budget - 2 * dev.GIB
    if hardware["nodes"]["0"]["free_bytes"] < client_budget:
        raise dev.RunError(f"NUMA node 0 requires {client_budget // dev.GIB} GiB free for retained audit payload")
    required = budget["additional_memory_budget_bytes"] + extra
    for limit in budget["cgroup_limits"]:
        if limit["remaining_bytes"] < required:
            raise dev.RunError("cgroup memory budget cannot retain the audited client payload")
    budget.update(additional_memory_budget_bytes=required, client_node0_budget_bytes=client_budget)
    return budget


def layout_query(broker, config_path, env, brokers):
    result = subprocess.run([str(broker), "--config", str(config_path), "--print-layout"],
                            env=env, capture_output=True, text=True, timeout=15)
    if result.returncode:
        raise dev.RunError("candidate layout query failed: " + result.stderr[-2000:])
    layout = json.loads(result.stdout)
    if (layout["region_bytes"] != dev.REGION_BYTES or layout["segment_size"] != SEGMENT_BYTES or
            layout["segment_count"] < brokers or layout["metadata_bytes"] <= 32 * dev.GIB or
            layout["metadata_bytes"] + layout["payload_bytes"] > dev.REGION_BYTES):
        raise dev.RunError("candidate reported an incompatible pilot geometry")
    return layout


def build_identity(build_dir):
    result = dev.build_provenance(build_dir)
    cache = dev.read_optional(build_dir / "CMakeCache.txt") or ""
    source = re.search(r"^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$", cache, re.MULTILINE)
    if source is None:
        raise dev.RunError("build is missing source provenance: " + str(build_dir))
    result["source_directory"] = source[1]
    commands_path = build_dir / "compile_commands.json"
    if not commands_path.is_file():
        raise dev.RunError("build requires compile_commands.json: " + str(build_dir))
    commands = json.loads(commands_path.read_text())
    topic = next((row for row in commands if row["file"].endswith("/embarlet/topic.cc")), None)
    if topic is None:
        raise dev.RunError("compile commands do not contain the production Topic")
    args = topic.get("arguments") or shlex.split(topic["command"])
    if any("COLLECT_LATENCY_STATS" in arg and not arg.startswith("-U") for arg in args):
        raise dev.RunError("latency instrumentation must be disabled in broker comparison builds")
    architecture_flags = [arg for arg in args if arg.startswith(("-m", "-std="))]
    compiler = result["cmake_cache"].get("CMAKE_CXX_COMPILER", args[0])
    macros = subprocess.run([compiler, *architecture_flags, "-dM", "-E", "-x", "c++", "/dev/null"],
                            capture_output=True, text=True, timeout=15, check=True).stdout
    native_atomic = "#define __x86_64__ " in macros and "#define __GCC_HAVE_SYNC_COMPARE_AND_SWAP_16 " in macros
    result.update(topic_compile_command=args, architecture_flags=architecture_flags,
                  pbr_mode_from_compile="atomic128" if native_atomic else "runtime-dependent",
                  pbr_mode_evidence="actual Topic compile flags and compiler predefined macros; baseline has no mode log")
    result["comparison_compile_contract"] = compile_contract(args, compiler, macros)
    if result["cmake_cache"].get("CMAKE_BUILD_TYPE") != "Release" or not native_atomic:
        raise dev.RunError("pilot requires Release and matching native atomic128 Topic builds")
    if result["cmake_cache"].get("EMBARCADERO_SANITIZER", "none") != "none" or any("sanitize" in arg for arg in args):
        raise dev.RunError("sanitizer builds cannot be compared in this pilot")
    return result


def machine_policy(hardware, assignment):
    return {**{key: hardware.get(key) for key in ("kernel", "thp_enabled", "tmpfs_thp", "numa_balancing")},
            "cpufreq_boost": dev.read_optional("/sys/devices/system/cpu/cpufreq/boost"),
            "governors": {str(cpu): dev.read_optional(f"/sys/devices/system/cpu/cpu{cpu}/cpufreq/scaling_governor")
                          for cpus in assignment.values() for cpu in cpus}}


def observed_placement(child, cpus, node, run_dir, role, shm_name=None):
    observed = dev.placement_snapshot(child, cpus, node, run_dir, role, shm_name)
    if not observed["thread_cpu_masks"]:
        raise dev.RunError(role + " has no observed thread CPU assignment")
    numa = (run_dir / f"{role}.numa_maps").read_text()
    anonymous_pages = {}
    policies = set()
    for line in numa.splitlines():
        if "file=" in line or not re.search(r"\banon=[1-9]", line):
            continue
        policies.add(line.split()[1])
        for found, count in re.findall(r"\bN(\d+)=(\d+)", line):
            anonymous_pages[found] = anonymous_pages.get(found, 0) + int(count)
    if not anonymous_pages.get(str(node)) or any(int(key) != node and value for key, value in anonymous_pages.items()):
        raise dev.RunError(f"{role} anonymous resident pages violate NUMA node {node}: {anonymous_pages}")
    observed.update(anonymous_mapping_pages=anonymous_pages, anonymous_mapping_policies=sorted(policies),
                    sample_monotonic=time.monotonic())
    return observed


def source_snapshot(source, destination, output):
    destination.mkdir(mode=0o700)
    def git(*args):
        return subprocess.run(["git", *args], cwd=source, capture_output=True, check=True, timeout=30).stdout
    revision = git("rev-parse", "HEAD").decode().strip()
    status_text = git("status", "--porcelain").decode()
    patch = git("diff", "--binary", "HEAD")
    (destination / "tracked.patch").write_bytes(patch)
    inventory = []
    total = 0
    for raw in git("ls-files", "--others", "--exclude-standard", "-z").split(b"\0"):
        if not raw:
            continue
        relative = Path(os.fsdecode(raw))
        path = (source / relative).resolve()
        if output == path or output in path.parents:
            continue
        if source != path and source not in path.parents:
            raise dev.RunError("untracked source symlink escapes checkout: " + str(relative))
        if not path.is_file():
            continue
        size = path.stat().st_size
        if size > 8 * dev.MIB or total + size > 64 * dev.MIB:
            raise dev.RunError("untracked source snapshot exceeds 64 MiB; use a clean comparison checkout")
        total += size
        target = destination / "untracked" / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, target)
        inventory.append({"path": str(relative), "bytes": size, "sha256": dev.binary_digest(path)})
    result = {"revision": revision, "git_status": status_text, "tracked_patch_sha256": hashlib.sha256(patch).hexdigest(),
              "untracked_files": inventory, "source_snapshot": str(destination), "complete": True,
              "scope": "current source checkout captured; association with compiled artifacts is recorded separately in build evidence"}
    dev.write_json(destination / "source.json", result)
    return result


def archived_source_snapshot(source, archive_path, destination):
    """Bind a non-git CMAKE_HOME_DIRECTORY to archive_source.py evidence."""
    destination.mkdir(mode=0o700)
    archive = destination / "source.tar.gz"
    inventory_path = destination / "source.tar.gz.json"
    shutil.copyfile(archive_path, archive)
    shutil.copyfile(archive_path.with_suffix(archive_path.suffix + ".json"), inventory_path)
    inventory = json.loads(inventory_path.read_text())
    revision = inventory.get("revision", "")
    files = inventory.get("files")
    if not isinstance(revision, str) or not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise dev.RunError("source archive inventory has no valid revision")
    if not isinstance(files, dict) or not files:
        raise dev.RunError("source archive inventory is empty or invalid")
    digest = dev.binary_digest(archive)
    if digest != inventory.get("archive_sha256"):
        raise dev.RunError("source archive SHA256 differs from inventory")
    # This mode is an exclusive extraction with an out-of-tree build. Extra
    # headers or globbed sources could otherwise change the build invisibly.
    actual_files = set()
    for path in source.rglob("*"):
        if path.is_symlink() or (not path.is_file() and not path.is_dir()):
            raise dev.RunError("CMAKE_HOME_DIRECTORY contains a symlink or special file: " + str(path))
        if path.is_file():
            actual_files.add(path.relative_to(source).as_posix())
    if actual_files != set(files):
        raise dev.RunError("CMAKE_HOME_DIRECTORY file inventory differs from archive: " +
                           repr(sorted(actual_files.symmetric_difference(files))[:20]))
    seen = set()
    with tarfile.open(archive, "r:gz") as captured:
        for member in captured:
            name = member.name
            relative = Path(name)
            if (relative.is_absolute() or ".." in relative.parts or str(relative) != name or
                    not member.isfile() or name in seen or name not in files):
                raise dev.RunError("source archive has an unsafe, duplicate, or unlisted member: " + name)
            seen.add(name)
            entry = files[name]
            if not isinstance(entry, dict):
                raise dev.RunError("invalid source archive file entry: " + name)
            contents = captured.extractfile(member)
            hashed = hashlib.sha256()
            for block in iter(lambda: contents.read(1024 * 1024), b""):
                hashed.update(block)
            if hashed.hexdigest() != entry.get("sha256") or member.mode != entry.get("mode"):
                raise dev.RunError("source archive member differs from inventory: " + name)
            actual = source / relative
            resolved = actual.resolve()
            if (source not in resolved.parents or actual.is_symlink() or not actual.is_file() or
                    dev.binary_digest(actual) != entry["sha256"] or
                    stat.S_IMODE(actual.stat().st_mode) != entry["mode"]):
                raise dev.RunError("CMAKE_HOME_DIRECTORY source differs from archive: " + name)
    if seen != set(files):
        raise dev.RunError("source archive is missing inventory files")
    result = {"revision": revision, "provenance": "archived dirty snapshot",
              "git_status": None, "archive_sha256": digest,
              "inventory_sha256": dev.binary_digest(inventory_path),
              "archive": str(archive), "inventory": str(inventory_path),
              "verified_files": len(seen), "source_directory": str(source),
              "source_snapshot": str(destination), "complete": True,
              "scope": "exact source file inventory, bytes, and modes match CMAKE_HOME_DIRECTORY; revision labels the base, not a clean checkout; compiled artifact association is recorded separately in build evidence"}
    dev.write_json(destination / "source.json", result)
    return result


def process_counters(pid):
    try:
        text = Path(f"/proc/{pid}/stat").read_text()
        fields = text[text.rfind(")") + 2:].split()
        ticks = os.sysconf("SC_CLK_TCK")
        return {"sample_monotonic": time.monotonic(), "user_seconds": int(fields[11]) / ticks,
                "system_seconds": int(fields[12]) / ticks, "minor_faults": int(fields[7]),
                "major_faults": int(fields[9]), "rss_bytes": int(fields[21]) * os.sysconf("SC_PAGESIZE")}
    except (OSError, ValueError, IndexError):
        return None


def validate_mapping_base(run_dir, role, shm_name):
    expected = dev.CXL_BASE_ADDR
    log = (run_dir / f"{role}.log").read_text(errors="replace")
    logged = [int(value, 16) for value in re.findall(
        r"CXL mapping successful at address:\s*(0x[0-9a-fA-F]+)\b", log)]
    numa = (run_dir / f"{role}.numa_maps").read_text()
    observed = []
    for line in numa.splitlines():
        fields = line.split()
        if "file=/dev/shm" + shm_name in fields:
            observed.append(int(fields[0], 16))
    if logged != [expected] or observed != [expected]:
        raise dev.RunError(f"{role} shared mapping base must be {hex(expected)}; "
                           f"logged={[hex(value) for value in logged]} "
                           f"observed={[hex(value) for value in observed]}")
    return {"expected": hex(expected), "logged": hex(logged[0]),
            "observed": hex(observed[0]), "source": f"{role}.log and {role}.numa_maps"}


def counter_delta(before, after):
    if not before or not after:
        return {"status": "unavailable"}
    return {field: after[field] - before[field] for field in
            ("user_seconds", "system_seconds", "minor_faults", "major_faults", "sample_monotonic")}


def collect_client(child):
    # Only this function reaps the client; OwnedProcesses.check_brokers never
    # polls it. Preserve its exact lifetime rusage before normal owned cleanup.
    pid, status, usage = os.wait4(child.pid, os.WNOHANG)
    if not pid:
        return None
    child.returncode = os.waitstatus_to_exitcode(status)
    return {"user_seconds": usage.ru_utime, "system_seconds": usage.ru_stime,
            "minor_faults": usage.ru_minflt, "major_faults": usage.ru_majflt,
            "max_rss_bytes": usage.ru_maxrss * 1024,
            "scope": "complete client lifetime, including initialization and serial payload audit"}


def validate_result(run_dir, payload_bytes, brokers, variant, build):
    with (run_dir / "throughput_benchmark_summary.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    expected = payload_bytes // dev.MESSAGE_BYTES
    if len(rows) != 1 or any(rows[0].get(key) != str(value) for key, value in
            {"total_message_size_bytes": payload_bytes, "message_size_bytes": dev.MESSAGE_BYTES,
             "message_count": expected, "order": 5, "ack_level": 1, "replication_factor": 0,
             "sequencer": "EMBARCADERO", "num_threads_per_broker": 1}.items()):
        raise dev.RunError("client result differs from the declared comparison workload")
    metrics = {"ack_completion_mib_s": float(rows[0]["publish_goodput_mbps"]),
               "audit_inclusive_e2e_mib_s": float(rows[0]["e2e_goodput_mbps"])}
    if any(not math.isfinite(value) or value <= 0 for value in metrics.values()):
        raise dev.RunError("client did not report finite positive completion throughput")
    log = (run_dir / "client.log").read_text(errors="replace")
    audits = re.findall(r"\[ORDERED_DELIVERY_AUDIT\] status=passed messages=(\d+) expected=(\d+) "
                        r"payload_bytes=(\d+) duplicates=(\d+) parse_errors=(\d+) export_gaps=(\d+) indexed_payload=1\b", log)
    if len(audits) != 1 or tuple(map(int, audits[0])) != (expected, expected, payload_bytes, 0, 0, 0):
        raise dev.RunError("exact indexed ordered delivery audit failed")
    ack = re.findall(r"\[ACK_VERIFY\] normalized_received=(\d+) raw_received=(\d+) target=(\d+) 100%", log)
    # ORDER5 Poll completes on order5_last_ack_hwm_. EpollAckThread publishes
    # that frontier before updating ack_received_; raw_received is a separate,
    # potentially lagging diagnostic snapshot, not a completion predicate.
    if len(ack) != 1 or int(ack[0][0]) != expected or int(ack[0][2]) != expected:
        raise dev.RunError("ACK completion count mismatch")
    routing = re.findall(r"\[ORDER5_ROUTING\]([^\n]*)", log)
    if not routing:
        raise dev.RunError("missing final ORDER5 routing counters")
    final_route = routing[-1]
    counters = dict(re.findall(r"(retransmit_attempts|session_fenced_observed|session_rto_min_ms)=(\d+)", final_route))
    if counters != {"retransmit_attempts": "0", "session_fenced_observed": "0", "session_rto_min_ms": "2000"}:
        raise dev.RunError("routing counters show retries, fencing, or an unexpected RTO")
    routed = {int(broker_id): int(count) for broker_id, count in re.findall(r"broker(\d+)_msgs=(\d+)", final_route)}
    if set(routed) != set(range(brokers)) or any(count <= 0 for count in routed.values()) or sum(routed.values()) != expected:
        raise dev.RunError("selected brokers did not all receive the declared workload")
    logs = {"client": log, **{f"broker-{i}": (run_dir / f"broker-{i}.log").read_text(errors="replace")
                             for i in range(brokers)}}
    failures = re.compile(r"retransmit_attempts=[1-9]|\[SESSION_FENCED_OBSERVED\]|\[SESSION_FENCE_LOCAL_COMMIT\]|"
                          r"\[ORDER5_SESSION_FENCE\]|\[SESSION_ROLLOVER|\[ORDERED_DELIVERY_AUDIT\] status=failed|"
                          r"Subscriber::Poll timeout|not all messages acknowledged|Invalid batch envelope|Malformed batch body|"
                          r"capacity exhausted|CXL memory exhausted|BLog.*exhaust|Segment rolled", re.IGNORECASE)
    for role, contents in logs.items():
        if failures.search(contents):
            raise dev.RunError(f"{role} reported a retry, fence, exhaustion, validation failure or timeout")
    modes = {role: sorted(set(re.findall(r"PBR reservation mode=(atomic128|mutex)\b", contents)))
             for role, contents in logs.items() if role.startswith("broker-")}
    if variant == "candidate" and any(value != [build["pbr_mode_from_compile"]] for value in modes.values()):
        raise dev.RunError("candidate runtime PBR mode does not match the intended compile path")
    pool = re.findall(r"Publisher unacked_byte_cap=(\d+) pool_bytes=(\d+) unacked_retention=(\S+)", log)
    if len(pool) != 1 or int(pool[0][0]) <= 0 or int(pool[0][1]) <= 0:
        raise dev.RunError("publisher pool and ACK credit were not observed")
    return metrics, {"messages": expected, "payload_bytes": payload_bytes, "indexed_payload": True,
                     "ack": {"normalized_received": int(ack[0][0]), "raw_received_diagnostic": int(ack[0][1]),
                             "target": int(ack[0][2]), "raw_received_is_completion_gate": False},
                     "routed_messages": routed, "routing_counters": counters, "pbr_runtime_modes": modes,
                     "pbr_compile_mode": build["pbr_mode_from_compile"], "publisher_pool_and_credit": pool}


def execute_run(args, entry, campaign, config, identities, assignment):
    output = args.output.resolve()
    run_dir = Path(tempfile.mkdtemp(prefix=f"embarcadero-perf-{os.getuid()}-", dir=output / "runs"))
    entry["manifest"] = str((run_dir / "manifest.json").relative_to(output))
    entry["status"] = "running"
    dev.write_json(output / "index.json", campaign)
    variant = entry["variant"]
    broker = identities[variant]["broker"]
    client = identities["candidate"]["client"]
    shm_path = Path("/dev/shm") / run_dir.name
    env, selected, removed = environment("/" + run_dir.name, args.brokers)
    config_path = run_dir / "effective-config.yaml"
    dev.write_json(config_path, config)
    manifest = {**entry, "status": "preflight", "profile": "matched-dram-broker-pilot", "backend": "dram-emulation",
                "protocol_revision": PROTOCOL_REVISION,
                "cxl_evidence": False, "payload_bytes": args.payload_mib * dev.MIB,
                "message_bytes": dev.MESSAGE_BYTES, "order": 5, "ack": 1, "replication_factor": 0,
                "effective_environment": selected, "ignored_environment_names": removed,
                "config_sha256": dev.binary_digest(config_path), "common_client_sha256": identities["candidate"]["client_sha256"],
                "cpu_assignment": assignment, "build": identities[variant]["build"],
                "artifact_identity": identities[variant], "common_client_identity": identities["candidate"],
                "shared_memory": str(shm_path), "forced_shutdown": [], "failure_reasons": [],
                "primary_metric": "application MiB/s through Publish+Poll ACK completion",
                "secondary_metric": "application MiB/s through serial indexed audit after ACK completion",
                "baseline_compatibility": campaign["baseline_compatibility"], "layout": campaign["layout"],
                "startup_timeout_seconds": args.startup_timeout, "client_deadline_seconds": 90,
                "ack_timeout_seconds": 60, "indexed_audit_timeout_seconds": 20,
                "shutdown_timeout_seconds": 60, "build_evidence": campaign.get("build_evidence"),
                "started_wall": time.time(), "started_monotonic": time.monotonic()}
    normalized_env = {key: value for key, value in selected.items() if key != "EMBARCADERO_CXL_SHM_NAME"}
    comparison_identity = {"protocol_revision": PROTOCOL_REVISION, "config": manifest["config_sha256"],
        "client": manifest["common_client_sha256"], "cpu_assignment": assignment,
        "payload_bytes": manifest["payload_bytes"], "environment": normalized_env}
    def bind(role):
        return [campaign["numactl"], "--physcpubind=" + ",".join(map(str, assignment[role])),
                "--membind=" + ("0" if role == "client" else "1")]
    commands = [bind(f"broker-{i}") + [broker, "--emul", "--config", str(config_path)] +
                (["--head"] if i == 0 else ["--follower", f"127.0.0.1:{dev.CONTROL_PORT}"])
                for i in range(args.brokers)]
    client_command = bind("client") + [client, "--config", str(config_path), "--head_addr", "127.0.0.1",
        "-t", "1", "-o", "5", "-a", "1", "-r", "0", "-n", "1", "-m", str(dev.MESSAGE_BYTES),
        "-s", str(manifest["payload_bytes"]), "--sequencer", "EMBARCADERO"]
    manifest.update(broker_commands=commands, client_command=client_command)
    owned = dev.OwnedProcesses(run_dir, env, 60)
    owned.start_wall = time.time()
    shm_identity = None
    markers = []
    start = time.monotonic()
    try:
        hardware = dev.topology()
        manifest["hardware"] = hardware
        manifest["machine_policy"] = machine_policy(hardware, assignment)
        manifest["comparison_fingerprint"] = fingerprint({**comparison_identity, "machine_policy": manifest["machine_policy"]})
        manifest["load_average_at_start"] = os.getloadavg()
        manifest["memory"] = memory_preflight(hardware, args.brokers, manifest["payload_bytes"])
        cpu_assignment(hardware, args.brokers)
        dev.check_ports([dev.CONTROL_PORT] + list(range(dev.DATA_PORT, dev.DATA_PORT + args.brokers)))
        for version in ("baseline", "candidate"):
            if dev.binary_digest(Path(identities[version]["broker"])) != identities[version]["broker_sha256"]:
                raise dev.RunError("broker binary changed during the campaign")
        if dev.binary_digest(Path(client)) != manifest["common_client_sha256"]:
            raise dev.RunError("common client binary changed during the campaign")
        dev.write_json(run_dir / "manifest.json", manifest)
        if args.dry_run:
            manifest["status"] = "dry-run"
            return manifest
        fd = os.open(shm_path, os.O_CREAT | os.O_EXCL | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        info = os.fstat(fd)
        shm_identity = (info.st_dev, info.st_ino)
        os.close(fd)
        manifest["placement"] = {}
        manifest["mapping_bases"] = {}
        for i, command in enumerate(commands):
            name = f"broker-{i}"
            child = owned.start(name, command)
            markers.append(Path(f"/tmp/embarlet_{child.pid}_ready"))
            manifest["pids"] = {role: process.pid for role, process in owned.children}
            dev.write_json(run_dir / "manifest.json", manifest)
            dev.wait_ready(child, owned, time.monotonic() + args.startup_timeout)
            manifest["placement"][name] = observed_placement(child, assignment[name], 1, run_dir,
                name, "/" + run_dir.name if i == 0 else None)
            manifest["mapping_bases"][name] = validate_mapping_base(run_dir, name, "/" + run_dir.name)
        manifest["startup_seconds"] = time.monotonic() - start
        before = {role: process_counters(child.pid) for role, child in owned.children}
        manifest["broker_counters_before_client"] = before
        workload = owned.start("client", client_command)
        manifest["pids"]["client"] = workload.pid
        manifest["status"] = "running"
        dev.write_json(run_dir / "manifest.json", manifest)
        client_start = time.monotonic()
        sampled = False
        while True:
            usage = collect_client(workload)
            if usage is not None:
                manifest["client_rusage"] = usage
                break
            owned.check_brokers()
            if time.monotonic() - client_start >= 90:
                raise dev.RunError("client exceeded its 90-second deadline")
            if not sampled and time.monotonic() - client_start >= 0.25:
                manifest["placement"]["client"] = observed_placement(workload, assignment["client"], 0, run_dir, "client")
                sampled = True
            time.sleep(0.05)
        owned.check_brokers()
        after = {role: process_counters(child.pid) for role, child in owned.children if role != "client"}
        manifest["broker_counters_after_client"] = after
        manifest["broker_client_window_counters"] = {role: counter_delta(before[role], after[role]) for role in before}
        manifest["broker_counter_scope"] = "before client launch through client exit; excludes broker initialization, includes client initialization and audit"
        manifest["client_wall_seconds"] = time.monotonic() - client_start
        if not sampled:
            raise dev.RunError("client placement was not observed before exit")
        if workload.returncode:
            raise dev.RunError(f"client exited with status {workload.returncode}")
        manifest["metrics"], manifest["validation"] = validate_result(run_dir, manifest["payload_bytes"],
            args.brokers, variant, identities[variant]["build"])
        manifest["status"] = "passed"
    except (dev.RunError, OSError, ValueError, KeyError, subprocess.TimeoutExpired) as error:
        manifest["status"] = "failed"
        manifest["failure_reasons"].append(str(error))
    finally:
        shutdown_start = time.monotonic()
        previous_handlers = {signum: signal.signal(signum, signal.SIG_IGN) for signum in (signal.SIGINT, signal.SIGTERM)}
        try:
            owned.close()
            manifest["forced_shutdown"] = owned.forced
            if owned.forced:
                manifest["status"] = "failed"
                manifest["failure_reasons"].append("forced termination was required: " + ", ".join(owned.forced))
        except (OSError, subprocess.TimeoutExpired) as error:
            manifest["status"] = "failed"
            manifest["failure_reasons"].append("cleanup: " + str(error))
        finally:
            for signum, handler in previous_handlers.items():
                signal.signal(signum, handler)
        manifest["shutdown_seconds"] = time.monotonic() - shutdown_start
        manifest["exit_codes"] = {role: child.poll() for role, child in owned.children}
        if manifest["status"] == "passed" and any(code != 0 for code in manifest["exit_codes"].values()):
            manifest["status"] = "failed"
            manifest["failure_reasons"].append("nonzero process exit during completion or shutdown")
        if shm_identity and all(child.poll() is not None for _, child in owned.children):
            try:
                info = shm_path.lstat()
                if (info.st_dev, info.st_ino) == shm_identity:
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
        manifest["shared_memory_removed"] = not shm_path.exists()
        if shm_identity and not manifest["shared_memory_removed"]:
            manifest["status"] = "failed"
            manifest["failure_reasons"].append("shared-memory path remained after owned cleanup")
        manifest["total_wall_seconds"] = time.monotonic() - start
        manifest["ended_monotonic"] = time.monotonic()
        manifest["finished_wall"] = time.time()
        dev.write_json(run_dir / "manifest.json", manifest)
    return manifest


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-build", type=Path, required=True)
    parser.add_argument("--candidate-build", type=Path, required=True)
    parser.add_argument("--candidate-source-archive", type=Path,
                        help="immutable archive_source.py archive plus .json inventory for a candidate built without .git")
    parser.add_argument("--output", type=Path, required=True, help="new artifact directory; existing paths are never overwritten")
    parser.add_argument("--brokers", type=int, choices=(1, 3), required=True)
    parser.add_argument("--pairs", type=int, default=6)
    parser.add_argument("--payload-mib", type=int, default=2048)
    parser.add_argument("--qualification-only", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--startup-timeout", type=float, default=180)
    parser.add_argument("--build-evidence", type=Path, help="matched build/source attestation JSON, copied into the campaign")
    args = parser.parse_args(argv)
    if args.pairs < 2 or args.pairs > 12 or args.pairs % 2:
        parser.error("pairs must be an even count in [2,12] for balanced AB/BA order")
    if not 32 <= args.payload_mib <= 2048:
        parser.error("payload must be 32..2048 MiB; 2048 is the comparison default")
    if not 30 <= args.startup_timeout <= 300:
        parser.error("startup timeout must be 30..300 seconds")
    return args


def main(argv=None):
    args = parse_args(argv)
    output = args.output.resolve()
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    (output / "runs").mkdir(mode=0o700)
    campaign = {"schema_version": 1, "protocol_revision": PROTOCOL_REVISION, "status": "preflight", "brokers": args.brokers,
        "payload_bytes": args.payload_mib * dev.MIB, "pairs": args.pairs,
        "qualification_only": args.qualification_only, "dry_run": args.dry_run,
        "runs": run_plan(args.brokers, args.pairs, args.qualification_only),
        "baseline_compatibility": {"expected_revision": BASELINE_REVISION, "layout_version": 4,
            "descriptor_supported": False, "layout_query_binary": "candidate",
            "basis": "source-reviewed equivalent metadata offsets; v5 descriptor occupies reserved bytes; each run uses a fresh region and one broker version"},
        "measurement_scope": "DRAM-only local matched broker pilot; one common audited candidate client; no CXL or sustained-throughput claim"}
    exit_code = 1
    handlers = {}
    try:
        hardware = dev.topology()
        assignment, cores = cpu_assignment(hardware, args.brokers)
        campaign.update(hardware=hardware, cpu_assignment=assignment, physical_cores=cores)
        campaign["numactl"] = shutil.which("numactl")
        if not campaign["numactl"]:
            raise dev.RunError("numactl is required")
        config = effective_config(args.brokers)
        config_path = output / "effective-config.yaml"
        dev.write_json(config_path, config)
        identities = {}
        for version, directory in (("baseline", args.baseline_build.resolve()), ("candidate", args.candidate_build.resolve())):
            broker = directory / "bin/embarlet"
            client = directory / "bin/throughput_test"
            if not os.access(broker, os.X_OK) or (version == "candidate" and not os.access(client, os.X_OK)):
                raise dev.RunError("missing executable for " + version)
            build = build_identity(directory)
            source_directory = Path(build["source_directory"]).resolve()
            if version == "candidate" and args.candidate_source_archive:
                source = archived_source_snapshot(source_directory, args.candidate_source_archive.resolve(),
                                                  output / (version + "-source"))
            else:
                source = source_snapshot(source_directory, output / (version + "-source"), output)
            if version == "baseline" and source["revision"] != BASELINE_REVISION:
                raise dev.RunError("baseline source is not the declared comparison revision")
            if version == "baseline" and source["git_status"].strip():
                raise dev.RunError("baseline source must be clean at the declared comparison revision")
            identities[version] = {"broker": str(broker), "broker_sha256": dev.binary_digest(broker),
                "client": str(client), "client_sha256": dev.binary_digest(client) if version == "candidate" else None,
                "build": build, "source": source}
        campaign["identities"] = identities
        if identities["baseline"]["build"]["comparison_compile_contract"] != identities["candidate"]["build"]["comparison_compile_contract"]:
            raise dev.RunError("baseline/candidate compiler, optimization, ISA, definitions or instrumentation differ")
        if args.build_evidence:
            evidence = json.loads(args.build_evidence.read_text())
            if evidence.get("production_or_build_input_drift") or evidence.get("shared_library_files_match") is not True:
                raise dev.RunError("build evidence reports source drift or mismatched runtime dependencies")
            for version in ("baseline", "candidate"):
                binaries = evidence["builds"][version]["binaries"]
                if binaries["embarlet"]["sha256"] != identities[version]["broker_sha256"]:
                    raise dev.RunError("broker does not match its build attestation")
            if evidence["builds"]["candidate"]["binaries"]["throughput_test"]["sha256"] != identities["candidate"]["client_sha256"]:
                raise dev.RunError("common client does not match build attestation")
            shutil.copyfile(args.build_evidence, output / "build-evidence.json")
            campaign["build_evidence"] = {"file": "build-evidence.json", "sha256": dev.binary_digest(output / "build-evidence.json")}
        env, _, _ = environment("/embarcadero-perf-layout-query", args.brokers)
        campaign["layout"] = layout_query(Path(identities["candidate"]["broker"]), config_path, env, args.brokers)
        for signum in (signal.SIGINT, signal.SIGTERM):
            handlers[signum] = signal.getsignal(signum)
            signal.signal(signum, dev.interrupted)
        # The same production lock covers every run and its complete cleanup.
        with dev.ClusterLock():
            for entry in campaign["runs"]:
                print(f"{entry['kind']} pair={entry['pair']} {entry['variant']} brokers={args.brokers}", flush=True)
                result = execute_run(args, entry, campaign, config, identities, assignment)
                entry["status"] = result["status"]
                entry["failure_reasons"] = result["failure_reasons"]
                for field in ("started_monotonic", "ended_monotonic", "started_wall", "finished_wall"):
                    entry[field] = result[field]
                dev.write_json(output / "index.json", campaign)
                if result["status"] == "failed":
                    raise dev.RunError("run failed; all artifacts retained: " + entry["manifest"])
        campaign["status"] = "dry-run" if args.dry_run else "passed"
        exit_code = 0
    except (dev.RunError, OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        campaign["status"] = "failed"
        campaign["error"] = str(error)
        print("perf_compare: " + str(error), file=sys.stderr)
    finally:
        for signum, handler in handlers.items():
            signal.signal(signum, handler)
        dev.write_json(output / "index.json", campaign)
    return exit_code


if __name__ == "__main__":
    sys.exit(main())
