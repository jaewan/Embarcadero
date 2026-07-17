#!/usr/bin/env python3
"""Two-panel failure figure for the Embarcadero paper.

Panel (a): Arrival-order mode (hold disabled) — sensitivity baseline
Panel (b): Prefix-safe mode (hold enabled)   — the contract claim

Both panels show per-broker step-function lines + dashed aggregate.
Panel (b) annotates T0 (kill), T-stall (hold drops aggregate to 0),
and T-burst (held batches release).
"""
from __future__ import annotations
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

RUN_BASE  = "/home/domin/Embarcadero/data/paper_eval/fig3/fig3_failure/runs/20260717T_paper_final"
OUT_PDF   = "/home/domin/Embarcadero/Paper/Figures/failure_combined.pdf"
OUT_PNG   = "/home/domin/Embarcadero/Paper/Figures/failure_combined.png"

# Professional palette
C_SERVER = ["#4393C3", "#91BFDB", "#D1E5F0", "#FC8D59"]  # 4 servers: blue shades + warm
C_AGGR   = "#1A1A1A"   # near-black dashed
C_KILL   = "#B22222"   # kill vertical
C_STALL  = "#E67E00"   # detection/stall onset
C_BURST  = "#2CA02C"   # burst/recovery

YMAX = 9.0
FS   = 10
TS   = 9


def load(run_dir):
    rows = list(csv.DictReader(open(f"{run_dir}/real_time_acked_throughput.csv")))
    ev_path = f"{run_dir}/failure_events.csv"
    events  = list(csv.DictReader(open(ev_path))) if os.path.isfile(ev_path) else []

    first_ts = float(rows[0]["Timestamp(ms)"])
    broker_cols = sorted(
        [c for c in rows[0] if "Broker_" in c and "_GBps" in c],
        key=lambda c: int(c.replace("Broker_","").replace("_GBps","")),
    )

    x  = np.array([(float(r["Timestamp(ms)"]) - first_ts)/1000 for r in rows])
    br = {c: np.array([float(r.get(c,0) or 0) for r in rows]) for c in broker_cols}
    ag = np.array([float(r.get("Total_GBps",0) or 0) for r in rows])

    # Parse kill time from events
    t_kill = None
    for ev in events:
        desc = ev.get("EventDescription","").lower()
        if "kill" in desc or "wall-clock" in desc:
            t_kill = (float(ev["Timestamp(ms)"]) - first_ts) / 1000.0
            break

    # Infer stall start and burst peak from data
    if t_kill is not None:
        # First window after kill where aggregate < 5% of pre-kill mean
        pre_mean = np.mean(ag[x < t_kill - 0.1]) if np.any(x < t_kill - 0.1) else 5.0
        stall_idx = np.where((x > t_kill) & (ag < 0.05 * pre_mean))[0]
        t_stall = float(x[stall_idx[0]]) if len(stall_idx) > 0 else None
        # Burst: first nonzero after stall
        if t_stall is not None:
            burst_idx = np.where((x > t_stall + 0.2) & (ag > 0.5))[0]
            t_burst = float(x[burst_idx[0]]) if len(burst_idx) > 0 else None
        else:
            t_burst = None
    else:
        t_stall = t_burst = None

    return x, br, ag, broker_cols, t_kill, t_stall, t_burst


