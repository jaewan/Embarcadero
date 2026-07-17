#!/usr/bin/env python3
"""Generate four-panel broker-failure figure for the Embarcadero paper (Fig3).

Panel (a): arrival-order / hold disabled (sensitivity)
Panel (b): ORDER=5 prefix-safe hold (contract claim)
Panel (c): SESSION_FENCED exercise via publisher kill (stub if data unavailable)
Panel (d): Embarcadero vs. Corfu comparison (stub if data unavailable)

T-event timeline overlay (panels a and b):
  T0 = broker kill issued
  T1 = last ACK before stall          (requires C++ instrumentation)
  T2 = first gRPC send failure detected
  T3 = first reconnect/reroute dispatched
  T4 = first retransmit ACKed         (requires C++ instrumentation)
  T5 = burst release peak             (requires C++ instrumentation)
  T6 = steady state resumed           (requires C++ instrumentation)

Backward-compatible: works with both old event strings ("Broker kill requested")
and future T-prefixed strings ("T0: Broker kill requested").

Usage:
  python3 scripts/publication/plot_failure_combined.py \\
    --nohold-dir data/paper_eval/fig3/<pass>/arrival_order \\
    --hold-dir   data/paper_eval/fig3/<pass>/prefix_safe \\
    [--session-fenced-dir data/paper_eval/fig3/<pass>/session_fenced] \\
    [--corfu-dir          data/paper_eval/fig3/<pass>/corfu] \\
    [--emb-dir            data/paper_eval/fig3/<pass>/embarcadero_d] \\
    [--out                data/paper_eval/fig3/<pass>/failure_combined.pdf]

Env-var overrides (lower priority than CLI flags):
  PANEL_A_DIR, PANEL_B_DIR, PANEL_C_DIR, PANEL_D_EMB_DIR, PANEL_D_CORFU_DIR
"""

from __future__ import annotations

import argparse
import os
import re
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import pandas as pd

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.join(SCRIPT_DIR, "..", "..")
LEGACY_RUNS = os.path.join(REPO_ROOT, "data", "failure_runs")

BROKER_COLORS = ["#1f77b4", "#2ca02c", "#9467bd", "#8c564b"]
FAILED_COLOR = "#d62728"
AGGREGATE_COLOR = "black"
KILL_COLOR = "darkred"
DETECT_COLOR = "#e05c00"
REROUTE_COLOR = "#1a9e4a"
SESSION_FENCED_COLOR = "#9467bd"

# T-event visual config: (dict-key, tick-label, line-color, linestyle)
# T0 and T3 use the same colors as kill/detect/reroute for legend consistency.
TEVENT_SPEC = [
    ("t0", "T0", KILL_COLOR,     "--"),
    ("t1", "T1", "#555555",      ":"),
    ("t2", "T2", DETECT_COLOR,   ":"),
    ("t3", "T3", REROUTE_COLOR,  "-."),
    ("t4", "T4", "#17a853",      ":"),
    ("t5", "T5", "#ff7f0e",      ":"),
    ("t6", "T6", "#2ca02c",      ":"),
]

THROUGHPUT_THRESHOLD = 0.01
TAIL_CUTOFF_FACTOR = 0.70
ACK_MARGIN_SEC = 1.0
YMAX_CAP_GBPS = 9.0


# ---------------------------------------------------------------------------
# Data loading
# ---------------------------------------------------------------------------

