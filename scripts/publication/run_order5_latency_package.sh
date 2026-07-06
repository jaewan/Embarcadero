#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$PROJECT_ROOT"

SCOUT_TAG="${SCOUT_TAG:-20260402_order5_latency_vs_load_scout_tp1_remote}"
FINAL_TAG="${FINAL_TAG:-20260402_order5_latency_vs_load_final_tp1_remote}"
OUT_BASE="${OUT_BASE:-$PROJECT_ROOT/data/latency_vs_load}"
SCOUT_LOAD_POINTS="${SCOUT_LOAD_POINTS:-250 500 1000 2000 3000 4000 5000}"
RUN_TS_SCOUT="${RUN_TS_SCOUT:-$(date -u +%Y%m%dT%H%M%SZ)}"
RUN_TS_FINAL="${RUN_TS_FINAL:-$(date -u +%Y%m%dT%H%M%SZ)}"

export PACING_MODE="${PACING_MODE:-open_loop}"
export SCENARIO="${SCENARIO:-remote}"
export REMOTE_CLIENT_HOST="${REMOTE_CLIENT_HOST:-c2}"
export BROKER_LISTEN_ADDR="${BROKER_LISTEN_ADDR:-10.10.10.10}"
export SEQUENCER="${SEQUENCER:-EMBARCADERO}"
export ORDER="${ORDER:-5}"
export ACK_LEVEL="${ACK_LEVEL:-1}"
export REPLICATION_FACTOR="${REPLICATION_FACTOR:-1}"
export THREADS_PER_BROKER="${THREADS_PER_BROKER:-1}"
export MSG_SIZE="${MSG_SIZE:-1024}"
export TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-134217728}"
export NUM_TRIALS="${NUM_TRIALS:-3}"
export NUM_BROKERS="${NUM_BROKERS:-4}"

if [[ "$SEQUENCER" != "EMBARCADERO" || "$ORDER" != "5" || "$ACK_LEVEL" != "1" || "$REPLICATION_FACTOR" != "1" || "$THREADS_PER_BROKER" != "1" ]]; then
  echo "ERROR: run_order5_latency_package.sh is pinned to the publication-safe ORDER=5 tp1 contract." >&2
  echo "       Got SEQUENCER=$SEQUENCER ORDER=$ORDER ACK_LEVEL=$ACK_LEVEL REPLICATION_FACTOR=$REPLICATION_FACTOR THREADS_PER_BROKER=$THREADS_PER_BROKER" >&2
  exit 1
fi

echo "==> Scout sweep ($SCOUT_TAG)"
LOAD_POINTS_MBPS="$SCOUT_LOAD_POINTS" \
BENCHMARK_TAG="$SCOUT_TAG" \
RUN_ID="$RUN_TS_SCOUT" \
OUT_BASE="$OUT_BASE" \
bash scripts/run_latency_vs_load.sh

SYSTEM_LABEL="${SEQUENCER}_order${ORDER}_ack${ACK_LEVEL}_rf${REPLICATION_FACTOR}"
SCOUT_RUN_DIR="$OUT_BASE/$SCOUT_TAG/$SYSTEM_LABEL/run_$RUN_TS_SCOUT"
SCOUT_SUMMARY="$SCOUT_RUN_DIR/summary.csv"
if [[ ! -f "$SCOUT_SUMMARY" ]]; then
  echo "ERROR: scout summary missing: $SCOUT_SUMMARY" >&2
  exit 1
fi

SELECTION_TMP="$(mktemp)"
python3 - "$SCOUT_SUMMARY" "$SELECTION_TMP" <<'PY'
import csv
import math
import pathlib
import sys

summary_path = pathlib.Path(sys.argv[1])
out_path = pathlib.Path(sys.argv[2])

rows = []
with summary_path.open(newline="") as handle:
    reader = csv.DictReader(handle)
    for row in reader:
        try:
            target = float(row["target_mbps"])
        except Exception:
            continue
        def f(name):
            v = row.get(name, "")
            try:
                return float(v) if v != "" else None
            except Exception:
                return None
        rows.append({
            "target": target,
            "p95": f("publish_to_deliver_p95_us_mean"),
            "goodput": f("achieved_publish_goodput_mbps_mean"),
            "offered": f("achieved_offered_load_mbps_mean"),
            "trials": row.get("trials", ""),
        })

if not rows:
    raise SystemExit("No scout rows parsed")

