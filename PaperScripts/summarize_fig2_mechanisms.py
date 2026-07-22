#!/usr/bin/env python3
"""Validate the clean Fig. 2 mechanism and epoch-sensitivity campaign."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import statistics
from pathlib import Path


MECHANISM_CELLS = [
    "fig2_mech_embar_o0_ack1_rf0",
    "fig2_mech_embar_o5_ack1_rf0",
    "fig2_mech_embar_o5_ack2_rf2_mem",
    "fig2_mech_embar_o5_ack2_rf2_disk",
]
EPOCHS_US = [250, 500, 1000, 2000]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--trials", type=int, default=3)
    parser.add_argument("--load-mbps", type=int, default=250)
    parser.add_argument("--manifest-out", type=Path)
    args = parser.parse_args()

    contract_path = args.campaign / "campaign_contract.md"
    results_path = args.campaign / "results.csv"
    contract = contract_path.read_text()
    if not re.search(r"^- Dirty: no\s*$", contract, re.MULTILINE):
        raise SystemExit("validation failed: campaign contract is not clean")
    all_rows = list(csv.DictReader(results_path.open()))
    wanted = set(MECHANISM_CELLS) | {f"fig2_epoch_tau{tau}" for tau in EPOCHS_US}
    rows = [row for row in all_rows if row["cell"] in wanted]
    if any(row["status"] != "ok" for row in rows):
        raise SystemExit("validation failed: a selected row failed")
    commits = {row["git_commit"] for row in rows}
    if len(commits) != 1:
        raise SystemExit("validation failed: selected rows mix commits")

    summary: list[dict[str, object]] = []
    for cell in sorted(wanted):
        group = [row for row in rows if row["cell"] == cell]
        if len(group) != args.trials:
            raise SystemExit(
                f"validation failed: {cell} has {len(group)} trials, "
                f"expected {args.trials}"
            )
        if {int(float(row["target_mbps"])) for row in group} != {args.load_mbps}:
            raise SystemExit(f"validation failed: {cell} uses the wrong load")
        expected_epoch = (
            int(cell.rsplit("tau", 1)[1]) if "fig2_epoch_tau" in cell else 500
        )
        if {int(row["epoch_us"]) for row in group} != {expected_epoch}:
            raise SystemExit(f"validation failed: {cell} uses the wrong epoch")
        p50 = [float(row["pub_ack_p50_us"]) for row in group]
        p99 = [float(row["pub_ack_p99_us"]) for row in group]
        summary.append(
            {
                "cell": cell,
                "target_mbps": args.load_mbps,
                "epoch_us": expected_epoch,
                "trials": len(group),
                "append_ack_p50_us_median": statistics.median(p50),
                "append_ack_p50_us_stddev": statistics.stdev(p50),
                "append_ack_p99_us_median": statistics.median(p99),
                "append_ack_p99_us_stddev": statistics.stdev(p99),
                "values_p50_us": p50,
                "values_p99_us": p99,
            }
        )

    manifest = {
        "schema": 1,
        "campaign": str(args.campaign),
        "git_commit": next(iter(commits)),
        "git_dirty": False,
        "inputs_sha256": {
            results_path.name: hashlib.sha256(results_path.read_bytes()).hexdigest(),
            contract_path.name: hashlib.sha256(contract_path.read_bytes()).hexdigest(),
        },
        "summary": summary,
    }
    if args.manifest_out:
        args.manifest_out.parent.mkdir(parents=True, exist_ok=True)
        args.manifest_out.write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