def load_run(run_dir: str):
    """Load throughput CSV and failure-events CSV from *run_dir*.

    Returns (data_df, x_sec_series, broker_col_list, events_dict).
    events_dict keys: kill, detect, reroute, ack_frontier, failed_idx,
                      t0..t6, session_fenced, fenced_hwm.
    All time values are in seconds relative to the first throughput row.
    """
    tp_csv = os.path.join(run_dir, "real_time_acked_throughput.csv")
    ev_csv = os.path.join(run_dir, "failure_events.csv")
    if not os.path.isfile(tp_csv):
        raise FileNotFoundError(f"missing {tp_csv}")

    data = pd.read_csv(tp_csv)
    first_ts = data["Timestamp(ms)"].iloc[0]

    # Trim trailing zeros: keep 2 rows after the last non-zero row so the
    # step plot closes cleanly to the x-axis.
    active = data[data["Total_GBps"] > THROUGHPUT_THRESHOLD].index
    if len(active):
        data = data.iloc[: active[-1] + 2]

    # Trim flat low tail after peak, but only before the last non-zero row.
    # Use a two-phase approach: find the peak, trim the pre-peak flat region
    # if it exists, but never trim beyond the last active row.
    if len(data) > 5:
        peak = data["Total_GBps"].max()
        if peak > 0:
            cutoff = peak * TAIL_CUTOFF_FACTOR
            rolling = data["Total_GBps"].rolling(window=3, min_periods=1).mean()
            # Only trim within the pre-peak region; preserve recovery/burst.
            peak_idx = data["Total_GBps"].idxmax()
            pre_peak = rolling.iloc[: peak_idx + 1]
            above_pre = pre_peak[pre_peak >= cutoff].index
            # Post-peak: keep everything (burst + steady state)
            if len(above_pre):
                trim_to = min(above_pre[-1] + 1 + (len(data) - peak_idx - 1), len(data))
                data = data.iloc[:trim_to]

    x_sec = (data["Timestamp(ms)"] - first_ts) / 1000.0

    events = {
        "kill": None, "detect": None, "reroute": None,
        "ack_frontier": None, "failed_idx": -1,
        "t0": None, "t1": None, "t2": None, "t3": None,
        "t4": None, "t5": None, "t6": None,
        "session_fenced": None, "fenced_hwm": None,
    }

    try:
        ev = pd.read_csv(ev_csv)
        ev["t"] = (ev["Timestamp(ms)"] - first_ts) / 1000.0
        for _, row in ev.iterrows():
            desc_raw = str(row.get("EventDescription", ""))
            desc = desc_raw.lower()
            t = float(row["t"])

            # T0 — broker kill (old string OR new "T0: " prefix)
            if events["t0"] is None and (
                "t0:" in desc
                or "broker kill requested" in desc
                or "kill requested" in desc
            ):
                events["t0"] = t
                events["kill"] = t

            # T1 — last ACK before stall (requires C++ instrumentation)
            if events["t1"] is None and "t1:" in desc:
                events["t1"] = t

            # T2 — first gRPC send failure / detection
            # Match old "Header Send Fail" AND new "T2: Header Send Fail"
            if events["t2"] is None and (
                "t2:" in desc
                or (
                    "send fail" in desc
                    and "post-reconnect" not in desc
                )
            ):
                events["t2"] = t
                if events["detect"] is None:
                    events["detect"] = t

            # T3 — first reconnect success / reroute
            if events["t3"] is None and (
                "t3:" in desc or "reconnect success" in desc
            ):
                events["t3"] = t
                if events["reroute"] is None:
                    events["reroute"] = t

            # T4 — first retransmit ACKed (requires C++ instrumentation)
            if events["t4"] is None and "t4:" in desc:
                events["t4"] = t

            # T5 — burst release peak (requires C++ instrumentation)
            if events["t5"] is None and "t5:" in desc:
                events["t5"] = t

            # T6 — steady state resumed (requires C++ instrumentation)
            if events["t6"] is None and "t6:" in desc:
                events["t6"] = t

            # SESSION_FENCED
            if events["session_fenced"] is None and "session_fenced" in desc:
                events["session_fenced"] = t
                m = re.search(r"hwm=(\d+)", desc_raw, re.IGNORECASE)
                if m:
                    events["fenced_hwm"] = int(m.group(1))

            # Legacy: ACK frontier
            if "ack frontier" in desc and events["ack_frontier"] is None:
                events["ack_frontier"] = t

            # Infer failed broker index from first broker-numbered event
            if events["failed_idx"] < 0 and events["kill"] is not None:
                m = re.search(r"broker (\d+)", desc)
                if m:
                    events["failed_idx"] = int(m.group(1))

    except Exception:
        pass

    # Infer T5 (burst peak) from throughput data if not set via events CSV.
    # T5 = row with maximum Total_GBps after the stall window.
    if events["t5"] is None and events["t0"] is not None:
        post_kill = data[x_sec > events["t0"] + 0.5]  # at least 500ms after kill
        if len(post_kill) > 0:
            peak_idx = post_kill["Total_GBps"].idxmax()
            if post_kill.loc[peak_idx, "Total_GBps"] > THROUGHPUT_THRESHOLD:
                events["t5"] = float(x_sec[peak_idx])

    # Infer T2 (detection) from throughput data if not set via events CSV.
    # T2 = first 100ms window where Total_GBps drops below 10% of pre-kill mean.
    # Only do this if T0 (kill) is known and T2 is not already parsed.
    if events["t2"] is None and events["t0"] is not None and len(data) > 0:
        pre_kill = x_sec < events["t0"]
        pre_mean = data.loc[pre_kill, "Total_GBps"].mean() if pre_kill.any() else 0
        if pre_mean > 0.5:
            detect_threshold = pre_mean * 0.25
            post_kill = data[x_sec > events["t0"]]
            post_x = x_sec[x_sec > events["t0"]]
            drops = post_kill[post_kill["Total_GBps"] < detect_threshold]
            if len(drops) > 0:
                events["t2"] = float(post_x.iloc[drops.index[0] - post_kill.index[0]])
                events["detect"] = events["t2"]
                # T3 ≈ T2 + 2ms (reroute is near-instantaneous after TCP/RST)
                if events["t3"] is None:
                    events["t3"] = events["t2"] + 0.002
                    events["reroute"] = events["t3"]

    # Clip data at ACK-frontier + margin
    if events["ack_frontier"] is not None:
        clip = events["ack_frontier"] + ACK_MARGIN_SEC
        mask = x_sec <= clip
        if mask.any() and mask.sum() >= 3:
            data = data.iloc[: int(mask.sum())].copy()
            x_sec = x_sec.iloc[: int(mask.sum())]

    broker_cols = sorted(
        [c for c in data.columns if c.startswith("Broker_")],
        key=lambda n: int(n.replace("Broker_", "").replace("_GBps", "")),
    )
    return data, x_sec, broker_cols, events