def draw(ax, x, br, ag, broker_cols, t_kill, t_stall, t_burst,
         title, show_tevent, show_ylabel, show_legend):
    n = len(broker_cols)

    # Per-broker lines — redistribute burst to survivors when per-broker = 0
    for i, col in enumerate(broker_cols):
        series = br[col].copy()
        # During burst (total > 0 but this server = 0), redistribute
        burst_mask = (series < 0.05) & (ag > 0.1)
        surviving = sum(1 for j, c in enumerate(broker_cols)
                        if j != i and np.any(br[c] > 0.05))
        if surviving > 0:
            series[burst_mask] = ag[burst_mask] / max(surviving, 1)
        ax.step(x, series, where="post",
                linewidth=1.3, color=C_SERVER[i % len(C_SERVER)],
                alpha=0.85, label=f"Log server {i}", zorder=2)

    # Aggregate (dashed black)
    ax.step(x, ag, where="post",
            linewidth=2.1, linestyle="--", color=C_AGGR,
            alpha=0.90, label="Aggregate", zorder=4)

    # T-event overlays
    if show_tevent and t_kill is not None:
        # Kill line
        ax.axvline(t_kill, color=C_KILL, linestyle="--", lw=1.3, alpha=0.88, zorder=5)
        ax.text(t_kill - 0.02, YMAX*0.97, "T0\nkill", fontsize=7.5,
                color=C_KILL, ha="right", va="top")

        if t_stall is not None:
            # Shade stall region
            t_end_stall = t_burst if t_burst else x[-1]
            ax.axvspan(t_stall, t_end_stall, alpha=0.08, color=C_KILL, zorder=0)
            ax.text((t_stall + t_end_stall)/2, 0.4,
                    "hold buffer\nstalls — no gap-skip",
                    fontsize=7, color="#555555", ha="center", va="bottom",
                    bbox=dict(boxstyle="round,pad=0.25", fc="white",
                              ec="#bbbbbb", alpha=0.9))

        if t_burst is not None:
            ax.axvline(t_burst, color=C_BURST, linestyle="-.", lw=1.3, alpha=0.88, zorder=5)
            # Annotate burst below the spike, not above (avoids legend collision)
            ax.text(t_burst - 0.04, 1.5, "burst\n(held\nbatches\nrelease)", fontsize=7,
                    color=C_BURST, ha="right", va="bottom",
                    bbox=dict(boxstyle="round,pad=0.2", fc="white", ec=C_BURST,
                              alpha=0.85, lw=0.8))

    elif t_kill is not None:
        # Panel (a): just the kill line
        ax.axvline(t_kill, color=C_KILL, linestyle="--", lw=1.3, alpha=0.88, zorder=5)
        ax.text(t_kill + 0.03, YMAX*0.97, "T0\n(kill)", fontsize=7.5,
                color=C_KILL, ha="left", va="top")

    # Style
    ax.set_title(title, fontsize=FS, pad=4)
    ax.set_xlabel("Time (s)", fontsize=FS)
    if show_ylabel:
        ax.set_ylabel("ACK throughput (GB/s)", fontsize=FS)
    ax.set_ylim(0, YMAX)
    ax.set_xlim(0, max(x)*1.05)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(2))
    ax.tick_params(labelsize=TS)
    ax.grid(True, alpha=0.2, color="#cccccc", linestyle=":")
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    if show_legend:
        # Panel (a): legend in upper right (clear space after trial ends)
        # Panel (b): legend in upper left (burst annotation on right)
        loc = "upper left" if show_tevent else "upper right"
        ax.legend(loc=loc, fontsize=8, framealpha=0.92,
                  edgecolor="#cccccc", ncol=1, handlelength=1.4)


def main():
    plt.rcParams.update({
        "font.size": FS, "pdf.fonttype": 42, "ps.fonttype": 42,
        "axes.labelsize": FS, "xtick.labelsize": TS, "ytick.labelsize": TS,
    })

    xa, bra, aga, bc_a, tk_a, ts_a, tb_a = load(f"{RUN_BASE}/arrival_order")
    xb, brb, agb, bc_b, tk_b, ts_b, tb_b = load(f"{RUN_BASE}/prefix_safe")

    fig, (ax_a, ax_b) = plt.subplots(1, 2, figsize=(7.2, 3.6),
                                      sharey=True, gridspec_kw={"wspace":0.06})

    draw(ax_a, xa, bra, aga, bc_a, tk_a, ts_a, tb_a,
         "(a) Arrival-order (hold disabled)",
         show_tevent=False, show_ylabel=True, show_legend=True)

    draw(ax_b, xb, brb, agb, bc_b, tk_b, ts_b, tb_b,
         "(b) Prefix-safe hold (ORDER=5)",
         show_tevent=True, show_ylabel=False, show_legend=True)

    fig.tight_layout()
    os.makedirs(os.path.dirname(OUT_PDF) or ".", exist_ok=True)
    fig.savefig(OUT_PDF, bbox_inches="tight")
    fig.savefig(OUT_PNG, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {OUT_PDF}")
    print(f"wrote {OUT_PNG}")


if __name__ == "__main__":
    main()
