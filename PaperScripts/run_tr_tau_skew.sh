#!/usr/bin/env bash
# Task 1 (T/tau vs T/R): 2-session ORDER=5 skew experiment.
#
# One "affected" session receives a periodic ~D ms predecessor delay (a held gap);
# one independent "control" session publishes continuously. NO broker kill. The
# broker runs with EMBAR_ORDER5_TR_TRACE=1 so it emits, per broker process, a CSV
# of {seal (tau), scan_pass (P), gap_detect, gap_release, commit} events. Sweeping
# tau shows that a held suffix is released only at epoch seals: seals-during-gap
# ~= T/tau (NOT T/P scanner passes).
#
# Uses the vetted run_multiclient.sh cluster/barrier lifecycle. Co-located clients
# (the sequencer hold/release mechanism is host-local). Graceful is not required
# for the CSV: the tracer flushes incrementally per producer thread.
#
#   TAUS="250 500 1000" TRIALS=3 bash PaperScripts/run_tr_tau_skew.sh
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TAUS="${TAUS:-250 500 1000}"
TRIALS="${TRIALS:-3}"
GAP_MS="${GAP_MS:-2}"                 # ~1.5 ms target; integer-ms hook -> 2 ms (measured T reported from data)
GAP_START="${GAP_START:-200}"          # first gap batch_seq (after warmup)
GAP_PERIOD="${GAP_PERIOD:-200}"        # re-inject a gap every N batches -> many samples
MSG_SIZE="${MSG_SIZE:-1024}"
TOTAL_BYTES="${TOTAL_BYTES:-$((4*1024*1024*1024))}"   # 2 GiB/session
TARGET_MBPS="${TARGET_MBPS:-200}"
OUT_ROOT="${OUT_ROOT:-$ROOT/data/paper_eval/tr_tau_task1/campaign}"
mkdir -p "$OUT_ROOT"
echo "commit=$(git rev-parse HEAD) dirty=[$(git status --porcelain --untracked-files=no | head -1)]" > "$OUT_ROOT/run_provenance.txt"
echo "embarlet_sha256=$(sha256sum build/bin/embarlet|awk '{print $1}')" >> "$OUT_ROOT/run_provenance.txt"
echo "throughput_test_sha256=$(sha256sum build/bin/throughput_test|awk '{print $1}')" >> "$OUT_ROOT/run_provenance.txt"

run_cell() {
  local tau="$1" trial="$2"
  local cell="$OUT_ROOT/tau${tau}_trial${trial}"
  rm -rf "$cell"; mkdir -p "$cell/mc_logs"
  echo ">>> tau=${tau}us trial=${trial} ($(date -u +%H:%M:%SZ)) -> $cell"
  env \
    SEQUENCER=EMBARCADERO ORDER=5 ACK=1 REPLICATION_FACTOR=1 \
    NUM_TRIALS=1 \
    NUM_CLIENTS=2 CLIENT_HOSTS_CSV="local,local" CLIENT_NUMAS_CSV="0,0" \
    NUM_BROKERS=4 \
    MESSAGE_SIZE="$MSG_SIZE" TOTAL_MESSAGE_SIZE="$TOTAL_BYTES" \
    CLIENT_EXTRA_ARGS="--target_mbps $TARGET_MBPS --steady_rate" \
    CLIENT_ORDER5_GAP_DELAYS_MS_PIPE="${GAP_MS}|0" \
    CLIENT_ORDER5_GAP_BATCH_SEQS_PIPE="${GAP_START}|0" \
    EMBARCADERO_ORDER5_GAP_PERIOD_BATCHES="$GAP_PERIOD" \
    EMBARCADERO_TEST_ORDER5_SESSION_TRACE=1 \
    EMBARCADERO_SESSION_LEASE_MS=180000 \
    EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=180000 \
    EMBAR_ORDER5_EPOCH_US="$tau" \
    EMBAR_ORDER5_TR_TRACE=1 \
    EMBAR_ORDER5_TR_TRACE_CSV="$cell/tr" \
    EMBARCADERO_CXL_SIZE=77309411328 \
    EMBARCADERO_CXL_ZERO_MODE=metadata \
    BROKER_IP=10.10.10.10 EMBARCADERO_HEAD_ADDR=10.10.10.10 \
    ALLOW_DIRTY_ARTIFACT=1 \
    LOG_DIR="$cell/mc_logs" \
    BENCHMARK_TAG="tr_tau_${tau}_t${trial}" \
    bash scripts/run_multiclient.sh > "$cell/driver.log" 2>&1
  local rc=$?
  # tracer files (head broker process carries the data)
  local trfiles
  trfiles=$(ls "$cell"/tr.pid*.t* 2>/dev/null | wc -l)
  local rows
  rows=$(cat "$cell"/tr.pid*.t* 2>/dev/null | grep -cvE '^type,' || echo 0)
  local seals gaps_d gaps_r commits
  seals=$(cat "$cell"/tr.pid*.t* 2>/dev/null | grep -c '^seal,' || echo 0)
  gaps_d=$(cat "$cell"/tr.pid*.t* 2>/dev/null | grep -c '^gap_detect,' || echo 0)
  gaps_r=$(cat "$cell"/tr.pid*.t* 2>/dev/null | grep -c '^gap_release,' || echo 0)
  commits=$(cat "$cell"/tr.pid*.t* 2>/dev/null | grep -c '^commit,' || echo 0)
  echo "    rc=$rc trace_files=$trfiles rows=$rows seals=$seals gap_detect=$gaps_d gap_release=$gaps_r commits=$commits"
}

for tau in $TAUS; do
  for t in $(seq 1 "$TRIALS"); do
    run_cell "$tau" "$t"
    # bounded settle + own-shm cleanup between cells (shared host: only our prefix)
    sleep 2
    for g in /dev/shm/CXL_SHARED_EXPERIMENT_${UID}*; do [[ -e "$g" ]] && rm -f "$g"; done 2>/dev/null || true
  done
done
echo "Done. $OUT_ROOT"
