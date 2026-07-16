#!/usr/bin/env python3
"""Plot Fig2 latency vs load + Embar mechanism ablation table.

Usage:
  python3 PaperScripts/plot_fig2_latency_vs_load.py \\
    --csv data/paper_eval/fig2/fig2_latency_vs_load/results.csv \\
    --pdf data/paper_eval/fig2/fig2_latency_vs_load/fig2_latency_vs_load.pdf \\
    --mech-csv data/paper_eval/fig2/fig2_latency_vs_load/mechanism_summary.csv \\
    --mech-pdf data/paper_eval/fig2/fig2_latency_vs_load/fig2_mechanism_ablation.pdf
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


# Primary load-sweep series (shared-log contract).
PRIMARY_SERIES = [
    ("fig2_embar_o5_ack2_rf2", "Embar O5 ACK2 RF2", "#1f77b4", "-"),
    ("fig2_embar_o5_ack1_rf0", "Embar O5 ACK1 RF0", "#1f77b4", "--"),
    ("fig2_embar_o5_ack2_rf2_nolinger", "Embar O5 RF2 no-linger", "#aec7e8", "--"),
]

BASELINE_SERIES = [
    ("fig2_corfu_o2_ack1_rf0", "Corfu O2", "#ff7f0e", "-"),
    ("fig2_scalog_o1_ack1_rf0", "Scalog O1", "#2ca02c", "-"),
    ("fig2_lazylog_o2_ack1_rf0", "LazyLog O2", "#d62728", "-"),
]

# Legacy RF0 cell names (older campaigns).
LEGACY_SERIES = [
    ("fig2_embar_o5_linger_rf0", "Embar O5 linger (legacy RF0)", "#9467bd", ":"),
]

MECH_ORDER = [
    ("fig2_mech_embar_o0_ack1_rf0", "O0 ACK1 RF0"),
    ("fig2_mech_embar_o5_ack1_rf0", "O5 ACK1 RF0"),
    ("fig2_mech_embar_o5_ack2_rf2", "O5 ACK2 RF2"),
]


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


def group_deliver(
    rows: list[dict[str, str]],
) -> dict[tuple[str, float], dict[str, list[float]]]:
    cells: dict[tuple[str, float], dict[str, list[float]]] = defaultdict(
        lambda: {"p50": [], "p99": []}
    )
    for row in rows:
        try:
            cell = row["cell"]
            target = float(row["target_mbps"])
            p50 = float(row["p50_us"])
            p99 = float(row["p99_us"])
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
) -> None:
    fig, ax = plt.subplots(figsize=(7.2, 4.4))
    for series in (PRIMARY_SERIES, BASELINE_SERIES, LEGACY_SERIES):
        for cell_name, label, color, ls in series:
            xs: list[float] = []
            ys: list[float] = []
            yerr: list[float] = []
            targets = sorted({t for (c, t) in cells if c == cell_name})
            for t in targets:
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
    ax.set_ylabel(f"Publish→deliver {metric} (µs)")
    ax.set_title(f"Fig 2: Latency vs load ({metric}; primary Embar O5 ACK2 RF2)")
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
    # Prefer panel=mechanism; fall back to cell name prefix.
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


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--pdf", required=True)
    ap.add_argument("--png", default=None)
    ap.add_argument("--mech-csv", default=None)
    ap.add_argument("--mech-pdf", default=None)
    ap.add_argument("--metric", choices=("p50", "p99", "both"), default="both")
    args = ap.parse_args()

    rows = load_ok(args.csv)
    cells = group_deliver(rows)

    if args.metric in ("p99", "both"):
        plot_load_sweep(cells, args.pdf, args.png, "p99")
    if args.metric in ("p50", "both"):
        if args.metric == "both" and args.pdf.endswith(".pdf"):
            p50_pdf = args.pdf[:-4] + "_p50.pdf"
            p50_png = (
                args.png[:-4] + "_p50.png"
                if args.png and args.png.endswith(".png")
                else None
            )
            plot_load_sweep(cells, p50_pdf, p50_png, "p50")
        elif args.metric == "p50":
            plot_load_sweep(cells, args.pdf, args.png, "p50")

    mech_csv = args.mech_csv
    if mech_csv is None:
        mech_csv = str(Path(args.csv).with_name("mechanism_summary.csv"))
    mech_pdf = args.mech_pdf
    if mech_pdf is None and args.pdf.endswith(".pdf"):
        mech_pdf = args.pdf[:-4].replace("fig2_latency_vs_load", "fig2_mechanism_ablation")
        if mech_pdf == args.pdf[:-4]:
            mech_pdf = args.pdf[:-4] + "_mechanism.pdf"
        else:
            mech_pdf = mech_pdf + ".pdf"
    write_mechanism_summary(rows, mech_csv, mech_pdf)


if __name__ == "__main__":
    main()
