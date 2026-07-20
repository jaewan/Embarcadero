#!/usr/bin/env python3
"""Validate that one ORDER=5 session gap does not stop another session's commits."""

import argparse
import csv
import glob
import os
import re
import sys


def client_id_and_gap(path, require_gap, configured_gap_ms):
    client_id = None
    start = end = None
    with open(path) as f:
        for line in f:
            match = re.search(r"\bclient_id=(\d+)", line)
            if match and client_id is None:
                client_id = int(match.group(1))
            if "[ORDER5_GAP_INJECT]" in line:
                wall = re.search(r"\bwall_ms=(\d+)", line)
                if "phase=start" in line and wall:
                    start = int(wall.group(1))
                elif "phase=end" in line and start is not None:
                    # New artifacts carry the observed end time. Retain a
                    # compatibility fallback for the original paper campaign.
                    end = (int(wall.group(1)) if wall
                           else start + configured_gap_ms)
    if client_id is None or (require_gap and (start is None or end is None)):
        raise ValueError(f"incomplete client/gap evidence in {path}")
    return client_id, start, end


def commits(path):
    rows = []
    pattern = re.compile(
        r"\[ORDER5_TEST_GOI_COMMIT\].*client=(\d+).*batch_seq=(\d+).*wall_ms=(\d+)")
    with open(path) as f:
        for line in f:
            match = pattern.search(line)
            if match:
                rows.append(tuple(int(x) for x in match.groups()))
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir")
    parser.add_argument("--affected-session", default="c40")
    parser.add_argument("--control-session", default="c41")
    parser.add_argument("--gap-batch-seq", type=int, default=64)
    parser.add_argument("--gap-duration-ms", type=int, default=3000)
    parser.add_argument("--max-control-commit-gap-ms", type=int, default=100)
    args = parser.parse_args()

    output = []
    errors = []
    affected_logs = sorted(glob.glob(os.path.join(
        args.run_dir, f"trial*_{args.affected_session}.log")))
    for affected_log in affected_logs:
        match = re.search(r"trial(\d+)_", os.path.basename(affected_log))
        trial = match.group(1)
        control_log = os.path.join(
            args.run_dir, f"trial{trial}_{args.control_session}.log")
        broker_log = os.path.join(
            args.run_dir, f"trial{trial}_attempt1_broker0.log")
        try:
            affected_id, start, end = client_id_and_gap(
                affected_log, True, args.gap_duration_ms)
            control_id, _, _ = client_id_and_gap(
                control_log, False, args.gap_duration_ms)
            trace = commits(broker_log)
        except (OSError, ValueError) as exc:
            errors.append(f"trial {trial}: {exc}")
            continue

        affected = sorted((wall, seq) for cid, seq, wall in trace
                          if cid == affected_id)
        control = sorted((wall, seq) for cid, seq, wall in trace
                         if cid == control_id)
        affected_during = [(wall, seq) for wall, seq in affected
                           if start <= wall < end]
        affected_post = [(wall, seq) for wall, seq in affected if wall >= end]
        control_mid = [(wall, seq) for wall, seq in control
                       if start + 100 <= wall < end - 100]
        control_gaps = [b[0] - a[0] for a, b in zip(control_mid, control_mid[1:])]
        max_control_gap = max(control_gaps, default=None)
        leaked = [seq for _, seq in affected_during
                  if seq >= args.gap_batch_seq]
        first_post = affected_post[0][1] if affected_post else None

        passed = (not leaked and first_post == args.gap_batch_seq and
                  len(control_mid) >= 100 and max_control_gap is not None and
                  max_control_gap <= args.max_control_commit_gap_ms)
        if not passed:
            errors.append(
                f"trial {trial}: leaked={leaked[:3]} first_post={first_post} "
                f"control_commits={len(control_mid)} max_gap={max_control_gap}")
        output.append({
            "trial": trial,
            "affected_client": affected_id,
            "control_client": control_id,
            "gap_batch_seq": args.gap_batch_seq,
            "gap_ms": end - start,
            "affected_commits_at_or_after_gap_seq_during_gap": len(leaked),
            "affected_first_post_gap_seq": first_post,
            "control_commits_during_gap": len(control_mid),
            "control_max_intercommit_ms": max_control_gap,
            "valid": int(passed),
        })

    if not output:
        errors.append("no trials analyzed")
    out_path = os.path.join(args.run_dir, "commit_isolation_summary.csv")
    fields = list(output[0].keys()) if output else [
        "trial", "valid"]
    with open(out_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields)
        writer.writeheader()
        writer.writerows(output)

    if errors:
        print("E4a commit-isolation contract: FAIL")
        for error in errors:
            print(f"  {error}")
        sys.exit(1)
    print("E4a commit-isolation contract: PASS")
    for row in output:
        print(f"  trial {row['trial']}: control commits={row['control_commits_during_gap']}, "
              f"max inter-commit={row['control_max_intercommit_ms']} ms; "
              f"affected resumes at seq={row['affected_first_post_gap_seq']}")


if __name__ == "__main__":
    main()
