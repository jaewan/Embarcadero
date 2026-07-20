#!/usr/bin/env python3
"""Build the paper slow-replica summary directly from per-trial stage CSVs."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import statistics
from pathlib import Path


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def stage_p99(path: Path, stage: str) -> float:
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row["Stage"] == stage:
                return float(row["p99"])
    raise ValueError(f"{path}: missing stage {stage}")


def collect(root: Path, system: str, ack: int) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    pattern = f"trial_*/{system}/*/stage_latency_summary_ack{ack}.csv"
    for path in sorted(root.glob(pattern)):
        trial = int(path.parts[-4].split("_")[-1])
        mode = path.parts[-2]
        rows.append(
            {
                "system": system,
                "trial": trial,
                "mode": mode,
                "ack": ack,
                "stage": "append_send_to_ack",
                "p99_us": stage_p99(path, "append_send_to_ack"),
                "source": str(path),
                "sha256": digest(path),
            }
        )
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--embar-root", type=Path, required=True)
    parser.add_argument("--scalog-root", type=Path, required=True)
    parser.add_argument("--csv", type=Path, required=True)
    parser.add_argument("--json", type=Path, required=True)
    args = parser.parse_args()

    rows = (
        collect(args.embar_root, "EMBARCADERO", 1)
        + collect(args.embar_root, "EMBARCADERO", 2)
        + collect(args.scalog_root, "SCALOG", 1)
    )
    expected = {
        ("EMBARCADERO", "baseline", 1),
        ("EMBARCADERO", "slow_injected", 1),
        ("EMBARCADERO", "baseline", 2),
        ("EMBARCADERO", "slow_injected", 2),
        ("SCALOG", "baseline", 1),
        ("SCALOG", "slow_injected", 1),
    }
    grouped: dict[tuple[str, str, int], list[float]] = {}
    for key in expected:
        values = [
            float(r["p99_us"])
            for r in rows
            if (r["system"], r["mode"], r["ack"]) == key
        ]
        if len(values) != 3:
            raise SystemExit(f"expected exactly three trials for {key}, got {len(values)}")
        grouped[key] = values

    args.csv.parent.mkdir(parents=True, exist_ok=True)
    with args.csv.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=list(rows[0]), lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)

    summary: dict[str, object] = {
        "selection": "all three per-trial stage summaries; no fallback/copy-success rows",
        "rows_csv": str(args.csv),
        "groups": {},
    }
    for key, values in sorted(grouped.items()):
        system, mode, ack = key
        summary["groups"][f"{system}.ack{ack}.{mode}"] = {
            "n": len(values),
            "median_p99_us": statistics.median(values),
            "values_p99_us": values,
        }
    for system, ack in (("EMBARCADERO", 1), ("EMBARCADERO", 2), ("SCALOG", 1)):
        baseline = statistics.median(grouped[(system, "baseline", ack)])
        slow = statistics.median(grouped[(system, "slow_injected", ack)])
        summary["groups"][f"{system}.ack{ack}.ratio"] = slow / baseline
    args.json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
