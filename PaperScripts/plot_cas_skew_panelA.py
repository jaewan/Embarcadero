#!/usr/bin/env python3
"""Panel A: CAS conditional-write failures vs injected inter-broker skew.

x = injected skew (ms, one broker delayed via tc netem); y = fraction of an
etcd/ZK client's compare-and-set operations rejected (application-visible,
unrepairable). Order-preserving logs (Embarcadero server-side hold, Corfu
token-before-write) stay at 0 across the skew range; write-before-order
(Scalog) commits past gaps, so rejections climb steeply with skew.

Usage:
  python3 PaperScripts/plot_cas_skew_panelA.py \
     --csv data/paper_eval/cas/cas_skew_panelA_full/panelA.csv \
     --out data/paper_eval/cas/cas_skew_panelA
"""
import argparse, csv, os
from collections import defaultdict
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SYS = ["EMBARCADERO", "CORFU", "SCALOG"]
COLOR = {"EMBARCADERO": "#1f77b4", "CORFU": "#d62728", "SCALOG": "#2ca02c"}
MARK = {"EMBARCADERO": "o", "CORFU": "s", "SCALOG": "^"}
LABEL = {"EMBARCADERO": "Embarcadero (hold)", "CORFU": "Corfu (token order)",
         "SCALOG": "Scalog (write-before-order)"}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--ops", type=int, default=100000, help="ops per run (for %% axis)")
    a = ap.parse_args()

    data = defaultdict(dict)  # system -> {delay: rejections}
    for r in csv.DictReader(open(a.csv)):
        try:
            data[r["system"]][float(r["delay_ms"])] = int(r["cas_rejections"])
        except (ValueError, KeyError):
            continue

    fig, ax = plt.subplots(figsize=(5.4, 3.6))
    for s in SYS:
        if s not in data:
            continue
        xs = sorted(data[s])
        ys = [100.0 * data[s][x] / a.ops for x in xs]
        ax.plot(xs, ys, "-", color=COLOR[s], marker=MARK[s], lw=1.8, ms=6,
                label=LABEL[s])
    ax.set_xlabel("Injected inter-broker skew (ms)")
    ax.set_ylabel("Rejected conditional writes (%)")
    ax.set_title("CAS metadata service: correctness vs skew (RF=1)")
    ax.set_ylim(-3, 100)
    ax.grid(True, ls=":", alpha=0.4)
    ax.legend(frameon=False, fontsize=8, loc="center right")
    fig.tight_layout()
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(f"{a.out}.{ext}", dpi=150, bbox_inches="tight")
    print(f"wrote {a.out}.png and {a.out}.pdf")


if __name__ == "__main__":
    main()
