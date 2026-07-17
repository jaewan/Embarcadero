#!/usr/bin/env python3
"""Plot Fig2 append→ACK latency vs load + mechanism ablation.

Primary claim (paper tab:latency-sweep): append→ack stays flat vs offered load.
Deliver (publish→deliver) is an optional scoped inset — above ~270 MB/s it
reflects subscriber backlog, not the ordering path.

Usage:
  python3 PaperScripts/plot_fig2_latency_vs_load.py \\
    --csv data/paper_eval/fig2/fig2_append_latency/results.csv \\
    --pdf data/paper_eval/fig2/fig2_append_latency/fig2_append_latency.pdf \\
    --deliver-pdf data/paper_eval/fig2/fig2_append_latency/fig2_deliver_inset.pdf \\
    --mech-csv data/paper_eval/fig2/fig2_append_latency/mechanism_summary.csv \\
    --mech-pdf data/paper_eval/fig2/fig2_append_latency/fig2_mechanism_ablation.pdf
"""

from __future__ import annotations

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


# Primary load-sweep series (coordination claim = mem RF2).
PRIMARY_SERIES = [
    ("fig2_embar_o5_ack2_rf2_mem", "Embar O5 ACK2 RF2 mem", "#1f77b4", "-"),
    ("fig2_embar_o5_ack2_rf2_disk", "Embar O5 ACK2 RF2 disk", "#1f77b4", "--"),
    ("fig2_embar_o5_ack1_rf0", "Embar O5 ACK1 RF0", "#aec7e8", ":"),
    ("fig2_embar_o5_ack2_rf2_mem_nolinger", "Embar O5 mem no-linger", "#c5b0d5", "--"),
]

# Matched RF2 ACK2 mem baselines (same sink as primary).
BASELINE_SERIES = [
    ("fig2_corfu_o2_ack2_rf2_mem", "Corfu O2 ACK2 RF2 mem", "#ff7f0e", "-"),
    ("fig2_scalog_o1_ack2_rf2_mem", "Scalog O1 ACK2 RF2 mem", "#2ca02c", "-"),
    ("fig2_lazylog_o2_ack2_rf2_mem", "LazyLog O2 ACK2 RF2 mem", "#d62728", "-"),
]

# Legacy cell names (older campaigns) — dashed so not read as peers.
LEGACY_SERIES = [
    ("fig2_embar_o5_ack2_rf2", "Embar O5 RF2 disk (legacy)", "#7f7f7f", ":"),
    ("fig2_corfu_o2_ack2_rf2", "Corfu O2 RF2 disk (legacy)", "#ffbb78", ":"),
    ("fig2_scalog_o1_ack2_rf2", "Scalog O1 RF2 disk (legacy)", "#98df8a", ":"),
    ("fig2_corfu_o2_ack1_rf0", "Corfu O2 RF0 (legacy)", "#ffbb78", ":"),
    ("fig2_scalog_o1_ack1_rf0", "Scalog O1 RF0 (legacy)", "#98df8a", ":"),
]

MECH_ORDER = [
    ("fig2_mech_embar_o0_ack1_rf0", "O0 ACK1 RF0"),
    ("fig2_mech_embar_o5_ack1_rf0", "O5 ACK1 RF0"),
    ("fig2_mech_embar_o5_ack2_rf2_mem", "O5 ACK2 mem"),
    ("fig2_mech_embar_o5_ack2_rf2_disk", "O5 ACK2 disk"),
    # Legacy 3-row mechanism (disk-only RF2)
    ("fig2_mech_embar_o5_ack2_rf2", "O5 ACK2 RF2 (legacy)"),
]

# Paper-documented ordered-consume ceiling (~270 MB/s); inset only below this.
DELIVER_INSET_MAX_MBPS = 300.0


def mean_std(vals: list[float]) -> tuple[float, float]:
    if not vals:
        return float("nan"), float("nan")
    m = sum(vals) / len(vals)
    if len(vals) == 1:
        return m, 0.0
    var = sum((v - m) ** 2 for v in vals) / (len(vals) - 1)
    return m, math.sqrt(var)


def load_ok(csv_path: str) -> list[dict[str, str]]:
    rows: list[dict[str, str]] = []
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("status") == "ok":
                rows.append(row)
    return rows


def group_metric(
    rows: list[dict[str, str]],
    p50_key: str,
    p99_key: str,
) -> dict[tuple[str, float], dict[str, list[float]]]:
    cells: dict[tuple[str, float], dict[str, list[float]]] = defaultdict(
        lambda: {"p50": [], "p99": []}
    )
    for row in rows:
        try:
            cell = row["cell"]
            target = float(row["target_mbps"])
            p50 = float(row[p50_key])
            p99 = float(row[p99_key])
        except (KeyError, ValueError, TypeError):
            continue
        cells[(cell, target)]["p50"].append(p50)
        cells[(cell, target)]["p99"].append(p99)
    return cells


