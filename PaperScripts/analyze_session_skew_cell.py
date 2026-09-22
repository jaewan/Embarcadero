#!/usr/bin/env python3
"""Fail-closed validation and summarization for one hot-ingress cell."""

from __future__ import annotations

import argparse
import csv
import json
import re
from pathlib import Path


ROUTING_RE = re.compile(r"\[(?:ORDER5_ROUTING|PUBLISH_ROUTING)\].*$")
BROKER_MSG_RE = re.compile(r"broker(\d+)_msgs=(\d+)")
TOTAL_BYTES_RE = re.compile(r"Total message size:\s*(\d+) bytes")
THREADS_RE = re.compile(r"Using threads_per_broker from command line:\s*(\d+)")
PACE_RE = re.compile(r"PublishThroughput pacing:.*\bquantum_bytes=(\d+)")
SLOT_RE = re.compile(r"QueueBuffer::AddBuffers.*\bslot_size=(\d+)")
RETRANSMIT_RE = re.compile(r"\bretransmit_attempts=(\d+)\b")
CLIENT_FAILURE_RE = re.compile(
    r"Header Send Fail|\[SESSION_FENCED(?:_OBSERVED)?\]|\[SESSION_REOPEN[^]]*\]|"
    r"PUBLISH_JOIN_TIMEOUT|"
    r"Publisher ACK (?:Timeout|Failure)",
    re.IGNORECASE,
)
BROKER_DUPLICATE_RE = re.compile(r"dropped duplicate ingest|ingest dedup drain hits", re.IGNORECASE)


