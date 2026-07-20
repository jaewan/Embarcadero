#!/usr/bin/env python3
"""Plot the per-session isolation figure for the Embarcadero paper.

Shows four independent sessions during a broker-1 failure:
  - Sessions 0, 2, 3 (on surviving brokers): throughput flat throughout.
  - Session 1 (on failed broker): stalls on detection, fences at lease, resumes.

Usage:
  python3 scripts/publication/plot_session_isolation.py \\
    --data-dir data/session_isolation/trial_1 \\
    --failed-broker 1 \\
    --kill-ms 1800 \\
    [--lease-ms 180000] \\
    [--out Paper/Figures/session_isolation.pdf]
"""

from __future__ import annotations

import argparse
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import pandas as pd

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.join(SCRIPT_DIR, "..", "..")

# Session colors: survivors blue palette, failed session red
SESSION_COLORS = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd"]
AGGREGATE_COLOR = "black"
KILL_COLOR = "darkred"
DETECT_COLOR = "#e05c00"
YMAX_CAP = 7.0  # GB/s cap for y-axis


def load_combined(data_dir: str, failed_broker: int) -> pd.DataFrame:
    combined = os.path.join(data_dir, "combined.csv")
    if not os.path.exists(combined):
        raise FileNotFoundError(f"combined.csv not found in {data_dir}")
    df = pd.read_csv(combined)
    df["t_sec"] = df["Timestamp(ms)"] / 1000.0
    return df


def make_figure(
    data_dir: str,
    failed_broker: int,
    kill_ms: int,
    lease_ms: int,
    detect_ms: int = 114,
) -> plt.Figure:
    df = load_combined(data_dir, failed_broker)
    n_sessions = sum(1 for c in df.columns if c.startswith("Session_") and c.endswith("_GBps"))
    session_cols = [f"Session_{i}_GBps" for i in range(n_sessions)]

    t = df["t_sec"]
    kill_s = kill_ms / 1000.0
    detect_s = (kill_ms + detect_ms) / 1000.0
    # Lease fires after kill; session fences and replays.
    fence_s = (kill_ms + lease_ms) / 1000.0

    fig, ax = plt.subplots(figsize=(4.5, 2.8))

    # Draw per-session throughput
    for i, col in enumerate(session_cols):
        if col not in df.columns:
            continue
        label = (
            f"Session {i} (broker {i}, failed)" if i == failed_broker
            else f"Session {i} (broker {i})"
        )
        color = SESSION_COLORS[i % len(SESSION_COLORS)]
        lw = 1.0 if i != failed_broker else 1.4
        ls = "-" if i != failed_broker else "--"
        ax.step(t, df[col], where="post", color=color, linewidth=lw,
                linestyle=ls, label=label, zorder=3 if i == failed_broker else 2)

    # Draw aggregate
    ax.step(t, df["Total_GBps"], where="post", color=AGGREGATE_COLOR,
            linewidth=1.6, linestyle=":", label="Aggregate", zorder=4)

    # Event lines
    ymax = min(df[session_cols].values.max() * 1.15, YMAX_CAP)
    ax.axvline(kill_s, color=KILL_COLOR, linewidth=0.9, linestyle="-", zorder=5)
    ax.axvline(detect_s, color=DETECT_COLOR, linewidth=0.9, linestyle="--", zorder=5)
    if fence_s < t.max():
        ax.axvline(fence_s, color="#666666", linewidth=0.8, linestyle=":", zorder=5)

    # Annotations
    def _ann(x, label, color, ypos=0.92):
        ax.text(x + 0.05, ypos * ymax, label, color=color,
                fontsize=6.5, va="top", ha="left", rotation=90)

    _ann(kill_s, "T0: kill", KILL_COLOR, 0.95)
    _ann(detect_s, "T2: detect", DETECT_COLOR, 0.95)
    if fence_s < t.max():
        _ann(fence_s, "T4: fence+replay", "#555555", 0.95)

    ax.set_xlabel("Time (s)", fontsize=8)
    ax.set_ylabel("ACK throughput (GB/s)", fontsize=8)
    ax.set_title(
        "Per-session isolation: only session pinned to failed broker stalls",
        fontsize=8, pad=4
    )
    ax.set_xlim(0, float(t.max()) * 1.03)
    ax.set_ylim(0, ymax)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(1.0))
    ax.tick_params(labelsize=7)
    ax.grid(axis="y", linewidth=0.4, alpha=0.5)

    # Annotation box explaining what the figure shows
    ax.annotate(
        "Surviving sessions\ncontinue at full rate",
        xy=(kill_s + 0.2, df[[c for c in session_cols if "0" in c]].iloc[-10:].mean().mean()),
        xytext=(kill_s + 0.8, ymax * 0.5),
        fontsize=6.5,
        arrowprops=dict(arrowstyle="->", color="#1f77b4", lw=0.8),
        color="#1f77b4",
    )

    ax.legend(
        fontsize=6.5, loc="lower right",
        handlelength=1.5, framealpha=0.85,
        ncol=1 if n_sessions <= 4 else 2,
    )

    fig.tight_layout()
    return fig


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--data-dir", default="data/session_isolation/trial_1",
                   help="Directory containing combined.csv")
    p.add_argument("--failed-broker", type=int, default=1,
                   help="Index of the killed broker (default 1)")
    p.add_argument("--kill-ms", type=int, default=1800,
                   help="Wall-clock ms after publish start when broker was killed")
    p.add_argument("--lease-ms", type=int, default=180000,
                   help="Session lease duration (ms)")
    p.add_argument("--detect-ms", type=int, default=114,
                   help="Detection latency after kill (ms)")
    p.add_argument(
        "--out",
        default=os.path.join(REPO_ROOT, "Paper", "Figures", "session_isolation.pdf"),
    )
    args = p.parse_args(argv)

    if not os.path.isdir(args.data_dir):
        print(f"ERROR: data-dir not found: {args.data_dir}", file=sys.stderr)
        return 1

    fig = make_figure(
        args.data_dir,
        args.failed_broker,
        args.kill_ms,
        args.lease_ms,
        args.detect_ms,
    )

    os.makedirs(os.path.dirname(os.path.abspath(args.out)) or ".", exist_ok=True)
    fig.savefig(args.out, bbox_inches="tight", dpi=300)
    png = args.out.replace(".pdf", ".png")
    fig.savefig(png, bbox_inches="tight", dpi=200)
    print(f"Saved: {args.out}")
    print(f"Saved: {png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
