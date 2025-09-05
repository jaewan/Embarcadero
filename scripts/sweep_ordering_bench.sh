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
CLIENTS_PER_BROKER=1
MSG_SIZE=1024
PATTERN=gaps
GAP=0.2
DUP=0.02
TARGET=1250
WARMUP=2
DURATION=10

cd "$BIN_DIR"

for FLUSH in 0 1; do
  for B in $(seq 1 32); do
    SUM_TMP="$OUT_DIR/tmp_summary.csv"
    THR_TMP="$OUT_DIR/tmp_threads.csv"
    rm -f "$SUM_TMP" "$THR_TMP"
    set +e
    ./order_micro_bench \
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
      --csv_out="$SUM_TMP" \
      --per_thread_csv="$THR_TMP" | cat
    RC=$?
    set -e
    if [ $RC -ne 0 ]; then
      echo "WARN: run failed for brokers=$B flush=$FLUSH (rc=$RC); continuing" >&2
      continue
    fi

    # Append flush to end of each CSV row
    if [ -f "$SUM_TMP" ]; then
      awk -v f=$FLUSH 'BEGIN{FS=","; OFS=","} { print $0, f }' "$SUM_TMP" >> "$SUMMARY_CSV"
      rm -f "$SUM_TMP"
    fi
    if [ -f "$THR_TMP" ]; then
      awk -v b=$B -v f=$FLUSH 'BEGIN{FS=","; OFS=","} { print b "," $0 "," f }' "$THR_TMP" >> "$THREADS_CSV"
      rm -f "$THR_TMP"
    fi
  done
done

echo "Sweep complete. Summary: $SUMMARY_CSV  Threads: $THREADS_CSV"
