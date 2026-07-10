#!/usr/bin/env python3
"""Aggregate YCSB evaluation results from kv_bench summary.csv files.

Usage:
    python3 scripts/aggregate_ycsb.py <data/ycsb_eval/RUNTAG>

The script walks the given directory recursively, collects all summary.csv
files, groups results by (system, workload, key_dist), computes median
throughput_ops_sec over trials, and prints a Markdown table.

It also prints improvement ratios of EMBARCADERO over each baseline
(CORFU, LAZYLOG) for every (workload, key_dist) combination.
"""

import argparse
import csv
import os
import statistics
import sys
from collections import defaultdict
from pathlib import Path


def find_summary_csvs(root: Path):
    """Yield (path, rows) for every summary.csv found under root."""
    for dirpath, _dirs, files in os.walk(root):
        for fname in files:
            if fname == "summary.csv":
                p = Path(dirpath) / fname
                try:
                    with p.open(newline="") as f:
                        reader = csv.DictReader(f)
                        rows = list(reader)
                    if rows:
                        yield p, rows
                except Exception as exc:  # noqa: BLE001
                    print(f"WARNING: could not read {p}: {exc}", file=sys.stderr)


def infer_cell_from_path(path: Path, row: dict) -> tuple[str, str, str]:
    """Return (system, workload, key_dist) from CSV row or directory name.

    kv_bench writes metadata columns: sequencer, and (when task-20 flags land)
    workload and key_dist.  Fall back to parent-directory name parsing when
    those columns are absent.
    """
    system = row.get("sequencer", "").strip().upper()
    workload = row.get("workload", "").strip().upper()
    key_dist = row.get("key_dist", "").strip().lower()

    # Fall back to directory-name parsing: SYSTEM_WL_DIST/run_id/summary.csv
    # Directory structure produced by run_ycsb_eval.sh:
    #   data/ycsb_eval/RUNTAG/SYSTEM_WL_DIST/run_id/summary.csv
    if not system or not workload or not key_dist:
        cell_dir = path.parent.parent  # go up past run_id/
        parts = cell_dir.name.split("_")  # e.g. EMBARCADERO_A_uniform
        if len(parts) >= 3:
            system = system or parts[0].upper()
            workload = workload or parts[1].upper()
            key_dist = key_dist or parts[2].lower()

    return system, workload, key_dist


def collect_results(root: Path) -> dict:
    """Return {(system, workload, key_dist): [throughput_ops_sec, ...]}."""
    results: dict = defaultdict(list)

    for path, rows in find_summary_csvs(root):
        for row in rows:
            system, workload, key_dist = infer_cell_from_path(path, row)
            tput_str = row.get("throughput_ops_sec", "").strip()
            if not (system and workload and key_dist and tput_str):
                continue
            try:
                tput = float(tput_str)
            except ValueError:
                continue
            results[(system, workload, key_dist)].append(tput)

    return results


def median_results(raw: dict) -> dict:
    """Return {(system, workload, key_dist): median_throughput}."""
    return {k: statistics.median(v) for k, v in raw.items() if v}


def build_table(medians: dict):
    """Print a Markdown table: rows=workload, cols=system+dist."""
    # Collect dimensions
    systems = sorted({k[0] for k in medians})
    workloads = sorted({k[1] for k in medians})
    dists = sorted({k[2] for k in medians})

    # Column order: EMBARCADERO first, then alphabetical baselines
    preferred_order = ["EMBARCADERO", "CORFU", "LAZYLOG"]
    systems_sorted = sorted(
        systems,
        key=lambda s: (preferred_order.index(s) if s in preferred_order else 99, s),
    )

    # Build column headers: system_dist
    col_keys = [(sys, dist) for sys in systems_sorted for dist in dists]
    col_headers = [f"{sys}\n{dist}" for sys, dist in col_keys]

    # Header row
    header = "| workload | " + " | ".join(
        f"{sys}_{dist}" for sys, dist in col_keys
    ) + " |"
    sep = "| --- | " + " | ".join("---:" for _ in col_keys) + " |"

    print("\n## Median Throughput (ops/s)\n")
    print(header)
    print(sep)

    for wl in workloads:
        cells = []
        for sys, dist in col_keys:
            val = medians.get((sys, wl, dist))
            cells.append(f"{val:,.0f}" if val is not None else "—")
        print(f"| {wl} | " + " | ".join(cells) + " |")


def build_ratio_table(medians: dict):
    """Print improvement ratios of EMBARCADERO over baselines."""
    baselines = ["CORFU", "LAZYLOG"]
    workloads = sorted({k[1] for k in medians})
    dists = sorted({k[2] for k in medians})

    print("\n## EMBARCADERO Speedup vs Baselines\n")

    for baseline in baselines:
        col_keys = [(baseline, dist) for dist in dists]
        header = f"| workload | " + " | ".join(
            f"vs {baseline} ({dist})" for dist in dists
        ) + " |"
        sep = "| --- | " + " | ".join("---:" for _ in dists) + " |"
        print(f"### EMBARCADERO / {baseline}\n")
        print(header)
        print(sep)

        for wl in workloads:
            cells = []
            for dist in dists:
                emb = medians.get(("EMBARCADERO", wl, dist))
                base = medians.get((baseline, wl, dist))
                if emb is not None and base is not None and base > 0:
                    ratio = emb / base
                    cells.append(f"{ratio:.2f}x")
                else:
                    cells.append("—")
            print(f"| {wl} | " + " | ".join(cells) + " |")
        print()


def main():
    parser = argparse.ArgumentParser(
        description="Aggregate YCSB evaluation results into a Markdown table."
    )
    parser.add_argument(
        "root",
        nargs="?",
        default="data/ycsb_eval",
        help="Root directory to search for summary.csv files (default: data/ycsb_eval)",
    )
    parser.add_argument(
        "--trials",
        type=int,
        default=0,
        help="Expected trials per cell (0=no check). Warns if a cell has fewer.",
    )
    args = parser.parse_args()

    root = Path(args.root)
    if not root.exists():
        print(f"ERROR: directory not found: {root}", file=sys.stderr)
        sys.exit(1)

    raw = collect_results(root)
    if not raw:
        print("No summary.csv files found or no valid rows.", file=sys.stderr)
        sys.exit(1)

    if args.trials > 0:
        for key, vals in raw.items():
            if len(vals) < args.trials:
                print(
                    f"WARNING: {key} has only {len(vals)}/{args.trials} trial(s)",
                    file=sys.stderr,
                )

    medians = median_results(raw)

    print(f"# YCSB Evaluation Results\n")
    print(f"Source: `{root}`")
    print(f"Cells found: {len(medians)}")

    build_table(medians)
    build_ratio_table(medians)


if __name__ == "__main__":
    main()
