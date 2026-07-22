#!/usr/bin/env python3
"""Reproduce the paper throughput figure from fixed-commit campaigns.

The script deliberately consumes raw campaign CSVs instead of embedding paper
numbers.  It accepts every successful row at the requested commit; no
performance-based outlier filtering is performed.  N<=3 uses overlap
throughput and N=4 uses ACK-drain aggregate bandwidth, matching the paper.
The third panel consumes the validated path-ablation summary.  It also writes
the exact selected rows and input hashes beside the figure.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import shutil
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


def load_ablation(path: Path) -> list[dict[str, str]]:
    wanted = ["V0", "V1", "V2", "V3", "V4"]
    rows: dict[str, dict[str, str]] = {}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("variant") in wanted:
                rows[row["variant"]] = row
    missing = [variant for variant in wanted if variant not in rows]
    if missing:
        raise SystemExit(f"path-ablation summary is missing variants: {missing}")
    for variant in wanted:
        if int(rows[variant]["trials"]) < 3:
            raise SystemExit(f"{variant} has fewer than three path-ablation trials")
    return [rows[variant] for variant in wanted]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--embar-csv", type=Path, required=True)
    parser.add_argument("--scalog-csv", type=Path, required=True)
    parser.add_argument("--path-summary", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--pdf", type=Path, required=True)
    parser.add_argument("--paper-pdf", type=Path)
    parser.add_argument("--png", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--selected-csv", type=Path, required=True)
    args = parser.parse_args()

    rows = (
        load(args.embar_csv, args.commit, "embar")
        + load(args.scalog_csv, args.commit, "scalog")
    )
    ablation = load_ablation(args.path_summary)
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
    fig, axes = plt.subplots(1, 3, figsize=(7.15, 2.35))
    for ax, (sink, title) in zip(axes[:2], SINKS):
        for system, (label, color, marker) in SYSTEMS.items():
            x = [1, 2, 3, 4]
            vals = [grouped[(system, sink, n)] for n in x]
            medians = [statistics.median(v) for v in vals]
            # Show every accepted trial. This exposes run-to-run variability
            # without centering a standard deviation on a median.
            for n, trials in zip(x, vals):
                if len(trials) == 1:
                    offsets = [0.0]
                else:
                    offsets = [(-0.055 + 0.11 * i / (len(trials) - 1))
                               for i in range(len(trials))]
                ax.scatter(
                    [n + offset for offset in offsets], trials,
                    color=color, marker=marker, s=10, alpha=0.28,
                    linewidths=0, zorder=2,
                )
            ax.plot(
                x[:3], medians[:3],
                label=label,
                color=color,
                marker=marker,
                linewidth=1.7,
                markersize=4.5,
                zorder=3,
            )
            # N=4 changes both the client roster and reported metric. Do not
            # draw it as a continuation of the remote overlap-throughput line.
            ax.plot(
                x[2:], medians[2:], color=color, linestyle="--",
                linewidth=1.0, zorder=2,
            )
            ax.scatter(
                [4], [medians[3]], facecolors="white", edgecolors=color,
                marker=marker, s=28, linewidths=1.2, zorder=4,
            )
        ax.set_title(title)
        ax.set_xlabel("Publishers ($N$)")
        ax.set_xticks([1, 2, 3, 4])
        ax.set_xticklabels(["1", "2", "3", "4*"])
        ax.set_xlim(0.75, 4.25)
        ax.grid(True, alpha=0.25, linestyle=":")
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
    axes[0].set_ylabel("Throughput (GB/s)")
    axes[0].set_ylim(0, 1.55)
    axes[1].set_ylim(0, 9.2)
    axes[0].legend(loc="best", frameon=True)

    ax = axes[2]
    variants = [row["variant"] for row in ablation]
    values = [float(row["median_gbps"]) for row in ablation]
    colors = ["#969696", "#2166ac", "#74add1", "#4393c3", "#2166ac"]
    bars = ax.bar(variants, values, color=colors, width=0.68)
    for bar, value in zip(bars, values):
        ax.text(
            bar.get_x() + bar.get_width() / 2, value + 0.22,
            f"{value:.2f}", ha="center", va="bottom", fontsize=6.2,
        )
    ax.set_title("(c) Path decomposition ($N=2$)")
    ax.set_xlabel("Variant")
    ax.set_ylim(0, 11.2)
    ax.grid(True, axis="y", alpha=0.25, linestyle=":")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    fig.tight_layout()

    args.pdf.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(
        args.pdf,
        bbox_inches="tight",
        metadata={"CreationDate": None, "ModDate": None},
    )
    if args.paper_pdf:
        args.paper_pdf.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.pdf, args.paper_pdf)
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
            portable_path(args.path_summary): sha256(args.path_summary),
        },
        "generator": {
            "path": portable_path(Path(__file__)),
            "sha256": sha256(Path(__file__).resolve()),
        },
        "outputs": {
            portable_path(args.pdf): sha256(args.pdf),
            **({portable_path(args.paper_pdf): sha256(args.paper_pdf)}
               if args.paper_pdf else {}),
            **({portable_path(args.png): sha256(args.png)} if args.png else {}),
        },
        "selected_rows_csv": portable_path(args.selected_csv),
        "summary": summary,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
