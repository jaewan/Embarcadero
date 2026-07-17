#!/usr/bin/env python3
"""Plot Fig1 append throughput vs N from the appendable campaign CSV.

Two-panel figure (disk | DRAM) at column width for two-column IEEE/ACM paper.

Usage:
  python3 PaperScripts/plot_fig1_throughput_scaling.py \\
    --csv data/paper_eval/fig1/fig1_rf2_ack2_scaling/results.csv \\
    --pdf Paper/Figures/throughput_scaling.pdf \\
    --png Paper/Figures/throughput_scaling.png
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
import matplotlib.ticker as ticker

# ---------------------------------------------------------------------------
# Publication color palette (IEEE/ACM standard)
# ---------------------------------------------------------------------------
COLOR_EMBAR  = "#2166AC"   # steel blue
COLOR_SCALOG = "#4DAC26"   # dark green
COLOR_CORFU  = "#D73027"   # deep red

MARKER_SIZE  = 7
LINE_WIDTH   = 1.8
FONT_SIZE    = 10
TICK_SIZE    = 9

# ---------------------------------------------------------------------------
# Series definition: (system, sink, label, color, linestyle, marker)
# Corfu is handled separately (single point, no line).
# ---------------------------------------------------------------------------
SERIES_DISK = [
    ("embar",  "disk", "Embarcadero disk", COLOR_EMBAR,  "-",  "o"),
    ("scalog", "disk", "CXL-Scalog disk",  COLOR_SCALOG, "-",  "^"),
]
SERIES_DRAM = [
    ("embar",  "mem",  "Embarcadero DRAM", COLOR_EMBAR,  "--", "o"),
    ("scalog", "mem",  "CXL-Scalog DRAM",  COLOR_SCALOG, "--", "^"),
]

X_TICKS  = [1, 2, 3, 4]
X_LABELS = ["1 remote", "2 remote", "3 remote", "3R+1L"]


# ---------------------------------------------------------------------------
# Data helpers
# ---------------------------------------------------------------------------

def mean_std(vals: list[float]) -> tuple[float, float]:
    if not vals:
        return float("nan"), float("nan")
    m = sum(vals) / len(vals)
    if len(vals) == 1:
        return m, 0.0
    var = sum((v - m) ** 2 for v in vals) / (len(vals) - 1)
    return m, math.sqrt(var)


def load_ok(csv_path: str) -> dict[tuple[str, str, int], list[float]]:
    """(system, sink, n) -> list of throughput values in GB/s."""
    cells: dict[tuple[str, str, int], list[float]] = defaultdict(list)
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("status") != "ok":
                continue
            try:
                system = row["system"]
                sink   = row["sink"]
                n      = int(row["n_clients"])
            except (KeyError, ValueError):
                continue
            # N=4 co-located point: overlap window collapses, use bandwidth_sum.
            if n == 4:
                metric_order = ("bandwidth_sum_gbps", "overlap_gbps", "send_done_sum_gbps")
            else:
                metric_order = ("overlap_gbps", "send_done_sum_gbps", "bandwidth_sum_gbps")
            val = None
            for key in metric_order:
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


# ---------------------------------------------------------------------------
# Plot helpers
# ---------------------------------------------------------------------------

def _draw_series(ax, cells, series, xs_all):
    """Draw a list of (system, sink, label, color, ls, marker) series on ax."""
    for system, sink, label, color, ls, marker in series:
        xs, ys, yerr = [], [], []
        for n in xs_all:
            vals = cells.get((system, sink, n), [])
            if not vals:
                continue
            m, s = mean_std(vals)
            xs.append(n)
            ys.append(m)
            yerr.append(s if len(vals) > 1 else 0.0)
        if not xs:
            continue
        ax.errorbar(
            xs, ys,
            yerr=yerr if any(e > 0 for e in yerr) else None,
            label=label,
            color=color,
            linestyle=ls,
            marker=marker,
            linewidth=LINE_WIDTH,
            markersize=MARKER_SIZE,
            capsize=3,
        )


def _style_ax(ax, show_ylabel: bool):
    """Apply shared axis styling."""
    ax.set_xticks(X_TICKS)
    ax.set_xticklabels(X_LABELS, fontsize=TICK_SIZE)
    ax.set_ylim(0, 10)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(2))
    ax.tick_params(axis="y", labelsize=TICK_SIZE)
    ax.set_xlabel("Number of publishers (N)", fontsize=FONT_SIZE)
    if show_ylabel:
        ax.set_ylabel("Append throughput (GB/s)", fontsize=FONT_SIZE)
    ax.grid(True, alpha=0.25, color="#cccccc", linestyle=":")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)


# ---------------------------------------------------------------------------
# Main plot function
# ---------------------------------------------------------------------------

def plot(csv_path: str, pdf_path: str, png_path: str | None) -> None:
    cells  = load_ok(csv_path)
    xs_all = sorted({n for (_, _, n) in cells.keys()} or [1, 2, 3, 4])

    plt.rcParams.update({
        "font.size":        FONT_SIZE,
        "axes.titlesize":   FONT_SIZE,
        "axes.labelsize":   FONT_SIZE,
        "xtick.labelsize":  TICK_SIZE,
        "ytick.labelsize":  TICK_SIZE,
        "legend.fontsize":  9,
        "pdf.fonttype":     42,
        "ps.fonttype":      42,
    })

    fig, (ax_disk, ax_mem) = plt.subplots(
        1, 2,
        figsize=(7.2, 4.0),
        sharey=True,
        gridspec_kw={"wspace": 0.08},
        constrained_layout=True,
    )

    # ---- Left panel: disk ------------------------------------------------
    _draw_series(ax_disk, cells, SERIES_DISK, xs_all)

    # Corfu: single red square at N=1, disk only (~0.46 GB/s)
    corfu_disk_vals = cells.get(("corfu", "disk", 1), [])
    if corfu_disk_vals:
        corfu_y = mean_std(corfu_disk_vals)[0]
    else:
        corfu_y = 0.46   # fallback sentinel from known measurement

    ax_disk.plot(
        [1], [corfu_y],
        marker="s", color=COLOR_CORFU, linestyle="none",
        markersize=MARKER_SIZE, label="CXL-Corfu disk (N=1)",
        zorder=5,
    )
    ax_disk.annotate(
        "Corfu\n(token limit)",
        xy=(1, corfu_y),
        xytext=(1.3, 1.8),
        fontsize=8,
        color=COLOR_CORFU,
        ha="left",
        arrowprops=dict(
            arrowstyle="->",
            color=COLOR_CORFU,
            lw=1.0,
            connectionstyle="arc3,rad=-0.2",
        ),
    )

    # "NVMe-bound" annotation — right edge, between the two disk lines
    ax_disk.text(
        3.7, 1.3,
        "NVMe-bound",
        fontsize=8, color="#555555",
        ha="right", va="center",
        style="italic",
    )

    ax_disk.set_title("(a) Disk-durable sink", fontsize=FONT_SIZE)
    _style_ax(ax_disk, show_ylabel=True)
    ax_disk.legend(
        loc="center right",
        framealpha=0.92,
        edgecolor="#cccccc",
        fontsize=9,
    )

    # ---- Right panel: DRAM -----------------------------------------------
    _draw_series(ax_mem, cells, SERIES_DRAM, xs_all)

    # "CXL+NIC-bound" annotation — below the plateau, not overlapping lines
    ax_mem.text(
        2.0, 5.2,
        "CXL+NIC-bound\n(N≤3 remote)",
        fontsize=8, color="#555555",
        ha="center", va="center",
        style="italic",
    )

    ax_mem.set_title("(b) DRAM-replica sink", fontsize=FONT_SIZE)
    _style_ax(ax_mem, show_ylabel=False)
    ax_mem.legend(
        loc="upper left",
        framealpha=0.92,
        edgecolor="#cccccc",
        fontsize=9,
    )


    os.makedirs(os.path.dirname(os.path.abspath(pdf_path)) or ".", exist_ok=True)
    fig.savefig(pdf_path, bbox_inches="tight")
    if png_path:
        fig.savefig(png_path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {pdf_path}" + (f"  and  {png_path}" if png_path else ""))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--csv",
        default="data/paper_eval/fig1/fig1_rf2_ack2_scaling/results.csv",
    )
    ap.add_argument(
        "--pdf",
        default="Paper/Figures/throughput_scaling.pdf",
    )
    ap.add_argument(
        "--png",
        default="Paper/Figures/throughput_scaling.png",
    )
    args = ap.parse_args()
    if not os.path.isfile(args.csv):
        raise SystemExit(f"CSV not found: {args.csv}")
    plot(args.csv, args.pdf, args.png)


if __name__ == "__main__":
    main()