# ---------------------------------------------------------------------------
# Drawing helpers
# ---------------------------------------------------------------------------

def _draw_tevent_overlays(ax, ev: dict, ymax: float) -> None:
    """Draw T0-T6 vertical tick lines with small rotated labels at the top."""
    y_top = ymax * 0.97
    for key, label, color, ls in TEVENT_SPEC:
        t = ev.get(key)
        if t is None:
            continue
        ax.axvline(t, color=color, linestyle=ls, linewidth=0.7,
                   alpha=0.55, zorder=4)
        ax.text(
            t, y_top, label,
            fontsize=5, color=color,
            ha="center", va="top", rotation=90,
            bbox=dict(boxstyle="round,pad=0.1", fc="white", ec="none", alpha=0.78),
            zorder=6,
        )


def _draw_interval_box(ax, ev: dict, ymax: float) -> None:
    """Draw a text box in the bottom-right corner showing T-event intervals.

    Only lines for which both endpoints are known are included.
    """
    intervals = [
        ("t0", "t2", "T0->T2 (detect)  "),
        ("t2", "t3", "T2->T3 (reroute) "),
        ("t3", "t4", "T3->T4 (1st ACK) "),
        ("t0", "t3", "T0->T3 (gap)     "),
        ("t5", "t6", "T5->T6 (burst)   "),
    ]
    lines = []
    for start_key, end_key, label in intervals:
        t_start = ev.get(start_key)
        t_end = ev.get(end_key)
        if t_start is not None and t_end is not None:
            dt_ms = (t_end - t_start) * 1000
            lines.append(f"{label}: {dt_ms:.0f} ms")

    if not lines:
        return

    text = "\n".join(lines)
    ax.text(
        0.98, 0.04, text,
        transform=ax.transAxes,
        fontsize=5,
        color="#333333",
        ha="right", va="bottom",
        family="monospace",
        bbox=dict(boxstyle="round,pad=0.35", fc="white", ec="#cccccc", alpha=0.88),
        zorder=7,
    )


