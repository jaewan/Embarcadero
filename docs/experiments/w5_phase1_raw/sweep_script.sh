#!/bin/bash
# Phase 1 funnel sweep: W5-B memserver on c3, 8 broker QPs (4 moscxl + 4 c1), 1 sequencer.
# Sweeps aggregate offered load (MB/s) across points; each point run TRIALS times.
set -u
BIN=~/Embarcadero-sessions/w5-variants/src/rdma_transport/build_rdma/bin
C1_BIN=/tmp/w5v/build_rdma/bin
MS_IP=10.10.10.181
NUM_BROKERS=8
PBR_SLOTS=16384
GOI_ENTRIES=2000000
BLOG_BYTES=268435456   # 256 MiB/broker * 8 = 2 GiB total
MSG_SIZE=4096
DURATION=8
SEQ_DURATION=10
OUT_DIR=/tmp/w5_phase1_results
mkdir -p "$OUT_DIR"

# points_mbps: aggregate targets across ALL 8 brokers combined (10%,20%,40%,60%,80%,100%,120% of
# the ~90Gb/s=11250MB/s single-QP-measured ceiling; per-broker target = point/NUM_BROKERS)
POINTS=(1125 2250 4500 6750 9000 11250 13500)
TRIALS=3

for point in "${POINTS[@]}"; do
  per_broker=$(( point / NUM_BROKERS ))
  for trial in $(seq 1 $TRIALS); do
    tag="point${point}mbps_trial${trial}"
    echo "=== $tag (per-broker target=${per_broker} MB/s) ==="

    ssh -o BatchMode=yes c3 "cd /tmp/w5v/build_rdma/bin && ./rdma_memserver --port=18600 --num-brokers=$NUM_BROKERS --pbr-slots=$PBR_SLOTS --goi-entries=$GOI_ENTRIES --host-blog --blog-bytes=$BLOG_BYTES --duration=$SEQ_DURATION" > "$OUT_DIR/${tag}_ms.log" 2>&1 &
    MSPID=$!
    sleep 3

    # 4 broker processes on moscxl (broker-id 0-3)
    BROKER_PIDS=()
    for i in 0 1 2 3; do
      $BIN/w5_broker_memserver --broker-id=$i --memserver-ip=$MS_IP --memserver-port=18600 \
        --num-brokers=$NUM_BROKERS --pbr-slots=$PBR_SLOTS --message-size=$MSG_SIZE \
        --target-mbps=$per_broker --duration=$DURATION --inflight=64 \
        > "$OUT_DIR/${tag}_broker${i}.log" 2>&1 &
      BROKER_PIDS+=($!)
    done

    # 4 broker processes on c1 (broker-id 4-7), launched via one SSH backgrounding all 4
    ssh -o BatchMode=yes c1 "
      for i in 4 5 6 7; do
        $C1_BIN/w5_broker_memserver --broker-id=\$i --memserver-ip=$MS_IP --memserver-port=18600 \
          --num-brokers=$NUM_BROKERS --pbr-slots=$PBR_SLOTS --message-size=$MSG_SIZE \
          --target-mbps=$per_broker --duration=$DURATION --inflight=64 \
          > /tmp/${tag}_broker\${i}.log 2>&1 &
      done
      wait
    " > "$OUT_DIR/${tag}_c1_brokers.log" 2>&1 &
    C1PID=$!

    sleep 1.5
    $BIN/w5_sequencer_memserver --memserver-ip=$MS_IP --memserver-port=18600 \
      --num-brokers=$NUM_BROKERS --pbr-slots=$PBR_SLOTS --duration=$SEQ_DURATION \
      > "$OUT_DIR/${tag}_seq.log" 2>&1 &
    SEQPID=$!

    for p in "${BROKER_PIDS[@]}"; do wait $p; done
    wait $C1PID
    wait $SEQPID
    wait $MSPID

    # pull c1's per-broker logs back
    for i in 4 5 6 7; do
      scp -q c1:/tmp/${tag}_broker${i}.log "$OUT_DIR/${tag}_broker${i}.log" 2>/dev/null
    done
    echo "--- $tag done ---"
  done
done
echo SWEEP_DONE
