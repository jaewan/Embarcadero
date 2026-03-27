#!/usr/bin/env bash
# Remote Corfu ORDER=2 throughput: run this script from the publisher machine (build/bin must exist locally).
#
# Typical three-node layout:
#   c2 — corfu_global_sequencer (gRPC)
#   c3 — embarlet brokers
#   c1 — this script + throughput_test (set CLUSTER_PUBLISHER_HOST when rsyncing)
#
# Required env (defaults match 10.10.10.x rack addresses for mos144/mos181):
#   REMOTE_BROKER_HOST          SSH host for brokers (default: c3)
#   REMOTE_CORFU_SEQUENCER_HOST SSH host for sequencer (default: c2); omit to collocate with brokers
#   EMBARCADERO_HEAD_ADDR       Broker head IP/hostname reachable from publisher (default: 10.10.10.181)
#   EMBARCADERO_CORFU_SEQ_IP    Sequencer IP for gRPC from publisher + brokers (default: 10.10.10.144)
#   REMOTE_PROJECT_ROOT         Path on remote hosts (default: $HOME/Embarcadero)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
export PROJECT_ROOT

export THROUGHPUT_SCRIPT_MODE=remote
export REMOTE_BROKER_HOST="${REMOTE_BROKER_HOST:-c3}"
export REMOTE_PROJECT_ROOT="${REMOTE_PROJECT_ROOT:-$HOME/Embarcadero}"
export REMOTE_BUILD_BIN="${REMOTE_BUILD_BIN:-$REMOTE_PROJECT_ROOT/build/bin}"
export REMOTE_CORFU_SEQUENCER_HOST="${REMOTE_CORFU_SEQUENCER_HOST:-c2}"
export REMOTE_CORFU_BUILD_BIN="${REMOTE_CORFU_BUILD_BIN:-$REMOTE_PROJECT_ROOT/build/bin}"

export EMBARCADERO_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-10.10.10.181}"
export EMBARCADERO_CORFU_SEQ_IP="${EMBARCADERO_CORFU_SEQ_IP:-10.10.10.144}"

export SEQUENCER=CORFU
export ORDER=2
export TOTAL_MESSAGE_SIZE="${TOTAL_MESSAGE_SIZE:-10737418240}"
export NUM_BROKERS="${NUM_BROKERS:-4}"
export NUM_TRIALS="${NUM_TRIALS:-1}"
export MESSAGE_SIZE="${MESSAGE_SIZE:-1024}"
export TEST_TYPE="${TEST_TYPE:-1}"
export ACK="${ACK:-1}"
export REPLICATION_FACTOR="${REPLICATION_FACTOR:-0}"

echo "Corfu remote throughput:"
echo "  REMOTE_BROKER_HOST=$REMOTE_BROKER_HOST"
echo "  REMOTE_CORFU_SEQUENCER_HOST=$REMOTE_CORFU_SEQUENCER_HOST"
echo "  EMBARCADERO_HEAD_ADDR=$EMBARCADERO_HEAD_ADDR"
echo "  EMBARCADERO_CORFU_SEQ_IP=$EMBARCADERO_CORFU_SEQ_IP"
echo "  ORDER=$ORDER TOTAL_MESSAGE_SIZE=$TOTAL_MESSAGE_SIZE NUM_BROKERS=$NUM_BROKERS"
echo ""

cd "$PROJECT_ROOT"
source "$PROJECT_ROOT/scripts/lib/run_throughput_impl.sh"
