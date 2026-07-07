#!/bin/bash
# Phase 2: W5-A host-kill leg-1 cost. broker0=moscxl, broker1=c1, memserver=c3.
# Usage: w5_phase2_trial.sh <trial_num> <out_dir>
set -u
TRIAL=$1
OUT=$2
mkdir -p "$OUT"

BIN=~/Embarcadero-sessions/w5-variants/src/rdma_transport/build_rdma/bin
C1_BIN=/tmp/w5v/build_rdma/bin
C3_BIN=/tmp/w5v/build_rdma/bin
MOSCXL_IP=10.10.10.10
C1_IP=10.10.10.11
C3_IP=10.10.10.181

MS_PORT=18670
RECOVERY_PORT=18695
BLOG_PORT=18740
REPLICA_PORT_B0=18750
REPLICA_PORT_B1=18751
TARGET_MBPS=206   # ~60% of measured single-broker unthrottled peak (2.749 Gb/s -> ~344 MB/s peak)
DURATION=30
KILL_AT=10
SPARE_BYTES=268435456  # 256 MiB

ssh -o BatchMode=yes c3 "$C3_BIN/rdma_memserver --port=$MS_PORT --num-brokers=2 --pbr-slots=16384 \
  --goi-entries=2000000 --spare-bytes=$SPARE_BYTES --recovery-port=$RECOVERY_PORT \
  --max-recoveries=1 --duration=$((DURATION+20))" > "$OUT/ms.log" 2>&1 &
MSPID=$!
sleep 3

$BIN/w5_broker_dram --broker-id=0 --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 \
  --pbr-slots=16384 --message-size=4096 --target-mbps=$TARGET_MBPS --duration=$DURATION --inflight=32 \
  --blog-port=$BLOG_PORT --replica-port=$REPLICA_PORT_B0 --right-neighbor-ip=$C1_IP \
  --right-neighbor-replica-port=$REPLICA_PORT_B1 > "$OUT/broker0.log" 2>&1 &
B0PID=$!

ssh -o BatchMode=yes c1 "$C1_BIN/w5_broker_dram --broker-id=1 --memserver-ip=$C3_IP --memserver-port=$MS_PORT \
  --num-brokers=2 --pbr-slots=16384 --message-size=4096 --target-mbps=$TARGET_MBPS --duration=$DURATION \
  --inflight=32 --blog-port=$BLOG_PORT --replica-port=$REPLICA_PORT_B1 --right-neighbor-ip=$MOSCXL_IP \
  --right-neighbor-replica-port=$REPLICA_PORT_B0 --replica-max-accepts=2" > "$OUT/broker1.log" 2>&1 &
B1PID=$!

sleep 2
$BIN/w5_sequencer_dram --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 --pbr-slots=16384 \
  --duration=$DURATION --broker-ips=$MOSCXL_IP,$C1_IP --blog-ports=$BLOG_PORT,$BLOG_PORT \
  > "$OUT/seq.log" 2>&1 &
SEQPID=$!

echo "[$(date -u +%FT%TZ)] trial $TRIAL: launched, memserver(c3)=$MSPID broker0(moscxl)=$B0PID seq(moscxl)=$SEQPID" >> "$OUT/timeline.log"

sleep $KILL_AT
KILL_T_MONO=$(date +%s.%N)
echo "$KILL_T_MONO" > "$OUT/kill_time"
kill -9 $B0PID
echo "[$(date -u +%FT%TZ)] trial $TRIAL: KILLED broker0 pid=$B0PID (host kill via kill -9; anonymous "\
"private DRAM Blog -- process death makes those bytes genuinely unrecoverable, no shared/file-backed "\
"substrate to recover from; kernel atomically tears down its QPs/MRs too, so the sequencer's next op "\
"gets a real completion-level failure, not a soft timeout)" >> "$OUT/timeline.log"

# Wait for detection (poll seq.log), up to 5s.
for i in $(seq 1 20); do
  if grep -q "declared dead" "$OUT/seq.log" 2>/dev/null; then break; fi
  sleep 0.25
done
grep "declared dead" "$OUT/seq.log" >> "$OUT/timeline.log" 2>/dev/null

# Recovery: broker1 (c1, survivor) holds replica-of-broker0. Read broker0's last-committed volume
# (approximated as message_size * batches broker0 reported before death; use its own log if present,
# else a generous fixed size within the replica region capacity).
RECOVERY_BYTES=134217728  # 128 MiB fixed probe size (well within the 1GiB replica region and 256MiB spare)
$BIN/rdma_recovery --source-ip=$C1_IP --source-port=$REPLICA_PORT_B1 --target-ip=$C3_IP \
  --target-port=$RECOVERY_PORT --bytes=$RECOVERY_BYTES > "$OUT/recovery.log" 2>&1
echo "recovery_rc=$?" >> "$OUT/timeline.log"

wait $B1PID $SEQPID $MSPID
echo "[$(date -u +%FT%TZ)] trial $TRIAL: all processes exited" >> "$OUT/timeline.log"
echo TRIAL_DONE
