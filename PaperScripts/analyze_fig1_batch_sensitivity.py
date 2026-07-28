#!/usr/bin/env python3
"""Validate and summarize the 64 GiB publisher-batch sensitivity campaign."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import statistics
from pathlib import Path


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("fig1_root", type=Path)
    parser.add_argument("--campaign", default="fig1_batch_sensitivity_v1")
    parser.add_argument("--batches-kib", default="64,256,2048")
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--total-bytes", type=int, default=64 * 1024**3)
    args = parser.parse_args()

    batches = [int(value) for value in args.batches_kib.split(",")]
    all_rows: list[dict[str, str]] = []
    inputs: dict[str, str] = {}
    contracts: dict[str, dict[str, str]] = {}
    for batch in batches:
        campaign = args.fig1_root / f"{args.campaign}_b{batch}k"
        result_path = campaign / "results.csv"
        rows = [
            row
            for row in csv.DictReader(result_path.open())
            if row["cell"] == "fig1_embar_o5_mem_n2" and row["status"] == "ok"
        ]
        if len(rows) != args.trials:
            raise SystemExit(
                f"{campaign}: expected {args.trials} valid rows, found {len(rows)}"
            )
        for row in rows:
            expected = {
                "system": "embar",
                "sink": "mem",
                "n_clients": "2",
                "msg_size": "4096",
                "num_brokers": "4",
                "rf": "2",
                "ack": "2",
                "batch_kb": str(batch),
                "total_bytes": str(args.total_bytes),
            }
            for field, value in expected.items():
                if row[field] != value:
                    raise SystemExit(
                        f"{campaign}: {field}={row[field]!r}, expected {value!r}"
                    )
        all_rows.extend(rows)
        inputs[str(result_path.relative_to(args.fig1_root))] = sha256(result_path)
        contract_paths = sorted(campaign.glob("multiclient/**/run_contract.csv"))
        if len(contract_paths) != 1:
            raise SystemExit(f"{campaign}: expected exactly one run_contract.csv")
        contract_path = contract_paths[0]
        contract_rows = list(csv.DictReader(contract_path.open()))
        if len(contract_rows) != 1:
            raise SystemExit(f"{contract_path}: expected exactly one contract row")
        contracts[str(batch)] = contract_rows[0]
        inputs[str(contract_path.relative_to(args.fig1_root))] = sha256(contract_path)
        broker_logs = sorted(campaign.glob("multiclient/**/trial*_attempt1_broker0.log"))
        if len(broker_logs) != args.trials:
            raise SystemExit(
                f"{campaign}: expected {args.trials} head-broker logs, found {len(broker_logs)}"
            )
        required_segments = 4 + (
            args.total_bytes + 8 * 1024**3 - 1
        ) // (8 * 1024**3)
        required_prefault_gib = required_segments * 8
        for broker_log in broker_logs:
            match = re.search(
                r"segment region prefault complete \((\d+) GB",
                broker_log.read_text(errors="replace"),
            )
            if not match or int(match.group(1)) < required_prefault_gib:
                raise SystemExit(
                    f"{broker_log}: missing {required_prefault_gib} GiB working-set prefault"
                )
            inputs[str(broker_log.relative_to(args.fig1_root))] = sha256(broker_log)

    commits = {row["git_commit"] for row in all_rows}
    if len(commits) != 1:
        raise SystemExit(f"campaign mixes commits: {sorted(commits)}")
    summary = []
    medians: dict[int, float] = {}
    for batch in batches:
        values = [
            float(row["overlap_gbps"])
            for row in all_rows
            if int(row["batch_kb"]) == batch
        ]
        median = statistics.median(values)
        medians[batch] = median
        summary.append(
            {
                "batch_kib": batch,
                "median_gbps": median,
                "min_gbps": min(values),
                "max_gbps": max(values),
                "median_batches_per_s": median * 1e9 / (batch * 1024),
            }
        )
    reference = medians[max(batches)]
    for row in summary:
        row["relative_to_largest_batch"] = row["median_gbps"] / reference

    output_dir = args.fig1_root / args.campaign
    output_dir.mkdir(parents=True, exist_ok=True)
    summary_path = output_dir / "batch_sensitivity_summary.csv"
    with summary_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary[0]))
        writer.writeheader()
        writer.writerows(summary)
    manifest = {
        "schema": 1,
        "claim": (
            "Acknowledged throughput over a 64 GiB transfer with two remote "
            "publishers and two-copy DRAM completion, varying publisher batch size."
        ),
        "git_commit": next(iter(commits)),
        "contracts": contracts,
        "inputs_sha256": inputs,
        "summary": summary,
    }
    manifest_path = output_dir / "batch_sensitivity_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
