#!/usr/bin/env python3
"""Paper-ready append-latency and epoch-sensitivity figure.

Panel (a) shows Embarcadero O5 ACK2 RF2 DRAM across offered load. Panel (b)
shows the matched epoch sweep at 250 MB/s. Every accepted trial is shown behind
the median lines; no performance outliers are removed.
"""
from __future__ import annotations
import argparse, csv, hashlib, json, os, shutil
from collections import defaultdict
from pathlib import Path
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
REPO_ROOT = Path(__file__).resolve().parents[1]


def sha256(path: str | Path) -> str:
    digest = hashlib.sha256()
    with Path(path).open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def portable(path: str | Path) -> str:
    resolved = Path(path).resolve()
    try:
        return str(resolved.relative_to(REPO_ROOT))
    except ValueError:
        return str(resolved)


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


def load_epochs(csv_path: str):
    p50s: dict[int, list[float]] = defaultdict(list)
    p99s: dict[int, list[float]] = defaultdict(list)
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            if row.get("status") != "ok":
                continue
            if not row.get("cell", "").startswith("fig2_epoch_tau"):
                continue
            try:
                epoch = int(float(row["epoch_us"]))
                p50 = float(row["pub_ack_p50_us"])
                p99 = float(row["pub_ack_p99_us"])
            except (KeyError, ValueError, TypeError):
                continue
            p50s[epoch].append(p50)
            p99s[epoch].append(p99)
    return p50s, p99s


