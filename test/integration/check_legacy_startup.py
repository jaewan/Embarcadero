#!/usr/bin/env python3
"""Optional ORDER0 ACK1 startup regression using the isolated DRAM lifecycle.

Requires the same hardware and 64 GiB region as tools/dev_cluster.py. This
checks legacy startup and completion, not payload correctness or performance.
It acquires the production runner lock and never kills another run's processes.
"""

import csv
import math
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import dev_cluster as runner


def legacy_smoke(run_dir):
    with (run_dir / "throughput_benchmark_summary.csv").open() as stream:
        rows = list(csv.DictReader(stream))
    if len(rows) != 1 or int(rows[0]["total_message_size_bytes"]) != runner.PAYLOAD_BYTES:
        raise runner.RunError("legacy regression did not complete the bounded workload")
    if any(rows[0][field] != expected for field, expected in
           {"order": "0", "ack_level": "1", "replication_factor": "0", "sequencer": "EMBARCADERO"}.items()):
        raise runner.RunError("client result does not match the legacy ORDER0 ACK1 profile")
    for field in ("publish_goodput_mbps", "e2e_goodput_mbps"):
        value = float(rows[0][field])
        if not math.isfinite(value) or value <= 0:
            raise runner.RunError("legacy E2E completion failed: " + field)
    log = (run_dir / "client.log").read_text(errors="replace")
    expected = runner.PAYLOAD_BYTES // runner.MESSAGE_BYTES
    ack = re.search(r"\[ACK_VERIFY\] normalized_received=(\d+) raw_received=(\d+) target=(\d+) 100%", log)
    if not ack or int(ack[1]) != expected or int(ack[2]) < expected or int(ack[3]) != expected:
        raise runner.RunError("legacy ACK path did not acknowledge the entire workload")
    if ("[SESSION_OPEN_ACK]" in log or "Subscriber::Poll timeout" in log or
            re.search(r"retransmit_attempts=[1-9]", log)):
        raise runner.RunError("legacy regression negotiated a session, timed out, or retransmitted")
    return {"result": rows[0], "scope":
            "legacy ORDER0 ACK1 startup and E2E completion only; no indexed payload or ordering audit"}


def main(argv=None):
    return runner.main(argv, profile=runner.SmokeProfile(
        name="dev-dram-legacy-startup-regression", order=0,
        audit_enabled=False, validator=legacy_smoke))


if __name__ == "__main__":
    sys.exit(main())