def fail(message: str) -> None:
    raise SystemExit(f"ERROR: {message}")


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--plan", required=True, type=Path)
    ap.add_argument("--log-dir", required=True, type=Path)
    ap.add_argument("--out", required=True, type=Path)
    args = ap.parse_args()

    plan = json.loads(args.plan.read_text())
    attempts_path = args.log_dir / "attempt_summary.csv"
    overlap_path = args.log_dir / "overlap_summary.csv"
    if not attempts_path.is_file() or not overlap_path.is_file():
        fail("missing attempt_summary.csv or overlap_summary.csv")

    successes: dict[int, int] = {}
    with attempts_path.open(newline="") as f:
        for row in csv.DictReader(f):
            if row["result"] == "success":
                trial = int(row["trial"])
                if trial in successes:
                    fail(f"trial {trial} has more than one successful attempt")
                successes[trial] = int(row["attempt"])
    expected_trials = int(plan["trials"])
    if set(successes) != set(range(1, expected_trials + 1)):
        fail(f"successful trials {sorted(successes)} != expected 1..{expected_trials}")

    overlaps: dict[int, tuple[float, float, int]] = {}
    with overlap_path.open(newline="") as f:
        for raw in csv.reader(f):
            if not raw or raw[0] == "trial":
                continue
            if len(raw) < 4:
                fail(f"malformed overlap row: {raw}")
            overlaps[int(raw[0])] = (float(raw[1]), float(raw[2]), int(raw[3]))

    rows = []
    for trial in range(1, expected_trials + 1):
        if trial not in overlaps:
            fail(f"trial {trial} lacks overlap result")
        overlap_gibps, overlap_ms, ts_clients = overlaps[trial]
        # Timeseries samples are quantized; tolerate one 100 ms bucket at each
        # edge, but require the common window to cover the registered transfer.
        min_overlap_ms = max(500, plan["duration_sec"] * 1000 - 200)
        if overlap_gibps <= 0 or overlap_ms < min_overlap_ms:
            fail(f"trial {trial} has invalid/short overlap: {overlap_gibps} GiB/s, {overlap_ms} ms")
        if ts_clients != plan["sessions"]:
            fail(f"trial {trial} overlap covers {ts_clients}, expected {plan['sessions']} clients")

        broker_messages = [0] * plan["brokers"]
        sent_total = 0
        ack_total = 0
        for session in plan["session_plan"]:
            tag = session["client_tag"]
            log_path = args.log_dir / f"trial{trial}_{tag}.log"
            ts_path = args.log_dir / f"trial{trial}_{tag}_timeseries.csv"
            if not log_path.is_file() or not ts_path.is_file():
                fail(f"trial {trial} session {session['id']} lacks log/timeseries")
            text = log_path.read_text(errors="replace")
            totals = [int(x) for x in TOTAL_BYTES_RE.findall(text)]
            if not totals or totals[-1] != session["total_bytes"]:
                fail(f"trial {trial} session {session['id']} byte contract mismatch: {totals[-1:]}")
            applied_threads = [int(x) for x in THREADS_RE.findall(text)]
            if applied_threads != [plan["threads_per_broker"]]:
                fail(
                    f"trial {trial} session {session['id']} applied threads {applied_threads} "
                    f"!= planned {plan['threads_per_broker']}"
                )
            applied_quantum = [int(x) for x in PACE_RE.findall(text)]
            if applied_quantum != [plan["pace_quantum_bytes"]]:
                fail(
                    f"trial {trial} session {session['id']} pacing quantum {applied_quantum} "
                    f"!= planned {plan['pace_quantum_bytes']}"
                )
            applied_slots = [int(x) for x in SLOT_RE.findall(text)]
            if applied_slots != [plan["queue_slot_size_bytes"]]:
                fail(
                    f"trial {trial} session {session['id']} queue slot size {applied_slots} "
                    f"!= planned {plan['queue_slot_size_bytes']}"
                )
            routing_lines = [line for line in text.splitlines() if ROUTING_RE.search(line)]
            if not routing_lines:
                fail(f"trial {trial} session {session['id']} lacks routing marker")
            if CLIENT_FAILURE_RE.search(text):
                fail(f"trial {trial} session {session['id']} contains reconnect/fence/ACK failure evidence")
            retransmits = RETRANSMIT_RE.findall(routing_lines[-1])
            if len(retransmits) != 1 or int(retransmits[0]) != 0:
                fail(
                    f"trial {trial} session {session['id']} has contaminated retransmit count "
                    f"{retransmits or ['missing']}"
                )
            counts = {int(b): int(v) for b, v in BROKER_MSG_RE.findall(routing_lines[-1]) if int(v) > 0}
            expected = set(range(plan["brokers"])) if plan["placement"] == "striped" else {session["broker"]}
            if set(counts) != expected:
                fail(f"trial {trial} session {session['id']} routed to {sorted(counts)}, expected {sorted(expected)}")
            if sum(counts.values()) * plan["message_size"] != session["total_bytes"]:
                fail(f"trial {trial} session {session['id']} routing counts do not cover exact bytes")
            for broker, count in counts.items():
                broker_messages[broker] += count

            with ts_path.open(newline="") as f:
                samples = list(csv.DictReader(f))
            if not samples:
                fail(f"trial {trial} session {session['id']} has empty timeseries")
            final_sent = max(int(float(r["Cum_Sent_Bytes"])) for r in samples)
            final_ack = max(int(float(r["Cum_Ack_Bytes"])) for r in samples)
            if final_sent != session["total_bytes"] or final_ack != session["total_bytes"]:
                fail(f"trial {trial} session {session['id']} incomplete sent/ack: {final_sent}/{final_ack}")
            sent_total += final_sent
            ack_total += final_ack

        expected_total = sum(s["total_bytes"] for s in plan["session_plan"])
        if sent_total != expected_total or ack_total != expected_total:
            fail(f"trial {trial} aggregate bytes incomplete")
        broker_bytes = [m * plan["message_size"] for m in broker_messages]
        broker_shares = [b / expected_total for b in broker_bytes]
        for log_path in args.log_dir.glob("*"):
            if not log_path.is_file():
                continue
            log_text = log_path.read_text(errors="replace")
            if re.search(
                r"Increase the Segment Size|segment[^\n]*(?:exhaust|capacity|full)",
                log_text, re.IGNORECASE):
                fail(f"trial {trial} contains segment-capacity warning in {log_path.name}")
            if log_path.name.startswith(f"trial{trial}_attempt") and BROKER_DUPLICATE_RE.search(log_text):
                fail(f"trial {trial} contains duplicate-ingest evidence in {log_path.name}")
        offered_gibps = plan["aggregate_target_mibps"] / 1024.0
        rows.append({
            "cell": plan["cell"], "mode": plan["mode"], "placement": plan["placement"],
            "sessions": plan["sessions"], "theta": plan["theta"], "trial": trial,
            "offered_mibps": plan["aggregate_target_mibps"],
            "overlap_gibps": overlap_gibps, "overlap_ms": overlap_ms,
            "acked_to_offered_ratio": overlap_gibps / offered_gibps,
            "total_bytes": expected_total,
            "assigned_hottest_broker_share": max(broker_shares),
            "broker_assigned_byte_shares": "|".join(f"{x:.6f}" for x in broker_shares),
            "verdict": "pass",
        })

    args.out.parent.mkdir(parents=True, exist_ok=True)
    with args.out.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


if __name__ == "__main__":
    main()
