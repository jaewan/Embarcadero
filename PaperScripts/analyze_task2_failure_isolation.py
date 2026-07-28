#!/usr/bin/env python3
"""Validate the failure-through-apply session-isolation campaign.

Session 0 is fully striped and therefore uses the failed log server before the
fault. Session 1 is still striped, but only across the three surviving log
servers. The validator requires a verified fault, correct final apply state for
both sessions, and measurable control-session ACK progress after the fault.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path

import pandas as pd


def file_hash(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_kv(path: Path) -> dict[str, str]:
    out: dict[str, str] = {}
    for line in path.read_text().splitlines():
        # The fault injector records compact lines such as
        # "target_broker=2 target_port=1216 resolved_pid=123". Parse each
        # whitespace-delimited field rather than treating the full suffix as
        # one value. Free-form cmdline/listener fields are not consumed here.
        for field in line.split():
            if "=" in field:
                key, value = field.split("=", 1)
                out[key] = value
    return out


def max_no_progress_ms(df: pd.DataFrame, start_ms: float, end_ms: float) -> int:
    final_ack = df["Cum_Ack_Bytes"].max()
    completion_rows = df[
        (df["Timestamp(ms)"] >= start_ms) & (df["Cum_Ack_Bytes"] >= final_ack)
    ]
    if not completion_rows.empty:
        end_ms = min(end_ms, float(completion_rows["Timestamp(ms)"].iloc[0]))
    window = df[(df["Timestamp(ms)"] >= start_ms) & (df["Timestamp(ms)"] <= end_ms)]
    if len(window) < 2:
        return -1
    timestamps = window["Timestamp(ms)"].to_numpy()
    cumulative = window["Cum_Ack_Bytes"].to_numpy()
    longest = 0
    run_start = timestamps[0]
    previous = cumulative[0]
    for timestamp, current in zip(timestamps[1:], cumulative[1:]):
        if current > previous:
            longest = max(longest, int(timestamp - run_start))
            run_start = timestamp
        previous = current
    return max(longest, int(timestamps[-1] - run_start))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--post-fault-ms", type=int, default=5000)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--expected-published", type=int, default=30_010_000)
    args = parser.parse_args()

    result_path = args.campaign / "results.csv"
    result_rows = list(csv.DictReader(result_path.open()))
    by_trial: dict[int, list[dict[str, str]]] = {}
    for row in result_rows:
        by_trial.setdefault(int(row["trial"]), []).append(row)
    if sorted(by_trial) != list(range(1, args.trials + 1)):
        raise SystemExit(
            f"expected trials 1..{args.trials}, found {sorted(by_trial)}"
        )

    summaries: list[dict[str, object]] = []
    input_hashes = {str(result_path.relative_to(args.campaign)): file_hash(result_path)}
    provenance: dict[str, str] = {}
    for name in ("provenance.txt", "worktree_status.txt", "worktree.patch"):
        path = args.campaign / name
        if path.exists():
            input_hashes[name] = file_hash(path)
            if name == "provenance.txt":
                provenance = read_kv(path)
    # Normalize artifacts produced by the older one-line provenance format.
    if "commit" in provenance and "git_commit" not in provenance:
        provenance["git_commit"] = provenance.pop("commit")
    if "git_dirty" not in provenance:
        status_path = args.campaign / "worktree_status.txt"
        provenance["git_dirty"] = (
            "1" if status_path.exists() and status_path.read_text().strip() else "0"
        )
    provenance.pop("dirty", None)
    for trial, rows in sorted(by_trial.items()):
        if len(rows) != 2:
            raise SystemExit(f"trial {trial}: expected exactly two session result rows")
        trial_dir = args.campaign / f"trial{trial}"
        event_path = trial_dir / "failure_event.log"
        origin_path = trial_dir / "timeseries" / "origin_epoch_ms.txt"
        affected_path = trial_dir / "timeseries" / "session_0.csv"
        control_path = trial_dir / "timeseries" / "session_1.csv"
        for path in (event_path, origin_path, affected_path, control_path):
            if not path.exists():
                raise SystemExit(f"trial {trial}: missing {path}")
            input_hashes[str(path.relative_to(args.campaign))] = file_hash(path)

        event = read_kv(event_path)
        if event.get("kill_verified") != "1":
            raise SystemExit(f"trial {trial}: failure was not verified")
        origin_ms = int(origin_path.read_text().strip())
        # epoch_ms_before is sampled immediately before SIGKILL. The "after"
        # timestamp is recorded only after listener-disappearance validation
        # and can lag the actual fault by several seconds.
        kill_epoch_ms = int(event["epoch_ms_before"])
        kill_ms = kill_epoch_ms - origin_ms
        end_ms = kill_ms + args.post_fault_ms

        affected = pd.read_csv(affected_path)
        control = pd.read_csv(control_path)
        failed_broker = int(event["target_broker"])
        affected_failed_path = f"Broker_{failed_broker}_sent_GiBps"
        control_failed_path = f"Broker_{failed_broker}_sent_GiBps"
        pre_start = max(0, kill_ms - 2000)
        affected_pre = affected[
            (affected["Timestamp(ms)"] >= pre_start)
            & (affected["Timestamp(ms)"] < kill_ms)
        ]
        control_pre = control[
            (control["Timestamp(ms)"] >= pre_start)
            & (control["Timestamp(ms)"] < kill_ms)
        ]
        control_post = control[
            (control["Timestamp(ms)"] >= kill_ms)
            & (control["Timestamp(ms)"] <= end_ms)
        ]
        if affected_pre.empty or control_pre.empty or control_post.empty:
            raise SystemExit(f"trial {trial}: insufficient aligned samples around fault")
        if affected_pre[affected_failed_path].max() <= 0:
            raise SystemExit(f"trial {trial}: affected session did not use failed server")
        if control[control_failed_path].max() > 0:
            raise SystemExit(f"trial {trial}: control session used failed server")
        affected_final_sent = int(affected["Cum_Sent_Bytes"].max())
        control_final_sent = int(control["Cum_Sent_Bytes"].max())
        affected_sent_at_fault = int(affected_pre["Cum_Sent_Bytes"].iloc[-1])
        control_sent_at_fault = int(control_pre["Cum_Sent_Bytes"].iloc[-1])
        if affected_sent_at_fault >= 0.95 * affected_final_sent:
            raise SystemExit(f"trial {trial}: affected session was nearly finished at fault")
        if control_sent_at_fault >= 0.95 * control_final_sent:
            raise SystemExit(f"trial {trial}: control session was nearly finished at fault")
        if affected_sent_at_fault < 0.05 * affected_final_sent:
            raise SystemExit(f"trial {trial}: affected session was not active before fault")
        if control_sent_at_fault < 0.05 * control_final_sent:
            raise SystemExit(f"trial {trial}: control session was not active before fault")
        control_pre_1s = control[
            (control["Timestamp(ms)"] >= kill_ms - 1000)
            & (control["Timestamp(ms)"] < kill_ms)
        ]
        control_post_1s = control[
            (control["Timestamp(ms)"] >= kill_ms)
            & (control["Timestamp(ms)"] <= kill_ms + 1000)
        ]
        if len(control_pre_1s) < 2 or len(control_post_1s) < 2:
            raise SystemExit(f"trial {trial}: insufficient one-second control windows")
        control_pre_progress = int(
            control_pre_1s["Cum_Ack_Bytes"].iloc[-1]
            - control_pre_1s["Cum_Ack_Bytes"].iloc[0]
        )
        control_post_1s_progress = int(
            control_post_1s["Cum_Ack_Bytes"].iloc[-1]
            - control_post_1s["Cum_Ack_Bytes"].iloc[0]
        )
        if control_pre_progress <= 0:
            raise SystemExit(f"trial {trial}: control was not active before the fault")
        if control_post_1s_progress <= 0:
            raise SystemExit(f"trial {trial}: control made no progress in first post-fault second")
        control_progress = int(
            control_post["Cum_Ack_Bytes"].iloc[-1] - control_post["Cum_Ack_Bytes"].iloc[0]
        )
        if control_progress <= 0:
            raise SystemExit(f"trial {trial}: control session made no post-fault progress")

        ordered_rows = sorted(rows, key=lambda row: int(row["session"]))
        if [int(row["session"]) for row in ordered_rows] != [0, 1]:
            raise SystemExit(f"trial {trial}: expected sessions 0 and 1")
        for row in ordered_rows:
            if row["system"] != "EMBARCADERO":
                raise SystemExit(
                    f"trial {trial} session {row['session']}: unexpected system"
                )
            if row["kill_verified"] != "1" or row["valid"] != "1":
                raise SystemExit(f"trial {trial} session {row['session']}: invalid result")
            applied = int(row["applied"])
            published = int(row["published"])
            if applied != args.expected_published or published != args.expected_published:
                raise SystemExit(
                    f"trial {trial} session {row['session']}: "
                    f"applied/published={applied}/{published}, expected "
                    f"{args.expected_published}/{args.expected_published}"
                )
            for field in ("session_reorders", "key_reorders", "final_mismatch", "failed_checks"):
                value = row[field].strip().lower()
                numeric = 0 if value in {"", "none"} else int(value)
                if numeric != 0:
                    raise SystemExit(
                        f"trial {trial} session {row['session']}: {field}={row[field]}"
                    )

        summaries.append(
            {
                "trial": trial,
                "failed_broker": failed_broker,
                "kill_ms_from_trace_origin": kill_ms,
                "affected_pre_fault_failed_path_gibps": float(
                    affected_pre[affected_failed_path].max()
                ),
                "affected_fraction_sent_at_fault": (
                    affected_sent_at_fault / affected_final_sent
                ),
                "control_fraction_sent_at_fault": control_sent_at_fault / control_final_sent,
                "control_pre_fault_ack_bytes_1s": control_pre_progress,
                "control_post_fault_ack_bytes_1s": control_post_1s_progress,
                "control_post_fault_ack_bytes_5s": control_progress,
                "affected_max_no_ack_progress_ms_5s": max_no_progress_ms(
                    affected, kill_ms, end_ms
                ),
                "control_max_no_ack_progress_ms_5s": max_no_progress_ms(
                    control, kill_ms, end_ms
                ),
                "entries_per_session": args.expected_published,
                "affected_valid": int(ordered_rows[0]["valid"]),
                "control_valid": int(ordered_rows[1]["valid"]),
            }
        )

    summary_path = args.campaign / "failure_isolation_summary.csv"
    with summary_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summaries[0]))
        writer.writeheader()
        writer.writerows(summaries)
    manifest = {
        "schema": 1,
        "claim": (
            "A fully striped affected session preserves apply order through a "
            "verified server fault while a control session striped across the "
            "surviving servers continues to acknowledge work."
        ),
        "campaign": str(args.campaign),
        "validation_contract": {
            "trials": args.trials,
            "sessions_per_trial": 2,
            "expected_entries_per_session": args.expected_published,
            "post_fault_window_ms": args.post_fault_ms,
        },
        "provenance": provenance,
        "inputs_sha256": input_hashes,
        "trials": summaries,
    }
    manifest_path = args.campaign / "failure_isolation_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
