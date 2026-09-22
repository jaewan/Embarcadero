#!/usr/bin/env python3
"""Select an owned local experiment runner or an explicit historical launcher.

This module only dispatches argv with exec. The selected runner owns validation,
locking, process groups, shared memory, deadlines, and result qualification.
"""

import argparse
import os
from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parents[1]
PROFILES = {
    "dev": "tools/dev_cluster.py",
    "fault": "test/integration/run_production_faults.py",
    "perf": "tools/perf_compare.py",
    "legacy-startup": "test/integration/check_legacy_startup.py",
    "analyze": "tools/analyze_perf_comparison.py",
}
# Only launchers named in the supported command inventory have compatibility
# routes. Other paper/host-provisioning scripts remain historical references.
LEGACY_LAUNCHERS = {
    name: "scripts/" + name + ".sh" for name in (
        "singlenode_run_throughput", "run_throughput", "run_multiclient",
        "run_latency", "run_throughput_latency_sweep", "run_latency_vs_load",
        "run_e2e_throughput_benchmark", "run_failures",
        "run_ordering_durability_ladder", "run_slow_replica_heterogeneity",
    )
}
LEGACY_LAUNCHERS["run_throughput_matrix"] = "scripts/publication/run_throughput_matrix.sh"


def parser():
    return argparse.ArgumentParser(
        description=__doc__,
        epilog=("Profiles: dev = audited DRAM smoke; fault = production fault cases; "
                "perf = matched DRAM pilot; legacy-startup = ORDER0/ACK1 startup; "
                "analyze = retained pilot analysis. Append --help for runner options. "
                "legacy requires an inventory name and retains historical SSH/cleanup "
                "side effects. No profile runs by default."))


def main(argv=None, *, repo_root=ROOT):
    command_line = list(sys.argv[1:] if argv is None else argv)
    cli = parser()
    cli.add_argument("profile", choices=tuple(PROFILES) + ("legacy",))
    cli.add_argument("arguments", nargs=argparse.REMAINDER)
    if not command_line:
        cli.print_help()
        return 0
    args = cli.parse_args(command_line)
    forwarded = args.arguments
    if forwarded[:1] == ["--"]:
        forwarded = forwarded[1:]
    if args.profile == "legacy":
        legacy = argparse.ArgumentParser(
            prog="experiment.py legacy",
            description="Historical research launchers: may use SSH, host tuning, and broad cleanup.")
        legacy.add_argument("launcher", choices=tuple(LEGACY_LAUNCHERS))
        legacy.add_argument("arguments", nargs=argparse.REMAINDER)
        selected = legacy.parse_args(forwarded)
        remainder = selected.arguments
        if remainder[:1] == ["--"]:
            remainder = remainder[1:]
        target = repo_root / LEGACY_LAUNCHERS[selected.launcher]
        command = ["/bin/bash", str(target), "--legacy", *remainder]
        print("Historical research route: " + selected.launcher +
              "; its original host and cleanup behavior applies.", file=sys.stderr, flush=True)
    else:
        target = repo_root / PROFILES[args.profile]
        command = [sys.executable, str(target), *forwarded]
    if not target.is_file():
        cli.error("missing entrypoint: " + str(target))
    # No shell expansion, environment translation, subprocess wrapper, or
    # fallback to historical behavior. Signals and exit status belong to runner.
    try:
        os.execv(command[0], command)
    except OSError as error:
        print("Cannot execute selected entrypoint: " + str(error), file=sys.stderr)
        return 126


if __name__ == "__main__":
    raise SystemExit(main())
