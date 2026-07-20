#!/usr/bin/env python3
"""Validate and summarize the three-trial primary append-latency sweep."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import statistics
from collections import defaultdict
from pathlib import Path

CELL = "fig2_embar_o5_ack2_rf2_mem"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--summary-csv", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument(
        "--loads", default="100 250 500 750 1000 1500 2000"
    )
    args = parser.parse_args()

    expected_loads = [int(v) for v in args.loads.split()]
    grouped: dict[int, list[dict[str, str]]] = defaultdict(list)
    with args.csv.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("status") != "ok" or row.get("cell") != CELL:
                continue
            if row.get("pacing_mode") != "steady":
                continue
            if row.get("rf") != "2" or row.get("ack") != "2" or row.get("sink") != "mem":
                continue
            grouped[int(float(row["target_mbps"]))].append(row)

    bad = {
        load: len(grouped[load])
        for load in expected_loads
        if len(grouped[load]) != args.trials
    }
    extras = sorted(set(grouped) - set(expected_loads))
    if bad or extras:
        raise SystemExit(f"incomplete/noncanonical cells: counts={bad}, extras={extras}")

    fields = [
        "target_mbps",
        "n",
        "p50_us_median",
        "p50_us_stddev",
        "p99_us_median",
        "p99_us_stddev",
        "values_p50_us",
        "values_p99_us",
    ]
    summary_rows = []
    for load in expected_loads:
        p50 = [float(row["pub_ack_p50_us"]) for row in grouped[load]]
        p99 = [float(row["pub_ack_p99_us"]) for row in grouped[load]]
        summary_rows.append(
            {
                "target_mbps": load,
                "n": len(p50),
                "p50_us_median": statistics.median(p50),
                "p50_us_stddev": statistics.stdev(p50),
                "p99_us_median": statistics.median(p99),
                "p99_us_stddev": statistics.stdev(p99),
                "values_p50_us": " ".join(str(v) for v in p50),
                "values_p99_us": " ".join(str(v) for v in p99),
            }
        )

    args.summary_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.summary_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(summary_rows)

    commits = sorted(
        {row["git_commit"] for rows in grouped.values() for row in rows}
    )
    passes = sorted(
        {row["pass_id"] for rows in grouped.values() for row in rows}
    )
    manifest = {
        "input": str(args.csv),
        "input_sha256": hashlib.sha256(args.csv.read_bytes()).hexdigest(),
        "cell": CELL,
        "selection": "all status=ok steady RF2 ACK2 memory-copy rows",
        "required_trials_per_load": args.trials,
        "loads_mbps": expected_loads,
        "git_commits": commits,
        "pass_ids": passes,
        "summary_csv": str(args.summary_csv),
        "validator": {
            "path": str(Path(__file__).resolve()),
            "sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        },
    }
    args.manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
