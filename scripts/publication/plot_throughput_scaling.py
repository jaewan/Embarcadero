#!/usr/bin/env python3
"""
Generate throughput-scaling figure for the Embarcadero paper.

Two panels: RF=1 (left) and RF=2 (right).
X-axis: number of concurrent publishers (N = 1, 2, 3).
Y-axis: overlap throughput (GB/s) — total bytes ACKed across all N clients
        during their simultaneous measurement window / window duration.

Embarcadero strong total ordering (ORDER=5): solid line, primary.
Embarcadero no total ordering  (ORDER=0): dashed line, secondary.
Baselines (Corfu, LazyLog, Scalog): solid lines.

Error bars show ± std across trials; cells with a single trial have no bar.

Usage:
  python plot_throughput_scaling.py [output.pdf]
"""

import os
import sys
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

# ── Paths ─────────────────────────────────────────────────────────────────────
SCRIPT_DIR  = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT   = os.path.join(SCRIPT_DIR, "..", "..")
DEFAULT_OUT = os.path.join(REPO_ROOT, "Paper", "Figures", "throughput_scaling.pdf")
OUTPUT      = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_OUT

# ── Canonical dataset ─────────────────────────────────────────────────────────
# Each entry: (system, order, rf, n) -> [overlap_gbps per trial]
#
# Sources and provenance:
#   Emb ORDER0/5 RF1 N1-3  : 1-trial each; dir 20260402_publication_matrix_appendable_tp
#                             commit 5dac576
#   Emb ORDER0   RF2 N1    : 3-trial; dir 20260402_embarcadero_rf2_complete
#                             commit a1ff4f4d
#   Emb ORDER0   RF2 N2-3  : 3-trial; dir 20260402_publication_throughput_regression_reruns
#                             commit fd9f425
#   Emb ORDER5   RF2 N1    : 3-trial across rf2_n1_fill + rf2_complete; commit a1ff4f4d
#   Emb ORDER5   RF2 N2-3  : 1-trial each; dir 20260402_publication_remaining_1trial
#                             commit 20802755
#   Corfu        RF1 N1-2  : 1-trial each; dir 20260402_corfu_scalog_rerun_current
#   Corfu        RF1 N3    : 3-trial; dir 20260402_publication_throughput_regression_reruns
#   Corfu        RF2 N1    : 1-trial; dir 20260402_corfu_scalog_rerun_current
#   Corfu        RF2 N2    : 3-trial; dir 20260402_publication_throughput_regression_reruns
#   Corfu        RF2 N3    : 1-trial; dir 20260402_corfu_scalog_rerun_current
#   LazyLog      RF1 N1-3  : 3-trial each; dir 20260402_lazylog_rerun_3trials
#   LazyLog      RF2 N1-2  : 3-trial each; dir 20260402_lazylog_rf2_fixed
#   LazyLog      RF2 N3    : 3-trial; dir 20260402_publication_throughput_regression_reruns
#   Scalog       RF1 N1-2  : 1-trial each; dir 20260402_corfu_scalog_rerun_current
#   Scalog       RF1 N3    : 3-trial; dir 20260402_publication_throughput_regression_reruns
#   Scalog       RF2 N1-3  : 1-trial each; dir 20260402_corfu_scalog_rerun_current

CANONICAL = {
    # Embarcadero ORDER=0 (no total ordering)
    ("embarcadero", 0, 1, 1): [6.245],
    ("embarcadero", 0, 1, 2): [9.379],
    ("embarcadero", 0, 1, 3): [15.771],
    ("embarcadero", 0, 2, 1): [3.158, 3.453, 3.942],
    ("embarcadero", 0, 2, 2): [4.930, 4.047, 4.138],
    ("embarcadero", 0, 2, 3): [7.469, 7.161, 5.860],
    # Embarcadero ORDER=5 (strong total ordering)
    ("embarcadero", 5, 1, 1): [5.166],
    ("embarcadero", 5, 1, 2): [9.808],
    ("embarcadero", 5, 1, 3): [18.634],
    ("embarcadero", 5, 2, 1): [3.944, 3.841, 3.842],
    ("embarcadero", 5, 2, 2): [7.119],
    ("embarcadero", 5, 2, 3): [18.223],
    # Corfu
    ("corfu", 2, 1, 1): [5.765],
    ("corfu", 2, 1, 2): [4.162],
    ("corfu", 2, 1, 3): [6.394, 7.150, 5.272],
    ("corfu", 2, 2, 1): [5.765],
    ("corfu", 2, 2, 2): [4.900, 3.473, 3.772],
    ("corfu", 2, 2, 3): [6.430],
    # LazyLog
    ("lazylog", 2, 1, 1): [4.683, 4.684, 4.683],
    ("lazylog", 2, 1, 2): [3.419, 2.496, 5.383],
    ("lazylog", 2, 1, 3): [7.653, 8.990, 4.996],
    ("lazylog", 2, 2, 1): [3.746, 3.842, 3.148],
    ("lazylog", 2, 2, 2): [2.662, 5.161, 5.218],
    ("lazylog", 2, 2, 3): [7.810, 7.763, 8.870],
    # Scalog
    ("scalog", 1, 1, 1): [3.654],
    ("scalog", 1, 1, 2): [6.644],
    ("scalog", 1, 1, 3): [8.658, 8.843, 6.683],
    ("scalog", 1, 2, 1): [3.856],
    ("scalog", 1, 2, 2): [4.382],
    ("scalog", 1, 2, 3): [8.185],
}

