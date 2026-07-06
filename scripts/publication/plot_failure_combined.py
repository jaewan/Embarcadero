#!/usr/bin/env python3
"""
Generate two-panel broker-failure figure for the Embarcadero paper.

Panel (a): ORDER=4  — weak total ordering (no epoch sequencer)
Panel (b): ORDER=5  — strong total ordering (epoch sequencer)

Both panels share the y-axis so the pre/post-failure throughput levels are
directly comparable.

Usage:
  python plot_failure_combined.py [output.pdf]
"""

import os, sys, re
import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ── Paths ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT   = os.path.join(SCRIPT_DIR, "..", "..")
RUNS_DIR    = os.path.join(REPO_ROOT, "data", "failure_runs")

ORDER4_DIR  = os.path.join(RUNS_DIR, "20260402_c4_order4_matchdiag")
ORDER5_DIR  = os.path.join(RUNS_DIR, "20260402_c4_order5_acked_blogdiag")

DEFAULT_OUT = os.path.join(REPO_ROOT, "Paper", "Figures", "failure_combined.pdf")
OUTPUT      = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT

# ── Style ─────────────────────────────────────────────────────────────────────
BROKER_COLORS = [
    "#1f77b4",   # blue   – broker 0
    "#2ca02c",   # green  – broker 1
    "#9467bd",   # purple – broker 2
    "#8c564b",   # brown  – broker 3
]
FAILED_COLOR    = "#d62728"   # red
AGGREGATE_COLOR = "black"

KILL_COLOR    = "darkred"
DETECT_COLOR  = "#e05c00"
REROUTE_COLOR = "#1a9e4a"

THROUGHPUT_THRESHOLD = 0.01
TAIL_CUTOFF_FACTOR   = 0.70
ACK_MARGIN_SEC       = 1.0

# Cap displayed y-axis at this value; ORDER=5 diagnostic mode produces a
# burst spike above 35 GB/s (backlog flush after reroute) that would compress
# the steady-state lines to near-zero. We annotate the clipped spike instead.
YMAX_CAP_GBPS = 9.0


# ── Data loading ──────────────────────────────────────────────────────────────
def load_run(run_dir):
    tp_csv  = os.path.join(run_dir, "real_time_acked_throughput.csv")
    ev_csv  = os.path.join(run_dir, "failure_events.csv")

    data = pd.read_csv(tp_csv)
    first_ts = data["Timestamp(ms)"].iloc[0]

    # Trim trailing zeros
    active = data[data["Total_GBps"] > THROUGHPUT_THRESHOLD].index
    if len(active):
        data = data.iloc[: active[-1] + 2]

    # Trim tail-off by rolling average
    if len(data) > 5:
        peak    = data["Total_GBps"].max()
        cutoff  = peak * TAIL_CUTOFF_FACTOR
        rolling = data["Total_GBps"].rolling(window=3, min_periods=1).mean()
        above   = rolling[rolling >= cutoff].index
        if len(above):
            data = data.iloc[: min(above[-1] + 1, len(data))]

    x_sec = (data["Timestamp(ms)"] - first_ts) / 1000.0

    # Parse events
    events = {"kill": None, "detect": None, "reroute": None,
               "ack_frontier": None, "failed_idx": -1}
    try:
        ev = pd.read_csv(ev_csv)
        ev["t"] = (ev["Timestamp(ms)"] - first_ts) / 1000.0
        for _, row in ev.iterrows():
            desc = row["EventDescription"].lower()
            t    = row["t"]
            if "broker kill" in desc and events["kill"] is None:
                events["kill"] = t
            if "send fail" in desc and events["detect"] is None:
                events["detect"] = t
            if "reconnect success" in desc and events["reroute"] is None:
                events["reroute"] = t
            if "ack frontier" in desc and events["ack_frontier"] is None:
                events["ack_frontier"] = t
            if events["failed_idx"] < 0:
                m = re.search(r"broker (\d+)", desc)
                if m:
                    events["failed_idx"] = int(m.group(1))
    except Exception:
        pass

    # Clip at ACK frontier + margin
    if events["ack_frontier"] is not None:
        clip = events["ack_frontier"] + ACK_MARGIN_SEC
        mask = x_sec <= clip
        if mask.any() and mask.sum() >= 3:
            data  = data.iloc[: int(mask.sum())].copy()
            x_sec = x_sec.iloc[: int(mask.sum())]

    broker_cols = sorted(
        [c for c in data.columns if c.startswith("Broker_")],
        key=lambda n: int(n.replace("Broker_", "").replace("_GBps", ""))
    )
    return data, x_sec, broker_cols, events


