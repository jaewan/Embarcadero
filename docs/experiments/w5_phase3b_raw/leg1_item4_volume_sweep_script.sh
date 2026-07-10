#!/bin/bash
# Sub-phase 3B leg-1 item 4: re-replication volume-vs-Blog-size sweep, 16MiB...1GiB.
# broker0=moscxl, broker1=c1 (replica-of-broker0 lives here), memserver=c3. No kill --
# just fill the ring with real replicated data, then sweep recovery --bytes against it.
set -u
BIN=~/Embarcadero-sessions/w5-variants/src/rdma_transport/build_rdma/bin
C1_BIN=/tmp/w5v/build_rdma/bin
C3_BIN=/tmp/w5v/build_rdma/bin
MOSCXL_IP=10.10.10.10
C1_IP=10.10.10.11
C3_IP=10.10.10.181
OUT=/tmp/w5_leg1_volume_sweep_results
mkdir -p "$OUT"

MS_PORT=18673
BLOG_PORT=18743
REPLICA_PORT_B0=18756
REPLICA_PORT_B1=18757
PBR_SLOTS=262144   # 262144*4096 = 1GiB exactly, fills the whole 1GiB replica region
DURATION=25

ssh -o BatchMode=yes c3 "$C3_BIN/rdma_memserver --port=$MS_PORT --num-brokers=2 --pbr-slots=$PBR_SLOTS \
  --goi-entries=2000000 --spare-bytes=1073741824 --recovery-port=18697 --max-recoveries=21 \
  --duration=$((DURATION+60))" > "$OUT/ms.log" 2>&1 &
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
  --right-neighbor-replica-port=$REPLICA_PORT_B0 --replica-max-accepts=25" > "$OUT/broker1.log" 2>&1 &
B1PID=$!

sleep 2
$BIN/w5_sequencer_dram --memserver-ip=$C3_IP --memserver-port=$MS_PORT --num-brokers=2 --pbr-slots=$PBR_SLOTS \
  --duration=$DURATION --broker-ips=$MOSCXL_IP,$C1_IP --blog-ports=$BLOG_PORT,$BLOG_PORT \
  > "$OUT/seq.log" 2>&1 &
SEQPID=$!

# Let it run for a few seconds to fill >=1GiB into broker0's replica-on-broker1 (unthrottled,
# ~19-24 Gb/s per earlier measurement -> 1GiB fills in well under a second; give it 5s margin).
sleep 5

# Sweep recovery --bytes against the NOW-POPULATED replica region while brokers/sequencer keep
# running undisturbed in the background (no kill for this item -- pure transfer characterization).
SIZES_MIB=(16 32 64 128 256 512 1024)
for mib in "${SIZES_MIB[@]}"; do
  bytes=$((mib * 1024 * 1024))
  for trial in 1 2 3; do
    $BIN/rdma_recovery --source-ip=$C1_IP --source-port=$REPLICA_PORT_B1 --target-ip=$C3_IP \
      --target-port=18697 --bytes=$bytes > "$OUT/recovery_${mib}MiB_trial${trial}.log" 2>&1
    echo "${mib}MiB trial${trial}: $(grep RESULT $OUT/recovery_${mib}MiB_trial${trial}.log)"
  done
done

kill -9 $B0PID $B1PID $SEQPID 2>/dev/null
wait $MSPID 2>/dev/null
echo SWEEP_DONE
