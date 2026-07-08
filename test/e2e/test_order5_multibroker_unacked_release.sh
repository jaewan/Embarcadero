#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BIN_DIR="$BUILD_DIR/bin"
CONFIG_DIR="$PROJECT_ROOT/config"
OUT_DIR="$BUILD_DIR/test_output/order5_multibroker_unacked_release"
LOCK_FILE="${EMBARCADERO_TESTBED_LOCK:-$(cd "$PROJECT_ROOT/.." && pwd)/testbed.lock}"

NUM_BROKERS="${EMBARCADERO_TEST_NUM_BROKERS:-4}"
TOTAL_BYTES="${EMBARCADERO_TEST_TOTAL_BYTES:-67108864}"
MESSAGE_SIZE="${EMBARCADERO_TEST_MESSAGE_SIZE:-1024}"

pids=()

fail() {
	echo "FAIL: $*" >&2
	if [ -d "$OUT_DIR" ]; then
		for f in "$OUT_DIR"/*.log; do
			[ -f "$f" ] || continue
			echo "--- $(basename "$f") tail ---" >&2
			tail -80 "$f" >&2 || true
		done
	fi
	exit 1
}

cleanup() {
	for pid in "${pids[@]:-}"; do
		if kill -0 "$pid" 2>/dev/null; then
			kill "$pid" 2>/dev/null || true
		fi
	done
	sleep 0.5
	for pid in "${pids[@]:-}"; do
		if kill -0 "$pid" 2>/dev/null; then
			kill -9 "$pid" 2>/dev/null || true
		fi
	done
	for pid in "${pids[@]:-}"; do
		wait "$pid" 2>/dev/null || true
	done
	rm -f /tmp/embarlet_*_ready 2>/dev/null || true
}

wait_ready() {
	local timeout="$1"
	for _ in $(seq 1 "$timeout"); do
		local count
		count=$(ls /tmp/embarlet_*_ready 2>/dev/null | wc -l || true)
		if [ "$count" -ge "$NUM_BROKERS" ]; then
			rm -f /tmp/embarlet_*_ready 2>/dev/null || true
			return 0
		fi
		for pid in "${pids[@]:-}"; do
			kill -0 "$pid" 2>/dev/null || return 1
		done
		sleep 1
	done
	return 1
}

run_test_body() {
	mkdir -p "$OUT_DIR"
	cd "$OUT_DIR"
	rm -f ./*.log /tmp/embarlet_*_ready 2>/dev/null || true

	[ -x "$BIN_DIR/embarlet" ] || fail "missing $BIN_DIR/embarlet"
	[ -x "$BIN_DIR/throughput_test" ] || fail "missing $BIN_DIR/throughput_test"

	env \
		NUM_BROKERS="$NUM_BROKERS" \
		EMBARCADERO_NUM_BROKERS="$NUM_BROKERS" \
		EMBAR_USE_HUGETLB=0 \
		EMBAR_ASSERT_COMMIT_ORDER=1 \
		EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1 \
		"$BIN_DIR/embarlet" --config "$CONFIG_DIR/embarcadero.yaml" --head --EMBARCADERO \
		> broker_0.log 2>&1 &
	pids+=("$!")
	sleep 1
	for b in $(seq 1 $((NUM_BROKERS - 1))); do
		env \
			NUM_BROKERS="$NUM_BROKERS" \
			EMBARCADERO_NUM_BROKERS="$NUM_BROKERS" \
			EMBAR_USE_HUGETLB=0 \
			EMBAR_ASSERT_COMMIT_ORDER=1 \
			EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1 \
			"$BIN_DIR/embarlet" --config "$CONFIG_DIR/embarcadero.yaml" --EMBARCADERO \
			> "broker_${b}.log" 2>&1 &
		pids+=("$!")
		sleep 0.35
	done

	wait_ready 120 || fail "brokers failed readiness"

	env \
		NUM_BROKERS="$NUM_BROKERS" \
		EMBARCADERO_NUM_BROKERS="$NUM_BROKERS" \
		EMBAR_USE_HUGETLB=0 \
		EMBAR_ASSERT_COMMIT_ORDER=1 \
		"$BIN_DIR/throughput_test" \
			--config "$CONFIG_DIR/client.yaml" \
			-n 1 -m "$MESSAGE_SIZE" -s "$TOTAL_BYTES" -t 5 -o 5 -a 1 \
			--sequencer EMBARCADERO --head_addr 127.0.0.1 -l 0 -r 0 \
			> client.log 2>&1 || fail "ORDER=5 multibroker client failed"

	grep -q "\\[ACK_VERIFY\\].*100%" client.log ||
		fail "client did not receive complete ACK1 progress"
	grep -q "\\[UNACKED_DRAIN\\].*bytes=0.*batches=0" client.log ||
		fail "unacked ledger did not drain to zero"
	if grep -R "COMMIT_ORDER_VIOLATION" . >/dev/null 2>&1; then
		fail "commit-order violation detected"
	fi
	local goi_total goi_unique
	goi_total=$(grep -h "\\[ORDER5_TEST_GOI_COMMIT\\]" broker_*.log 2>/dev/null | wc -l || true)
	goi_unique=$(grep -h "\\[ORDER5_TEST_GOI_COMMIT\\]" broker_*.log 2>/dev/null |
		sed -n 's/.*client=\([0-9][0-9]*\).*session_epoch=\([0-9][0-9]*\).*batch_seq=\([0-9][0-9]*\).*/\1:\2:\3/p' |
		sort -u | wc -l || true)
	[ "$goi_total" -gt 0 ] || fail "broker-side GOI commit trace was empty"
	[ "$goi_total" -eq "$goi_unique" ] ||
		fail "broker-side GOI commit-count mismatch total=$goi_total unique=$goi_unique"

	echo "PASS ORDER5 multibroker unacked release: brokers=$NUM_BROKERS bytes=$TOTAL_BYTES goi_commits=$goi_total"
}

exec 9>"$LOCK_FILE"
flock 9
trap cleanup EXIT INT TERM
run_test_body
