#!/usr/bin/env python3
"""Plot the two correctness-gated end-to-end comparison figures for the paper.

Panel A: KV correctness-gated latency-throughput frontier (RF=1).
  x = sustained throughput (ops/s), y = apply (commit) latency p50 (us).
  Filled marker = Valid across the point; hollow red-edged = Valid failed
  (e.g. Scalog's per-session FIFO break at high in-flight depth).

Panel B: append->ACK latency vs offered load (RF=2/ACK=2, mem-copy sink).
  x = offered load (MB/s), y = append->ACK p50 (ms, log). p99 as light band.

Usage:
  python3 PaperScripts/plot_kv_frontier_and_appendack.py \
     --frontier data/paper_eval/kv_frontier_<ts>/frontier.csv \
     --appendack data/paper_eval/appendack/<tag>/appendack.csv \
     --out data/paper_eval/embar_dominance
"""
import argparse, os
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

SYS = ["EMBARCADERO", "CORFU", "SCALOG"]
COLOR = {"EMBARCADERO": "#1f77b4", "CORFU": "#d62728", "SCALOG": "#2ca02c"}
MARK = {"EMBARCADERO": "o", "CORFU": "s", "SCALOG": "^"}
LABEL = {"EMBARCADERO": "Embarcadero (O5)", "CORFU": "Corfu (O2)", "SCALOG": "Scalog (O1)"}


def plot_frontier(ax, df):
    for s in SYS:
        d = df[df.system == s].sort_values("throughput_ops_med")
        if d.empty:
            continue
        ax.plot(d.throughput_ops_med, d.apply_p50_us_med, "-", color=COLOR[s],
                lw=1.6, zorder=2, label=LABEL[s])
        for _, r in d.iterrows():
            valid = int(r.valid_all) == 1
            ax.scatter(r.throughput_ops_med, r.apply_p50_us_med, s=60,
                       marker=MARK[s], zorder=3,
                       facecolor=COLOR[s] if valid else "white",
                       edgecolor=COLOR[s] if valid else "red",
                       linewidths=1.6)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("Sustained throughput (ops/s)")
    ax.set_ylabel("Apply (commit) latency p50 (µs)")
    ax.set_title("(a) KV correctness-gated frontier (RF=1)")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.legend(frameon=False, fontsize=8, loc="upper left")
    # hollow-marker legend note
    ax.scatter([], [], s=60, marker="o", facecolor="white", edgecolor="red",
               linewidths=1.6, label="Valid FAILED")
    ax.legend(frameon=False, fontsize=8, loc="upper left")


def plot_appendack(ax, df):
    for s in SYS:
        d = df[(df.system == s) & (df.status == "ok")].copy()
        if d.empty:
            continue
        d = d.sort_values("target_mbps")
        p50 = d.pub_ack_p50_us / 1000.0
        p99 = d.pub_ack_p99_us / 1000.0
        ax.plot(d.target_mbps, p50, "-", color=COLOR[s], marker=MARK[s],
                lw=1.6, ms=6, label=LABEL[s])
        ax.fill_between(d.target_mbps, p50, p99, color=COLOR[s], alpha=0.12)
    ax.set_xscale("log"); ax.set_yscale("log")
    ax.set_xlabel("Offered load (MB/s)")
    ax.set_ylabel("append→ACK latency p50 (ms)")
    ax.set_title("(b) append→ACK vs offered load (RF=2, ACK=2)")
    ax.grid(True, which="both", ls=":", alpha=0.4)
    ax.legend(frameon=False, fontsize=8, loc="upper left")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--frontier", required=True)
    ap.add_argument("--appendack", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    fr = pd.read_csv(a.frontier)
    aa = pd.read_csv(a.appendack)
    for c in ["throughput_ops_med", "apply_p50_us_med", "valid_all"]:
        fr[c] = pd.to_numeric(fr[c], errors="coerce")
    for c in ["target_mbps", "pub_ack_p50_us", "pub_ack_p99_us"]:
        aa[c] = pd.to_numeric(aa[c], errors="coerce")
    fr = fr.dropna(subset=["throughput_ops_med", "apply_p50_us_med"])

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.2))
    plot_frontier(ax1, fr)
    plot_appendack(ax2, aa)
    fig.tight_layout()
    os.makedirs(os.path.dirname(a.out) or ".", exist_ok=True)
    for ext in ("png", "pdf"):
        fig.savefig(f"{a.out}.{ext}", dpi=150, bbox_inches="tight")
    print(f"wrote {a.out}.png and {a.out}.pdf")


if __name__ == "__main__":
    main()
