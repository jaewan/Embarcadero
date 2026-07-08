#!/bin/bash
# Sub-phase 3B review fix (BLOCKER-1): rerun the backlogged-kill scenario with the FIXED
# rdma_recovery verify tool (programmatic ring-seam exclusion via --pbr-slots/--seam-final-seq),
# to demonstrate a clean AUTOMATED PASS (no hand-waved "1 mismatch = PASS") when a real ring seam
# is present. broker0=moscxl, broker1=c1, memserver=c3. Same params as the original leg-1 item1/2
# script, except: (a) the recovery step now uses --dump-file instead of inline verify (the
# sentinel/seam value isn't known until the memserver's own end-of-run dump, which happens AFTER
# the timed recovery step), (b) after all processes exit, parse ms.log for
# "sentinel[broker=0]=<N>" and run a second, --verify-only (no RDMA) pass against the dumped bytes
# with the seam programmatically excluded.
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

MS_PORT=18681
RECOVERY_PORT=18698
BLOG_PORT=18745
REPLICA_PORT_B0=18758
REPLICA_PORT_B1=18759
PBR_SLOTS=200000
DURATION=20
KILL_AT=4
SPARE_BYTES=268435456  # 256 MiB
RECOVERY_BYTES=40960000   # 10000 slots * 4096 bytes/slot
VERIFY_SLOTS=10000
VERIFY_SLOT_BYTES=4096

ssh -o BatchMode=yes c3 "$C3_BIN/rdma_memserver --port=$MS_PORT --num-brokers=2 --pbr-slots=$PBR_SLOTS \
  --goi-entries=2000000 --spare-bytes=$SPARE_BYTES --recovery-port=$RECOVERY_PORT \
  --max-recoveries=1 --duration=$((DURATION+20))" > "$OUT/ms.log" 2>&1 &
MSPID=$!
sleep 3

$BIN/w5_broker_dram --broker-id=0 --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 \
  --pbr-slots=$PBR_SLOTS --message-size=4096 --target-mbps=0 --duration=$DURATION --inflight=32 \
  --blog-port=$BLOG_PORT --replica-port=$REPLICA_PORT_B0 --right-neighbor-ip=$C1_IP \
  --right-neighbor-replica-port=$REPLICA_PORT_B1 > "$OUT/broker0.log" 2>&1 &
B0PID=$!

ssh -o BatchMode=yes c1 "$C1_BIN/w5_broker_dram --broker-id=1 --memserver-ip=$C3_IP --memserver-port=$MS_PORT \
  --num-brokers=2 --pbr-slots=$PBR_SLOTS --message-size=4096 --target-mbps=0 --duration=$DURATION \
  --inflight=32 --blog-port=$BLOG_PORT --replica-port=$REPLICA_PORT_B1 --right-neighbor-ip=$MOSCXL_IP \
  --right-neighbor-replica-port=$REPLICA_PORT_B0 --replica-max-accepts=2" > "$OUT/broker1.log" 2>&1 &
B1PID=$!

sleep 2
$BIN/w5_sequencer_dram --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 --pbr-slots=$PBR_SLOTS \
  --duration=$DURATION --broker-ips=$MOSCXL_IP,$C1_IP --blog-ports=$BLOG_PORT,$BLOG_PORT \
  > "$OUT/seq.log" 2>&1 &
SEQPID=$!

echo "[$(date -u +%FT%TZ)] seamfix trial $TRIAL: launched, memserver(c3)=$MSPID broker0(moscxl)=$B0PID seq(moscxl)=$SEQPID" >> "$OUT/timeline.log"

sleep $KILL_AT
KILL_T_MONO=$(date +%s.%N)
echo "$KILL_T_MONO" > "$OUT/kill_time"
kill -9 $B0PID
echo "[$(date -u +%FT%TZ)] seamfix trial $TRIAL: KILLED broker0 pid=$B0PID at kill_t_mono=$KILL_T_MONO" >> "$OUT/timeline.log"

for i in $(seq 1 20); do
  if grep -q "declared dead" "$OUT/seq.log" 2>/dev/null; then break; fi
  sleep 0.25
done
grep "declared dead\|RDMA_ERROR\|LEASE_TIMEOUT" "$OUT/seq.log" >> "$OUT/timeline.log" 2>/dev/null

# Pass 1 (timed, on the critical path): move the bytes for real, ALSO dump a local copy for
# offline re-scoring once the seam is known. No inline verify here (seam not known yet).
$BIN/rdma_recovery --source-ip=$C1_IP --source-port=$REPLICA_PORT_B1 --target-ip=$C3_IP \
  --target-port=$RECOVERY_PORT --bytes=$RECOVERY_BYTES --dump-file="$OUT/recovered.bin" \
  > "$OUT/recovery_move.log" 2>&1
echo "recovery_move_rc=$?" >> "$OUT/timeline.log"

wait $B1PID $SEQPID $MSPID
echo "[$(date -u +%FT%TZ)] seamfix trial $TRIAL: all processes exited" >> "$OUT/timeline.log"

# Pass 2 (offline, no RDMA): now that memserver has dumped its end-of-run sentinel array, extract
# the real final published sequence number for broker 0 and re-score the dumped bytes with the
# ring seam programmatically excluded.
FINAL_SEQ=$(grep -oP 'sentinel\[broker=0\]=\K[0-9]+' "$OUT/ms.log")
echo "final_global_seq_broker0=$FINAL_SEQ" >> "$OUT/timeline.log"
$BIN/rdma_recovery --bytes=$RECOVERY_BYTES --verify-only="$OUT/recovered.bin" \
  --verify-slot-bytes=$VERIFY_SLOT_BYTES --verify-slots=$VERIFY_SLOTS \
  --pbr-slots=$PBR_SLOTS --seam-final-seq=$FINAL_SEQ \
  > "$OUT/recovery_verify.log" 2>&1
echo "recovery_verify_rc=$?" >> "$OUT/timeline.log"

rm -f "$OUT/recovered.bin"   # raw payload bytes, not needed once scored -- keep the archive small
echo TRIAL_DONE
