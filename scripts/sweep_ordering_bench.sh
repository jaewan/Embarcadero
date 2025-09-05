#!/usr/bin/env bash
set -uo pipefail

BIN_DIR="/home/domin/Embarcadero/build/bin"
OUT_DIR="/home/domin/Embarcadero/data/order_bench"
mkdir -p "$OUT_DIR"

SUMMARY_CSV="$OUT_DIR/sweep_summary.csv"
THREADS_CSV="$OUT_DIR/sweep_threads.csv"

# Write headers if files are new
if [ ! -f "$SUMMARY_CSV" ]; then
  echo "brokers,clients_per_broker,message_size,batch_size,pattern,gap_ratio,dup_ratio,target_msgs_per_s,throughput_avg,total_batches,total_ordered,total_skipped,total_dups,atomic_fetch_add,claimed_msgs,total_lock_ns,total_assign_ns,flush" > "$SUMMARY_CSV"
fi
if [ ! -f "$THREADS_CSV" ]; then
  echo "brokers,broker,clients_per_broker,message_size,batch_size,pattern,gap_ratio,dup_ratio,target_msgs_per_s,num_seen,num_ordered,num_skipped,num_dups,fetch_add,claimed_msgs,lock_ns,assign_ns,flush" > "$THREADS_CSV"
fi

# Parameters
CLIENTS_PER_BROKER=${CLIENTS_PER_BROKER:-1}
MSG_SIZE=${MSG_SIZE:-1024}
PATTERN=${PATTERN:-gaps}
GAP=${GAP:-0.2}
DUP=${DUP:-0.02}
TARGET=${TARGET:-1250}
WARMUP=${WARMUP:-2}
DURATION=${DURATION:-10}
REPEATS=${REPEATS:-1}
MAX_BROKERS=${MAX_BROKERS:-32}
# Optional sequencer pinning (comma-separated list). Empty disables.
PIN_SEQ_CPUS=${PIN_SEQ_CPUS:-}
# Headers-only fast path and synthetic memory toggle for scalability sweeps
HEADERS_ONLY=${HEADERS_ONLY:-1}
USE_REAL_CXL=${USE_REAL_CXL:-0}

cd "$BIN_DIR"

for FLUSH in 0 1; do
  for B in $(seq 1 $MAX_BROKERS); do
    for R in $(seq 1 $REPEATS); do
      SUM_TMP="$OUT_DIR/tmp_summary.csv"
      THR_TMP="$OUT_DIR/tmp_threads.csv"
      rm -f "$SUM_TMP" "$THR_TMP"
      set +e
      CMD=(./order_micro_bench \
        --brokers=$B \
        --clients_per_broker=$CLIENTS_PER_BROKER \
        --message_size=$MSG_SIZE \
        --pattern=$PATTERN \
        --gap_ratio=$GAP \
        --dup_ratio=$DUP \
        --target_msgs_per_s=$TARGET \
        --warmup_s=$WARMUP \
        --duration_s=$DURATION \
        --flush_metadata=$FLUSH \
        --headers_only=$HEADERS_ONLY \
        --use_real_cxl=$USE_REAL_CXL \
        --csv_out="$SUM_TMP" \
        --per_thread_csv="$THR_TMP")
      if [ -n "$PIN_SEQ_CPUS" ]; then
        CMD+=(--pin_seq_cpus="$PIN_SEQ_CPUS")
      fi
      "${CMD[@]}" | cat
      RC=$?
      set -e
      if [ $RC -ne 0 ]; then
        echo "WARN: run failed for brokers=$B flush=$FLUSH repeat=$R (rc=$RC); continuing" >&2
        continue
      fi

      # Append flush and repeat to end of each CSV row
      if [ -f "$SUM_TMP" ]; then
        awk -v f=$FLUSH -v b=$B -v r=$R 'BEGIN{FS=","; OFS=","} { print $0, f }' "$SUM_TMP" >> "$SUMMARY_CSV"
        rm -f "$SUM_TMP"
      fi
      if [ -f "$THR_TMP" ]; then
        awk -v b=$B -v f=$FLUSH -v r=$R 'BEGIN{FS=","; OFS=","} { print b "," $0 "," f }' "$THR_TMP" >> "$THREADS_CSV"
        rm -f "$THR_TMP"
      fi
    done
  done
done

echo "Sweep complete. Summary: $SUMMARY_CSV  Threads: $THREADS_CSV"
