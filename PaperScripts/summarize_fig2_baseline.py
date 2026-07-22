#!/usr/bin/env python3
"""Validate one clean Corfu or Scalog append-latency campaign."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import statistics
from pathlib import Path


CELLS = {
    "corfu": "fig2_corfu_o2_ack2_rf2_mem",
    "scalog": "fig2_scalog_o1_ack2_rf2_mem",
}


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--system", choices=sorted(CELLS), required=True)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--loads", default="100 250 500 1000 2000")
    parser.add_argument("--manifest-out", type=Path)
    args = parser.parse_args()

    contract_path = args.campaign / "campaign_contract.md"
    results_path = args.campaign / "results.csv"
    contract = contract_path.read_text()
    if not re.search(r"^- Dirty: no\s*$", contract, re.MULTILINE):
        raise SystemExit("validation failed: campaign contract is not clean")

    expected_loads = [int(value) for value in args.loads.split()]
    rows = [
        row
        for row in csv.DictReader(results_path.open())
        if row["cell"] == CELLS[args.system]
    ]
    if any(row["status"] != "ok" for row in rows):
        raise SystemExit("validation failed: campaign contains a failed row")
    commits = {row["git_commit"] for row in rows}
    if len(commits) != 1:
        raise SystemExit("validation failed: campaign mixes commits")

    summary = []
    for load in expected_loads:
        group = [row for row in rows if int(float(row["target_mbps"])) == load]
        if len(group) != args.trials:
            raise SystemExit(
                f"validation failed: load {load} has {len(group)} trials, "
                f"expected {args.trials}"
            )
        p50 = [float(row["pub_ack_p50_us"]) for row in group]
        p99 = [float(row["pub_ack_p99_us"]) for row in group]
        summary.append(
            {
                "target_mbps": load,
                "trials": len(group),
                "append_ack_p50_us_median": statistics.median(p50),
                "append_ack_p99_us_median": statistics.median(p99),
                "achieved_offered_mbps_median": statistics.median(
                    float(row["achieved_offered_mbps"]) for row in group
                ),
                "values_p50_us": p50,
                "values_p99_us": p99,
            }
        )

    manifest = {
        "schema": 1,
        "system": args.system,
        "campaign": str(args.campaign),
        "cell": CELLS[args.system],
        "git_commit": next(iter(commits)),
        "git_dirty": False,
        "inputs_sha256": {
            results_path.name: hashlib.sha256(results_path.read_bytes()).hexdigest(),
            contract_path.name: hashlib.sha256(contract_path.read_bytes()).hexdigest(),
        },
        "summary": summary,
    }
    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
