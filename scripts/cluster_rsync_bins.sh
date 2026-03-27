#!/usr/bin/env bash
# Copy locally built binaries + broker lifecycle script to cluster nodes (no internet on targets).
# Usage:
#   ./scripts/cluster_rsync_bins.sh c2 c3
#   CLUSTER_PUBLISHER_HOST=c1 ./scripts/cluster_rsync_bins.sh c2 c3   # also sync throughput_test + client config
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REMOTE_ROOT="${REMOTE_PROJECT_ROOT:-$HOME/Embarcadero}"
BIN_LOCAL="$ROOT/build/bin"

if [ ! -x "$BIN_LOCAL/embarlet" ] || [ ! -x "$BIN_LOCAL/throughput_test" ] || [ ! -x "$BIN_LOCAL/corfu_global_sequencer" ]; then
  echo "ERROR: missing binaries under $BIN_LOCAL (build first: cmake --build build --target embarlet throughput_test corfu_global_sequencer)" >&2
  exit 1
fi

HOSTS=("$@")
if [ "${#HOSTS[@]}" -eq 0 ]; then
  echo "Usage: $0 <ssh_host> [ssh_host ...]" >&2
  exit 2
fi

for h in "${HOSTS[@]}"; do
  echo "==> $h: rsync bins + scripts/lib"
  rsync -avz \
    "$BIN_LOCAL/embarlet" \
    "$BIN_LOCAL/throughput_test" \
    "$BIN_LOCAL/corfu_global_sequencer" \
    "$h:$REMOTE_ROOT/build/bin/"
  rsync -avz "$ROOT/scripts/lib/broker_lifecycle.sh" "$h:$REMOTE_ROOT/scripts/lib/"
done

if [ -n "${CLUSTER_PUBLISHER_HOST:-}" ]; then
  echo "==> ${CLUSTER_PUBLISHER_HOST}: publisher-only sync"
  rsync -avz \
    "$BIN_LOCAL/throughput_test" \
    "${CLUSTER_PUBLISHER_HOST}:$REMOTE_ROOT/build/bin/"
  rsync -avz "$ROOT/config/client.yaml" "${CLUSTER_PUBLISHER_HOST}:$REMOTE_ROOT/config/"
  rsync -avz "$ROOT/scripts/run_throughput.sh" "${CLUSTER_PUBLISHER_HOST}:$REMOTE_ROOT/scripts/"
  rsync -avz "$ROOT/scripts/lib/run_throughput_impl.sh" "${CLUSTER_PUBLISHER_HOST}:$REMOTE_ROOT/scripts/lib/"
  rsync -avz "$ROOT/scripts/lib/broker_lifecycle.sh" "${CLUSTER_PUBLISHER_HOST}:$REMOTE_ROOT/scripts/lib/"
  if [ -f "$ROOT/scripts/run_corfu_remote_throughput.sh" ]; then
    rsync -avz "$ROOT/scripts/run_corfu_remote_throughput.sh" "${CLUSTER_PUBLISHER_HOST}:$REMOTE_ROOT/scripts/"
  fi
fi

echo "Done."
