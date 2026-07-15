#!/usr/bin/env python3
"""SMR-FIFO eval (paper Q3, tab:kv-pipelined) tables + figures.

Reads the aggregated summary.csv written by
benchmarks/kv_store/run_smr_fifo_eval.sh and emits:

  - <outdir>/smr_kv_pipe_ops.pdf : Pipe ops/s per system, Valid=NO hatched (Fig 1)
  - <outdir>/smr_fifo_tax.pdf    : Pipe vs Serialize per system (FIFO tax, Fig 2)
  - --markdown <file>            : markdown table matching
                                   `System | Pipe (ops/s) | FIFO | Slowdown | Valid`

Medians across trials. Slowdown is vs the Embarcadero Pipe median.
Plots are skipped (with a note) if matplotlib is unavailable.
"""

import argparse
import csv
import statistics
import sys
from collections import defaultdict

SYSTEM_LABEL = {
    "EMBARCADERO": "Embarcadero",
    "CORFU": "CXL-Corfu",
    "SCALOG": "CXL-Scalog",
    "LAZYLOG": "CXL-LazyLog",
}
SYSTEM_ORDER = ["EMBARCADERO", "CORFU", "SCALOG", "LAZYLOG"]


def load_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def aggregate(rows):
    """-> {(sequencer, mode): dict} medians + validity consensus."""
    groups = defaultdict(list)
    for r in rows:
        groups[(r["sequencer"], r["mode"])].append(r)
    out = {}
    for key, rs in groups.items():
        tputs = [float(r["write_throughput_ops_sec"]) for r in rs]
        valid = all(r["valid"] == "1" for r in rs)
        fifo_modes = {r.get("fifo_mode", "") for r in rs}
        failed = sorted({r.get("failed_checks", "none") for r in rs} - {"none"})
        out[key] = {
            "ops_median": statistics.median(tputs),
            "trials": len(rs),
            "valid": valid,
            "fifo_mode": "/".join(sorted(fifo_modes)),
            "failed_checks": "+".join(failed) if failed else "none",
            "mismatch_keys": max(int(r.get("final_mismatch_keys", 0) or 0) for r in rs),
            "key_reorders": max(int(r.get("key_reorders", 0) or 0) for r in rs),
        }
    return out


def fmt_ops(v):
    return f"{v:,.0f}"


def make_markdown(agg):
    ref = agg.get(("EMBARCADERO", "pipe"))
    lines = [
        "<!-- fills tab:kv-pipelined; Valid = store size stays K, applied==published,",
        "     AND per-key final value == last session-submitted version (session FIFO) -->",
        "| System | Mode | Pipe (ops/s) | FIFO | Slowdown | Valid |",
        "|--------|------|-------------:|------|---------:|-------|",
    ]
    for seq in SYSTEM_ORDER:
        for mode in ("pipe", "serialize"):
            a = agg.get((seq, mode))
            if a is None:
                continue
            slow = "---"
            if ref and a is not ref and a["ops_median"] > 0:
                slow = f"{ref['ops_median'] / a['ops_median']:.1f}x"
            valid = "YES" if a["valid"] else f"**NO** ({a['failed_checks']})"
            lines.append(
                f"| {SYSTEM_LABEL.get(seq, seq)} | {mode} | {fmt_ops(a['ops_median'])} "
                f"| {a['fifo_mode']} | {slow} | {valid} |"
            )
    lines.append("")
    lines.append(
        "Medians over trials. Slowdown vs Embarcadero Pipe. A Valid=NO striped "
        "Pipe row for a write-before-order system is the expected Q3 result "
        "(per-publisher FIFO violated under striping), not a harness failure."
    )
    lines.append(
        "FIDELITY: harness CXL-LazyLog Pipe gates appends on binding (not "
        "LazyLog's own append contract) — label or withhold that row. "
        "CXL-Corfu token FIFO is faithful as of the [[CORFU_FIFO_FIX]] ordered "
        "token stage (2026-07-16). See benchmarks/kv_store/README_SMR_FIFO.md."
    )
    return "\n".join(lines) + "\n"


def make_plots(agg, outdir):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib unavailable; skipping figures", file=sys.stderr)
        return
    plt.rcParams.update({
        "font.size": 9,
        "axes.titlesize": 9,
        "axes.labelsize": 9,
        "pdf.fonttype": 42,
        "ps.fonttype": 42,
    })

    # Fig 1: Pipe ops/s, Valid=NO hatched.
    systems = [s for s in SYSTEM_ORDER if (s, "pipe") in agg]
    if systems:
        fig, ax = plt.subplots(figsize=(3.4, 2.2))
        xs = range(len(systems))
        for x, s in zip(xs, systems):
            a = agg[(s, "pipe")]
            ax.bar(
                x, a["ops_median"],
                color="#4C72B0" if a["valid"] else "white",
                edgecolor="#4C72B0",
                hatch=None if a["valid"] else "///",
                width=0.6,
            )
            if not a["valid"]:
                ax.text(x, a["ops_median"], "invalid", ha="center", va="bottom", fontsize=7)
        ax.set_xticks(list(xs))
        ax.set_xticklabels([SYSTEM_LABEL[s] for s in systems], rotation=15)
        ax.set_ylabel("Pipelined overwrites (ops/s)")
        ax.set_title("SMR KV: pipelined ops/s (hatched = Valid=NO)")
        fig.tight_layout()
        fig.savefig(f"{outdir}/smr_kv_pipe_ops.pdf")
        plt.close(fig)
        print(f"wrote {outdir}/smr_kv_pipe_ops.pdf")

    # Fig 2: FIFO tax — pipe vs serialize per system (log scale).
    both = [s for s in SYSTEM_ORDER if (s, "pipe") in agg and (s, "serialize") in agg]
    if both:
        fig, ax = plt.subplots(figsize=(3.4, 2.2))
        w = 0.35
        for i, s in enumerate(both):
            p, z = agg[(s, "pipe")], agg[(s, "serialize")]
            ax.bar(i - w / 2, p["ops_median"], width=w,
                   color="#4C72B0" if p["valid"] else "white",
                   edgecolor="#4C72B0", hatch=None if p["valid"] else "///",
                   label="Pipe" if i == 0 else None)
            ax.bar(i + w / 2, z["ops_median"], width=w,
                   color="#DD8452" if z["valid"] else "white",
                   edgecolor="#DD8452", hatch=None if z["valid"] else "///",
                   label="Serialize" if i == 0 else None)
        ax.set_yscale("log")
        ax.set_xticks(range(len(both)))
        ax.set_xticklabels([SYSTEM_LABEL[s] for s in both], rotation=15)
        ax.set_ylabel("ops/s (log)")
        ax.set_title("Cost of restoring session FIFO without native holds")
        ax.legend(frameon=False, fontsize=7)
        fig.tight_layout()
        fig.savefig(f"{outdir}/smr_fifo_tax.pdf")
        plt.close(fig)
        print(f"wrote {outdir}/smr_fifo_tax.pdf")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--csv", required=True, help="aggregated summary.csv")
    ap.add_argument("--outdir", required=True, help="directory for PDFs")
    ap.add_argument("--markdown", help="write markdown table here")
    args = ap.parse_args()

    rows = load_rows(args.csv)
    if not rows:
        print("no rows in CSV", file=sys.stderr)
        return 1
    agg = aggregate(rows)

    md = make_markdown(agg)
    if args.markdown:
        with open(args.markdown, "w") as f:
            f.write(md)
        print(f"wrote {args.markdown}")
    else:
        print(md)

    make_plots(agg, args.outdir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
