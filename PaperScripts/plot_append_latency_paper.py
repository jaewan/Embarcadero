#!/usr/bin/env python3
"""Paper-ready append_latency.pdf for Embarcadero.

Shows ONLY Embarcadero O5 ACK2 RF2 DRAM across offered load (the primary sweep).
X-axis: offered load (MB/s), Y-axis: append→ACK P50 and P99 (µs), linear scale.
Single-panel column-width figure.
"""
from __future__ import annotations
import argparse, csv, math, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# Publication colors
C_P50 = "#2166AC"   # steel blue
C_P99 = "#6BAED6"   # lighter blue for P99

FONT_SIZE = 10
TICK_SIZE = 9
PRIMARY_CELL = "fig2_embar_o5_ack2_rf2_mem"


def load(csv_path: str):
    """Return {target_mbps: [p50_us, ...]} and {target_mbps: [p99_us, ...]}."""
    p50s: dict[int, list[float]] = defaultdict(list)
    p99s: dict[int, list[float]] = defaultdict(list)
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("status") != "ok":
                continue
            if row.get("cell") != PRIMARY_CELL:
                continue
            try:
                tgt  = int(float(row["target_mbps"]))
                p50  = float(row["pub_ack_p50_us"])
                p99  = float(row["pub_ack_p99_us"])
            except (KeyError, ValueError, TypeError):
                continue
            p50s[tgt].append(p50)
            p99s[tgt].append(p99)
    return p50s, p99s


def median(vals):
    if not vals: return float("nan")
    s = sorted(vals)
    n = len(s)
    return (s[n//2-1] + s[n//2]) / 2 if n % 2 == 0 else s[n//2]


def plot(csv_path: str, pdf_path: str, png_path: str | None):
    p50s, p99s = load(csv_path)

    loads = sorted(p50s.keys())
    x   = np.array(loads, dtype=float)
    y50 = np.array([median(p50s[t]) / 1000.0 for t in loads])  # → ms
    y99 = np.array([median(p99s[t]) / 1000.0 for t in loads])

    plt.rcParams.update({
        "font.size": FONT_SIZE, "pdf.fonttype": 42, "ps.fonttype": 42,
        "axes.labelsize": FONT_SIZE, "xtick.labelsize": TICK_SIZE,
        "ytick.labelsize": TICK_SIZE, "legend.fontsize": 9,
    })

    fig, ax = plt.subplots(figsize=(3.5, 3.0))  # single column

    ax.plot(x, y50, color=C_P50, marker="o", linewidth=1.8,
            markersize=6, label="P50", zorder=3)
    ax.plot(x, y99, color=C_P99, marker="s", linewidth=1.5,
            markersize=5, linestyle="--", label="P99", zorder=3)

    # Horizontal reference at 1ms
    ax.axhline(1.0, color="#999999", linestyle=":", linewidth=0.9, alpha=0.7)
    ax.text(100, 1.05, "1 ms", fontsize=7.5, color="#888888", va="bottom")

    ax.set_xlabel("Offered load (MB/s)", fontsize=FONT_SIZE)
    ax.set_ylabel("Append→ACK latency (ms)", fontsize=FONT_SIZE)
    ax.set_xlim(0, 2200)
    ax.set_ylim(0, max(y99) * 1.18)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(0.5))
    ax.tick_params(labelsize=TICK_SIZE)
    ax.grid(True, alpha=0.2, color="#cccccc", linestyle=":")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(loc="upper left", framealpha=0.92, edgecolor="#cccccc",
              handlelength=1.5)

    fig.tight_layout()
    os.makedirs(os.path.dirname(os.path.abspath(pdf_path)) or ".", exist_ok=True)
    fig.savefig(pdf_path, bbox_inches="tight")
    if png_path:
        fig.savefig(png_path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {pdf_path}" + (f" and {png_path}" if png_path else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", default="data/paper_eval/fig2/fig2_append_latency/results.csv")
    ap.add_argument("--pdf", default="Paper/Figures/append_latency.pdf")
    ap.add_argument("--png", default="Paper/Figures/append_latency.png")
    args = ap.parse_args()
    plot(args.csv, args.pdf, args.png)


if __name__ == "__main__":
    main()
