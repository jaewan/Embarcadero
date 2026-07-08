#!/bin/bash
# Sub-phase 3B leg-1 item 3: measure --durable-replicate cost, no kill, steady-state comparison.
# broker0=moscxl, broker1=c1, memserver=c3. Same 206 MB/s (~60% of peak) config as Phase 2.
set -u
TRIAL=$1
OUT=$2
DURABLE_FLAG=$3   # "--durable-replicate" or ""
mkdir -p "$OUT"

BIN=~/Embarcadero-sessions/w5-variants/src/rdma_transport/build_rdma/bin
C1_BIN=/tmp/w5v/build_rdma/bin
C3_BIN=/tmp/w5v/build_rdma/bin
MOSCXL_IP=10.10.10.10
C1_IP=10.10.10.11
C3_IP=10.10.10.181

MS_PORT=18672
BLOG_PORT=18742
REPLICA_PORT_B0=18754
REPLICA_PORT_B1=18755
TARGET_MBPS=0
DURATION=20
PBR_SLOTS=16384

ssh -o BatchMode=yes c3 "$C3_BIN/rdma_memserver --port=$MS_PORT --num-brokers=2 --pbr-slots=$PBR_SLOTS \
  --goi-entries=2000000 --duration=$((DURATION+10))" > "$OUT/ms.log" 2>&1 &
MSPID=$!
sleep 3

$BIN/w5_broker_dram --broker-id=0 --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 \
  --pbr-slots=$PBR_SLOTS --message-size=4096 --target-mbps=$TARGET_MBPS --duration=$DURATION --inflight=32 \
  --blog-port=$BLOG_PORT --replica-port=$REPLICA_PORT_B0 --right-neighbor-ip=$C1_IP \
  --right-neighbor-replica-port=$REPLICA_PORT_B1 $DURABLE_FLAG > "$OUT/broker0.log" 2>&1 &
B0PID=$!

ssh -o BatchMode=yes c1 "$C1_BIN/w5_broker_dram --broker-id=1 --memserver-ip=$C3_IP --memserver-port=$MS_PORT \
  --num-brokers=2 --pbr-slots=$PBR_SLOTS --message-size=4096 --target-mbps=$TARGET_MBPS --duration=$DURATION \
  --inflight=32 --blog-port=$BLOG_PORT --replica-port=$REPLICA_PORT_B1 --right-neighbor-ip=$MOSCXL_IP \
  --right-neighbor-replica-port=$REPLICA_PORT_B0 --replica-max-accepts=1 $DURABLE_FLAG" > "$OUT/broker1.log" 2>&1 &
B1PID=$!

sleep 2
$BIN/w5_sequencer_dram --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 --pbr-slots=$PBR_SLOTS \
  --duration=$DURATION --broker-ips=$MOSCXL_IP,$C1_IP --blog-ports=$BLOG_PORT,$BLOG_PORT \
  > "$OUT/seq.log" 2>&1 &
SEQPID=$!

wait $B0PID $B1PID $SEQPID $MSPID
echo TRIAL_DONE
