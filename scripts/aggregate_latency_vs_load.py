#!/usr/bin/env python3
import argparse
import csv
import os
import pathlib
from collections import defaultdict


def load_key_value_file(path: pathlib.Path) -> dict:
    data = {}
    if not path.exists():
        return data
    for line in path.read_text().splitlines():
        if "=" not in line:
            continue
        key, value = line.split("=", 1)
        data[key.strip()] = value.strip()
    return data


def maybe_float(value):
    if value is None:
        return None
    text = str(value).strip()
    if text == "":
        return None
    try:
        return float(text)
    except ValueError:
        return None


def maybe_int(value):
    if value is None:
        return None
    text = str(value).strip()
    if text == "":
        return None
    try:
        return int(text)
    except ValueError:
        return None


def mean(values):
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    return sum(vals) / len(vals)


def find_trial_rows(run_dir: pathlib.Path):
    rows = []
    for summary_path in sorted(run_dir.rglob("latency_benchmark_summary.csv")):
        trial_dir = summary_path.parent
        metadata = load_key_value_file(trial_dir / "run_metadata.txt")
        with summary_path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                merged = dict(row)
                merged.update(metadata)
                merged["artifact_dir"] = str(trial_dir)
                rows.append(merged)
    return rows


def write_trial_rows(rows, output_path: pathlib.Path):
    fields = [
        "target_mbps",
        "achieved_offered_load_mbps",
        "achieved_publish_goodput_mbps",
        "achieved_e2e_goodput_mbps",
        "publish_to_deliver_p50_us",
        "publish_to_deliver_p95_us",
        "publish_to_deliver_p99_us",
        "pub_submit_p50_us",
        "pub_submit_p95_us",
        "pub_submit_p99_us",
        "pub_ack_p50_us",
        "pub_ack_p95_us",
        "pub_ack_p99_us",
        "pub_ordered_p50_us",
        "pub_ordered_p95_us",
        "pub_ordered_p99_us",
        "trial",
        "mode",
        "sequencer",
        "order",
        "ack_level",
        "message_size_bytes",
        "total_message_size_bytes",
        "message_count",
        "offered_wire_bytes",
        "offered_payload_bytes",
        "order5_export_overruns",
        "order5_export_skipped_batches",
        "artifact_dir",
    ]
    with output_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def _trial_index(row):
    try:
        return int(str(row.get("trial", "0")))
    except (TypeError, ValueError):
        return 0


def write_summary(rows, output_path: pathlib.Path, warmup_trials: int = 0):
    grouped = defaultdict(list)
    for row in rows:
        grouped[row.get("target_mbps", "")].append(row)

    fields = [
        "target_mbps",
        "trials",
        "warmup_trials_excluded",
        "achieved_offered_load_mbps_mean",
        "achieved_publish_goodput_mbps_mean",
        "achieved_e2e_goodput_mbps_mean",
        "publish_to_deliver_p50_us_mean",
        "publish_to_deliver_p95_us_mean",
        "publish_to_deliver_p99_us_mean",
        "pub_submit_p50_us_mean",
        "pub_submit_p95_us_mean",
        "pub_submit_p99_us_mean",
        "pub_ack_p50_us_mean",
        "pub_ack_p95_us_mean",
        "pub_ack_p99_us_mean",
        "pub_ordered_p50_us_mean",
        "pub_ordered_p95_us_mean",
        "pub_ordered_p99_us_mean",
    ]

    metric_fields = [
        "achieved_offered_load_mbps",
        "achieved_publish_goodput_mbps",
        "achieved_e2e_goodput_mbps",
        "publish_to_deliver_p50_us",
        "publish_to_deliver_p95_us",
        "publish_to_deliver_p99_us",
        "pub_submit_p50_us",
        "pub_submit_p95_us",
        "pub_submit_p99_us",
        "pub_ack_p50_us",
        "pub_ack_p95_us",
        "pub_ack_p99_us",
        "pub_ordered_p50_us",
        "pub_ordered_p95_us",
        "pub_ordered_p99_us",
    ]

    with output_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for target in sorted(grouped.keys(), key=lambda v: maybe_float(v) if maybe_float(v) is not None else float("inf")):
            trial_rows = grouped[target]
            # [[EVAL WARM-UP]] Per load point, drop the first N (cold) trials before averaging the
            # percentiles/goodput — the first trial after a fresh cluster start is deterministically
            # slow on this testbed (see docs/experiments/rf_throughput_warmup_artifact.md). Raw trials
            # are still written to --trial-output. Fall back to all trials if a point has too few.
            ordered = sorted(trial_rows, key=_trial_index)
            wu = max(0, warmup_trials)
            measured = ordered[wu:] if len(ordered) > wu else ordered
            out = {
                "target_mbps": target,
                "trials": len(trial_rows),
                "warmup_trials_excluded": len(ordered) - len(measured),
            }
            for field in metric_fields:
                out[f"{field}_mean"] = ""
                value = mean([maybe_float(row.get(field)) for row in measured])
                if value is not None:
                    out[f"{field}_mean"] = f"{value:.6f}"
            writer.writerow(out)


def main():
    parser = argparse.ArgumentParser(description="Aggregate latency-vs-load benchmark trials.")
    parser.add_argument("--input-run-dir", required=True, help="Run directory produced by scripts/run_latency_vs_load.sh")
    parser.add_argument("--trial-output", required=True, help="Output CSV with one row per trial")
    parser.add_argument("--summary-output", required=True, help="Output CSV with one row per offered-load point")
    # [[EVAL WARM-UP]] Exclude the first N cold trials PER LOAD POINT from the mean (raw preserved).
    # Mirrors aggregate_e2e_throughput.py; see docs/baselines/fairness_appendix.md. Default 1;
    # override via --warmup-trials or the WARMUP_TRIALS env var.
    parser.add_argument("--warmup-trials", type=int,
                        default=int(os.environ.get("WARMUP_TRIALS", "1")))
    args = parser.parse_args()

    run_dir = pathlib.Path(args.input_run_dir)
    rows = find_trial_rows(run_dir)
    if not rows:
        raise SystemExit(f"No latency_benchmark_summary.csv files found under {run_dir}")

    write_trial_rows(rows, pathlib.Path(args.trial_output))  # raw: ALL trials preserved
    write_summary(rows, pathlib.Path(args.summary_output), warmup_trials=max(0, args.warmup_trials))


if __name__ == "__main__":
    main()
