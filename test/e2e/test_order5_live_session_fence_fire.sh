#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$PROJECT_ROOT/build"
BIN_DIR="$BUILD_DIR/bin"
CONFIG_DIR="$PROJECT_ROOT/config"
OUT_DIR="$BUILD_DIR/test_output/order5_live_session_fence_fire"
LOCK_FILE="${EMBARCADERO_TESTBED_LOCK:-$(cd "$PROJECT_ROOT/.." && pwd)/testbed.lock}"

SESSION_EPOCH="${EMBARCADERO_TEST_LIVE_SESSION_EPOCH:-1}"
BROKER_ENV_SESSION_EPOCH="${EMBARCADERO_TEST_LIVE_BROKER_ENV_SESSION_EPOCH:-101}"
LEASE_MS="${EMBARCADERO_TEST_LIVE_SESSION_LEASE_MS:-500}"
CLAIMED_WAIT_MS="${EMBARCADERO_TEST_LIVE_CLAIMED_WAIT_MS:-650}"
MESSAGE_SIZE=1024
FIRST_TOTAL_BYTES=$((8 * 1024 * 1024))
SECOND_TOTAL_BYTES=$((2 * 1024 * 1024))

broker_pid=""

fail() {
	echo "FAIL: $*" >&2
	if [ -f "$OUT_DIR/broker_0.log" ]; then
		echo "--- broker tail ---" >&2
		tail -120 "$OUT_DIR/broker_0.log" >&2 || true
	fi
	if [ -f "$OUT_DIR/client_fenced.log" ]; then
		echo "--- client_fenced tail ---" >&2
		tail -80 "$OUT_DIR/client_fenced.log" >&2 || true
	fi
	if [ -f "$OUT_DIR/client_unaffected.log" ]; then
		echo "--- client_unaffected tail ---" >&2
		tail -80 "$OUT_DIR/client_unaffected.log" >&2 || true
	fi
	exit 1
}

cleanup() {
	local pid="${broker_pid:-}"
	if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
		kill "$pid" 2>/dev/null || true
		sleep 0.5
	fi
	if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
		kill -9 "$pid" 2>/dev/null || true
	fi
	if [ -n "$pid" ]; then
		wait "$pid" 2>/dev/null || true
	fi
	rm -f /tmp/embarlet_*_ready 2>/dev/null || true
}

wait_ready() {
	local pid="$1"
	local timeout="$2"
	local ready_file="/tmp/embarlet_${pid}_ready"
	for _ in $(seq 1 "$timeout"); do
		if [ -f "$ready_file" ]; then
			rm -f "$ready_file"
			return 0
		fi
		if ! kill -0 "$pid" 2>/dev/null; then
			return 1
		fi
		sleep 1
	done
	return 1
}

wait_log() {
	local pattern="$1"
	local timeout="$2"
	for _ in $(seq 1 "$timeout"); do
		if grep -q "$pattern" "$OUT_DIR/broker_0.log" 2>/dev/null; then
			return 0
		fi
		if [ -n "${broker_pid:-}" ] && ! kill -0 "$broker_pid" 2>/dev/null; then
			return 1
		fi
		sleep 1
	done
	return 1
}

