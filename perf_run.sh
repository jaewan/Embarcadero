#!/usr/bin/env bash
# Perf regression runner (Track 02 follow-on). Invoked under the shared testbed
# flock. $1 = test id.
set -o pipefail
cd ~/Embarcadero-sessions/02-tla-spec
ACT=~/Embarcadero-sessions/activity.log
LOGDIR=~/Embarcadero-sessions/02-tla-spec/perf_logs
mkdir -p "$LOGDIR"
stamp() { date -u +%FT%TZ; }
note() { echo "[$(stamp)] 02-tla-spec: $*" >> "$ACT"; }

case "$1" in
  test1_o0)
    note "START test1 O0 loopback (3 trials)"
    NUM_BROKERS=4 TEST_TYPE=5 ORDER=0 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 \
      THREADS_PER_BROKER=4 SEQUENCER=EMBARCADERO NUM_TRIALS=3 \
      bash scripts/singlenode_run_throughput.sh 2>&1 | tee "$LOGDIR/test1_o0.log"
    note "END test1"
    ;;
  test2_o5)
    note "START test2 O5 loopback + stall gate (5 trials, no retry)"
    NUM_BROKERS=4 TEST_TYPE=5 ORDER=5 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 \
      THREADS_PER_BROKER=4 SEQUENCER=EMBARCADERO NUM_TRIALS=5 TRIAL_MAX_ATTEMPTS=1 \
      bash scripts/singlenode_run_throughput.sh 2>&1 | tee "$LOGDIR/test2_o5.log"
    note "END test2"
    ;;
  test3_o0)
    note "START test3 real-wire c3 O0 (3 trials)"
    CLIENT_HOSTS_CSV=c3 NUM_CLIENTS=1 NUM_TRIALS=3 \
      REMOTE_CLIENT_BIN_DIR=/home/domin/Embarcadero-sessions/01-core-protocol/build/bin \
      CLIENT_LD_LIBRARY_PATH=/home/domin/Embarcadero-sessions/01-core-protocol/build/bin \
      EMBARCADERO_HEAD_ADDR=10.10.10.10 NUM_BROKERS=4 TEST_TYPE=5 ACK=1 MESSAGE_SIZE=1024 \
      THREADS_PER_BROKER=4 TOTAL_MESSAGE_SIZE=16106127360 SEQUENCER=EMBARCADERO \
      REPLICATION_FACTOR=0 ORDER=0 bash scripts/run_multiclient.sh 2>&1 | tee "$LOGDIR/test3_o0.log"
    note "END test3_o0"
    ;;
  test3_o5)
    note "START test3 real-wire c3 O5 (3 trials, EMBAR_ASSERT_COMMIT_ORDER=1)"
    EMBAR_ASSERT_COMMIT_ORDER=1 TRIAL_MAX_ATTEMPTS=1 CLIENT_HOSTS_CSV=c3 NUM_CLIENTS=1 NUM_TRIALS=3 \
      REMOTE_CLIENT_BIN_DIR=/home/domin/Embarcadero-sessions/01-core-protocol/build/bin \
      CLIENT_LD_LIBRARY_PATH=/home/domin/Embarcadero-sessions/01-core-protocol/build/bin \
      EMBARCADERO_HEAD_ADDR=10.10.10.10 NUM_BROKERS=4 TEST_TYPE=5 ACK=1 MESSAGE_SIZE=1024 \
      THREADS_PER_BROKER=4 TOTAL_MESSAGE_SIZE=16106127360 SEQUENCER=EMBARCADERO \
      REPLICATION_FACTOR=0 ORDER=5 bash scripts/run_multiclient.sh 2>&1 | tee "$LOGDIR/test3_o5.log"
    note "END test3_o5"
    ;;
  *) echo "unknown test $1"; exit 2;;
esac
