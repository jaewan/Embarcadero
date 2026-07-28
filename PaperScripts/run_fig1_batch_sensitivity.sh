#!/usr/bin/env bash
# Long-transfer batch-size sensitivity for the implemented Embarcadero path.
#
# Each point uses the existing publication-grade Fig. 1 driver and its remote
# client synchronization/provenance checks. The experiment fixes two remote
# publishers, four log servers, 4 KiB records, two-copy DRAM completion, and
# ordered ACKs; only the publisher batch size changes.
#
# Publication run:
#   NUM_TRIALS=3 TOTAL_BYTES=$((64<<30)) \
#     bash PaperScripts/run_fig1_batch_sensitivity.sh
#
# Smoke:
#   BATCH_KB_VALUES="64" NUM_TRIALS=1 TOTAL_BYTES=$((8<<30)) \
#     ALLOW_DIRTY_ARTIFACT=1 bash PaperScripts/run_fig1_batch_sensitivity.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BATCH_KB_VALUES="${BATCH_KB_VALUES:-64 256 2048}"
NUM_TRIALS="${NUM_TRIALS:-3}"
TOTAL_BYTES="${TOTAL_BYTES:-$((64 * 1024 * 1024 * 1024))}"
MSG_SIZE="${MSG_SIZE:-4096}"
PARENT_CAMPAIGN="${PARENT_CAMPAIGN:-fig1_batch_sensitivity}"
ALLOW_DIRTY_ARTIFACT="${ALLOW_DIRTY_ARTIFACT:-0}"
# Each broker needs an initial segment, and the aggregate payload consumes
# ceil(total/8GiB) more segment extents as logs roll. Prefault both sets before
# broker readiness; otherwise a long run measures serialized first-touch page
# faults after the initial 32 GiB rather than steady ingest.
SEGMENT_BYTES=$((8 * 1024 * 1024 * 1024))
REQUIRED_CXL_SEGMENTS="${REQUIRED_CXL_SEGMENTS:-$(( \
  4 + (TOTAL_BYTES + SEGMENT_BYTES - 1) / SEGMENT_BYTES \
))}"

for batch_kb in $BATCH_KB_VALUES; do
  if ! [[ "$batch_kb" =~ ^[0-9]+$ ]] || [[ "$batch_kb" -le 0 ]]; then
    echo "ERROR: invalid batch size: $batch_kb KiB" >&2
    exit 2
  fi
  campaign="${PARENT_CAMPAIGN}_b${batch_kb}k"
  echo "=== batch sensitivity: ${batch_kb} KiB, campaign=$campaign ==="
  CAMPAIGN_ID="$campaign" \
  NUM_TRIALS="$NUM_TRIALS" \
  TOTAL_BYTES="$TOTAL_BYTES" \
  MSG_SIZE="$MSG_SIZE" \
  CLIENT_PUB_BATCH_KB="$batch_kb" \
  N_VALUES=2 \
  ONLY_CELLS=fig1_embar_o5_mem_n2 \
  SKIP_BASELINES=1 \
  SKIP_DISK=1 \
  SKIP_MEM=0 \
  WAIT_FOR_IDLE=1 \
  ALLOW_DIRTY_ARTIFACT="$ALLOW_DIRTY_ARTIFACT" \
  EMBARCADERO_REQUIRED_CXL_SEGMENTS="$REQUIRED_CXL_SEGMENTS" \
    bash PaperScripts/run_fig1_throughput_scaling.sh
done

out="$ROOT/data/paper_eval/fig1/$PARENT_CAMPAIGN"
mkdir -p "$out"
python3 - "$ROOT/data/paper_eval/fig1" "$PARENT_CAMPAIGN" "$out/results.csv" \
  $BATCH_KB_VALUES <<'PY'
import csv
import pathlib
import sys

fig1_root = pathlib.Path(sys.argv[1])
parent = sys.argv[2]
output = pathlib.Path(sys.argv[3])
batches = [int(value) for value in sys.argv[4:]]
rows = []
for batch in batches:
    source = fig1_root / f"{parent}_b{batch}k" / "results.csv"
    if not source.exists():
        raise SystemExit(f"missing campaign CSV: {source}")
    with source.open(newline="") as handle:
        for row in csv.DictReader(handle):
            if row.get("cell") == "fig1_embar_o5_mem_n2":
                rows.append(row)
if not rows:
    raise SystemExit("no batch-sensitivity rows found")
with output.open("w", newline="") as handle:
    writer = csv.DictWriter(handle, fieldnames=list(rows[0]))
    writer.writeheader()
    writer.writerows(rows)
print(output)
PY

echo "Batch-sensitivity aggregate: $out/results.csv"
