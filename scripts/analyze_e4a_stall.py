#!/usr/bin/env python3
"""E4a analysis: per-session ACK stall around a broker kill.

Inputs (a directory, e.g. data/failure_suite/<tag>/e4a):
  trial<N>_<session>_timeseries.csv  — per-session (per client process) throughput
      timeseries written by throughput_test: Timestamp(ms), Broker_*_sent/ack,
      Sent_GiBps, Ack_GiBps, Total_GBps, Cum_Sent_Bytes, Cum_Ack_Bytes.
      Timestamps are relative to the synchronized barrier
      (EMBARCADERO_THROUGHPUT_TIMESERIES_ORIGIN_MS), the same axis as the kill record.
  trial<N>_broker_kill.csv — written by run_multiclient.sh kill injection:
      trial,attempt,broker_id,pid,signal,kill_wall_ms,kill_rel_ms,kill_rc

Publication failure metrics (run separately when needed):
  --layer sent  — client remux/send stall (Sent_GiBps)
  --layer ack   — ACK stall for the configured ack_level (Ack_GiBps)
      Use ACK_LEVEL=1 runs for ordered ACK1 stall and ACK_LEVEL=2 RF>=2 runs
      for durable ACK2 stall; do not mix layers within one CDF.

Per session we report:
  baseline_gbps  — median rate in [WARMUP_MS, kill_rel) (pre-kill steady state)
  stall_ms       — time from kill to first post-kill sample with
                   rate >= RECOVERY_FRAC * baseline (sustained for 2 samples)
  dip_frac       — min post-kill rate / baseline (how deep the dip went)
  killed_broker_residual — post-recovery mean of the killed broker's column
                   (should be ~0: traffic re-routed, not resumed to the dead broker)

The headline output is the stall CDF across sessions x trials.
"""

import argparse
import csv
import glob
import os
import re
import statistics
import sys

WARMUP_MS = 500          # ignore ramp-up when computing pre-kill baseline
BASELINE_WINDOW_MS = 2000  # prefer the last 2s before kill (skips idle zeros before push-go)
RECOVERY_FRAC = 0.5      # session considered recovered at >=50% of baseline
SUSTAIN_SAMPLES = 2      # recovery must hold for this many consecutive samples


def read_timeseries(path):
    rows = []
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader, None)
        if header is None:
            return None, []
        for r in reader:
            if not r or not r[0].strip():
                continue
            try:
                rows.append([float(x) for x in r])
            except ValueError:
                continue
    return header, rows


def read_kill_record(path):
    with open(path) as f:
        reader = csv.DictReader(f)
        for row in reader:
            return row
    return None


def analyze_session(ts_path, kill_rel_ms, killed_broker_id, layer="ack"):
    header, rows = read_timeseries(ts_path)
    if not rows:
        return {"error": "empty_timeseries"}

    # Prefer multi-layer columns when present; fall back to Total_GBps.
    rate_names = {
        "ack": ("Ack_GiBps", "Total_GBps"),
        "sent": ("Sent_GiBps", "Total_GBps"),
        "total": ("Total_GBps",),
    }
    names = rate_names.get(layer, rate_names["ack"])
    total_idx = None
    for name in names:
        for i, col in enumerate(header):
            if col.strip() == name:
                total_idx = i
                break
        if total_idx is not None:
            break
    if total_idx is None:
        total_idx = len(header) - 1

    kb_idx = None
    for i, col in enumerate(header):
        if re.fullmatch(rf"\s*Broker_{killed_broker_id}(_sent)?_Gi?Bps\s*", col):
            kb_idx = i
            break
        if re.fullmatch(rf"\s*Broker_{killed_broker_id}_GBps\s*", col):
            kb_idx = i
            break

    pre_lo = max(WARMUP_MS, kill_rel_ms - BASELINE_WINDOW_MS)
    pre = [r[total_idx] for r in rows if pre_lo <= r[0] < kill_rel_ms]
    # If the short window is empty (very early kill), fall back to all post-warmup pre-kill.
    if not pre:
        pre = [r[total_idx] for r in rows if WARMUP_MS <= r[0] < kill_rel_ms]
    post = [(r[0], r[total_idx]) for r in rows if r[0] >= kill_rel_ms]
    if not pre:
        return {"error": "no_pre_kill_samples"}
    if not post:
        return {"error": "no_post_kill_samples (client finished before kill?)"}

    baseline = statistics.median(pre)
    if baseline <= 0:
        # Push-go cells can still have sparse zeros in the window; use nonzero median.
        nonzero = [v for v in pre if v > 0]
        if nonzero:
            baseline = statistics.median(nonzero)
        else:
            return {"error": "zero_baseline"}

    threshold = RECOVERY_FRAC * baseline
    stall_ms = None
    for i in range(len(post)):
        window = post[i:i + SUSTAIN_SAMPLES]
        if len(window) == SUSTAIN_SAMPLES and all(v >= threshold for _, v in window):
            stall_ms = post[i][0] - kill_rel_ms
            break
    recovered = stall_ms is not None

    dip_frac = min(v for _, v in post) / baseline

    residual = None
    if kb_idx is not None and recovered:
        tail = [r[kb_idx] for r in rows if r[0] >= kill_rel_ms + stall_ms]
        if tail:
            residual = statistics.mean(tail)

    return {
        "baseline_gbps": round(baseline, 4),
        "stall_ms": round(stall_ms, 1) if recovered else None,
        "recovered": recovered,
        "dip_frac": round(dip_frac, 3),
        "killed_broker_residual_gbps": round(residual, 5) if residual is not None else None,
        "post_kill_mean_gbps": round(statistics.mean(v for _, v in post), 4),
    }


