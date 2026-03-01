#!/usr/bin/env python3
import argparse
import csv
import glob
import math
import os
import re
from collections import defaultdict

import matplotlib.pyplot as plt


def to_float(v):
    if v is None:
        return None
    s = str(v).strip()
    if s == "" or s.upper() == "N/A" or s.upper() == "FAIL":
        return None
    try:
        return float(s)
    except ValueError:
        # Accept decorated numeric fields like "929.30 MB/s".
        m = re.search(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", s)
        return float(m.group(0)) if m else None


def mean_and_ci(values):
    vals = [v for v in values if v is not None]
    n = len(vals)
    if n == 0:
        return None, None, 0
    mean = sum(vals) / n
    if n == 1:
        return mean, 0.0, 1
    var = sum((x - mean) ** 2 for x in vals) / (n - 1)
    std = math.sqrt(var)
    ci95 = 1.96 * std / math.sqrt(n)
    return mean, ci95, n


def load_trials(input_dir):
    trial_summaries = sorted(glob.glob(os.path.join(input_dir, "trial_*", "summary.csv")))
    if not trial_summaries:
        raise FileNotFoundError(f"No trial summary.csv files under {input_dir}")

    data = defaultdict(lambda: defaultdict(list))
    for path in trial_summaries:
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            for row in reader:
                target = to_float(row.get("target_mbps"))
                if target is None:
                    continue
                key = int(target)
                for field in [
                    "actual_pub_mbps",
                    "actual_e2e_mbps",
                    "pub_p50_us",
                    "pub_p99_us",
                    "pub_p999_us",
                    "e2e_p50_us",
                    "e2e_p99_us",
                    "e2e_p999_us",
                ]:
                    data[key][field].append(to_float(row.get(field)))
    return data


def write_ci_summary(data, output_csv):
    fields = [
        "target_mbps",
        "actual_pub_mbps_mean", "actual_pub_mbps_ci95", "actual_pub_mbps_n",
        "actual_e2e_mbps_mean", "actual_e2e_mbps_ci95", "actual_e2e_mbps_n",
        "pub_p50_us_mean", "pub_p50_us_ci95", "pub_p50_us_n",
        "pub_p99_us_mean", "pub_p99_us_ci95", "pub_p99_us_n",
        "pub_p999_us_mean", "pub_p999_us_ci95", "pub_p999_us_n",
        "e2e_p50_us_mean", "e2e_p50_us_ci95", "e2e_p50_us_n",
        "e2e_p99_us_mean", "e2e_p99_us_ci95", "e2e_p99_us_n",
        "e2e_p999_us_mean", "e2e_p999_us_ci95", "e2e_p999_us_n",
    ]
    with open(output_csv, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        for target in sorted(data.keys()):
            row = {"target_mbps": target}
            for field in [
                "actual_pub_mbps",
                "actual_e2e_mbps",
                "pub_p50_us",
                "pub_p99_us",
                "pub_p999_us",
                "e2e_p50_us",
                "e2e_p99_us",
                "e2e_p999_us",
            ]:
                mean, ci95, n = mean_and_ci(data[target][field])
                row[f"{field}_mean"] = "" if mean is None else f"{mean:.6f}"
                row[f"{field}_ci95"] = "" if ci95 is None else f"{ci95:.6f}"
                row[f"{field}_n"] = n
            writer.writerow(row)


def plot_frontier(data, prefix):
    targets = sorted(data.keys())
    x = []
    p50 = []
    p99 = []
    p999 = []
    p50_ci = []
    p99_ci = []
    p999_ci = []
    for t in targets:
        x.append(t)
        m, c, _ = mean_and_ci(data[t]["e2e_p50_us"])
        p50.append(m)
        p50_ci.append(c if c is not None else 0.0)
        m, c, _ = mean_and_ci(data[t]["e2e_p99_us"])
        p99.append(m)
        p99_ci.append(c if c is not None else 0.0)
        m, c, _ = mean_and_ci(data[t]["e2e_p999_us"])
        p999.append(m)
        p999_ci.append(c if c is not None else 0.0)

    def filter_series(xs, ys, cs):
        fx, fy, fc = [], [], []
        for xi, yi, ci in zip(xs, ys, cs):
            if yi is None:
                continue
            fx.append(xi)
            fy.append(yi)
            fc.append(ci)
        return fx, fy, fc

    x50, y50, c50 = filter_series(x, p50, p50_ci)
    x99, y99, c99 = filter_series(x, p99, p99_ci)
    x999, y999, c999 = filter_series(x, p999, p999_ci)

    plt.figure(figsize=(9, 5))
    if x50:
        plt.errorbar(x50, y50, yerr=c50, marker="o", capsize=3, label="p50")
    if x99:
        plt.errorbar(x99, y99, yerr=c99, marker="s", capsize=3, label="p99")
    if x999:
        plt.errorbar(x999, y999, yerr=c999, marker="^", capsize=3, label="p99.9")
    plt.xlabel("Target offered load (MB/s)")
    plt.ylabel("End-to-end latency (us)")
    plt.title("Latency Frontier with 95% CI")
    plt.grid(alpha=0.3)
    plt.legend()
    plt.tight_layout()
    plt.savefig(f"{prefix}.png", dpi=200)
    plt.savefig(f"{prefix}.pdf")


def main():
    parser = argparse.ArgumentParser(description="Aggregate latency sweep trials into CI summary and frontier plot.")
    parser.add_argument("--input", required=True, help="Directory containing trial_*/summary.csv")
    parser.add_argument("--output", required=True, help="Output CSV for CI summary")
    parser.add_argument("--plot-prefix", required=True, help="Output prefix for .png/.pdf frontier plots")
    args = parser.parse_args()

    data = load_trials(args.input)
    write_ci_summary(data, args.output)
    plot_frontier(data, args.plot_prefix)


if __name__ == "__main__":
    main()
