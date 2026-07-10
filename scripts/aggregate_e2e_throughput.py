#!/usr/bin/env python3
import argparse
import csv
import os
import pathlib
import sys


def maybe_float(value):
    if value is None:
        return None
    text = str(value).strip()
    if not text:
        return None
    try:
        return float(text)
    except ValueError:
        return None


def mean(values):
    vals = [v for v in values if v is not None]
    if not vals:
        return None
    return sum(vals) / len(vals)


def load_rows(run_dir: pathlib.Path):
    rows = []
    for summary_path in sorted(run_dir.glob("trial_*/throughput_benchmark_summary.csv")):
        trial_dir = summary_path.parent
        trial_label = trial_dir.name.replace("trial_", "")
        with summary_path.open(newline="") as handle:
            reader = csv.DictReader(handle)
            for row in reader:
                row["trial"] = trial_label
                row["artifact_dir"] = str(trial_dir)
                rows.append(row)
    return rows


def write_trial_rows(rows, output_path: pathlib.Path):
    fields = [
        "trial",
        "message_size_bytes",
        "total_message_size_bytes",
        "message_count",
        "num_threads_per_broker",
        "ack_level",
        "order",
        "replication_factor",
        "sequencer",
        "publish_goodput_mbps",
        "e2e_goodput_mbps",
        "artifact_dir",
    ]
    with output_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for row in rows:
            writer.writerow({field: row.get(field, "") for field in fields})


def write_summary(rows, output_path: pathlib.Path, warmup_excluded: int = 0):
    fields = [
        "trials",
        "warmup_trials_excluded",
        "publish_goodput_mbps_mean",
        "e2e_goodput_mbps_mean",
        "message_size_bytes",
        "total_message_size_bytes",
        "num_threads_per_broker",
        "ack_level",
        "order",
        "replication_factor",
        "sequencer",
    ]
    first = rows[0]
    out = {
        "trials": len(rows),
        "warmup_trials_excluded": warmup_excluded,
        "message_size_bytes": first.get("message_size_bytes", ""),
        "total_message_size_bytes": first.get("total_message_size_bytes", ""),
        "num_threads_per_broker": first.get("num_threads_per_broker", ""),
        "ack_level": first.get("ack_level", ""),
        "order": first.get("order", ""),
        "replication_factor": first.get("replication_factor", ""),
        "sequencer": first.get("sequencer", ""),
    }
    pub_mean = mean([maybe_float(r.get("publish_goodput_mbps")) for r in rows])
    e2e_mean = mean([maybe_float(r.get("e2e_goodput_mbps")) for r in rows])
    out["publish_goodput_mbps_mean"] = "" if pub_mean is None else f"{pub_mean:.6f}"
    out["e2e_goodput_mbps_mean"] = "" if e2e_mean is None else f"{e2e_mean:.6f}"

    with output_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerow(out)


def main():
    parser = argparse.ArgumentParser(description="Aggregate end-to-end throughput benchmark trials.")
    parser.add_argument("--input-run-dir", required=True)
    parser.add_argument("--trial-output", required=True)
    parser.add_argument("--summary-output", required=True)
    # [[EVAL WARM-UP]] Exclude the first N (cold) trials from the mean. On this testbed the first
    # trial after a fresh cluster start is deterministically slow (verified: it is the slowest even
    # on an idle host, e.g. SCALOG RF=1 publish 2979 -> 3611 -> 3588 MB/s), which — run all-RF1-
    # then-all-RF2 — spuriously inflated later configs and made RF=2 look faster than RF=1. Dropping
    # the cold trial(s) removes that deterministic artifact; raw trials are still written to
    # --trial-output. Default 1; override with --warmup-trials or the WARMUP_TRIALS env var. See
    # docs/experiments/rf_throughput_warmup_artifact.md and docs/baselines/fairness_appendix.md.
    parser.add_argument("--warmup-trials", type=int,
                        default=int(os.environ.get("WARMUP_TRIALS", "1")))
    args = parser.parse_args()

    rows = load_rows(pathlib.Path(args.input_run_dir))
    if not rows:
        raise SystemExit(f"No throughput_benchmark_summary.csv files found under {args.input_run_dir}")
    # Sort by numeric trial index so the "first" trial is truly the first-run cold one.
    def _trial_key(r):
        try:
            return int(str(r.get("trial", "0")))
        except ValueError:
            return 0
    rows.sort(key=_trial_key)
    write_trial_rows(rows, pathlib.Path(args.trial_output))  # raw: ALL trials preserved
    warmup = max(0, args.warmup_trials)
    measured = rows[warmup:] if len(rows) > warmup else rows
    excluded = len(rows) - len(measured)
    if excluded:
        sys.stderr.write(
            f"[aggregate_e2e_throughput] excluding {excluded} warm-up trial(s) from the mean "
            f"({len(measured)} measured of {len(rows)} total)\n")
    write_summary(measured, pathlib.Path(args.summary_output), warmup_excluded=excluded)


if __name__ == "__main__":
    main()