def percentile(sorted_vals, p):
    if not sorted_vals:
        return None
    k = (len(sorted_vals) - 1) * p
    lo, hi = int(k), min(int(k) + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("run_dir", help="directory with trial*_timeseries.csv + trial*_broker_kill.csv")
    ap.add_argument("--output", default=None, help="per-session summary CSV path")
    ap.add_argument("--layer", default="both", choices=("ack", "sent", "total", "both"),
                    help="which throughput layer to analyze (default: both ack+sent)")
    args = ap.parse_args()

    kill_files = sorted(glob.glob(os.path.join(args.run_dir, "trial*_broker_kill.csv")))
    if not kill_files:
        sys.exit(f"No trial*_broker_kill.csv in {args.run_dir} — was the kill armed?")

    layers = ["ack", "sent"] if args.layer == "both" else [args.layer]
    results = []
    for kf in kill_files:
        m = re.search(r"trial(\d+)_broker_kill\.csv$", kf)
        trial = m.group(1)
        kill = read_kill_record(kf)
        if kill is None or kill.get("kill_rc", "1") not in ("0", ""):
            print(f"WARNING: trial {trial}: kill record missing/failed ({kf}), skipping", file=sys.stderr)
            continue
        kill_rel_ms = float(kill["kill_rel_ms"])
        killed_id = int(kill["broker_id"])

        ts_files = sorted(glob.glob(os.path.join(args.run_dir, f"trial{trial}_*_timeseries.csv")))
        if not ts_files:
            print(f"WARNING: trial {trial}: no timeseries files", file=sys.stderr)
            continue
        for tsf in ts_files:
            tag = re.search(rf"trial{trial}_(.+)_timeseries\.csv$", tsf).group(1)
            for layer in layers:
                r = analyze_session(tsf, kill_rel_ms, killed_id, layer=layer)
                r.update({"trial": trial, "session": tag, "layer": layer,
                          "kill_rel_ms": kill_rel_ms, "killed_broker": killed_id})
                results.append(r)

    if not results:
        sys.exit("No sessions analyzed.")

    fields = ["trial", "session", "layer", "killed_broker", "kill_rel_ms", "baseline_gbps",
              "stall_ms", "recovered", "dip_frac", "killed_broker_residual_gbps",
              "post_kill_mean_gbps", "error"]
    out = args.output or os.path.join(args.run_dir, "stall_summary.csv")
    with open(out, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=fields)
        w.writeheader()
        for r in results:
            w.writerow({k: r.get(k, "") for k in fields})

    for layer in layers:
        ok = [r for r in results if r.get("layer") == layer and "error" not in r]
        errs = [r for r in results if r.get("layer") == layer and "error" in r]
        unrecovered = [r for r in ok if not r["recovered"]]
        stalls = sorted(r["stall_ms"] for r in ok if r["recovered"])

        print(f"\n=== E4a {layer}-layer stall CDF ({len(stalls)} recovered sessions, "
              f"{len(unrecovered)} unrecovered, {len(errs)} errors) ===")
        if stalls:
            for label, p in [("min", 0), ("p25", .25), ("p50", .5), ("p75", .75),
                             ("p90", .9), ("p99", .99), ("max", 1)]:
                print(f"  {label:>4}: {percentile(stalls, p):8.1f} ms")
        for r in unrecovered:
            print(f"  UNRECOVERED[{layer}]: trial {r['trial']} session {r['session']} "
                  f"(dip_frac={r['dip_frac']})")
        for r in errs:
            print(f"  ERROR[{layer}]: trial {r['trial']} session {r['session']}: {r['error']}")

    print(f"\nPer-session detail: {out}")
    ok_all = [r for r in results if "error" not in r]
    errs_all = [r for r in results if "error" in r]
    unrecovered_all = [r for r in ok_all if not r["recovered"]]
    stalls_all = [r for r in ok_all if r["recovered"]]
    if errs_all or unrecovered_all or not stalls_all:
        sys.exit(1)


if __name__ == "__main__":
    main()