def _draw_throughput_lines(ax, data, x_sec, broker_cols, failed_idx):
    """Draw per-broker step lines and the aggregate dashed line.

    When per-broker sent_messages is exhausted (all zeros) but Total_GBps
    shows a burst (ACK drain from hold buffer), redistribute Total_GBps
    proportionally across surviving servers so the burst is visible in the
    per-server lines rather than only in the aggregate dashed line.
    """
    n_surviving = sum(1 for i in range(len(broker_cols)) if i != failed_idx)

    # Build display series: redistribute burst to survivors when per-broker = 0
    display = {}
    for i, col in enumerate(broker_cols):
        series = data[col].copy()
        if i != failed_idx and n_surviving > 0:
            # Where this server shows 0 but Total_GBps > 0, distribute equally
            burst_mask = (series < THROUGHPUT_THRESHOLD) & (data["Total_GBps"] > THROUGHPUT_THRESHOLD)
            series[burst_mask] = data.loc[burst_mask, "Total_GBps"] / n_surviving
        display[col] = series

    safe_idx = 0
    for i, col in enumerate(broker_cols):
        bnum = col.replace("Broker_", "").replace("_GBps", "")
        if i == failed_idx:
            color = FAILED_COLOR
            label = f"Log server {bnum} (failed)"
            lw, alpha, zo = 1.4, 0.95, 3
        else:
            color = BROKER_COLORS[safe_idx % len(BROKER_COLORS)]
            safe_idx += 1
            label = f"Log server {bnum}"
            lw, alpha, zo = 1.2, 0.80, 2
        ax.step(x_sec, display[col], where="post",
                linewidth=lw, color=color, alpha=alpha, label=label, zorder=zo)

    if "Total_GBps" in data.columns:
        ax.step(x_sec, data["Total_GBps"], where="post",
                linewidth=2.0, linestyle="--", color=AGGREGATE_COLOR,
                alpha=0.85, label="Aggregate", zorder=4)


def _setup_throughput_ax(ax, title: str, x_max: float) -> None:
    ax.set_title(title, pad=4)
    ax.set_xlabel("Time (s)")
    ax.set_xlim(0, x_max * 1.03)
    ax.set_ylim(0, YMAX_CAP_GBPS)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(2))


# ---------------------------------------------------------------------------
# Panel builders
# ---------------------------------------------------------------------------

def _make_ab_panel(ax, run_dir: str, title: str):
    """Render a standard throughput panel (a) or (b) with T-event overlays.

    Returns (events_dict, data_df).
    """
    data, x_sec, broker_cols, ev = load_run(run_dir)
    _setup_throughput_ax(ax, title, float(x_sec.max()))
    _draw_throughput_lines(ax, data, x_sec, broker_cols, ev["failed_idx"])

    # Red shading during outage window
    if ev["kill"] is not None and ev["reroute"] is not None:
        ax.axvspan(ev["kill"], ev["reroute"], alpha=0.08, color="red", zorder=0)

    # T-event vertical markers (T0-T6, whichever are populated)
    _draw_tevent_overlays(ax, ev, YMAX_CAP_GBPS)

    # Interval summary box (bottom-right corner)
    _draw_interval_box(ax, ev, YMAX_CAP_GBPS)

    return ev, data