run_test_body() {
	mkdir -p "$OUT_DIR"
	mkdir -p "$OUT_DIR/disk"
	cd "$OUT_DIR"
	: > broker_0.log
	: > client_fenced.log
	: > client_unaffected.log
	rm -f /tmp/embarlet_*_ready 2>/dev/null || true

	[ -x "$BIN_DIR/embarlet" ] || fail "missing $BIN_DIR/embarlet"
	[ -x "$BIN_DIR/throughput_test" ] || fail "missing $BIN_DIR/throughput_test"
	[ -f "$CONFIG_DIR/embarcadero.yaml" ] || fail "missing broker config"
	[ -f "$CONFIG_DIR/client.yaml" ] || fail "missing client config"

	env \
		NUM_BROKERS=1 \
		EMBARCADERO_NUM_BROKERS=1 \
		EMBAR_USE_HUGETLB=0 \
		EMBARCADERO_CXL_ZERO_MODE=metadata \
		EMBARCADERO_REPLICA_DISK_DIRS="$OUT_DIR/disk" \
		EMBARCADERO_SESSION_EPOCH="$BROKER_ENV_SESSION_EPOCH" \
		EMBARCADERO_SESSION_LEASE_MS="$LEASE_MS" \
		EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1 \
		EMBARCADERO_TEST_ORDER5_CLAIMED_WAIT_MS="$CLAIMED_WAIT_MS" \
		EMBARCADERO_TEST_ORDER5_STUCK_CLAIMED_BATCH_SEQ=0 \
		EMBARCADERO_TEST_ORDER5_STUCK_CLAIMED_SESSION_EPOCH="$SESSION_EPOCH" \
		"$BIN_DIR/embarlet" --config "$CONFIG_DIR/embarcadero.yaml" --head --EMBARCADERO \
		> broker_0.log 2>&1 &
	broker_pid="$!"
	trap cleanup EXIT INT TERM

	wait_ready "$broker_pid" 120 || fail "broker failed readiness"
	sleep 1

	set +e
	env NUM_BROKERS=1 \
		EMBARCADERO_NUM_BROKERS=1 \
		EMBAR_USE_HUGETLB=0 \
		EMBARCADERO_ACK_TIMEOUT_SEC=10 \
		"$BIN_DIR/throughput_test" \
			--config "$CONFIG_DIR/client.yaml" \
			-n 1 -m "$MESSAGE_SIZE" -s "$FIRST_TOTAL_BYTES" -t 5 -o 5 -a 1 \
			--sequencer EMBARCADERO --head_addr 127.0.0.1 -l 0 -r 0 \
			> client_fenced.log 2>&1
	local fenced_client_rc=$?
	set -e

	wait_log "\\[ORDER5_TEST_STUCK_CLAIMED_INJECT\\]" 10 ||
		fail "stuck claimed injection did not fire"
	wait_log "\\[ORDER5_TEST_SKIP_MARKER_PUSHED\\].*num_msg=0" 20 ||
		fail "scanner did not push targeted num_msg=0 skip marker"
	wait_log "\\[ORDER5_TARGETED_SKIP\\].*session_epoch=$SESSION_EPOCH" 20 ||
		fail "targeted scanner skip did not reach Level5"
	wait_log "\\[ORDER5_SESSION_FENCE\\].*session_epoch=$SESSION_EPOCH" 20 ||
		fail "session fence did not fire"
	wait_log "\\[ORDER5_FENCED_SUFFIX_DROP\\].*session_epoch=$SESSION_EPOCH" 20 ||
		fail "fenced suffix was not dropped by the live pipeline"
	if ! grep -q "\\[SESSION_FENCED_OBSERVED\\]" client_fenced.log; then
		fail "fenced client did not observe SessionFenced control"
	fi
	if ! grep -q "\\[SESSION_OPEN_ACK\\].*assigned_session_epoch=$SESSION_EPOCH" client_fenced.log; then
		fail "client did not complete SessionOpen/SessionOpenAck handshake"
	fi
	local next_epoch=$((SESSION_EPOCH + 1))
	if ! grep -q "\\[SESSION_OPEN_ACK\\].*assigned_session_epoch=$next_epoch" client_fenced.log; then
		fail "client did not reopen at incremented session epoch $next_epoch"
	fi
	if ! grep -q "\\[SESSION_OPEN\\].*requested_session_epoch=$SESSION_EPOCH.*assigned_session_epoch=$SESSION_EPOCH" broker_0.log; then
		fail "broker env epoch overrode real SessionOpen request"
	fi
	if ! grep -q "\\[SESSION_REOPEN_RESUBMIT\\]" client_fenced.log; then
		fail "client did not reopen session and resubmit suffix"
	fi
	if [ "$fenced_client_rc" -ne 0 ]; then
		fail "fenced client did not recover and complete after SessionFenced"
	fi
	grep -q "\\[ACK_VERIFY\\].*100%" client_fenced.log ||
		fail "fenced client did not receive complete ACK1 progress after reopen"

	local fence_count
	fence_count=$(grep -c "\\[ORDER5_SESSION_FENCE\\].*session_epoch=$SESSION_EPOCH" broker_0.log || true)
	[ "$fence_count" -eq 1 ] || fail "expected exactly one session fence, saw $fence_count"

	local drop_line drop_client drop_seq
	drop_line=$(grep "\\[ORDER5_FENCED_SUFFIX_DROP\\].*session_epoch=$SESSION_EPOCH" broker_0.log | head -1)
	drop_client=$(printf '%s\n' "$drop_line" | sed -n 's/.*client=\([0-9][0-9]*\).*/\1/p')
	drop_seq=$(printf '%s\n' "$drop_line" | sed -n 's/.*batch_seq=\([0-9][0-9]*\).*/\1/p')
	[ -n "$drop_client" ] || fail "could not parse dropped client from: $drop_line"
	[ -n "$drop_seq" ] || fail "could not parse dropped batch_seq from: $drop_line"
	if grep -q "\\[ORDER5_TEST_GOI_COMMIT\\].*client=$drop_client.*session_epoch=$SESSION_EPOCH.*batch_seq=$drop_seq" broker_0.log; then
		fail "dropped suffix client=$drop_client batch_seq=$drop_seq was committed to GOI"
	fi
	local duplicate_commits
	duplicate_commits=$(awk -v client="$drop_client" '
		/ORDER5_TEST_GOI_COMMIT/ && $0 ~ ("client=" client) {
			epoch = ""; seq = "";
			for (i = 1; i <= NF; i++) {
				if ($i ~ /^session_epoch=/) { split($i, a, "="); epoch = a[2]; }
				if ($i ~ /^batch_seq=/) { split($i, b, "="); seq = b[2]; }
			}
			if (epoch != "" && seq != "") {
				key = epoch ":" seq;
				count[key]++;
			}
		}
		END {
			dups = 0;
			for (k in count) {
				if (count[k] > 1) dups++;
			}
			print dups;
		}' broker_0.log)
	[ "$duplicate_commits" -eq 0 ] ||
		fail "duplicate broker-side GOI commits detected for client=$drop_client"

	env NUM_BROKERS=1 \
		EMBARCADERO_NUM_BROKERS=1 \
		EMBAR_USE_HUGETLB=0 \
		"$BIN_DIR/throughput_test" \
			--config "$CONFIG_DIR/client.yaml" \
			-n 1 -m "$MESSAGE_SIZE" -s "$SECOND_TOTAL_BYTES" -t 5 -o 5 -a 1 \
			--sequencer EMBARCADERO --head_addr 127.0.0.1 -l 0 -r 0 \
			> client_unaffected.log 2>&1 || fail "unaffected second session failed"

	grep -q "\\[ACK_VERIFY\\].*100%" client_unaffected.log ||
		fail "second session did not receive complete ACK1 progress"
	if grep -q "W1\\.2_COMMIT_ORDER_VIOLATION\\|COMMIT_ORDER_VIOLATION" broker_0.log; then
		fail "commit-order violation detected"
	fi

	echo "PASS live ORDER5 fence-fire: fence_count=$fence_count dropped_client=$drop_client dropped_batch_seq=$drop_seq"
}

exec 9>"$LOCK_FILE"
flock 9
run_test_body
