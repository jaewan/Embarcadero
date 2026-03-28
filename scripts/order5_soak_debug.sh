#!/bin/bash
#
# Repeated ORDER=5 throughput soak with artifact preservation.
# Runs the normal multiclient workload repeatedly, archives broker/client logs
# for every run, and optionally reruns failed attempts with heavy ORDER=5 traces.
#
# Environment:
#   ORDER5_SOAK_RUNS              Number of baseline runs (default: 10)
#   ORDER5_SOAK_ARTIFACT_DIR      Output directory (default: /tmp/order5_soak_debug_<ts>)
#   ORDER5_SOAK_TRACE_ON_FAILURE  1 to rerun failed runs with traces (default: 1)
#   All run_multiclient.sh env vars are honored (NUM_CLIENTS, ORDER, ACK, etc.)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
RUNS="${ORDER5_SOAK_RUNS:-10}"
TRACE_ON_FAILURE="${ORDER5_SOAK_TRACE_ON_FAILURE:-1}"
STAMP="$(date +%Y%m%d_%H%M%S)"
ARTIFACT_DIR="${ORDER5_SOAK_ARTIFACT_DIR:-/tmp/order5_soak_debug_${STAMP}}"
SUMMARY_FILE="$ARTIFACT_DIR/summary.tsv"

mkdir -p "$ARTIFACT_DIR"
printf "run\tmode\tstatus\tgbps\tartifact_dir\n" >"$SUMMARY_FILE"

copy_run_artifacts() {
	local dest="$1"
	mkdir -p "$dest"
	for f in /tmp/broker_*.log; do
		[ -f "$f" ] && cp "$f" "$dest/"
	done
	if [ -d "$PROJECT_ROOT/multiclient_logs" ]; then
		find "$PROJECT_ROOT/multiclient_logs" -maxdepth 1 -type f -name 'trial1*' -exec cp {} "$dest/" \;
	fi
}

extract_gbps() {
	local log_file="$1"
	python3 - "$log_file" <<'PY'
import re, sys
txt = open(sys.argv[1]).read()
m = re.search(r'\((\d+\.\d+) GB/s\)', txt)
print(m.group(1) if m else "")
PY
}

run_once() {
	local run_idx="$1"
	local mode="$2"
	local out="$3"
	local artifact_subdir="$4"
	shift 4

	echo "=== RUN ${run_idx} (${mode}) ==="
	(
		cd "$PROJECT_ROOT"
		env "$@" ./scripts/run_multiclient.sh >"$out" 2>&1 || true
	)

	copy_run_artifacts "$artifact_subdir"

	local status="ok"
	if rg -n "ACK timeout|timed out|Publish test failed|TOTAL    → 0 MB/s" "$out" >/dev/null 2>&1; then
		status="fail"
	fi
	local gbps
	gbps="$(extract_gbps "$out")"
	printf "%s\t%s\t%s\t%s\t%s\n" "$run_idx" "$mode" "$status" "${gbps:-}" "$artifact_subdir" >>"$SUMMARY_FILE"

	if [ "$status" = "ok" ]; then
		rg -n "TOTAL    →" "$out" || true
	else
		rg -n "TOTAL    →|ACK timeout|timed out|Publish test failed" "$out" || true
	fi

	return 0
}

for run_idx in $(seq 1 "$RUNS"); do
	base_log="$ARTIFACT_DIR/run_${run_idx}.log"
	base_artifacts="$ARTIFACT_DIR/run_${run_idx}_base"
	run_once "$run_idx" "base" "$base_log" "$base_artifacts" \
		NUM_CLIENTS="${NUM_CLIENTS:-3}" \
		NUM_TRIALS="${NUM_TRIALS:-1}" \
		TRIAL_MAX_ATTEMPTS="${TRIAL_MAX_ATTEMPTS:-1}" \
		TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-16106127360}" \
		MESSAGE_SIZE="${MESSAGE_SIZE:-1024}" \
		ORDER="${ORDER:-5}" \
		ACK="${ACK:-1}" \
		REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}" \
		SEQUENCER="${SEQUENCER:-EMBARCADERO}"

	if [ "$TRACE_ON_FAILURE" = "1" ] &&
	   rg -n "ACK timeout|timed out|Publish test failed|TOTAL    → 0 MB/s" "$base_log" >/dev/null 2>&1; then
		trace_log="$ARTIFACT_DIR/run_${run_idx}_trace.log"
		trace_artifacts="$ARTIFACT_DIR/run_${run_idx}_trace"
		run_once "$run_idx" "trace" "$trace_log" "$trace_artifacts" \
			NUM_CLIENTS="${NUM_CLIENTS:-3}" \
			NUM_TRIALS=1 \
			TRIAL_MAX_ATTEMPTS=1 \
			TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-16106127360}" \
			MESSAGE_SIZE="${MESSAGE_SIZE:-1024}" \
			ORDER="${ORDER:-5}" \
			ACK="${ACK:-1}" \
			REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}" \
			SEQUENCER="${SEQUENCER:-EMBARCADERO}" \
			EMBARCADERO_ORDER5_TRACE=1 \
			EMBAR_FRONTIER_TRACE=1 \
			EMBARCADERO_ORDER5_PHASE_DIAG=1
	fi
done

echo
echo "Artifacts: $ARTIFACT_DIR"
echo "Summary:   $SUMMARY_FILE"
