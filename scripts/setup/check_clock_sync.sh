#!/usr/bin/env bash
# Read-only clock-sync preflight for distributed campaigns (YCSB plan
# Section 6, Task 2). scripts/cluster_setup.sh --check does not verify clock
# skew at all, and the only in-band detector (run_multiclient.sh's barrier
# code) is reactive — it warns after a trial's barrier already fired, past
# the point where the trial's concurrency is salvageable. This script queries
# chrony's already-running tracking state (does not (re)configure chrony —
# see scripts/setup/sync_clocks.sh for that, an interactive/sudo one-time
# setup step, not a per-campaign check) and fails closed if any host's
# reported offset from the NTP source exceeds MAX_OFFSET_MS.
#
# Usage:
#   bash scripts/setup/check_clock_sync.sh [host ...]   # default: c4 c3 c1
#   MAX_OFFSET_MS=20 bash scripts/setup/check_clock_sync.sh c4 c3

set -euo pipefail

MAX_OFFSET_MS="${MAX_OFFSET_MS:-50}"
HOSTS=("$@")
if [[ "${#HOSTS[@]}" -eq 0 ]]; then
    HOSTS=(c4 c3 c1)
fi

fail=0

check_offset_ms() {
    # chronyc tracking's "System time" line reads e.g.
    # "System time     : 0.000123456 seconds fast of NTP time" — extract the
    # seconds value and convert to milliseconds (sign-agnostic; we only care
    # about magnitude of skew, not direction).
    local tracking_output="$1"
    local seconds
    seconds="$(echo "$tracking_output" | grep -oP 'System time\s*:\s*\K[0-9.]+' || true)"
    if [[ -z "$seconds" ]]; then
        echo "" # signal "could not parse"
        return
    fi
    awk -v s="$seconds" 'BEGIN { printf "%.3f", s * 1000 }'
}

echo "=== Clock sync preflight (threshold: ${MAX_OFFSET_MS}ms) ==="

echo "-- local (moscxl) --"
if local_tracking="$(chronyc tracking 2>&1)"; then
    local_offset_ms="$(check_offset_ms "$local_tracking")"
    if [[ -z "$local_offset_ms" ]]; then
        echo "WARNING: could not parse local chronyc tracking output" >&2
        echo "$local_tracking"
    else
        echo "  offset=${local_offset_ms}ms"
        if awk -v o="$local_offset_ms" -v m="$MAX_OFFSET_MS" 'BEGIN { exit !(o > m) }'; then
            echo "  FAIL: local offset ${local_offset_ms}ms exceeds ${MAX_OFFSET_MS}ms" >&2
            fail=1
        fi
    fi
else
    echo "WARNING: chronyc tracking failed locally (chrony not running?) — cannot verify local clock" >&2
    echo "$local_tracking" >&2
fi

for host in "${HOSTS[@]}"; do
    echo "-- $host --"
    if ! remote_tracking="$(ssh -o BatchMode=yes -o ConnectTimeout=5 "$host" "chronyc tracking" 2>&1)"; then
        echo "  FAIL: could not query chronyc tracking on $host (unreachable or chrony not running)" >&2
        echo "  $remote_tracking" >&2
        fail=1
        continue
    fi
    offset_ms="$(check_offset_ms "$remote_tracking")"
    if [[ -z "$offset_ms" ]]; then
        echo "  FAIL: could not parse chronyc tracking output on $host" >&2
        echo "  $remote_tracking" >&2
        fail=1
        continue
    fi
    echo "  offset=${offset_ms}ms"
    if awk -v o="$offset_ms" -v m="$MAX_OFFSET_MS" 'BEGIN { exit !(o > m) }'; then
        echo "  FAIL: $host offset ${offset_ms}ms exceeds ${MAX_OFFSET_MS}ms" >&2
        fail=1
    fi
done

echo ""
if [[ "$fail" -eq 0 ]]; then
    echo "PASS: all hosts within ${MAX_OFFSET_MS}ms of their NTP source"
else
    echo "FAIL: one or more hosts exceed the clock-skew threshold or could not be checked." >&2
    echo "      Do not launch a synchronized-barrier campaign until this passes." >&2
    echo "      If chrony is not configured, see scripts/setup/sync_clocks.sh (interactive, one-time)." >&2
fi
exit "$fail"
