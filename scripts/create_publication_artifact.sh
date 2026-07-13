#!/usr/bin/env bash
# Create an immutable publication artifact directory with provenance.
# Usage:
#   scripts/create_publication_artifact.sh <artifact_root> [label]
# Copies effective env knobs, git commit, binary hashes, and NUMA/topology snapshots.
set -euo pipefail

ARTIFACT_ROOT="${1:?artifact root required}"
LABEL="${2:-run}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)"
OUT="$ARTIFACT_ROOT/${LABEL}_${STAMP}"
mkdir -p "$OUT/provenance" "$OUT/logs" "$OUT/timeseries"

{
  echo "created_utc=$STAMP"
  echo "label=$LABEL"
  echo "host=$(hostname)"
  echo "cwd=$PROJECT_ROOT"
  echo "commit=$(git -C "$PROJECT_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
  echo "commit_dirty=$(git -C "$PROJECT_ROOT" status --porcelain 2>/dev/null | wc -l | tr -d ' ')"
  echo "branch=$(git -C "$PROJECT_ROOT" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
} > "$OUT/provenance/run_meta.txt"

{
  echo "EMBARCADERO_REPLICATION_FACTOR=${EMBARCADERO_REPLICATION_FACTOR:-}"
  echo "EMBARCADERO_ACK_LEVEL=${EMBARCADERO_ACK_LEVEL:-${ACK_LEVEL:-}}"
  echo "EMBARCADERO_CXL_COHERENT=${EMBARCADERO_CXL_COHERENT:-}"
  echo "EMBARCADERO_STAGE_TRACE=${EMBARCADERO_STAGE_TRACE:-}"
  echo "EMBARCADERO_DISABLE_PUSH_GO=${EMBARCADERO_DISABLE_PUSH_GO:-}"
  echo "NUM_BROKERS=${NUM_BROKERS:-}"
  echo "NUM_CLIENTS=${NUM_CLIENTS:-}"
  echo "NUM_TRIALS=${NUM_TRIALS:-}"
  echo "ORDER=${ORDER:-}"
  echo "MESSAGE_SIZE=${MESSAGE_SIZE:-}"
  echo "TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-}"
  echo "MIN_OVERLAP_MS=${MIN_OVERLAP_MS:-10000}"
} > "$OUT/provenance/effective_env.txt"

BIN_DIR="${BUILD_BIN:-$PROJECT_ROOT/build/bin}"
{
  for b in embarlet throughput_test; do
    p="$BIN_DIR/$b"
    if [[ -x "$p" ]]; then
      echo "$b sha256=$(sha256sum "$p" | awk '{print $1}') size=$(stat -c%s "$p" 2>/dev/null || stat -f%z "$p")"
    else
      echo "$b missing"
    fi
  done
} > "$OUT/provenance/binary_hashes.txt"

(command -v numactl >/dev/null && numactl -H || true) > "$OUT/provenance/numa.txt" 2>&1 || true
(uname -a; lscpu 2>/dev/null | head -40 || true) > "$OUT/provenance/cpu.txt" 2>&1 || true
(cat /proc/meminfo 2>/dev/null | head -20 || true) > "$OUT/provenance/meminfo.txt" 2>&1 || true

echo "$OUT"