rows.sort(key=lambda r: r["target"])
valid_p95 = [r["p95"] for r in rows if r["p95"] is not None and r["p95"] > 0]
baseline_p95 = min(valid_p95) if valid_p95 else None

def round50(x):
    return int(round(x / 50.0) * 50)

knee = rows[-1]["target"]
knee_reason = "max_target_no_clear_knee"
for r in rows:
    target = r["target"]
    p95 = r["p95"]
    goodput = r["goodput"]
    offered = r["offered"]
    if baseline_p95 is not None and p95 is not None and p95 >= baseline_p95 * 2.0:
        knee = target
        knee_reason = f"p95_jump_{p95:.3f}_from_baseline_{baseline_p95:.3f}"
        break
    if goodput is not None and goodput < target * 0.95:
        knee = target
        knee_reason = f"goodput_below_95pct_{goodput:.3f}_of_{target:.3f}"
        break
    if offered is not None and offered < target * 0.95:
        knee = target
        knee_reason = f"offered_below_95pct_{offered:.3f}_of_{target:.3f}"
        break

loads = set()
loads.add(int(rows[0]["target"]))
for factor in (0.5, 0.75, 0.9, 1.0, 1.1, 1.25):
    cand = round50(knee * factor)
    if cand > 0:
        loads.add(cand)

for r in rows:
    if r["target"] < knee:
        loads.add(int(r["target"]))
    if r["target"] >= knee:
        loads.add(int(r["target"]))
        break

max_target = max(r["target"] for r in rows)
ladder = sorted(x for x in loads if x >= rows[0]["target"] and x <= max(max_target, round50(knee * 1.25)))

if len(ladder) < 5:
    ladder = sorted({int(r["target"]) for r in rows})

note = []
note.append(f"knee_mbps={int(knee)}")
note.append(f"knee_reason={knee_reason}")
note.append(f"baseline_p95_us={'' if baseline_p95 is None else f'{baseline_p95:.6f}'}")
note.append("final_load_points_mbps=" + " ".join(str(x) for x in ladder))
note.append("scout_rows=")
for r in rows:
    p95_str = "" if r["p95"] is None else f"{r['p95']:.6f}"
    goodput_str = "" if r["goodput"] is None else f"{r['goodput']:.6f}"
    offered_str = "" if r["offered"] is None else f"{r['offered']:.6f}"
    note.append(
        f"  target={int(r['target'])} "
        f"p95_us={p95_str} "
        f"goodput_mbps={goodput_str} "
        f"offered_mbps={offered_str} "
        f"trials={r['trials']}"
    )

out_path.write_text("\n".join(note) + "\n")
PY

FINAL_LOAD_POINTS="$(awk -F= '/^final_load_points_mbps=/{print $2}' "$SELECTION_TMP")"
KNEE_MBPS="$(awk -F= '/^knee_mbps=/{print $2}' "$SELECTION_TMP")"
KNEE_REASON="$(awk -F= '/^knee_reason=/{print $2}' "$SELECTION_TMP")"

echo "==> Final sweep ($FINAL_TAG)"
echo "    Knee: $KNEE_MBPS MB/s ($KNEE_REASON)"
echo "    Final load ladder: $FINAL_LOAD_POINTS"

LOAD_POINTS_MBPS="$FINAL_LOAD_POINTS" \
BENCHMARK_TAG="$FINAL_TAG" \
RUN_ID="$RUN_TS_FINAL" \
OUT_BASE="$OUT_BASE" \
bash scripts/run_latency_vs_load.sh

FINAL_RUN_DIR="$OUT_BASE/$FINAL_TAG/$SYSTEM_LABEL/run_$RUN_TS_FINAL"
NOTE_PATH="$FINAL_RUN_DIR/load_selection_note.md"
{
  echo "# Load Selection Note"
  echo
  echo "- Scout run: \`$SCOUT_RUN_DIR\`"
  echo "- Final run: \`$FINAL_RUN_DIR\`"
  echo "- Knee estimate: \`$KNEE_MBPS MB/s\`"
  echo "- Knee trigger: \`$KNEE_REASON\`"
  echo "- Final load ladder: \`$FINAL_LOAD_POINTS\`"
  echo
  echo "## Scout Summary"
  cat "$SELECTION_TMP"
} > "$NOTE_PATH"

rm -f "$SELECTION_TMP"

echo "ORDER=5 latency package complete."
echo "  Scout: $SCOUT_RUN_DIR"
echo "  Final: $FINAL_RUN_DIR"
echo "  Note : $NOTE_PATH"