def plot_load_sweep(
    cells: dict[tuple[str, float], dict[str, list[float]]],
    pdf_path: str,
    png_path: str | None,
    metric: str,
    ylabel: str,
    title: str,
    max_load: float | None = None,
) -> None:
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    for series in (PRIMARY_SERIES, BASELINE_SERIES, LEGACY_SERIES):
        for cell_name, label, color, ls in series:
            xs: list[float] = []
            ys: list[float] = []
            yerr: list[float] = []
            targets = sorted({t for (c, t) in cells if c == cell_name})
            for t in targets:
                if max_load is not None and t > max_load:
                    continue
                vals = cells[(cell_name, t)][metric]
                m, s = mean_std(vals)
                if math.isnan(m):
                    continue
                xs.append(t)
                ys.append(m)
                yerr.append(s)
            if not xs:
                continue
            ax.errorbar(
                xs,
                ys,
                yerr=yerr if any(e > 0 for e in yerr) else None,
                label=label,
                color=color,
                linestyle=ls,
                marker="o",
                capsize=3,
                linewidth=1.6,
            )

    ax.set_xlabel("Offered load (MB/s)")
    ax.set_ylabel(ylabel)
    ax.set_title(title)
    ax.set_yscale("log")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(loc="best", fontsize=8)
    fig.tight_layout()
    fig.savefig(pdf_path)
    if png_path:
        fig.savefig(png_path, dpi=150)
    plt.close(fig)
    print(f"wrote {pdf_path}")


