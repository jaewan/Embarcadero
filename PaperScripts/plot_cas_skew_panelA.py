#!/usr/bin/env python3
"""CAS dependent-conditional-update failures vs injected inter-server skew.

x = injected skew (ms, one log server delayed via tc netem); y = fraction of a
client's pipelined compare-and-set operations rejected. A rejection is a
same-key dependent-chain inversion whose downstream chained ops also fail until
the chain re-anchors (cascade-amplified downstream damage, not the raw inversion
rate). Order-preserving logs (Embarcadero server-side hold; Corfu token-before-
write) stay at 0; write-before-order (Scalog) climbs with skew.

Reads per-trial rows and plots the median with a min–max band across trials.

Usage:
  python3 PaperScripts/plot_cas_skew_panelA.py \
     --csv data/paper_eval/cas/cas_skew_panelA_3trial/panelA.csv \
     --out data/paper_eval/cas/cas_skew_panelA_3trial/cas_skew
"""
import argparse, csv, hashlib, json, os, statistics
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
    ap.add_argument("--require-trials", type=int, default=3)
    a = ap.parse_args()

    # system -> delay -> [rejections per trial]
    data = defaultdict(lambda: defaultdict(list))
    for r in csv.DictReader(open(a.csv)):
        try:
            rej = int(r["cas_rejections"])
        except (ValueError, KeyError):
            continue  # NA / missing trial
        data[r["system"]][float(r["delay_ms"])].append(rej)

    expected_delays = {0.0, 1.0, 2.0, 5.0, 10.0}
    bad = {
        (s, delay): len(data[s][delay])
        for s in SYS
        for delay in expected_delays
        if len(data[s][delay]) != a.require_trials
    }
    extras = {
        (s, delay) for s in data for delay in data[s]
        if s not in SYS or delay not in expected_delays
    }
    if bad or extras:
        raise SystemExit(
            f"incomplete CAS matrix: bad_counts={bad}, extras={sorted(extras)}"
        )

    fig, ax = plt.subplots(figsize=(4.45, 2.62))
    # Draw the rising series first. Embarcadero and Corfu are identical at
    # zero; a solid blue line with large hollow circles surrounds Corfu's
    # smaller red squares, while Corfu's dashed segments remain visible.
    for s in ("SCALOG", "EMBARCADERO", "CORFU"):
        if s not in data:
            continue
        xs = sorted(data[s])
        med, lo, hi = [], [], []
        for x in xs:
            v = [100.0 * r / a.ops for r in data[s][x]]
            med.append(statistics.median(v)); lo.append(min(v)); hi.append(max(v))
        ax.fill_between(xs, lo, hi, color=COLOR[s], alpha=0.15, linewidth=0)
        style = {
            "SCALOG": dict(ls="-", ms=5.5, mfc=COLOR[s], zorder=2),
            "EMBARCADERO": dict(
                ls="-", ms=8.0, mfc="none", mew=1.6, zorder=3
            ),
            "CORFU": dict(ls="--", ms=4.2, mfc=COLOR[s], zorder=4),
        }[s]
        ax.plot(
            xs, med, color=COLOR[s], marker=MARK[s], lw=1.7,
            label=LABEL[s], **style
        )
    n_trials = max((len(v) for s in data for v in data[s].values()), default=0)
    ax.set_xlabel("Injected inter-server skew (ms)")
    ax.set_ylabel("Rejected conditional writes (%)")
    ax.set_ylim(-5, 85)
    ax.grid(True, ls=":", alpha=0.4)
    ax.legend(frameon=False, fontsize=7.4, loc="center right")
    fig.tight_layout()
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(
            f"{a.out}.{ext}", dpi=180, bbox_inches="tight",
            metadata={"CreationDate": None, "ModDate": None},
        )
    manifest = {
        "contract": {
            "systems": SYS,
            "delays_ms": sorted(expected_delays),
            "trials_per_cell": a.require_trials,
            "ops_per_trial": a.ops,
            "metric": "cascade-amplified rejected dependent CAS operations",
        },
        "input": {
            "path": a.csv,
            "sha256": hashlib.sha256(open(a.csv, "rb").read()).hexdigest(),
        },
        "outputs": {
            ext: hashlib.sha256(open(f"{a.out}.{ext}", "rb").read()).hexdigest()
            for ext in ("png", "pdf")
        },
    }
    with open(f"{a.out}.manifest.json", "w") as handle:
        json.dump(manifest, handle, indent=2, sort_keys=True)
        handle.write("\n")
    print(f"wrote {a.out}.png and {a.out}.pdf (median of {n_trials} trials, min-max band)")


if __name__ == "__main__":
    main()
