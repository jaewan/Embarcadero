#!/bin/bash
set -u
BIN=~/Embarcadero-sessions/w5-variants/src/rdma_transport/build_rdma/bin
C1_BIN=/tmp/w5v/build_rdma/bin
C3_BIN=/tmp/w5v/build_rdma/bin
MS_IP=10.10.10.181
NUM_BROKERS=8
PBR_SLOTS=16384
GOI_ENTRIES=2000000
BLOG_BYTES=268435456
MSG_SIZE=4096
DURATION=8
SEQ_DURATION=10
SEQ_THREADS=8
OUT_DIR=/tmp/w5_priority_retest_results
mkdir -p "$OUT_DIR"

POINTS=(11250 13500)
TRIALS=3

for point in "${POINTS[@]}"; do
  per_broker=$(( point / NUM_BROKERS ))
  for trial in $(seq 1 $TRIALS); do
    tag="point${point}mbps_trial${trial}"
    echo "=== $tag (per-broker target=${per_broker} MB/s) ==="

    # NIC counter snapshot BEFORE
    ssh -o BatchMode=yes c3 "ethtool -S ens801f0np0 2>/dev/null | grep -E 'rx_bytes_phy|tx_bytes_phy|rx_discards_phy|tx_discards_phy|rx_pause|tx_pause'" > "$OUT_DIR/${tag}_c3_nic_before.log" 2>&1
    ethtool -S enp193s0f0np0 2>/dev/null | grep -E 'rx_bytes_phy|tx_bytes_phy|rx_discards_phy|tx_discards_phy|rx_pause|tx_pause' > "$OUT_DIR/${tag}_moscxl_nic_before.log" 2>&1

    ssh -o BatchMode=yes c3 "numactl --cpunodebind=1 --membind=1 $C3_BIN/rdma_memserver --port=18600 --num-brokers=$NUM_BROKERS --pbr-slots=$PBR_SLOTS --goi-entries=$GOI_ENTRIES --host-blog --blog-bytes=$BLOG_BYTES --num-sequencer-conns=$SEQ_THREADS --duration=$SEQ_DURATION" > "$OUT_DIR/${tag}_ms.log" 2>&1 &
    MSPID=$!
    sleep 3

    BROKER_PIDS=()
    for i in 0 1 2 3; do
      numactl --cpunodebind=1 --membind=1 \
        $BIN/w5_broker_memserver --broker-id=$i --memserver-ip=$MS_IP --memserver-port=18600 \
        --num-brokers=$NUM_BROKERS --pbr-slots=$PBR_SLOTS --message-size=$MSG_SIZE \
        --target-mbps=$per_broker --duration=$DURATION --inflight=64 \
        > "$OUT_DIR/${tag}_broker${i}.log" 2>&1 &
      BROKER_PIDS+=($!)
    done

    ssh -o BatchMode=yes c1 "
      for i in 4 5 6 7; do
        numactl --cpunodebind=0 --membind=0 \
          $C1_BIN/w5_broker_memserver --broker-id=\$i --memserver-ip=$MS_IP --memserver-port=18600 \
          --num-brokers=$NUM_BROKERS --pbr-slots=$PBR_SLOTS --message-size=$MSG_SIZE \
          --target-mbps=$per_broker --duration=$DURATION --inflight=64 \
          > /tmp/${tag}_broker\${i}.log 2>&1 &
      done
      wait
    " > "$OUT_DIR/${tag}_c1_brokers.log" 2>&1 &
    C1PID=$!

    sleep 1.5
    numactl --cpunodebind=1 --membind=1 \
      $BIN/w5_sequencer_memserver --memserver-ip=$MS_IP --memserver-port=18600 \
      --num-brokers=$NUM_BROKERS --pbr-slots=$PBR_SLOTS --duration=$SEQ_DURATION --seq-threads=$SEQ_THREADS \
      > "$OUT_DIR/${tag}_seq.log" 2>&1 &
    SEQPID=$!

    for p in "${BROKER_PIDS[@]}"; do wait $p; done
    wait $C1PID
    wait $SEQPID
    wait $MSPID

    # NIC counter snapshot AFTER
    ssh -o BatchMode=yes c3 "ethtool -S ens801f0np0 2>/dev/null | grep -E 'rx_bytes_phy|tx_bytes_phy|rx_discards_phy|tx_discards_phy|rx_pause|tx_pause'" > "$OUT_DIR/${tag}_c3_nic_after.log" 2>&1
    ethtool -S enp193s0f0np0 2>/dev/null | grep -E 'rx_bytes_phy|tx_bytes_phy|rx_discards_phy|tx_discards_phy|rx_pause|tx_pause' > "$OUT_DIR/${tag}_moscxl_nic_after.log" 2>&1

    for i in 4 5 6 7; do
      scp -q c1:/tmp/${tag}_broker${i}.log "$OUT_DIR/${tag}_broker${i}.log" 2>/dev/null
    done
    echo "--- $tag done ---"
  done
done
echo RETEST_DONE