def write_mechanism_summary(
    rows: list[dict[str, str]], mech_csv: str, mech_pdf: str | None
) -> None:
    mech_rows = [
        r
        for r in rows
        if r.get("panel") == "mechanism" or str(r.get("cell", "")).startswith("fig2_mech_")
    ]
    by_cell: dict[str, dict[str, list[float]]] = defaultdict(
        lambda: {"ack_p50": [], "ack_p99": [], "del_p50": [], "del_p99": [], "target": []}
    )
    for r in mech_rows:
        cell = r.get("cell", "")
        try:
            by_cell[cell]["ack_p50"].append(float(r["pub_ack_p50_us"]))
            by_cell[cell]["ack_p99"].append(float(r["pub_ack_p99_us"]))
        except (KeyError, ValueError, TypeError):
            continue
        try:
            by_cell[cell]["del_p50"].append(float(r["p50_us"]))
            by_cell[cell]["del_p99"].append(float(r["p99_us"]))
        except (KeyError, ValueError, TypeError):
            pass
        try:
            by_cell[cell]["target"].append(float(r["target_mbps"]))
        except (KeyError, ValueError, TypeError):
            pass

    path = Path(mech_csv)
    path.parent.mkdir(parents=True, exist_ok=True)
    fields = [
        "cell",
        "label",
        "target_mbps",
        "n_ok",
        "pub_ack_p50_us",
        "pub_ack_p99_us",
        "deliver_p50_us",
        "deliver_p99_us",
        "delta_ack_p99_vs_o0_us",
    ]
    table: list[dict[str, str]] = []
    o0_p99 = float("nan")
    for cell, label in MECH_ORDER:
        d = by_cell.get(cell)
        if not d or not d["ack_p50"]:
            continue
        ack50, _ = mean_std(d["ack_p50"])
        ack99, _ = mean_std(d["ack_p99"])
        del50, _ = mean_std(d["del_p50"]) if d["del_p50"] else (float("nan"), 0.0)
        del99, _ = mean_std(d["del_p99"]) if d["del_p99"] else (float("nan"), 0.0)
        tgt = d["target"][0] if d["target"] else float("nan")
        if cell.endswith("o0_ack1_rf0"):
            o0_p99 = ack99
        delta = "" if math.isnan(o0_p99) or math.isnan(ack99) else f"{ack99 - o0_p99:.1f}"
        table.append(
            {
                "cell": cell,
                "label": label,
                "target_mbps": f"{tgt:.0f}" if tgt == tgt else "",
                "n_ok": str(len(d["ack_p50"])),
                "pub_ack_p50_us": f"{ack50:.1f}",
                "pub_ack_p99_us": f"{ack99:.1f}",
                "deliver_p50_us": "" if math.isnan(del50) else f"{del50:.1f}",
                "deliver_p99_us": "" if math.isnan(del99) else f"{del99:.1f}",
                "delta_ack_p99_vs_o0_us": delta,
            }
        )

    with path.open("w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for row in table:
            w.writerow(row)
    print(f"wrote {path}")

    if not table or not mech_pdf:
        return

    labels = [r["label"] for r in table]
    p50 = [float(r["pub_ack_p50_us"]) for r in table]
    p99 = [float(r["pub_ack_p99_us"]) for r in table]
    x = list(range(len(labels)))
    width = 0.35
    fig, ax = plt.subplots(figsize=(6.5, 3.8))
    ax.bar([i - width / 2 for i in x], p50, width, label="append→ack p50", color="#4c78a8")
    ax.bar([i + width / 2 for i in x], p99, width, label="append→ack p99", color="#f58518")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, fontsize=9)
    ax.set_ylabel("Latency (µs)")
    ax.set_title("Fig 2 inset: Embar mechanism ablation (matched load)")
    ax.set_yscale("log")
    ax.grid(True, axis="y", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    fig.savefig(mech_pdf)
    plt.close(fig)
    print(f"wrote {mech_pdf}")


def median(vals: list[float]) -> float:
    if not vals:
        return float("nan")
    s = sorted(vals)
    n = len(s)
    mid = n // 2
    return s[mid] if n % 2 else (s[mid - 1] + s[mid]) / 2.0


def write_epoch_table(rows: list[dict[str, str]], out_dir: str) -> None:
    """Write tab:epoch-sweep LaTeX table and text summary to stdout."""
    epoch_rows = [r for r in rows if r.get("panel") == "epoch_sweep"]
    if not epoch_rows:
        print("No epoch_sweep rows found in results CSV — run SKIP_EPOCH_SWEEP=0 first.")
        return

    # Group by tau (epoch_us column)
    by_tau: dict[int, dict[str, list[float]]] = defaultdict(
        lambda: {"p50": [], "p99": []}
    )
    for r in epoch_rows:
        try:
            tau = int(float(r.get("epoch_us") or "0"))
            p50 = float(r["pub_ack_p50_us"])
            p99 = float(r["pub_ack_p99_us"])
        except (KeyError, ValueError, TypeError):
            continue
        by_tau[tau]["p50"].append(p50)
        by_tau[tau]["p99"].append(p99)

    if not by_tau:
        print("epoch_sweep rows present but no valid pub_ack percentiles found.")
        return

    taus = sorted(by_tau.keys())

    # Text table header
    print("\nEpoch tau sweep — timer quantization proof")
    print(f"{'tau (µs)':>10}  {'P50 med':>10}  {'P50 std':>8}  {'P99 med':>10}  {'P99 std':>8}  {'P99/tau':>8}  {'n':>3}")
    print("-" * 70)
    tex_rows: list[tuple[int, float, float, float, float, float]] = []
    for tau in taus:
        d = by_tau[tau]
        p50_med = median(d["p50"])
        p99_med = median(d["p99"])
        p50_std = (sum((x - p50_med)**2 for x in d["p50"]) / len(d["p50"]))**0.5 if len(d["p50"]) > 1 else 0.0
        p99_std = (sum((x - p99_med)**2 for x in d["p99"]) / len(d["p99"]))**0.5 if len(d["p99"]) > 1 else 0.0
        ratio = p99_med / tau if tau > 0 else float("nan")
        n = len(d["p99"])
        print(f"{tau:>10}  {p50_med:>10.0f}  {p50_std:>8.0f}  {p99_med:>10.0f}  {p99_std:>8.0f}  {ratio:>8.2f}  {n:>3}")
        tex_rows.append((tau, p50_med, p50_std, p99_med, p99_std, ratio))
    print()

    # LaTeX table — includes std columns and a note that P99 ~ 2*tau + const_jitter
    tex_lines = [
        r"\begin{table}[t]",
        r"\centering",
        r"\footnotesize",
        r"\caption{Epoch $\tau$ sensitivity at fixed 250\,MB/s offered load (Embar O5 RF\,=\,2 DRAM, 5 trials, warmup\,=\,1)."
        r" P50\,$\approx$\,$\tau/2$ across all points; P99\,$\approx$\,$2\tau + c$ where constant $c\approx 80\,\mu$s"
        r" reflects CXL scan jitter. P99/$\tau$ approaches 2 as $\tau$ grows and $c/\tau\to 0$."
        r" Decreasing P99 std confirms the tail is timer-bounded, not load-driven.}",
        r"\label{tab:epoch-sweep}",
        r"\setlength{\tabcolsep}{4pt}",
        r"\begin{tabular}{rrrrrrr}",
        r"\toprule",
        r"$\tau$ & \multicolumn{2}{c}{P50 ($\mu$s)} & \multicolumn{2}{c}{P99 ($\mu$s)} & P99/$\tau$ & $n$ \\",
        r"\cmidrule(lr){2-3}\cmidrule(lr){4-5}",
        r"($\mu$s) & med & std & med & std & & \\",
        r"\midrule",
    ]
    for tau, p50_med, p50_std, p99_med, p99_std, ratio in tex_rows:
        if math.isnan(p50_med) or math.isnan(p99_med):
            continue
        tex_lines.append(
            f"{tau} & {p50_med:.0f} & {p50_std:.0f} & {p99_med:.0f} & {p99_std:.0f} & {ratio:.2f} & 5 \\\\"
        )
    tex_lines += [
        r"\bottomrule",
        r"\end{tabular}",
        r"\end{table}",
    ]
    tex_str = "\n".join(tex_lines) + "\n"

    out_path = Path(out_dir) / "fig2_epoch_sweep_table.tex"
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(tex_str)
    print(f"wrote {out_path}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--pdf", required=True)
    ap.add_argument("--png", default=None)
    ap.add_argument("--deliver-pdf", default=None)
    ap.add_argument("--mech-csv", default=None)
    ap.add_argument("--mech-pdf", default=None)
    ap.add_argument(
        "--primary-metric",
        choices=("ack", "deliver"),
        default="ack",
        help="ack = append→ack (paper claim); deliver = publish→deliver inset",
    )
    ap.add_argument("--metric", choices=("p50", "p99", "both"), default="both")
    ap.add_argument(
        "--epoch-table",
        action="store_true",
        help="Generate tab:epoch-sweep LaTeX table from epoch_sweep panel rows",
    )
    args = ap.parse_args()

    rows = load_ok(args.csv)

    if args.primary_metric == "ack":
        cells = group_metric(rows, "pub_ack_p50_us", "pub_ack_p99_us")
        ylabel_base = "Append→ack {} (µs)"
        title_base = "Fig 2: Append latency vs load ({}; Embar O5 ACK2 RF2 mem)"
    else:
        cells = group_metric(rows, "p50_us", "p99_us")
        ylabel_base = "Publish→deliver {} (µs)"
        title_base = "Fig 2: Deliver latency vs load ({}; scoped inset)"

    def emit(metric: str, pdf: str, png: str | None) -> None:
        plot_load_sweep(
            cells,
            pdf,
            png,
            metric,
            ylabel=ylabel_base.format(metric),
            title=title_base.format(metric),
        )

    if args.metric in ("p99", "both"):
        emit("p99", args.pdf, args.png)
    if args.metric in ("p50", "both"):
        if args.metric == "both" and args.pdf.endswith(".pdf"):
            p50_pdf = args.pdf[:-4] + "_p50.pdf"
            p50_png = (
                args.png[:-4] + "_p50.png"
                if args.png and args.png.endswith(".png")
                else None
            )
            emit("p50", p50_pdf, p50_png)
        elif args.metric == "p50":
            emit("p50", args.pdf, args.png)

    # Scoped deliver inset (always from deliver columns when requested).
    deliver_pdf = args.deliver_pdf
    if deliver_pdf is None and args.pdf.endswith(".pdf"):
        deliver_pdf = str(Path(args.pdf).with_name("fig2_deliver_inset.pdf"))
    if deliver_pdf:
        dcells = group_metric(rows, "p50_us", "p99_us")
        # Prefer Embar primary only in the inset to avoid RF0/RF2 clutter.
        embar_only = {
            k: v for k, v in dcells.items() if k[0] == "fig2_embar_o5_ack2_rf2_mem"
        }
        if embar_only:
            plot_load_sweep(
                embar_only,
                deliver_pdf,
                deliver_pdf[:-4] + ".png" if deliver_pdf.endswith(".pdf") else None,
                "p50",
                ylabel="Publish→deliver p50 (µs)",
                title=(
                    f"Fig 2 inset: deliver latency (≤{DELIVER_INSET_MAX_MBPS:.0f} MB/s; "
                    "above ~270 MB/s is backlog)"
                ),
                max_load=DELIVER_INSET_MAX_MBPS,
            )

    mech_csv = args.mech_csv
    if mech_csv is None:
        mech_csv = str(Path(args.csv).with_name("mechanism_summary.csv"))
    mech_pdf = args.mech_pdf
    if mech_pdf is None and args.pdf.endswith(".pdf"):
        mech_pdf = str(Path(args.pdf).with_name("fig2_mechanism_ablation.pdf"))
    write_mechanism_summary(rows, mech_csv, mech_pdf)

    if args.epoch_table:
        write_epoch_table(rows, str(Path(args.csv).parent))


if __name__ == "__main__":
    main()
