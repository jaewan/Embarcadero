#!/usr/bin/env python3
"""Validate and summarize the Fig. 1 path-decomposition campaign.

The paper compares N<=3 using overlap throughput.  This program deliberately
defaults to that metric and refuses incomplete, dirty, or mixed-build input so
that a different CSV column cannot be substituted silently during transcription.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path


EXPECTED_CELLS = {
    "v0_order0_ack1_rf0": "V0",
    "v1_order5_ack1_rf0": "V1",
    "v2_order5_ack1_rf2_copy": "V2",
    "v3_order5_ack2_rf2_acct": "V3",
    "v4_order5_ack2_rf2_copy": "V4",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("input", type=Path)
    parser.add_argument("--n-clients", type=int, default=2)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--metric", default="overlap_gbps")
    parser.add_argument("--csv-out", type=Path)
    parser.add_argument("--manifest-out", type=Path)
    return parser.parse_args()


def fail(message: str) -> None:
    raise SystemExit(f"validation failed: {message}")


def main() -> None:
    args = parse_args()
    raw = args.input.read_bytes()
    rows = list(csv.DictReader(raw.decode().splitlines()))
    selected = [
        row
        for row in rows
        if int(row["n_clients"]) == args.n_clients
        and row["cell"] in EXPECTED_CELLS
    ]
    if not selected:
        fail(f"no N={args.n_clients} rows")
    if args.metric not in selected[0]:
        fail(f"missing metric {args.metric!r}")

    commits = {row["git_commit"] for row in selected}
    embarlet_hashes = {row["embarlet_md5"] for row in selected}
    client_hashes = {row["client_md5"] for row in selected}
    if len(commits) != 1 or len(embarlet_hashes) != 1 or len(client_hashes) != 1:
        fail("selected rows mix commits or binary hashes")
    if any(row["git_dirty_files"] != "0" for row in selected):
        fail("selected rows include a dirty worktree")
    if any(row["status"] != "ok" for row in selected):
        fail("selected rows include a failed trial")

    summaries: list[dict[str, object]] = []
    for cell, label in EXPECTED_CELLS.items():
        group = [row for row in selected if row["cell"] == cell]
        trial_ids = sorted(int(row["trial"]) for row in group)
        expected_trials = list(range(1, args.trials + 1))
        if trial_ids != expected_trials:
            fail(f"{label} trials are {trial_ids}, expected {expected_trials}")
        values = [float(row[args.metric]) for row in group]
        summaries.append(
            {
                "variant": label,
                "cell": cell,
                "n_clients": args.n_clients,
                "metric": args.metric,
                "trials": len(values),
                "median_gbps": statistics.median(values),
                "values_gbps": values,
            }
        )

    by_variant = {row["variant"]: row for row in summaries}
    v0 = float(by_variant["V0"]["median_gbps"])
    for row in summaries:
        value = float(row["median_gbps"])
        row["delta_from_v0_gbps"] = value - v0
        row["delta_from_v0_pct"] = 100.0 * (value - v0) / v0

    def comparison(before: str, after: str) -> dict[str, float | str]:
        before_value = float(by_variant[before]["median_gbps"])
        after_value = float(by_variant[after]["median_gbps"])
        return {
            "before": before,
            "after": after,
            "delta_gbps": after_value - before_value,
            "delta_pct": 100.0 * (after_value - before_value) / before_value,
        }

    comparisons = {
        "ordering": comparison("V0", "V1"),
        "background_replica_copy": comparison("V1", "V2"),
        "ack2_accounting": comparison("V1", "V3"),
        "ack2_payload_copy": comparison("V3", "V4"),
    }

    manifest = {
        "schema": 1,
        "input": str(args.input),
        "input_sha256": hashlib.sha256(raw).hexdigest(),
        "selection": {"n_clients": args.n_clients, "metric": args.metric},
        "validation": {
            "required_trials_per_variant": args.trials,
            "status": "ok",
            "git_dirty_files": 0,
            "git_commit": next(iter(commits)),
            "embarlet_md5": next(iter(embarlet_hashes)),
            "client_md5": next(iter(client_hashes)),
        },
        "comparisons": comparisons,
        "summary": summaries,
    }

    if args.csv_out:
        args.csv_out.parent.mkdir(parents=True, exist_ok=True)
        with args.csv_out.open("w", newline="") as handle:
            writer = csv.DictWriter(
                handle,
                fieldnames=[
                    "variant",
                    "cell",
                    "n_clients",
                    "metric",
                    "trials",
                    "median_gbps",
                    "delta_from_v0_gbps",
                    "delta_from_v0_pct",
                ],
            )
            writer.writeheader()
            for row in summaries:
                writer.writerow({key: row[key] for key in writer.fieldnames})
    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")

    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
