#!/usr/bin/env python3
"""Reproduce paper Fig. 1 from the fixed-commit Embarcadero/Scalog campaigns.

The script deliberately consumes raw campaign CSVs instead of embedding paper
numbers.  It accepts every successful row at the requested commit; no
performance-based outlier filtering is performed.  N<=3 uses overlap
throughput and N=4 uses ACK-drain aggregate bandwidth, matching the paper.
It also writes the exact selected rows and input hashes beside the figure.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import statistics
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


SYSTEMS = {
    "embar": ("Embarcadero", "#2166ac", "o"),
    "scalog": ("CXL-Scalog", "#1b9e77", "s"),
}
SINKS = (("disk", "(a) NVMe-durable replica"), ("mem", "(b) DRAM replica"))
REPO_ROOT = Path(__file__).resolve().parents[1]


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def portable_path(path: Path) -> str:
    """Prefer repository-relative paths so manifests work after cloning."""
    resolved = path.resolve()
    try:
        return str(resolved.relative_to(REPO_ROOT))
    except ValueError:
        return str(resolved)


def load(path: Path, commit: str, expected_system: str) -> list[dict[str, str]]:
    selected: list[dict[str, str]] = []
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("status") != "ok":
                continue
            if row.get("git_commit") != commit:
                continue
            if row.get("system") != expected_system:
                continue
            if row.get("sink") not in {"disk", "mem"}:
                continue
            if row.get("rf") != "2" or row.get("ack") != "2":
                continue
            if row.get("msg_size") != "4096" or row.get("num_brokers") != "4":
                continue
            n = int(row["n_clients"])
            metric = "bandwidth_sum_gbps" if n == 4 else "overlap_gbps"
            try:
                value = float(row[metric])
            except (KeyError, TypeError, ValueError):
                continue
            row = dict(row)
            row["figure_metric"] = metric
            row["figure_value_gbps"] = f"{value:.9f}"
            selected.append(row)
    return selected


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--embar-csv", type=Path, required=True)
    parser.add_argument("--scalog-csv", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--pdf", type=Path, required=True)
    parser.add_argument("--png", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--selected-csv", type=Path, required=True)
    args = parser.parse_args()

    rows = (
        load(args.embar_csv, args.commit, "embar")
        + load(args.scalog_csv, args.commit, "scalog")
    )
    grouped: dict[tuple[str, str, int], list[float]] = defaultdict(list)
    for row in rows:
        grouped[(row["system"], row["sink"], int(row["n_clients"]))].append(
            float(row["figure_value_gbps"])
        )

    missing = [
        (system, sink, n)
        for system in SYSTEMS
        for sink, _ in SINKS
        for n in range(1, 5)
        if len(grouped[(system, sink, n)]) < 3
    ]
    if missing:
        raise SystemExit(f"fewer than three selected trials for cells: {missing}")

    plt.rcParams.update(
        {
            "font.family": "serif",
            "font.size": 8,
            "axes.labelsize": 8,
            "axes.titlesize": 8.5,
            "xtick.labelsize": 7,
            "ytick.labelsize": 7,
            "legend.fontsize": 7,
            "pdf.fonttype": 42,
        }
    )
    fig, axes = plt.subplots(1, 2, figsize=(7.0, 3.05), sharey=True)
    for ax, (sink, title) in zip(axes, SINKS):
        for system, (label, color, marker) in SYSTEMS.items():
            x = [1, 2, 3, 4]
            vals = [grouped[(system, sink, n)] for n in x]
            medians = [statistics.median(v) for v in vals]
            stddev = [statistics.stdev(v) if len(v) > 1 else 0.0 for v in vals]
            ax.errorbar(
                x,
                medians,
                yerr=stddev,
                label=label,
                color=color,
                marker=marker,
                linewidth=1.7,
                markersize=5,
                capsize=2.5,
            )
        ax.set_title(title)
        ax.set_xlabel("Concurrent publishers ($N$)")
        ax.set_xticks([1, 2, 3, 4])
        ax.set_xlim(0.75, 4.25)
        ax.grid(True, alpha=0.25, linestyle=":")
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    axes[0].set_ylabel("ACK-paced throughput (GB/s)")
    axes[0].set_ylim(bottom=0)
    axes[0].legend(loc="best", frameon=True)
    fig.tight_layout()

    args.pdf.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(
        args.pdf,
        bbox_inches="tight",
        metadata={"CreationDate": None, "ModDate": None},
    )
    if args.png:
        args.png.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(args.png, dpi=200, bbox_inches="tight")
    plt.close(fig)

    args.selected_csv.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = list(rows[0].keys())
    with args.selected_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=fieldnames, lineterminator="\n"
        )
        writer.writeheader()
        writer.writerows(rows)

    summary = {}
    for (system, sink, n), values in sorted(grouped.items()):
        summary[f"{system}.{sink}.n{n}"] = {
            "n": len(values),
            "median_gbps": statistics.median(values),
            "stddev_gbps": statistics.stdev(values) if len(values) > 1 else 0.0,
            "values_gbps": values,
        }
    manifest = {
        "contract": {
            "git_commit": args.commit,
            "selection": "all status=ok rows at commit; no outlier filtering",
            "n_le_3_metric": "overlap_gbps",
            "n_eq_4_metric": "bandwidth_sum_gbps",
            "rf": 2,
            "ack": 2,
            "message_bytes": 4096,
            "brokers": 4,
        },
        "inputs": {
            portable_path(args.embar_csv): sha256(args.embar_csv),
            portable_path(args.scalog_csv): sha256(args.scalog_csv),
        },
        "generator": {
            "path": portable_path(Path(__file__)),
            "sha256": sha256(Path(__file__).resolve()),
        },
        "outputs": {
            portable_path(args.pdf): sha256(args.pdf),
            **({portable_path(args.png): sha256(args.png)} if args.png else {}),
        },
        "selected_rows_csv": portable_path(args.selected_csv),
        "summary": summary,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
