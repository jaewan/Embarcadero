#!/usr/bin/env python3
"""Plot Fig1 append throughput vs N from the appendable campaign CSV.

Usage:
  python3 PaperScripts/plot_fig1_throughput_scaling.py \\
    --csv data/paper_eval/fig1/fig1_rf2_ack2_scaling/results.csv \\
    --pdf data/paper_eval/fig1/fig1_rf2_ack2_scaling/fig1_throughput_scaling.pdf
"""

from __future__ import annotations

import argparse
import csv
import math
import os
from collections import defaultdict

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


SERIES = [
    # (system, sink, label, color, linestyle, marker)
    ("embar", "disk", "Embar disk", "#1f77b4", "-", "o"),
    ("embar", "mem", "Embar mem", "#1f77b4", "--", "o"),
    ("corfu", "disk", "Corfu disk", "#ff7f0e", "-", "s"),
    ("corfu", "mem", "Corfu mem", "#ff7f0e", "--", "s"),
    ("scalog", "disk", "Scalog disk", "#2ca02c", "-", "^"),
    ("scalog", "mem", "Scalog mem", "#2ca02c", "--", "^"),
    ("lazylog", "disk", "LazyLog disk", "#d62728", "-", "D"),
    ("lazylog", "mem", "LazyLog mem", "#d62728", "--", "D"),
]


def mean_std(vals: list[float]) -> tuple[float, float]:
    if not vals:
        return float("nan"), float("nan")
    m = sum(vals) / len(vals)
    if len(vals) == 1:
        return m, 0.0
    var = sum((v - m) ** 2 for v in vals) / (len(vals) - 1)
    return m, math.sqrt(var)


def load_ok(csv_path: str) -> dict[tuple[str, str, int], list[float]]:
    """(system, sink, n) -> list of overlap_gbps (fallback send_done / bandwidth)."""
    cells: dict[tuple[str, str, int], list[float]] = defaultdict(list)
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("status") != "ok":
                continue
            try:
                system = row["system"]
                sink = row["sink"]
                n = int(row["n_clients"])
            except (KeyError, ValueError):
                continue
            val = None
            for key in ("overlap_gbps", "send_done_sum_gbps", "bandwidth_sum_gbps"):
                raw = (row.get(key) or "").strip()
                if not raw:
                    continue
                try:
                    val = float(raw)
                    break
                except ValueError:
                    continue
            if val is None:
                continue
            cells[(system, sink, n)].append(val)
    return cells


def plot(csv_path: str, pdf_path: str, png_path: str | None) -> None:
    cells = load_ok(csv_path)
    fig, ax = plt.subplots(figsize=(7.2, 4.4))

    xs_all = sorted({n for (_, _, n) in cells.keys()} or [1, 2, 3, 4])

    for system, sink, label, color, ls, marker in SERIES:
        xs, ys, yerr, ns = [], [], [], []
        for n in xs_all:
            vals = cells.get((system, sink, n), [])
            if not vals:
                continue
            m, s = mean_std(vals)
            xs.append(n)
            ys.append(m)
            yerr.append(s if len(vals) > 1 else 0.0)
            ns.append(len(vals))
        if not xs:
            continue
        ax.errorbar(
            xs,
            ys,
            yerr=yerr if any(e > 0 for e in yerr) else None,
            label=f"{label}" + (f" (n={min(ns)}–{max(ns)})" if ns else ""),
            color=color,
            linestyle=ls,
            marker=marker,
            linewidth=1.8,
            markersize=6,
            capsize=3,
        )

    ax.set_xlabel("Number of publishers (N)")
    ax.set_ylabel("Append throughput (GB/s)")
    ax.set_xticks([1, 2, 3, 4])
    ax.set_xticklabels(["1 remote", "2 remote", "3 remote", "3R+1L"])
    ax.set_title("Fig 1 draft — RF=2 ACK=2 (overlap GB/s)")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=8, ncol=2, loc="best")
    fig.tight_layout()

    os.makedirs(os.path.dirname(os.path.abspath(pdf_path)) or ".", exist_ok=True)
    fig.savefig(pdf_path)
    if png_path:
        fig.savefig(png_path, dpi=160)
    plt.close(fig)
    print(f"wrote {pdf_path}" + (f" and {png_path}" if png_path else ""))


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--csv",
        default="data/paper_eval/fig1/fig1_rf2_ack2_scaling/results.csv",
    )
    ap.add_argument(
        "--pdf",
        default="data/paper_eval/fig1/fig1_rf2_ack2_scaling/fig1_throughput_scaling.pdf",
    )
    ap.add_argument(
        "--png",
        default="data/paper_eval/fig1/fig1_rf2_ack2_scaling/fig1_throughput_scaling.png",
    )
    args = ap.parse_args()
    if not os.path.isfile(args.csv):
        raise SystemExit(f"CSV not found: {args.csv}")
    plot(args.csv, args.pdf, args.png)


if __name__ == "__main__":
    main()