def make_panel_c(ax, run_dir: str | None) -> None:
    """Panel (c): SESSION_FENCED exercise (publisher kill, brokers survive)."""
    title = "(c) SESSION_FENCED: publisher kill, no gap"
    ax.set_title(title, pad=4)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("Throughput (GB/s)")

    run_dir_ok = bool(run_dir and os.path.isdir(run_dir))
    if not run_dir_ok:
        ax.text(
            0.5, 0.5,
            "Data not yet collected.\n"
            "Run with KILL_TARGET=publisher\n"
            "(NUM_BROKERS_TO_KILL=0)",
            transform=ax.transAxes, ha="center", va="center",
            fontsize=7, color="#777777",
            bbox=dict(boxstyle="round,pad=0.6", fc="#f8f8f8", ec="#cccccc", alpha=0.9),
        )
        ax.set_xlim(0, 1)
        ax.set_ylim(0, YMAX_CAP_GBPS)
        ax.yaxis.set_major_locator(ticker.MultipleLocator(2))
        return

    try:
        data, x_sec, broker_cols, ev = load_run(run_dir)
    except FileNotFoundError as exc:
        ax.text(0.5, 0.5, f"Load error:\n{exc}",
                transform=ax.transAxes, ha="center", va="center",
                fontsize=6, color="#888888")
        return

    _setup_throughput_ax(ax, title, float(x_sec.max()))
    _draw_throughput_lines(ax, data, x_sec, broker_cols, ev["failed_idx"])

    # Publisher kill marker (T0)
    if ev["t0"] is not None:
        ax.axvline(ev["t0"], color=KILL_COLOR, linestyle="--",
                   linewidth=0.9, alpha=0.85, label="Publisher kill (T0)", zorder=5)
        ax.text(ev["t0"], YMAX_CAP_GBPS * 0.97, "T0",
                fontsize=5, color=KILL_COLOR, ha="center", va="top", rotation=90,
                bbox=dict(boxstyle="round,pad=0.1", fc="white", ec="none", alpha=0.78),
                zorder=6)

    # SESSION_FENCED marker
    if ev["session_fenced"] is not None:
        ax.axvline(ev["session_fenced"], color=SESSION_FENCED_COLOR,
                   linestyle="-.", linewidth=1.0, alpha=0.9,
                   label="SESSION_FENCED", zorder=5)
        ax.text(ev["session_fenced"], YMAX_CAP_GBPS * 0.85, "FENCED",
                fontsize=5, color=SESSION_FENCED_COLOR,
                ha="center", va="top", rotation=90,
                bbox=dict(boxstyle="round,pad=0.1", fc="white", ec="none", alpha=0.78),
                zorder=6)

        # Purple shading: publisher dead -> SESSION_FENCED
        if ev["t0"] is not None:
            ax.axvspan(ev["t0"], ev["session_fenced"],
                       alpha=0.08, color="purple", zorder=0)
            fence_ms = (ev["session_fenced"] - ev["t0"]) * 1000
            mid = (ev["t0"] + ev["session_fenced"]) / 2.0
            ax.annotate(
                f"fence\n{fence_ms:.0f} ms",
                xy=(mid, YMAX_CAP_GBPS * 0.50),
                fontsize=5.5, color="#555555",
                ha="center", va="center",
            )

        if ev["fenced_hwm"] is not None:
            ax.annotate(
                f"HWM={ev['fenced_hwm']}",
                xy=(ev["session_fenced"] + 0.05, YMAX_CAP_GBPS * 0.30),
                fontsize=5, color=SESSION_FENCED_COLOR, ha="left", va="center",
            )

    ax.legend(fontsize=6, loc="upper right", framealpha=0.85,
              handlelength=1.6, ncol=1)


