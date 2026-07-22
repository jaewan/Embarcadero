#!/usr/bin/env python3
"""Validate the two-session gap-isolation result used in Q2."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--manifest-out", type=Path)
    args = parser.parse_args()

    summary_path = args.campaign / "commit_isolation_summary.csv"
    contract_path = args.campaign / "run_contract.csv"
    rows = list(csv.DictReader(summary_path.open()))
    contracts = list(csv.DictReader(contract_path.open()))
    if len(rows) != 3 or sorted(int(row["trial"]) for row in rows) != [1, 2, 3]:
        raise SystemExit("validation failed: expected trials 1, 2, and 3")
    if len(contracts) != 1:
        raise SystemExit("validation failed: expected one run contract")
    contract = contracts[0]
    if contract["git_dirty"].lower() not in {"0", "false"}:
        raise SystemExit("validation failed: campaign used a dirty worktree")
    if any(row["valid"] != "1" for row in rows):
        raise SystemExit("validation failed: at least one trial is invalid")
    if any(row["affected_commits_at_or_after_gap_seq_during_gap"] != "0" for row in rows):
        raise SystemExit("validation failed: affected session committed past its gap")
    if any(row["affected_first_post_gap_seq"] != row["gap_batch_seq"] for row in rows):
        raise SystemExit("validation failed: affected session did not repair the gap first")

    manifest = {
        "schema": 1,
        "campaign": str(args.campaign),
        "git_commit": contract["git_commit"],
        "git_dirty": False,
        "inputs_sha256": {
            summary_path.name: hashlib.sha256(summary_path.read_bytes()).hexdigest(),
            contract_path.name: hashlib.sha256(contract_path.read_bytes()).hexdigest(),
        },
        "trials": rows,
        "medians": {
            "control_commits_during_gap": statistics.median(
                int(row["control_commits_during_gap"]) for row in rows
            ),
            "control_max_intercommit_ms": statistics.median(
                int(row["control_max_intercommit_ms"]) for row in rows
            ),
        },
    }
    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