# ── Figure ─────────────────────────────────────────────────────────────────────
def make_figure():
    plt.rcParams.update({
        "font.family":       "serif",
        "font.size":         8,
        "axes.titlesize":    8.5,
        "axes.labelsize":    8,
        "xtick.labelsize":   7,
        "ytick.labelsize":   7,
        "legend.fontsize":   7,
        "legend.framealpha": 0.9,
        "axes.spines.top":   False,
        "axes.spines.right": False,
        "axes.grid":         True,
        "grid.color":        "#dedede",
        "grid.linewidth":    0.5,
        "lines.linewidth":   1.4,
        "pdf.fonttype":      42,
    })

    panels = [
        (ORDER4_DIR, "(a) Strong total ordering, hold timeout\u202f=\u202f0"),
        (ORDER5_DIR, "(b) Strong total ordering, hold timeout\u202f=\u202f750\u202fms"),
    ]

    fig, axes = plt.subplots(
        1, 2, figsize=(6.8, 2.9),
        sharey=True,
        constrained_layout=True,
    )

    for ax, (run_dir, title) in zip(axes, panels):
        data, x_sec, broker_cols, ev = load_run(run_dir)
        failed = ev["failed_idx"]

        ax.set_title(title, pad=4)
        ax.set_xlabel("Time (s)")
        ax.set_xlim(0, x_sec.max() * 1.03)

        # ── Broker lines ──
        safe_idx = 0
        for i, col in enumerate(broker_cols):
            bnum = col.replace("Broker_", "").replace("_GBps", "")
            if i == failed:
                color = FAILED_COLOR
                label = f"Log server {bnum} (failed)"
                lw, alpha, zo = 1.4, 0.95, 3
            else:
                color = BROKER_COLORS[safe_idx % len(BROKER_COLORS)]
                safe_idx += 1
                label = f"Log server {bnum}"
                lw, alpha, zo = 1.2, 0.80, 2
            ax.step(x_sec, data[col], where="post",
                    linewidth=lw, color=color, alpha=alpha,
                    label=label, zorder=zo)

        # Aggregate
        if "Total_GBps" in data.columns:
            ax.step(x_sec, data["Total_GBps"], where="post",
                    linewidth=2.0, linestyle="--", color=AGGREGATE_COLOR,
                    alpha=0.85, label="Aggregate", zorder=4)

        # ── Event markers ──
        if ev["kill"] is not None and ev["reroute"] is not None:
            ax.axvspan(ev["kill"], ev["reroute"],
                       alpha=0.08, color="red", zorder=0)
        if ev["kill"] is not None:
            ax.axvline(ev["kill"],  color=KILL_COLOR,    linestyle="--",
                       linewidth=0.9, alpha=0.85, label="Kill", zorder=5)
        if ev["detect"] is not None:
            ax.axvline(ev["detect"], color=DETECT_COLOR,  linestyle=":",
                       linewidth=0.9, alpha=0.85, label="Detect", zorder=5)
        if ev["reroute"] is not None:
            ax.axvline(ev["reroute"], color=REROUTE_COLOR, linestyle="-.",
                       linewidth=0.9, alpha=0.85, label="Reroute", zorder=5)

        # ── Recovery gap annotation ──
        if ev["kill"] is not None and ev["reroute"] is not None:
            gap = ev["reroute"] - ev["kill"]
            mid = (ev["kill"] + ev["reroute"]) / 2
            ypos = data["Total_GBps"].max() * 0.10
            ax.annotate(
                f"{gap*1000:.0f}\u202fms",
                xy=(mid, ypos), fontsize=6, ha="center", color="#555555",
            )

    axes[0].set_ylabel("Throughput (GB/s)")
    for ax in axes:
        ax.set_ylim(0, YMAX_CAP_GBPS)
        ax.yaxis.set_major_locator(ticker.MultipleLocator(2))

    # Annotate the ORDER=5 off-scale backlog burst (38 GB/s peak after reroute)
    _, x5, _, ev5 = load_run(ORDER5_DIR)
    if ev5["reroute"] is not None:
        axes[1].annotate(
            "backlog\nflush\n(38 GB/s)",
            xy=(ev5["reroute"] + 0.03, YMAX_CAP_GBPS * 0.995),
            xytext=(ev5["reroute"] + 0.50, YMAX_CAP_GBPS * 0.62),
            fontsize=5.5, color="#555555", ha="left", va="top",
            arrowprops=dict(
                arrowstyle="-|>",
                color="#555555",
                lw=0.7,
                connectionstyle="arc3,rad=0.15",
            ),
        )

    # ── Unified legend below both panels ──
    handles, labels = [], []
    seen = set()
    for ax in axes:
        for h, l in zip(*ax.get_legend_handles_labels()):
            if l not in seen:
                handles.append(h)
                labels.append(l)
                seen.add(l)

    fig.legend(
        handles, labels,
        loc="lower center",
        ncol=5,
        bbox_to_anchor=(0.5, -0.18),
        frameon=True,
        handlelength=1.8,
    )

    return fig


# ── Main ───────────────────────────────────────────────────────────────────────
def main():
    fig = make_figure()
    os.makedirs(os.path.dirname(OUTPUT), exist_ok=True)
    fig.savefig(OUTPUT, bbox_inches="tight", dpi=300)
    png = OUTPUT.replace(".pdf", ".png")
    fig.savefig(png, bbox_inches="tight", dpi=200)
    print(f"Saved: {OUTPUT}")
    print(f"Saved: {png}")


if __name__ == "__main__":
    main()