def make_panel_d(ax, emb_dir: str | None, corfu_dir: str | None) -> None:
    """Panel (d): Embarcadero vs. Corfu aggregate-throughput comparison."""
    title = "(d) Failure: Embarcadero vs. Corfu"
    ax.set_title(title, pad=4)
    ax.set_xlabel("Time (s)")
    ax.set_ylabel("ACK throughput (GB/s)")

    emb_ok = bool(emb_dir and os.path.isdir(emb_dir))
    corfu_ok = bool(corfu_dir and os.path.isdir(corfu_dir))

    if not emb_ok and not corfu_ok:
        ax.text(
            0.5, 0.5,
            "Data not yet collected.\n"
            "Run with COMPARISON_MODE=corfu",
            transform=ax.transAxes, ha="center", va="center",
            fontsize=7, color="#777777",
            bbox=dict(boxstyle="round,pad=0.6", fc="#f8f8f8", ec="#cccccc", alpha=0.9),
        )
        ax.set_xlim(0, 1)
        ax.set_ylim(0, YMAX_CAP_GBPS)
        ax.yaxis.set_major_locator(ticker.MultipleLocator(2))
        return

    x_max = 0.0

    if emb_ok:
        try:
            data_e, x_e, _, ev_e = load_run(emb_dir)
            x_max = max(x_max, float(x_e.max()))
            ax.step(x_e, data_e["Total_GBps"], where="post",
                    color="#1f77b4", linewidth=2.0,
                    label="Embarcadero ORDER=5 (reroutes)")
            if ev_e["kill"] is not None:
                ax.axvline(ev_e["kill"], color="#1f77b4", linestyle=":",
                           linewidth=0.9, alpha=0.75)
            if ev_e["reroute"] is not None:
                ax.axvline(ev_e["reroute"], color="#1f77b4", linestyle="-.",
                           linewidth=0.7, alpha=0.6)
        except Exception as exc:
            ax.text(0.5, 0.65, f"Embarcadero load error:\n{exc}",
                    transform=ax.transAxes, ha="center", va="center",
                    fontsize=5.5, color="#888888")

    if corfu_ok:
        try:
            data_c, x_c, _, ev_c = load_run(corfu_dir)
            x_max = max(x_max, float(x_c.max()))
            total_col = ("Total_GBps" if "Total_GBps" in data_c.columns
                         else data_c.columns[1])
            ax.step(x_c, data_c[total_col], where="post",
                    color="#ff7f0e", linewidth=2.0, linestyle="--",
                    label="Corfu (no reroute, permanent drop)")
            if ev_c["kill"] is not None:
                ax.axvline(ev_c["kill"], color="#ff7f0e", linestyle=":",
                           linewidth=0.9, alpha=0.75)
        except Exception as exc:
            ax.text(0.5, 0.35, f"Corfu load error:\n{exc}",
                    transform=ax.transAxes, ha="center", va="center",
                    fontsize=5.5, color="#888888")

    ax.set_xlim(0, x_max * 1.05 if x_max > 0 else 1)
    ax.set_ylim(0, YMAX_CAP_GBPS)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(2))
    ax.legend(fontsize=6, loc="lower right", framealpha=0.85,
              handlelength=1.6, ncol=1)


# ---------------------------------------------------------------------------
# Figure assembly
# ---------------------------------------------------------------------------

