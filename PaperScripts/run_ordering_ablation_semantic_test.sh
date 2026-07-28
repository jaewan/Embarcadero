#!/usr/bin/env bash
# Fail-closed semantic gate for the ORDER=5 session-FIFO cost ablation.
#
# A delayed predecessor forces two batches across seal boundaries. Normal
# ORDER=5 must hold the successor and pass the apply-order checker. The
# otherwise identical bypass mode must publish the successor first and fail
# specifically on session_fifo_apply_order.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

OUT_ROOT="${OUT_ROOT:-$ROOT/data/paper_eval/fig1/ordering_ablation_semantic_$(date -u +%Y%m%dT%H%M%SZ)}"
[[ ! -e "$OUT_ROOT" ]] || {
  echo "ERROR: output root already exists: $OUT_ROOT" >&2
  exit 2
}
mkdir -p "$OUT_ROOT"

dirty_count="$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')"
if [[ "$dirty_count" != 0 && "${ALLOW_DIRTY_SEMANTIC_TEST:-0}" != 1 ]]; then
  echo "ERROR: tracked worktree is dirty; commit before producing semantic evidence" >&2
  exit 2
fi

TRIALS="${TRIALS:-1}"
GAP_DELAY_MS="${GAP_DELAY_MS:-250}"
GAP_BATCH_SEQ="${GAP_BATCH_SEQ:-2}"
GAP_PERIOD_BATCHES="${GAP_PERIOD_BATCHES:-4}"
OPS="${OPS:-8000}"
KEYS="${KEYS:-1000}"
WARMUP_OPS="${WARMUP_OPS:-1000}"
CLIENT_BATCH_KB="${CLIENT_BATCH_KB:-64}"
EPOCH_US="${EPOCH_US:-500}"

commit="$(git rev-parse HEAD)"
embarlet_sha="$(sha256sum build/bin/embarlet | awk '{print $1}')"
client_sha="$(sha256sum build/bin/kv_ycsb_bench | awk '{print $1}')"
printf 'mode,trial,valid,session_reorders,failed_checks,published,applied,head_marker\n' \
  > "$OUT_ROOT/semantic_test_summary.csv"

run_mode() {
  local mode="$1" bypass="$2" trial="$3"
  local cell="$OUT_ROOT/${mode}_trial${trial}"
  mkdir -p "$cell"
  echo ">>> semantic gate mode=$mode bypass=$bypass trial=$trial"

  EMBARCADERO_ORDER5_BYPASS_SESSION_FIFO_ABLATION="$bypass" \
  EMBARCADERO_ORDER5_GAP_DELAY_MS="$GAP_DELAY_MS" \
  EMBARCADERO_ORDER5_GAP_BATCH_SEQ="$GAP_BATCH_SEQ" \
  EMBARCADERO_ORDER5_GAP_PERIOD_BATCHES="$GAP_PERIOD_BATCHES" \
  EMBARCADERO_CLIENT_PUB_BATCH_KB="$CLIENT_BATCH_KB" \
  EMBARCADERO_SESSION_LEASE_MS=30000 \
  EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=30000 \
  EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1 \
  EMBAR_ORDER5_EPOCH_US="$EPOCH_US" \
  SMR_FIFO_SEQUENCERS=EMBARCADERO \
  SMR_FIFO_MODES=pipe \
  SMR_FIFO_NUM_TRIALS=1 \
  SMR_FIFO_SESSIONS=1 \
  SMR_FIFO_RF=1 \
  SMR_FIFO_ACK=1 \
  SMR_FIFO_RECORD_COUNT="$KEYS" \
  SMR_FIFO_OPERATION_COUNT="$OPS" \
  SMR_FIFO_WARMUP_OPS="$WARMUP_OPS" \
  SMR_FIFO_PUB_THREADS=1 \
  BROKER_READY_TIMEOUT_SEC=300 \
  BENCH_TIMEOUT_SEC=180 \
  OUT_ROOT="$cell" \
  bash benchmarks/kv_store/run_smr_fifo_eval.sh \
    > "$cell/driver.log" 2>&1

  local summary
  summary="$(find "$cell" -path '*/summary.csv' -type f ! -path "$cell/summary.csv" -print -quit)"
  [[ -n "$summary" && -s "$summary" ]] || {
    echo "ERROR: $mode trial $trial produced no per-run summary" >&2
    return 1
  }
  cp build/bin/broker_0.log "$cell/head_broker.log"
  local client_log="$cell/EMBARCADERO_pipe_trial1_s0.log"
  [[ -s "$client_log" ]] || {
    echo "ERROR: $mode trial $trial produced no publisher log" >&2
    return 1
  }

  python3 - "$summary" "$mode" "$bypass" "$trial" "$cell/head_broker.log" \
    "$client_log" "$GAP_BATCH_SEQ" "$GAP_DELAY_MS" "$GAP_PERIOD_BATCHES" \
    "$OUT_ROOT/semantic_test_summary.csv" <<'PY'
import csv
import pathlib
import re
import sys

summary, mode, bypass, trial, broker_log, client_log, gap_seq, gap_ms, gap_period, aggregate = sys.argv[1:]
with open(summary, newline="") as handle:
    rows = list(csv.DictReader(handle))
if len(rows) != 1:
    raise SystemExit(f"{summary}: expected one result row, got {len(rows)}")
row = rows[0]
valid = int(row["valid"])
reorders = int(row["session_reorders"])
failed = row["failed_checks"]
published = int(row["published_entries"])
applied = int(row["applied_entries"])
log = pathlib.Path(broker_log).read_text(errors="replace")
publisher_log = pathlib.Path(client_log).read_text(errors="replace")
marker = int("[ORDER5_SESSION_FIFO_ABLATION]" in log)
effective = f"order5_fifo_ablation={bypass}"
if effective not in log:
    raise SystemExit(f"{broker_log}: missing {effective}")
if published != applied:
    raise SystemExit(f"{mode}: published={published} applied={applied}")
starts = [
    (int(seq), int(delay))
    for seq, delay in re.findall(
        r"\[ORDER5_GAP_INJECT\] phase=start batch_seq=(\d+) delay_ms=(\d+)",
        publisher_log,
    )
]
ends = [
    int(seq)
    for seq in re.findall(
        r"\[ORDER5_GAP_INJECT\] phase=end batch_seq=(\d+)", publisher_log
    )
]
minimum = 2 if int(gap_period) > 0 else 1
start_seqs = [seq for seq, _ in starts]
expected_seqs = [
    int(gap_seq) + i * int(gap_period) for i in range(len(starts))
] if int(gap_period) > 0 else [int(gap_seq)]
if (
    len(starts) < minimum
    or start_seqs != ends
    or start_seqs != expected_seqs
    or any(delay != int(gap_ms) for _, delay in starts)
):
    raise SystemExit(
        f"{client_log}: expected at least {minimum} complete forced-gap intervals "
        f"from batch_seq={gap_seq}, period={gap_period}, delay_ms={gap_ms}; "
        f"starts={starts} ends={ends}"
    )
if mode == "normal":
    if valid != 1 or reorders != 0 or marker != 0:
        raise SystemExit(
            f"normal ORDER=5 must be valid with zero reorders and no marker: "
            f"valid={valid} reorders={reorders} marker={marker}"
        )
elif mode == "bypass":
    if valid != 0 or reorders <= 0 or marker != 1:
        raise SystemExit(
            f"bypass must be invalid with observed reorders and marker: "
            f"valid={valid} reorders={reorders} marker={marker}"
        )
    if "session_fifo_apply_order" not in failed:
        raise SystemExit(f"bypass failed for wrong reason(s): {failed}")
else:
    raise SystemExit(f"unexpected mode {mode}")
with open(aggregate, "a", newline="") as handle:
    csv.writer(handle, lineterminator="\n").writerow(
        [mode, trial, valid, reorders, failed, published, applied, marker]
    )
PY
}

