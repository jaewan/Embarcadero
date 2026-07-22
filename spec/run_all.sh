#!/usr/bin/env bash
# Reproduce every scenario from a clean checkout. Outputs go to untracked
# directories so the checked-in reference results are never overwritten.
set -euo pipefail

spec_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
jar=${TLA2TOOLS_JAR:-$spec_dir/tla2tools.jar}
state_root=${TLC_STATE_ROOT:-$spec_dir/states}
result_root=${TLC_RESULT_ROOT:-$spec_dir/reproduced-results}
workers=${TLC_WORKERS:-4}
heap=${TLC_HEAP:-8g}
cfgs="core crash_pre_post_pbr retransmit_race dup_in_guard_window lease_false_positive stale_cv_ack_relay stale_cv_bug_demo client_crash_midstream seq_failover_open_gap module_loss_ack_window reader_agreement exactly_once_fence"

if [[ ! -f "$jar" ]]; then
  echo "missing tla2tools.jar: $jar" >&2
  echo "download it as documented in spec/README.md or set TLA2TOOLS_JAR" >&2
  exit 2
fi
mkdir -p "$state_root" "$result_root"

for cfg in $cfgs; do
  scenario_state="$state_root/$cfg"
  if [[ "$scenario_state" != "$state_root/"* ]]; then
    echo "unsafe TLC state path: $scenario_state" >&2
    exit 2
  fi
  rm -rf -- "$scenario_state"
  mkdir -p "$scenario_state/tmp"
  echo "TLC $cfg"
  status=0
  java "-Xmx$heap" -Djava.io.tmpdir="$scenario_state/tmp" -XX:+UseParallelGC \
    -cp "$jar" tlc2.TLC -deadlock -workers "$workers" \
    -metadir "$scenario_state" -config "$spec_dir/$cfg.cfg" \
    "$spec_dir/MCEmbarcadero.tla" > "$result_root/$cfg.txt" 2>&1 || status=$?
  if [[ "$cfg" == stale_cv_bug_demo ]]; then
    if ! grep -q 'Invariant Safety is violated' "$result_root/$cfg.txt"; then
      echo "expected stale-CV counterexample was not reproduced" >&2
      exit 1
    fi
  elif [[ $status -ne 0 ]] || ! grep -q 'Model checking completed. No error has been found.' "$result_root/$cfg.txt"; then
    echo "TLC scenario failed: $cfg (see $result_root/$cfg.txt)" >&2
    exit 1
  fi
done

echo "results: $result_root"