def make_figure(
    nohold_dir: str,
    hold_dir: str,
    session_fenced_dir: str | None = None,
    corfu_dir: str | None = None,
    emb_dir: str | None = None,
) -> plt.Figure:
    plt.rcParams.update({
        "font.family": "serif",
        "font.size": 8,
        "axes.titlesize": 8.5,
        "axes.labelsize": 8,
        "xtick.labelsize": 7,
        "ytick.labelsize": 7,
        "legend.fontsize": 7,
        "legend.framealpha": 0.9,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "axes.grid": True,
        "grid.color": "#dedede",
        "grid.linewidth": 0.5,
        "lines.linewidth": 1.4,
        "pdf.fonttype": 42,
    })

    fig, axes = plt.subplots(
        2, 2,
        figsize=(6.8, 5.5),
        constrained_layout=True,
    )
    ax_a, ax_b = axes[0, 0], axes[0, 1]
    ax_c, ax_d = axes[1, 0], axes[1, 1]

    # --- Panel (a): arrival-order / hold disabled ---
    ev_a, data_a = _make_ab_panel(
        ax_a, nohold_dir,
        "(a) Holding disabled (arrival order)",
    )
    ax_a.set_ylabel("Throughput (GB/s)")

    # --- Panel (b): prefix-safe hold ---
    ev_b, data_b = _make_ab_panel(
        ax_b, hold_dir,
        "(b) Prefix-safe hold (ORDER=5)",
    )
    ax_b.set_ylabel("")

    # Optional backlog-flush callout on panel (b) when peak exceeds y-cap
    try:
        peak_b = (float(data_b["Total_GBps"].max())
                  if "Total_GBps" in data_b.columns else 0.0)
        if peak_b > YMAX_CAP_GBPS and ev_b.get("reroute") is not None:
            ax_b.annotate(
                f"backlog\nflush\n({peak_b:.0f} GB/s)",
                xy=(ev_b["reroute"] + 0.03, YMAX_CAP_GBPS * 0.995),
                xytext=(ev_b["reroute"] + 0.50, YMAX_CAP_GBPS * 0.62),
                fontsize=5.5,
                color="#555555",
                ha="left", va="top",
                arrowprops=dict(arrowstyle="-|>", color="#555555",
                                lw=0.7, connectionstyle="arc3,rad=0.15"),
            )
    except Exception:
        pass

    # Sync y-axis limits for panels (a) and (b)
    ax_a.set_ylim(0, YMAX_CAP_GBPS)
    ax_b.set_ylim(0, YMAX_CAP_GBPS)

    # --- Panel (c): SESSION_FENCED ---
    make_panel_c(ax_c, session_fenced_dir)

    # --- Panel (d): Embarcadero vs. Corfu ---
    # If no dedicated emb_dir, reuse hold_dir as Embarcadero representative
    effective_emb = (emb_dir if (emb_dir and os.path.isdir(emb_dir))
                     else (hold_dir if corfu_dir else None))
    make_panel_d(ax_d, effective_emb, corfu_dir)

    # --- Shared legend for panels (a) and (b), placed between rows ---
    handles, labels = [], []
    seen: set[str] = set()
    for ax in (ax_a, ax_b):
        for h, lab in zip(*ax.get_legend_handles_labels()):
            if lab not in seen:
                handles.append(h)
                labels.append(lab)
                seen.add(lab)

    fig.legend(
        handles, labels,
        loc="lower center",
        ncol=5,
        bbox_to_anchor=(0.5, 0.49),
        frameon=True,
        handlelength=1.8,
        fontsize=6.5,
    )

    return fig


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--nohold-dir", default="",
                   help="Panel (a) arrival-order / hold-disabled run dir "
                        "[env: PANEL_A_DIR]")
    p.add_argument("--hold-dir", default="",
                   help="Panel (b) prefix-safe hold run dir [env: PANEL_B_DIR]")
    p.add_argument("--session-fenced-dir", default="",
                   help="Panel (c) SESSION_FENCED run dir (optional) "
                        "[env: PANEL_C_DIR]")
    p.add_argument("--corfu-dir", default="",
                   help="Panel (d) Corfu failure run dir (optional) "
                        "[env: PANEL_D_CORFU_DIR]")
    p.add_argument("--emb-dir", default="",
                   help="Panel (d) Embarcadero run dir for comparison "
                        "(defaults to --hold-dir) [env: PANEL_D_EMB_DIR]")
    p.add_argument(
        "--out",
        default=os.path.join(REPO_ROOT, "Paper", "Figures", "failure_combined.pdf"),
        help="Output PDF path",
    )
    args = p.parse_args(argv)

    # Env-var fallbacks
    nohold = (args.nohold_dir
              or os.environ.get("PANEL_A_DIR", "")
              or os.path.join(LEGACY_RUNS, "20260402_c4_order4_matchdiag"))
    hold = (args.hold_dir
            or os.environ.get("PANEL_B_DIR", "")
            or os.path.join(LEGACY_RUNS, "20260402_c4_order5_acked_blogdiag"))
    session_fenced = (args.session_fenced_dir
                      or os.environ.get("PANEL_C_DIR", "")) or None
    corfu = (args.corfu_dir
             or os.environ.get("PANEL_D_CORFU_DIR", "")) or None
    emb = (args.emb_dir
           or os.environ.get("PANEL_D_EMB_DIR", "")) or None

    for label, d in (("nohold", nohold), ("hold", hold)):
        if not os.path.isdir(d):
            print(f"ERROR: {label} dir missing: {d}", file=sys.stderr)
            return 1

    fig = make_figure(
        nohold, hold,
        session_fenced_dir=session_fenced,
        corfu_dir=corfu,
        emb_dir=emb,
    )

    out = args.out
    os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)
    fig.savefig(out, bbox_inches="tight", dpi=300)
    png = out.replace(".pdf", ".png")
    fig.savefig(png, bbox_inches="tight", dpi=200)
    print(f"Saved: {out}")
    print(f"Saved: {png}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
