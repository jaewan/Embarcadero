#!/usr/bin/env python3
"""Validate the paper failure traces and derive the prefix-safe timeline."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import statistics
from datetime import datetime
from pathlib import Path


STAMP = re.compile(r"^[A-Z](\d{8}) (\d{2}:\d{2}:\d{2}\.\d{6})")
EVENT_MS = re.compile(r"Failure/Event @ (\d+) ms: (.*)$")


def stamp(line: str) -> datetime:
    match = STAMP.match(line)
    if not match:
        raise ValueError(f"missing glog timestamp: {line.rstrip()}")
    return datetime.strptime(" ".join(match.groups()), "%Y%m%d %H:%M:%S.%f")


def one(lines: list[str], marker: str) -> str:
    matches = [line for line in lines if marker in line]
    if len(matches) != 1:
        raise ValueError(f"expected one {marker!r}, found {len(matches)}")
    return matches[0]


def parse_trial(path: Path) -> dict[str, object]:
    lines = path.read_text(errors="replace").splitlines()
    kill_line = one(lines, "Failure threshold reached (wall-clock)")
    fence_line = one(lines, "[SESSION_FENCED_OBSERVED]")
    replay_line = one(lines, "[SESSION_REOPEN_RESUBMIT_BEGIN]")
    ack_line = one(lines, "Failure threshold reached (ACK frontier)")
    drain_line = one(lines, "[UNACKED_DRAIN]")

    event_rows: list[tuple[int, str]] = []
    for line in lines:
        match = EVENT_MS.search(line)
        if match:
            event_rows.append((int(match.group(1)), match.group(2)))
    kill_ms = next(ms for ms, event in event_rows if "wall-clock" in event)
    first_failure_ms = min(ms for ms, event in event_rows if "Send Fail" in event)
    first_reconnect_ms = min(ms for ms, event in event_rows if "Reconnect Success" in event)
    ack_ms = next(ms for ms, event in event_rows if "ACK frontier" in event)
    kill_time = stamp(kill_line)
    suffix_match = re.search(r"suffix_batches=(\d+)", replay_line)
    drain_match = re.search(r"bytes=(\d+) batches=(\d+)", drain_line)
    if not suffix_match or not drain_match:
        raise ValueError(f"missing suffix/drain counters in {path}")
    if drain_match.groups() != ("0", "0"):
        raise ValueError(f"unacknowledged suffix did not drain in {path}")

    return {
        "trial": int(re.search(r"trial(\d+)", path.name).group(1)),
        "kill_to_first_failure_ms": first_failure_ms - kill_ms,
        "kill_to_first_reconnect_ms": first_reconnect_ms - kill_ms,
        "kill_to_fence_ms": (stamp(fence_line) - kill_time).total_seconds() * 1000,
        "kill_to_resubmit_begin_ms":
            (stamp(replay_line) - kill_time).total_seconds() * 1000,
        "kill_to_ack_frontier_ms": ack_ms - kill_ms,
        "suffix_batches": int(suffix_match.group(1)),
        "unacked_bytes_end": 0,
        "unacked_batches_end": 0,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--manifest-out", type=Path)
    args = parser.parse_args()

    contract = (args.campaign / "campaign_contract.md").read_text()
    pass_match = re.search(r"pass_id: `?(\d+T\d+Z)`?", contract)
    commit_match = re.search(r"git_commit: `?([0-9a-f]{40})`?", contract)
    dirty_match = re.search(r"git_dirty_files: `?(\d+)`?", contract)
    if not pass_match or not commit_match or not dirty_match:
        raise SystemExit("validation failed: incomplete campaign contract")
    if dirty_match.group(1) != "0":
        raise SystemExit("validation failed: campaign used a dirty worktree")

    trace_dir = args.campaign / "runs" / pass_match.group(1) / "prefix_safe"
    paths = sorted(trace_dir.glob("client_failure_trial*.log"))
    if len(paths) != 3:
        raise SystemExit(f"validation failed: expected 3 traces, found {len(paths)}")
    trials = [parse_trial(path) for path in paths]
    if [row["trial"] for row in trials] != [1, 2, 3]:
        raise SystemExit("validation failed: trials must be numbered 1, 2, 3")

    metric_names = [
        "kill_to_first_failure_ms",
        "kill_to_first_reconnect_ms",
        "kill_to_fence_ms",
        "kill_to_resubmit_begin_ms",
        "kill_to_ack_frontier_ms",
        "suffix_batches",
    ]
    medians = {
        name: statistics.median(float(row[name]) for row in trials)
        for name in metric_names
    }
    inputs = {
        str(path.relative_to(args.campaign)): hashlib.sha256(path.read_bytes()).hexdigest()
        for path in paths
    }
    manifest = {
        "schema": 1,
        "campaign": str(args.campaign),
        "git_commit": commit_match.group(1),
        "git_dirty_files": 0,
        "pass_id": pass_match.group(1),
        "inputs_sha256": inputs,
        "trials": trials,
        "medians": medians,
        "scope": "protocol ACK/fence timeline; no downstream apply-state audit",
    }
    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