# ── Visual series ─────────────────────────────────────────────────────────────
# (system, order, display_label, linestyle, zorder)
SERIES = [
    ("embarcadero", 5, r"\sys",   "-", 5),
    ("lazylog",     2, "LazyLog", "-", 3),
    ("scalog",      1, "Scalog",  "-", 3),
    ("corfu",       2, "Corfu",   "-", 3),
]

COLORS = {
    ("embarcadero", 5): "#1a6faf",
    ("lazylog",     2): "#e05c00",
    ("scalog",      1): "#1a9e4a",
    ("corfu",       2): "#c0392b",
}
MARKERS = {
    ("embarcadero", 5): "o",
    ("lazylog",     2): "^",
    ("scalog",      1): "D",
    ("corfu",       2): "v",
}
LW = {
    ("embarcadero", 5): 1.8,
}

CXL_CEIL = 21.0   # GB/s — CXL memory bandwidth ceiling on moscxl


# ── Figure ────────────────────────────────────────────────────────────────────
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
        "lines.linewidth":   1.5,
        "pdf.fonttype":      42,
    })

    N_VALS = [1, 2, 3]
    panels = [
        (1, "(a) Without replication (RF\u202f=\u202f1)"),
        (2, "(b) With replication (RF\u202f=\u202f2)"),
    ]

    fig, axes = plt.subplots(
        1, 2, figsize=(6.8, 2.6),
        sharey=True,
        constrained_layout=True,
    )

    for ax, (rf, title) in zip(axes, panels):
        ax.set_title(title, pad=4)
        ax.set_xlabel("Concurrent publishers ($N$)")
        ax.set_xticks(N_VALS)
        ax.set_xlim(0.7, 3.3)
        ax.set_ylim(0, 22)

        # CXL ceiling
        ax.axhline(CXL_CEIL, color="#aaaaaa", linewidth=0.8, linestyle=":",
                   zorder=1)

        for sys_name, order, label, ls, zo in SERIES:
            color  = COLORS[(sys_name, order)]
            marker = MARKERS[(sys_name, order)]
            lw     = LW.get((sys_name, order), 1.5)

            xs, ys, errs = [], [], []
            for n in N_VALS:
                trials = CANONICAL.get((sys_name, order, rf, n))
                if trials is None:
                    continue
                xs.append(n)
                ys.append(float(np.median(trials)))
                errs.append(float(np.std(trials, ddof=0)) if len(trials) > 1 else 0.0)

            if not xs:
                continue

            ax.errorbar(
                xs, ys,
                yerr=errs,
                fmt=f"{marker}{ls}",
                color=color,
                linewidth=lw,
                markersize=5,
                label=label,
                capsize=2.5,
                elinewidth=0.8,
                zorder=zo,
            )

    axes[0].set_ylabel("Overlap throughput (GB/s)")
    axes[0].yaxis.set_major_locator(ticker.MultipleLocator(5))

    # CXL ceiling annotation (right panel only, outside crowded area)
    axes[1].annotate(
        "CXL BW ceiling",
        xy=(0.72, CXL_CEIL + 0.15), fontsize=5.5, color="#888888", va="bottom",
    )

    # Unified legend below both panels
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
        bbox_to_anchor=(0.5, -0.20),
        frameon=True,
        handlelength=2.2,
        columnspacing=1.0,
    )

    return fig


# ── Main ──────────────────────────────────────────────────────────────────────
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