def median(vals):
    if not vals: return float("nan")
    s = sorted(vals)
    n = len(s)
    return (s[n//2-1] + s[n//2]) / 2 if n % 2 == 0 else s[n//2]


def plot(
    csv_path: str,
    epoch_csv_path: str,
    pdf_path: str,
    paper_pdf_path: str | None,
    png_path: str | None,
    manifest_path: str | None,
    require_trials: int,
    expected_loads: list[int],
    expected_epochs: list[int],
):
    p50s, p99s = load(csv_path)
    bad = {load: len(values) for load, values in p50s.items()
           if len(values) != require_trials}
    missing = sorted(set(expected_loads) - set(p50s))
    extras = sorted(set(p50s) - set(expected_loads))
    if bad or missing or extras:
        raise SystemExit(
            f"primary plot requires exactly {require_trials} successful trials "
            f"at loads {expected_loads}; bad_counts={bad}, "
            f"missing={missing}, extras={extras}"
        )
    epoch_p50s, epoch_p99s = load_epochs(epoch_csv_path)
    epoch_bad = {
        epoch: len(values) for epoch, values in epoch_p50s.items()
        if len(values) != require_trials
    }
    epoch_missing = sorted(set(expected_epochs) - set(epoch_p50s))
    epoch_extras = sorted(set(epoch_p50s) - set(expected_epochs))
    if epoch_bad or epoch_missing or epoch_extras:
        raise SystemExit(
            f"epoch plot requires exactly {require_trials} successful trials "
            f"at epochs {expected_epochs}; bad_counts={epoch_bad}, "
            f"missing={epoch_missing}, extras={epoch_extras}"
        )

    loads = expected_loads
    x   = np.array(loads, dtype=float)
    y50 = np.array([median(p50s[t]) / 1000.0 for t in loads])  # → ms
    y99 = np.array([median(p99s[t]) / 1000.0 for t in loads])

    plt.rcParams.update({
        "font.size": FONT_SIZE, "pdf.fonttype": 42, "ps.fonttype": 42,
        "axes.labelsize": FONT_SIZE, "xtick.labelsize": TICK_SIZE,
        "ytick.labelsize": TICK_SIZE, "legend.fontsize": 9,
    })

    fig, axes = plt.subplots(1, 2, figsize=(7.1, 2.55))

    def raw_points(ax, xs, groups, scale=1000.0, color=C_P50, marker="o"):
        for x_value, values in zip(xs, groups):
            offsets = [-0.018, 0.0, 0.018] if len(values) == 3 else [0.0]
            span = max(xs) - min(xs)
            ax.scatter(
                [x_value + offset * span for offset in offsets[:len(values)]],
                [value / scale for value in values],
                color=color, marker=marker, s=10, alpha=0.25,
                linewidths=0, zorder=2,
            )

    ax = axes[0]
    raw_points(ax, loads, [p50s[t] for t in loads], color=C_P50, marker="o")
    raw_points(ax, loads, [p99s[t] for t in loads], color=C_P99, marker="s")
    ax.plot(x, y50, color=C_P50, marker="o", linewidth=1.8,
            markersize=5, label="P50", zorder=3)
    ax.plot(x, y99, color=C_P99, marker="s", linewidth=1.5,
            markersize=4.5, linestyle="--", label="P99", zorder=3)
    ax.axhline(1.0, color="#999999", linestyle=":", linewidth=0.9, alpha=0.7)
    ax.text(100, 1.04, "1 ms", fontsize=7.5, color="#888888", va="bottom")
    ax.set_title("(a) Offered-load sweep")
    ax.set_xlabel("Offered load (MB/s)")
    ax.set_ylabel("Append→ACK latency (ms)")
    ax.set_xlim(0, 2200)
    ax.set_ylim(0, max(y99) * 1.18)
    ax.yaxis.set_major_locator(ticker.MultipleLocator(0.5))
    ax.legend(loc="upper left", framealpha=0.92, edgecolor="#cccccc",
              handlelength=1.5)

    ax = axes[1]
    epochs = expected_epochs
    e50 = np.array([median(epoch_p50s[t]) / 1000.0 for t in epochs])
    e99 = np.array([median(epoch_p99s[t]) / 1000.0 for t in epochs])
    raw_points(ax, epochs, [epoch_p50s[t] for t in epochs], color=C_P50, marker="o")
    raw_points(ax, epochs, [epoch_p99s[t] for t in epochs], color=C_P99, marker="s")
    ax.plot(epochs, e50, color=C_P50, marker="o", linewidth=1.8,
            markersize=5, label="P50", zorder=3)
    ax.plot(epochs, e99, color=C_P99, marker="s", linewidth=1.5,
            markersize=4.5, linestyle="--", label="P99", zorder=3)
    ax.plot(epochs, np.array(epochs) / 2000.0, color="#777777",
            linestyle=":", linewidth=1.0, label=r"$\tau/2$")
    ax.set_title("(b) Epoch sweep at 250 MB/s")
    ax.set_xlabel(r"Epoch $\tau$ ($\mu$s)")
    ax.set_ylim(0, max(e99) * 1.16)
    ax.legend(loc="upper left", framealpha=0.92, edgecolor="#cccccc",
              handlelength=1.5)

    for ax in axes:
        ax.tick_params(labelsize=TICK_SIZE)
        ax.grid(True, alpha=0.2, color="#cccccc", linestyle=":")
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)

    fig.tight_layout()
    os.makedirs(os.path.dirname(os.path.abspath(pdf_path)) or ".", exist_ok=True)
    fig.savefig(
        pdf_path, bbox_inches="tight",
        metadata={"CreationDate": None, "ModDate": None},
    )
    if paper_pdf_path:
        os.makedirs(os.path.dirname(os.path.abspath(paper_pdf_path)) or ".", exist_ok=True)
        shutil.copyfile(pdf_path, paper_pdf_path)
    if png_path:
        fig.savefig(png_path, dpi=160, bbox_inches="tight")
    plt.close(fig)
    if manifest_path:
        manifest = {
            "contract": {
                "primary_cell": PRIMARY_CELL,
                "load_trials_per_cell": require_trials,
                "epoch_trials_per_cell": require_trials,
                "selection": "all status=ok rows; no performance filtering",
            },
            "inputs": {
                portable(csv_path): sha256(csv_path),
                portable(epoch_csv_path): sha256(epoch_csv_path),
            },
            "generator": {
                "path": portable(Path(__file__)),
                "sha256": sha256(Path(__file__)),
            },
            "outputs": {
                portable(pdf_path): sha256(pdf_path),
                **({portable(paper_pdf_path): sha256(paper_pdf_path)}
                   if paper_pdf_path else {}),
                **({portable(png_path): sha256(png_path)} if png_path else {}),
            },
            "loads_mbps": expected_loads,
            "epochs_us": expected_epochs,
        }
        manifest_file = Path(manifest_path)
        manifest_file.parent.mkdir(parents=True, exist_ok=True)
        manifest_file.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"wrote {pdf_path}" + (f" and {png_path}" if png_path else ""))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--csv",
        default="data/paper_eval/fig2/fig2_append_latency_clean_ad8a064f/results.csv",
    )
    ap.add_argument(
        "--epoch-csv",
        default="data/paper_eval/fig2/fig2_mechanism_epoch_clean_fd1a36ce/results.csv",
    )
    ap.add_argument(
        "--pdf",
        default="data/paper_eval/fig2/fig2_append_latency_clean_ad8a064f/append_latency.pdf",
    )
    ap.add_argument("--paper-pdf", default="Paper/Figures/append_latency.pdf")
    ap.add_argument(
        "--png",
        default="data/paper_eval/fig2/fig2_append_latency_clean_ad8a064f/append_latency.png",
    )
    ap.add_argument(
        "--manifest",
        default="data/paper_eval/fig2/fig2_append_latency_clean_ad8a064f/figure_manifest.json",
    )
    ap.add_argument("--require-trials", type=int, default=3)
    ap.add_argument("--loads", default="100 250 500 750 1000 1500 2000")
    ap.add_argument("--epochs", default="250 500 1000 2000")
    args = ap.parse_args()
    plot(
        args.csv,
        args.epoch_csv,
        args.pdf,
        args.paper_pdf,
        args.png,
        args.manifest,
        args.require_trials,
        [int(value) for value in args.loads.split()],
        [int(value) for value in args.epochs.split()],
    )


if __name__ == "__main__":
    main()
