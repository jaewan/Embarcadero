#!/usr/bin/env python3
"""Plot the fixed-N throughput-regime comparison used by the paper.

The figure uses ACK-drain throughput for every bar.  RF=2 rows come from the
matched Fig. 1 campaign CSV; RF=0 rows come from the matched overnight harness
because that harness contains the baseline ordering-only cells.  Every input
is fail-closed on commit, contract, and trial count.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import shutil
import statistics
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


REPO_ROOT = Path(__file__).resolve().parents[1]
TRIALS = 3

SERIES = {
    "embar": ("Embarcadero", "#2166ac", ""),
    "scalog": ("CXL-Scalog", "#1b9e77", ""),
    "corfu": ("CXL-Corfu", "#d95f02", ""),
    "lazylog": ("CXL-LazyLog†", "#7570b3", "////"),
    "embar_o0": ("Embar O0 reference‡", "#969696", "xx"),
}

GROUPS = [
    ("disk", "NVMe durable\nRF2 / ACK2"),
    ("mem", "DRAM replica\nRF2 / ACK2"),
    ("rf0", "Replication off\nACK1"),
]

RF2_CELLS = {
    "fig1_embar_o5_disk_n2": ("disk", "embar", 5, 2, 2,
                               "ordered + media durable"),
    "fig1_scalog_o1_disk_n2": ("disk", "scalog", 1, 2, 2,
                                "native cut + media durable"),
    "fig1_corfu_o2_disk_n2": ("disk", "corfu", 2, 2, 2,
                               "token ordered + media durable"),
    "fig1_lazylog_o2_disk_n2": ("disk", "lazylog", 2, 2, 2,
                                 "pre-binding metadata/data durable"),
    "fig1_embar_o5_mem_n2": ("mem", "embar", 5, 2, 2,
                              "ordered + DRAM replicated"),
    "fig1_scalog_o1_mem_n2": ("mem", "scalog", 1, 2, 2,
                               "native cut + DRAM replicated"),
    "fig1_corfu_o2_mem_n2": ("mem", "corfu", 2, 2, 2,
                              "token ordered + DRAM replicated"),
}

RF0_CELLS = {
    "e2_embar5_rf0_ack1_n2": ("rf0", "embar", 5, 0, 1,
                               "ordered, replication off"),
    "e2_scalog_rf0_n2": ("rf0", "scalog", 1, 0, 1,
                          "native cut, replication off"),
    "e2_corfu_rf0_n2": ("rf0", "corfu", 2, 0, 1,
                         "token ordered, replication off"),
    "e2_embar0_rf0_ack1_n2": ("rf0", "embar_o0", 0, 0, 1,
                               "unordered ingestion ceiling"),
}

TOTAL_RE = re.compile(r"TOTAL\s+→.*?\(([0-9.]+) GB/s\)")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def portable(path: Path) -> str:
    resolved = path.resolve()
    try:
        return str(resolved.relative_to(REPO_ROOT))
    except ValueError:
        return str(resolved)


def require_equal(row: dict[str, str], expected: dict[str, str], cell: str) -> None:
    bad = {key: (row.get(key), value) for key, value in expected.items()
           if row.get(key) != value}
    if bad:
        raise SystemExit(f"contract mismatch for {cell}: {bad}")


def load_rf2(path: Path, commit: str) -> list[dict[str, object]]:
    by_cell: dict[str, list[dict[str, str]]] = {cell: [] for cell in RF2_CELLS}
    with path.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("cell") in by_cell and row.get("status") == "ok":
                by_cell[row["cell"]].append(row)

    selected: list[dict[str, object]] = []
    for cell, (group, series, order, rf, ack, semantics) in RF2_CELLS.items():
        rows = by_cell[cell]
        if len(rows) != TRIALS:
            raise SystemExit(f"{cell}: expected {TRIALS} successful rows, got {len(rows)}")
        for index, row in enumerate(rows, 1):
            require_equal(row, {
                "git_commit": commit, "n_clients": "2", "msg_size": "4096",
                "total_bytes": "10737418240", "num_brokers": "4",
                "rf": str(rf), "ack": str(ack), "order": str(order),
                "threads": "6", "batch_kb": "2048",
            }, cell)
            selected.append({
                "group": group, "series": series, "cell": cell,
                "trial": index, "order": order, "rf": rf, "ack": ack,
                "semantics": semantics,
                "throughput_gbps": float(row["bandwidth_sum_gbps"]),
            })
    return selected


def load_rf0(root: Path, commit: str) -> tuple[list[dict[str, object]], list[Path]]:
    selected: list[dict[str, object]] = []
    inputs: list[Path] = []
    for cell, (group, series, order, rf, ack, semantics) in RF0_CELLS.items():
        cell_dir = root / cell
        log_path = root.parents[2] / "logs" / f"{cell}.log"
        attempts_path = cell_dir / "attempt_summary.csv"
        contract_path = cell_dir / "run_contract.csv"
        for path in (log_path, attempts_path, contract_path):
            if not path.is_file():
                raise SystemExit(f"missing RF0 input: {path}")
            inputs.append(path)

        with attempts_path.open(newline="") as handle:
            attempts = list(csv.DictReader(handle))
        successes = [row for row in attempts if row.get("result") == "success"]
        if len(successes) != TRIALS or len(attempts) != TRIALS:
            raise SystemExit(
                f"{cell}: expected exactly {TRIALS} first-attempt successes; "
                f"attempts={attempts}"
            )

        with contract_path.open(newline="") as handle:
            contracts = list(csv.DictReader(handle))
        if len(contracts) != 1:
            raise SystemExit(f"{cell}: expected one run contract")
        require_equal(contracts[0], {
            "git_commit": commit, "git_dirty": "false", "order": str(order),
            "ack_level": str(ack), "replication_factor": str(rf),
        }, cell)

        values = [float(value) for value in TOTAL_RE.findall(log_path.read_text())]
        if len(values) != TRIALS:
            raise SystemExit(f"{cell}: expected {TRIALS} TOTAL values, got {values}")
        for trial, value in enumerate(values, 1):
            selected.append({
                "group": group, "series": series, "cell": cell,
                "trial": trial, "order": order, "rf": rf, "ack": ack,
                "semantics": semantics, "throughput_gbps": value,
            })
    return selected, inputs


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--rf2-csv", required=True, type=Path)
    parser.add_argument("--rf0-root", required=True, type=Path,
                        help=".../multiclient/logs/<run-tag> directory")
    parser.add_argument("--commit", required=True)
    parser.add_argument("--pdf", required=True, type=Path)
    parser.add_argument("--paper-pdf", type=Path)
    parser.add_argument("--png", type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--selected-csv", required=True, type=Path)
    args = parser.parse_args()

    rows = load_rf2(args.rf2_csv, args.commit)
    rf0_rows, rf0_inputs = load_rf0(args.rf0_root, args.commit)
    rows.extend(rf0_rows)

    values: dict[tuple[str, str], list[float]] = {}
    for row in rows:
        values.setdefault((str(row["group"]), str(row["series"])), []).append(
            float(row["throughput_gbps"])
        )

    plt.rcParams.update({
        "font.family": "serif", "font.size": 8, "axes.labelsize": 8,
        "xtick.labelsize": 7.5, "ytick.labelsize": 7,
        "legend.fontsize": 6.8, "pdf.fonttype": 42,
    })
    fig, ax = plt.subplots(figsize=(6.5, 2.15))
    centers = [0.0, 1.0, 2.0]
    series_order = ["embar", "scalog", "corfu", "lazylog", "embar_o0"]
    offsets = {series: (index - 2) * 0.15
               for index, series in enumerate(series_order)}
    handles = []
    labels = []

    for series in series_order:
        label, color, hatch = SERIES[series]
        first_bar = None
        for center, (group, _) in zip(centers, GROUPS):
            trial_values = values.get((group, series))
            if not trial_values:
                if series == "lazylog" and group in {"mem", "rf0"}:
                    ax.text(center + offsets[series], 0.22, "N/A", ha="center",
                            va="bottom", fontsize=5.8, color="#666666")
                continue
            median = statistics.median(trial_values)
            lower = median - min(trial_values)
            upper = max(trial_values) - median
            bar = ax.bar(
                center + offsets[series], median, width=0.135,
                color=color if series != "embar_o0" else "white",
                edgecolor=color, linewidth=0.9, hatch=hatch,
                yerr=[[lower], [upper]], capsize=2,
                error_kw={"elinewidth": 0.7, "capthick": 0.7}, zorder=3,
            )
            if first_bar is None:
                first_bar = bar[0]
            ax.text(
                center + offsets[series], median + 0.20,
                f"{median:.2f}" if median >= 0.1 else f"{median:.3f}",
                ha="center", va="bottom", fontsize=5.8, rotation=0,
            )
        if first_bar is not None:
            handles.append(first_bar)
            labels.append(label)

    ax.set_xticks(centers)
    ax.set_xticklabels([label for _, label in GROUPS])
    ax.set_ylabel("ACK-drain throughput (GB/s)")
    ax.set_ylim(0, 12.4)
    ax.set_xlim(-0.55, 2.55)
    ax.grid(True, axis="y", alpha=0.25, linestyle=":", zorder=0)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.legend(handles, labels, loc="upper center", bbox_to_anchor=(0.5, 1.19),
              ncol=3, frameon=True, columnspacing=1.1, handlelength=1.5)
    fig.tight_layout(rect=(0, 0, 1, 0.88))

    args.pdf.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(args.pdf, bbox_inches="tight",
                metadata={"CreationDate": None, "ModDate": None})
    if args.png:
        args.png.parent.mkdir(parents=True, exist_ok=True)
        fig.savefig(args.png, dpi=200, bbox_inches="tight")
    plt.close(fig)
    if args.paper_pdf:
        args.paper_pdf.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(args.pdf, args.paper_pdf)

    args.selected_csv.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = ["group", "series", "cell", "trial", "order", "rf", "ack",
                  "semantics", "throughput_gbps"]
    with args.selected_csv.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)

    summary = {}
    for key, trial_values in sorted(values.items()):
        summary[".".join(key)] = {
            "n": len(trial_values), "median_gbps": statistics.median(trial_values),
            "min_gbps": min(trial_values), "max_gbps": max(trial_values),
            "values_gbps": trial_values,
        }
    inputs = [args.rf2_csv, *rf0_inputs]
    manifest = {
        "contract": {
            "git_commit": args.commit, "metric": "bandwidth_sum_gbps",
            "selection": "all three successful fixed-commit trials; no filtering",
            "n_clients": 2, "client_hosts": ["c4", "c3"],
            "message_bytes": 4096, "bytes_per_client": 10737418240,
            "aggregate_bytes": 21474836480,
            "brokers": 4, "threads_per_broker": 6, "batch_kb": 2048,
            "lazylog_note": "faithful pre-binding durable ACK; not ordered delivery",
            "embar_o0_note": "unordered ingestion reference; not Scalog-equivalent",
        },
        "inputs": {portable(path): sha256(path) for path in inputs},
        "generator": {"path": portable(Path(__file__)),
                      "sha256": sha256(Path(__file__).resolve())},
        "outputs": {
            portable(args.pdf): sha256(args.pdf),
            portable(args.selected_csv): sha256(args.selected_csv),
            **({portable(args.paper_pdf): sha256(args.paper_pdf)}
               if args.paper_pdf else {}),
            **({portable(args.png): sha256(args.png)} if args.png else {}),
        },
        "selected_rows_csv": portable(args.selected_csv),
        "summary": summary,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")


if __name__ == "__main__":
    main()
