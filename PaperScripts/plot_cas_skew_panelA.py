#!/usr/bin/env python3
"""Panel A: CAS dependent-conditional-update failures vs injected inter-broker skew.

x = injected skew (ms, one broker delayed via tc netem); y = fraction of a
client's pipelined compare-and-set operations rejected. A rejection is a
same-key dependent-chain inversion whose downstream chained ops also fail until
the chain re-anchors (cascade-amplified downstream damage, not the raw inversion
rate). Order-preserving logs (Embarcadero server-side hold; Corfu token-before-
write) stay at 0; write-before-order (Scalog) climbs with skew.

Reads per-trial rows and plots the median with a min–max band across trials.

Usage:
  python3 PaperScripts/plot_cas_skew_panelA.py \
     --csv data/paper_eval/cas/cas_skew_panelA/panelA.csv \
     --out data/paper_eval/cas/cas_skew_panelA
"""
import argparse, csv, os, statistics
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
    ap.add_argument("--ops", type=int, default=100000)
    a = ap.parse_args()

    # system -> delay -> [rejections per trial]
    data = defaultdict(lambda: defaultdict(list))
    for r in csv.DictReader(open(a.csv)):
        try:
            rej = int(r["cas_rejections"])
        except (ValueError, KeyError):
            continue  # NA / missing trial
        data[r["system"]][float(r["delay_ms"])].append(rej)

    fig, ax = plt.subplots(figsize=(5.6, 3.7))
    for s in SYS:
        if s not in data:
            continue
        xs = sorted(data[s])
        med, lo, hi = [], [], []
        for x in xs:
            v = [100.0 * r / a.ops for r in data[s][x]]
            med.append(statistics.median(v)); lo.append(min(v)); hi.append(max(v))
        ax.fill_between(xs, lo, hi, color=COLOR[s], alpha=0.15, linewidth=0)
        ax.plot(xs, med, "-", color=COLOR[s], marker=MARK[s], lw=1.8, ms=6,
                label=LABEL[s])
    n_trials = max((len(v) for s in data for v in data[s].values()), default=0)
    ax.set_xlabel("Injected inter-broker skew (ms)")
    ax.set_ylabel("Rejected conditional writes (%)")
    ax.set_title(f"Dependent CAS updates: correctness vs skew "
                 f"(RF=1, median of {n_trials} trials)")
    ax.set_ylim(-3, 100)
    ax.grid(True, ls=":", alpha=0.4)
    ax.legend(frameon=False, fontsize=8, loc="center right")
    fig.tight_layout()
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(f"{a.out}.{ext}", dpi=150, bbox_inches="tight")
    print(f"wrote {a.out}.png and {a.out}.pdf (median of {n_trials} trials, min-max band)")


if __name__ == "__main__":
    main()
