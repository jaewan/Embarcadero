#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$PROJECT_ROOT"

TAG="${TAG:-$(date -u +%Y%m%d)_e3_order5_slo_curve}"
RUN_TS="${RUN_TS:-$(date -u +%Y%m%dT%H%M%SZ)}"
PEAK_MBPS="${PEAK_MBPS:?set PEAK_MBPS to this systems own measured peak MB/s}"
LOAD_PCTS="${LOAD_PCTS:-10 50 70 90}"
MSG_SIZE="${MSG_SIZE:-1024}"
TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-1073741824}"
NUM_TRIALS="${NUM_TRIALS:-5}"
NUM_BROKERS="${NUM_BROKERS:-4}"
REMOTE_CLIENT_HOST="${REMOTE_CLIENT_HOST:-c4}"
BROKER_LISTEN_ADDR="${BROKER_LISTEN_ADDR:-10.10.10.10}"
TESTBED_LOCK="${TESTBED_LOCK:-$HOME/Embarcadero-sessions/testbed.lock}"
ACTIVITY_LOG="${ACTIVITY_LOG:-$HOME/Embarcadero-sessions/activity.log}"
RAW_DOC_ROOT="${RAW_DOC_ROOT:-$PROJECT_ROOT/docs/experiments/e3_slo_raw/$TAG}"

LOAD_POINTS_MBPS="$(python3 - "$PEAK_MBPS" "$LOAD_PCTS" <<'PY'
import sys
peak = float(sys.argv[1])
pcts = [float(x) for x in sys.argv[2].split()]
print(" ".join(str(max(1, int(round(peak * p / 100.0)))) for p in pcts))
PY
)"

mkdir -p "$(dirname "$TESTBED_LOCK")" "$(dirname "$ACTIVITY_LOG")" "$RAW_DOC_ROOT"

exec {LOCK_FD}>"$TESTBED_LOCK"
flock "$LOCK_FD"

printf '%s START e3_order5_slo_curve tag=%s peak_mbps=%s loads="%s" commit=%s\n' \
  "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$TAG" "$PEAK_MBPS" "$LOAD_POINTS_MBPS" "$(git rev-parse HEAD)" >> "$ACTIVITY_LOG"
trap 'printf "%s END e3_order5_slo_curve tag=%s status=%s\n" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$TAG" "$?" >> "$ACTIVITY_LOG"' EXIT

env \
  EMBARCADERO_DISABLE_PATTERN_KILL=1 \
  REMOTE_CLIENT_HOST="$REMOTE_CLIENT_HOST" \
  BROKER_LISTEN_ADDR="$BROKER_LISTEN_ADDR" \
  LOAD_POINTS_MBPS="$LOAD_POINTS_MBPS" \
  PACING_MODE="${PACING_MODE:-open_loop}" \
  BENCHMARK_TAG="$TAG" \
  RUN_ID="$RUN_TS" \
  SEQUENCER="EMBARCADERO" \
  ORDER="5" \
  ACK_LEVEL="1" \
  REPLICATION_FACTOR="1" \
  MSG_SIZE="$MSG_SIZE" \
  TOTAL_MESSAGE_SIZE="$TOTAL_MESSAGE_SIZE" \
  NUM_TRIALS="$NUM_TRIALS" \
  NUM_BROKERS="$NUM_BROKERS" \
  SCENARIO="remote" \
  SYSTEM_LABEL="embarcadero_order5_ack1_rf1" \
  bash scripts/run_latency_vs_load.sh

RUN_DIR="$PROJECT_ROOT/data/latency_vs_load/$TAG/embarcadero_order5_ack1_rf1/run_$RUN_TS"
cp "$RUN_DIR/metadata.env" "$RAW_DOC_ROOT/metadata.env"
cp "$RUN_DIR/benchmark_contract.md" "$RAW_DOC_ROOT/benchmark_contract.md"
cp "$RUN_DIR/trial_results.csv" "$RAW_DOC_ROOT/trial_results.csv"
cp "$RUN_DIR/summary.csv" "$RAW_DOC_ROOT/summary.csv"

python3 - "$RUN_DIR/trial_results.csv" "$RAW_DOC_ROOT/e3_slo_threshold_curve.csv" <<'PY'
import csv
import sys

inp, outp = sys.argv[1], sys.argv[2]
rows = list(csv.DictReader(open(inp, newline="")))
thresholds_ms = [1, 2, 5, 10, 20, 50, 100]

lat_cols = [c for c in rows[0].keys() if c.lower() in {"p99_us", "p999_us", "p99_9_us", "publish_to_deliver_p99_us"}] if rows else []
lat_col = lat_cols[0] if lat_cols else None
throughput_cols = [c for c in rows[0].keys() if c.lower() in {"achieved_mbps", "e2e_goodput_mbps", "publish_goodput_mbps", "target_mbps"}] if rows else []
tp_col = throughput_cols[0] if throughput_cols else None

with open(outp, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["threshold_ms", "throughput_mbps", "latency_column", "throughput_column"])
    if not lat_col or not tp_col:
        for t in thresholds_ms:
            w.writerow([t, "", lat_col or "", tp_col or ""])
    else:
        for threshold in thresholds_ms:
            eligible = []
            for row in rows:
                try:
                    lat_ms = float(row[lat_col]) / 1000.0
                    tp = float(row[tp_col])
                except (TypeError, ValueError):
                    continue
                if lat_ms <= threshold:
                    eligible.append(tp)
            w.writerow([threshold, f"{max(eligible):.6f}" if eligible else "", lat_col, tp_col])
PY

echo "E3 raw tree: $RAW_DOC_ROOT"
echo "E3 summary:  $RAW_DOC_ROOT/summary.csv"