for trial in $(seq 1 "$TRIALS"); do
  run_mode normal 0 "$trial"
  run_mode bypass 1 "$trial"
done

python3 - "$OUT_ROOT" "$commit" "$dirty_count" "$embarlet_sha" "$client_sha" \
  "$TRIALS" "$GAP_DELAY_MS" "$GAP_BATCH_SEQ" "$GAP_PERIOD_BATCHES" \
  "$OPS" "$KEYS" "$CLIENT_BATCH_KB" "$EPOCH_US" <<'PY'
import csv
import hashlib
import json
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
commit, dirty, embarlet, client = sys.argv[2:6]
trials, gap_ms, gap_seq, gap_period, ops, keys, batch_kb, epoch_us = map(int, sys.argv[6:])
summary = root / "semantic_test_summary.csv"
rows = list(csv.DictReader(summary.open(newline="")))
if len(rows) != 2 * trials:
    raise SystemExit(f"expected {2 * trials} semantic rows, got {len(rows)}")
def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
artifact_hashes = {
    str(path.relative_to(root)): sha(path)
    for path in sorted(root.rglob("*"))
    if path.is_file()
    and path.name not in {"semantic_test_manifest.json", "SHA256SUMS"}
}
manifest = {
    "schema": 1,
    "status": "pass",
    "claim": "normal ORDER=5 holds a cross-seal successor; the matched bypass publishes it out of session order",
    "git_commit": commit,
    "git_dirty_files": int(dirty),
    "binaries": {"embarlet_sha256": embarlet, "kv_ycsb_bench_sha256": client},
    "contract": {
        "trials_per_mode": trials,
        "gap_delay_ms": gap_ms,
        "gap_batch_seq": gap_seq,
        "gap_period_batches": gap_period,
        "operations": ops,
        "keys": keys,
        "client_batch_kb": batch_kb,
        "epoch_us": epoch_us,
    },
    "summary_csv": str(summary),
    "summary_sha256": sha(summary),
    "rows": rows,
    "artifacts": artifact_hashes,
}
(root / "semantic_test_manifest.json").write_text(
    json.dumps(manifest, indent=2, sort_keys=True) + "\n"
)
PY

(
  cd "$OUT_ROOT"
  find . -type f ! -name SHA256SUMS -print0 |
    sort -z |
    xargs -0 sha256sum > SHA256SUMS
)
echo "PASS: semantic ablation gate -> $OUT_ROOT"
