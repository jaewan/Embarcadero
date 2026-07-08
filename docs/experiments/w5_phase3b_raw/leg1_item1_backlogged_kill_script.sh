#!/bin/bash
# Sub-phase 3B leg-1 items 1+2: backlogged/loss-inducing kill + checksummed recovery.
# broker0=moscxl, broker1=c1, memserver=c3.
set -u
TRIAL=$1
OUT=$2
DURABLE_FLAG=${3:-}   # pass "--durable-replicate" for item 3, empty for item 1
mkdir -p "$OUT"

BIN=~/Embarcadero-sessions/w5-variants/src/rdma_transport/build_rdma/bin
C1_BIN=/tmp/w5v/build_rdma/bin
C3_BIN=/tmp/w5v/build_rdma/bin
MOSCXL_IP=10.10.10.10
C1_IP=10.10.10.11
C3_IP=10.10.10.181

MS_PORT=18671
RECOVERY_PORT=18696
BLOG_PORT=18741
REPLICA_PORT_B0=18752
REPLICA_PORT_B1=18753
PBR_SLOTS=200000   # large ring so a few hundred ms of unthrottled backlog doesn't wrap before the kill
DURATION=20
KILL_AT=4
SPARE_BYTES=268435456  # 256 MiB

ssh -o BatchMode=yes c3 "$C3_BIN/rdma_memserver --port=$MS_PORT --num-brokers=2 --pbr-slots=$PBR_SLOTS \
  --goi-entries=2000000 --spare-bytes=$SPARE_BYTES --recovery-port=$RECOVERY_PORT \
  --max-recoveries=1 --duration=$((DURATION+20))" > "$OUT/ms.log" 2>&1 &
MSPID=$!
sleep 3

$BIN/w5_broker_dram --broker-id=0 --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 \
  --pbr-slots=$PBR_SLOTS --message-size=4096 --target-mbps=0 --duration=$DURATION --inflight=32 \
  --blog-port=$BLOG_PORT --replica-port=$REPLICA_PORT_B0 --right-neighbor-ip=$C1_IP \
  --right-neighbor-replica-port=$REPLICA_PORT_B1 $DURABLE_FLAG > "$OUT/broker0.log" 2>&1 &
B0PID=$!

ssh -o BatchMode=yes c1 "$C1_BIN/w5_broker_dram --broker-id=1 --memserver-ip=$C3_IP --memserver-port=$MS_PORT \
  --num-brokers=2 --pbr-slots=$PBR_SLOTS --message-size=4096 --target-mbps=0 --duration=$DURATION \
  --inflight=32 --blog-port=$BLOG_PORT --replica-port=$REPLICA_PORT_B1 --right-neighbor-ip=$MOSCXL_IP \
  --right-neighbor-replica-port=$REPLICA_PORT_B0 --replica-max-accepts=2 $DURABLE_FLAG" > "$OUT/broker1.log" 2>&1 &
B1PID=$!

sleep 2
$BIN/w5_sequencer_dram --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 --pbr-slots=$PBR_SLOTS \
  --duration=$DURATION --broker-ips=$MOSCXL_IP,$C1_IP --blog-ports=$BLOG_PORT,$BLOG_PORT \
  > "$OUT/seq.log" 2>&1 &
SEQPID=$!

echo "[$(date -u +%FT%TZ)] trial $TRIAL: launched, memserver(c3)=$MSPID broker0(moscxl)=$B0PID seq(moscxl)=$SEQPID durable=[$DURABLE_FLAG]" >> "$OUT/timeline.log"

sleep $KILL_AT
KILL_T_MONO=$(date +%s.%N)
echo "$KILL_T_MONO" > "$OUT/kill_time"
kill -9 $B0PID
echo "[$(date -u +%FT%TZ)] trial $TRIAL: KILLED broker0 pid=$B0PID at kill_t_mono=$KILL_T_MONO" >> "$OUT/timeline.log"

for i in $(seq 1 20); do
  if grep -q "declared dead" "$OUT/seq.log" 2>/dev/null; then break; fi
  sleep 0.25
done
grep "declared dead\|RDMA_ERROR\|LEASE_TIMEOUT" "$OUT/seq.log" >> "$OUT/timeline.log" 2>/dev/null

# Recovery: read+verify the FIRST 10000 slots of broker0's replica (deterministic pre-wrap pattern:
# slot k's every byte == k & 0xFF), then write to memserver's spare region.
RECOVERY_BYTES=40960000   # 10000 slots * 4096 bytes/slot
$BIN/rdma_recovery --source-ip=$C1_IP --source-port=$REPLICA_PORT_B1 --target-ip=$C3_IP \
  --target-port=$RECOVERY_PORT --bytes=$RECOVERY_BYTES --verify-slot-bytes=4096 --verify-slots=10000 \
  > "$OUT/recovery.log" 2>&1
echo "recovery_rc=$?" >> "$OUT/timeline.log"

wait $B1PID $SEQPID $MSPID
echo "[$(date -u +%FT%TZ)] trial $TRIAL: all processes exited" >> "$OUT/timeline.log"
echo TRIAL_DONE
