#!/bin/bash
# scripts/run_multiclient.sh
#
# Multi-client throughput orchestration for Embarcadero.
# Starts brokers locally, then launches N physical clients in parallel with a
# synchronized (future-timestamp) barrier start.
#
# Client roster (order defines who is added at each NUM_CLIENTS level):
#   NUM_CLIENTS=1  → c4              (NUMA 1)
#   NUM_CLIENTS=2  → c4, c3          (NUMA 1, 1)
#   NUM_CLIENTS=3  → c4, c3, local   (NUMA 1, 1, 0)
#   NUM_CLIENTS=4  → c4, c3, local, c2
#   NUM_CLIENTS=5  → c4, c3, local, c2, c1
#
# All configuration via environment variables:
#   NUM_CLIENTS, NUM_BROKERS, NUM_TRIALS, TRIAL_MAX_ATTEMPTS
#   TOTAL_MESSAGE_SIZE, MESSAGE_SIZE, THREADS_PER_BROKER
#   TEST_TYPE, ORDER, ACK, REPLICATION_FACTOR, SEQUENCER
#   EMBARCADERO_HEAD_ADDR (broker IP, default 10.10.10.143)
#   EMBARCADERO_ORDER0_FAST_PATH, EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES,
#   EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE, EMBARCADERO_BATCH_SIZE,
#   EMBARCADERO_CLIENT_PUB_BATCH_KB, EMBARCADERO_NETWORK_IO_THREADS,
#   EMBARCADERO_ORDER5_HOME_BROKERS, LOCAL_CLIENT_NUMA,
#   REMOTE_CORFU_SEQUENCER_HOST, REMOTE_CORFU_BUILD_BIN,
#   REMOTE_SCALOG_SEQUENCER_HOST, REMOTE_SCALOG_BUILD_BIN
#   EMBARCADERO_CORFU_SEQ_IP / EMBARCADERO_CORFU_SEQ_PORT for CORFU + SSH clients
#   REMOTE_LAZYLOG_SEQUENCER_HOST, REMOTE_LAZYLOG_BUILD_BIN
#   EMBARCADERO_LAZYLOG_SEQ_IP / EMBARCADERO_LAZYLOG_SEQ_PORT for LAZYLOG + SSH clients
#   EMBARCADERO_SCALOG_SEQ_IP / EMBARCADERO_SCALOG_SEQ_PORT for SCALOG
#   REMOTE_CLIENT_BIN_DIR to override the SSH client executable directory
#
# Example:
#   NUM_CLIENTS=3 NUM_BROKERS=4 MESSAGE_SIZE=8192 scripts/run_multiclient.sh

set -euo pipefail

RUN_LOCK_FILE="${RUN_LOCK_FILE:-/tmp/embarcadero_run_multiclient.lock}"
exec {RUN_LOCK_FD}>"$RUN_LOCK_FILE"
if ! flock -n "$RUN_LOCK_FD"; then
    echo "ERROR: another benchmark orchestrator is already running (lock: $RUN_LOCK_FILE)." >&2
    echo "       Clear the existing run or override RUN_LOCK_FILE if you intentionally need a separate harness lock." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Cluster topology — order determines activation sequence
# "local" means this broker machine (where brokers run); everything else is SSH
# ---------------------------------------------------------------------------
declare -a CLIENT_HOSTS=( "c4"  "c3"    "local" "c2"  "c1"  )
# NUMA pinning per client — must match the NUMA node of each client's high-speed NIC:
#   c4: ens801f0np0 (100G) is on NUMA 1 → pin to NUMA 1
#   c3: ens801f0np0 (100G) is on NUMA 1 → pin to NUMA 1
#   c2: 1G only, NUMA 0 → pin to NUMA 0 (1G NIC)
#   c1: enp24s0f0np0 (100G, 10.10.10.11) is on NUMA 0 → pin to NUMA 0 (was wrongly 1)
#       Corrected 2026-07-11: NUMA 1 pinning caused cross-NUMA DMA, reducing c1 throughput
#       from ~5.2 GB/s (c4 at NUMA-local) to ~3.33 GB/s.
declare -a CLIENT_NUMAS=( "1"   "1"     ""      "0"   "0"   )
if [[ -n "${CLIENT_HOSTS_CSV:-}" ]]; then
    IFS=',' read -r -a CLIENT_HOSTS <<< "$CLIENT_HOSTS_CSV"
fi
if [[ -n "${CLIENT_NUMAS_CSV:-}" ]]; then
    IFS=',' read -r -a CLIENT_NUMAS <<< "$CLIENT_NUMAS_CSV"
fi
MAX_CLIENTS=${#CLIENT_HOSTS[@]}

# ---------------------------------------------------------------------------
# Configuration (all overrideable via environment)
# ---------------------------------------------------------------------------
NUM_CLIENTS=${NUM_CLIENTS:-1}
NUM_BROKERS=${NUM_BROKERS:-4}
NUM_TRIALS=${NUM_TRIALS:-5}
TRIAL_MAX_ATTEMPTS=${TRIAL_MAX_ATTEMPTS:-3}

# Total bytes across ALL clients combined; divided equally per client
TOTAL_MESSAGE_SIZE=${TOTAL_MESSAGE_SIZE:-8589934592}   # 8 GiB default
MESSAGE_SIZE=${MESSAGE_SIZE:-1024}
THREADS_PER_BROKER=${THREADS_PER_BROKER:-4}

TEST_TYPE=${TEST_TYPE:-5}          # 5 = publish-only
ORDER=${ORDER:-0}
ACK=${ACK:-1}
REPLICATION_FACTOR=${REPLICATION_FACTOR:-0}
# A benchmark's fixed TestTopic name starts broker slot sequences at zero.  A
# durable Corfu sidecar must therefore not be reused for a distinct invocation:
# its conflict detection would (correctly) reject the new values.  The default
# below isolates each logical trial *and retry attempt*.  Set this to 0 only
# for an intentional restart/redrive test, where the same slot/value identities
# are being replayed against the same durable medium.
REPLICA_DISK_DIRS_TEMPLATE=${EMBARCADERO_REPLICA_DISK_DIRS:-}
CORFU_REPLICA_ISOLATE_ATTEMPTS=${EMBARCADERO_CORFU_REPLICA_ISOLATE_ATTEMPTS:-1}
if [[ "$CORFU_REPLICA_ISOLATE_ATTEMPTS" != "0" && "$CORFU_REPLICA_ISOLATE_ATTEMPTS" != "1" ]]; then
    echo "ERROR: EMBARCADERO_CORFU_REPLICA_ISOLATE_ATTEMPTS must be 0 or 1" >&2
    exit 1
fi
SEQUENCER=${SEQUENCER:-EMBARCADERO}
CONTROL_TRANSPORT=${EMBARCADERO_BASELINE_CONTROL_TRANSPORT:-grpc}
DRY_RUN=${DRY_RUN:-0}
REQUIRE_FAITHFUL_LAZYLOG=${REQUIRE_FAITHFUL_LAZYLOG:-0}
LAZYLOG_METADATA_CONTRACT="not_applicable"
LAZYLOG_METADATA_REPLICA_COUNT=0
LAZYLOG_METADATA_READY_TIMEOUT_SEC=${LAZYLOG_METADATA_READY_TIMEOUT_SEC:-20}
ACK_DURABILITY_CONTRACT="not_applicable"

BROKER_READY_TIMEOUT_SEC=${BROKER_READY_TIMEOUT_SEC:-120}
if [[ "$SEQUENCER" == "CORFU" && -z "${BROKER_READY_TIMEOUT_SEC_OVERRIDE_APPLIED:-}" && "${BROKER_READY_TIMEOUT_SEC:-120}" == "120" ]]; then
    BROKER_READY_TIMEOUT_SEC=300
fi

BROKER_REACHABILITY_TIMEOUT_SEC=${BROKER_REACHABILITY_TIMEOUT_SEC:-20}
BROKER_REACHABILITY_POLL_SEC=${BROKER_REACHABILITY_POLL_SEC:-0.1}
# Stagger between broker process launches. Full CXL zeroing is memory-heavy so default
# 1s; metadata-mode (+ bg/sync prefault) is light enough to launch back-to-back.
if [[ -z "${BROKER_START_STAGGER_SEC:-}" ]]; then
    if [[ "${EMBARCADERO_CXL_ZERO_MODE:-full}" == "metadata" ]]; then
        BROKER_START_STAGGER_SEC=0
    else
        BROKER_START_STAGGER_SEC=1
    fi
fi
# Extra settle after sockets listen. ORDER=5 EMBARCADERO already waits on real
# scanner/head convergence below, so a fixed sleep is pure waste there.
if [[ -z "${BROKER_READY_PROPAGATION_SEC:-}" ]]; then
    if [[ "${ORDER:-0}" == "5" && "${SEQUENCER:-EMBARCADERO}" == "EMBARCADERO" ]]; then
        BROKER_READY_PROPAGATION_SEC=0
    else
        BROKER_READY_PROPAGATION_SEC=4
    fi
fi
ORDER5_CLUSTER_READY_TIMEOUT_SEC=${ORDER5_CLUSTER_READY_TIMEOUT_SEC:-30}
# Barrier lead time for SSH client launch + clock skew. Prior default of 8s was
# overly conservative on this LAN (SSH spawn is typically <1s).
START_DELAY_SEC=${START_DELAY_SEC:-3}
MIN_REMOTE_START_DELAY_SEC=${MIN_REMOTE_START_DELAY_SEC:-3}
QUIET=${QUIET:-0}

# ---------------------------------------------------------------------------
# Failure injection (E4 suite): timed kill of one broker mid-trial.
# BROKER_KILL_AFTER_SEC > 0 arms a killer that waits until every client log
# shows "Publisher push start" (Init+warmup finished), then sends
# BROKER_KILL_SIGNAL to broker BROKER_KILL_ID after BROKER_KILL_AFTER_SEC of
# steady publish. Kill timestamps land in $LOG_DIR/trial<N>_broker_kill.csv on
# the same axis as the client timeseries CSVs (ORIGIN_MS = launch barrier).
# ---------------------------------------------------------------------------
BROKER_KILL_AFTER_SEC=${BROKER_KILL_AFTER_SEC:-0}
BROKER_KILL_ID=${BROKER_KILL_ID:-1}
BROKER_KILL_SIGNAL=${BROKER_KILL_SIGNAL:-KILL}
BROKER_KILL_PUSH_WAIT_SEC=${BROKER_KILL_PUSH_WAIT_SEC:-120}
# Client timeseries sampling interval (ms); client default is 100 if unset.
EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS=${EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS:-}
# Extra args appended verbatim to the throughput_test command line
# (e.g. "--target_mbps 500 --steady_rate" for paced failure experiments).
CLIENT_EXTRA_ARGS=${CLIENT_EXTRA_ARGS:-}

# Performance knobs (set to empty string to disable)
EMBARCADERO_ORDER0_FAST_PATH=${EMBARCADERO_ORDER0_FAST_PATH:-1}
EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES=${EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES:-524288}
EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE=${EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE:-1}
EMBARCADERO_BATCH_SIZE=${EMBARCADERO_BATCH_SIZE:-524288}
EMBARCADERO_CLIENT_PUB_BATCH_KB=${EMBARCADERO_CLIENT_PUB_BATCH_KB:-512}
# ReqReceive threads bind 1:1 to live publish connections. Undersizing this
# (historical default 4/8) permanently starves N*THREADS_PER_BROKER connects
# per broker — the N=2 SessionOpen "storm" was mostly pool exhaustion.
# Auto-raise unless the caller already set a sufficient value.
_EMB_NET_IO_USERSET=0
if [[ -n "${EMBARCADERO_NETWORK_IO_THREADS:-}" ]]; then
    _EMB_NET_IO_USERSET=1
fi
EMBARCADERO_NETWORK_IO_THREADS=${EMBARCADERO_NETWORK_IO_THREADS:-4}
EMBARCADERO_ORDER5_HOME_BROKERS=${EMBARCADERO_ORDER5_HOME_BROKERS:-}
# Epoch period for ORDER=5 sequencer (µs). Reads EMBAR_ORDER5_EPOCH_US from env;
# default 500 µs (2000 epochs/sec). Shorter epochs (e.g. 200) raise throughput ceiling
# at the cost of higher per-epoch CPU. Broker clamps to [100, 5000] µs internally.
EMBAR_ORDER5_EPOCH_US=${EMBAR_ORDER5_EPOCH_US:-}
EMBARCADERO_CORFU_SEQ_IP=${EMBARCADERO_CORFU_SEQ_IP:-}
EMBARCADERO_CORFU_SEQ_PORT=${EMBARCADERO_CORFU_SEQ_PORT:-}
# Corfu clients derive each ingress host from membership and use this common
# broker-id-indexed port base (proxy endpoint = host:(base + broker_id)).
EMBARCADERO_CORFU_PROXY_PORT_BASE=${EMBARCADERO_CORFU_PROXY_PORT_BASE:-50100}
# Corfu RF>1 has one ordered chain per source broker.  Give every broker the
# same complete broker-id -> replica-service membership map; Topic derives its
# RF-1 successors with GetReplicationSetBroker.  A caller can override this
# for multi-host deployments with e.g. 0@c1:50053,1@c2:50054.
EMBARCADERO_CORFU_REPLICA_ENDPOINTS=${EMBARCADERO_CORFU_REPLICA_ENDPOINTS:-}
EMBARCADERO_LAZYLOG_SEQ_IP=${EMBARCADERO_LAZYLOG_SEQ_IP:-}
EMBARCADERO_LAZYLOG_SEQ_PORT=${EMBARCADERO_LAZYLOG_SEQ_PORT:-}
CLIENT_LD_LIBRARY_PATH=${CLIENT_LD_LIBRARY_PATH:-${LD_LIBRARY_PATH:-}}
REMOTE_CORFU_SEQUENCER_HOST=${REMOTE_CORFU_SEQUENCER_HOST:-}
REMOTE_CORFU_BUILD_BIN=${REMOTE_CORFU_BUILD_BIN:-}
REMOTE_LAZYLOG_SEQUENCER_HOST=${REMOTE_LAZYLOG_SEQUENCER_HOST:-}
REMOTE_LAZYLOG_BUILD_BIN=${REMOTE_LAZYLOG_BUILD_BIN:-}
EMBARCADERO_SCALOG_SEQ_IP=${EMBARCADERO_SCALOG_SEQ_IP:-}
EMBARCADERO_SCALOG_SEQ_PORT=${EMBARCADERO_SCALOG_SEQ_PORT:-}
REMOTE_SCALOG_SEQUENCER_HOST=${REMOTE_SCALOG_SEQUENCER_HOST:-}
REMOTE_SCALOG_BUILD_BIN=${REMOTE_SCALOG_BUILD_BIN:-}
# ---------------------------------------------------------------------------
# Derived paths
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
# Keep artifacts coupled to the actual compiled CXL layout.  The mailbox ring
# protocol has its own header version, but this records the enclosing CXL
# region layout and must not silently remain at an old hand-written constant.
CXL_LAYOUT_VERSION="$(sed -nE 's/.*kCxlLayoutVersion = ([0-9]+).*/\1/p' \
    "$PROJECT_ROOT/src/cxl_manager/baseline_cxl_layout.h" | head -1)"
if ! [[ "$CXL_LAYOUT_VERSION" =~ ^[0-9]+$ ]]; then
    echo "ERROR: could not determine CXL layout version from baseline_cxl_layout.h" >&2
    exit 1
fi
# Keep every locally launched role (brokers, sequencer, and local client) on one
# explicitly selected build.  This is especially important for the baseline
# services, whose protobuf ABI must match the broker binary.  A caller may point
# this at a separately configured build tree, but must not mix role binaries.
BUILD_BIN="${BUILD_BIN:-$PROJECT_ROOT/build/bin}"
REMOTE_CLIENT_BIN_DIR=${REMOTE_CLIENT_BIN_DIR:-$BUILD_BIN}
BROKER_CONFIG="${BROKER_CONFIG:-config/embarcadero.yaml}"
CLIENT_CONFIG="${CLIENT_CONFIG:-config/client.yaml}"
BROKER_CONFIG_ABS="$PROJECT_ROOT/$BROKER_CONFIG"
CLIENT_CONFIG_ABS="$PROJECT_ROOT/$CLIENT_CONFIG"
# A direct invocation is frequently used for baseline diagnostics and durable
# RF sweeps.  Keep the historical path as the default, but honour the same
# OUT_BASE/BENCHMARK_TAG contract as the overnight harness so independent
# cells never overwrite one another's evidence.
# Resolve OUT_BASE without double-prefixing when the caller already passed an
# absolute path (overnight sets OUT_BASE=$PROJECT_ROOT/data/...).
resolve_project_path() {
    local p="$1"
    if [[ -z "$p" ]]; then
        echo ""
    elif [[ "$p" == /* ]]; then
        echo "$p"
    else
        echo "$PROJECT_ROOT/$p"
    fi
}

# Keep a durable Corfu sidecar scoped to one artifact/trial/attempt.  The
# protocol deliberately treats a different value for an existing
# (topic, broker, batch-seq) slot as a conflict, so merely using `%TRIAL%` is
# insufficient: a retry starts a new client identity while broker batch
# sequences restart at zero.  This helper changes only the directory selected
# for an independent benchmark attempt; it never weakens WriteOnce conflict or
# idempotence semantics.  Explicit restart/redrive tests can opt out with
# EMBARCADERO_CORFU_REPLICA_ISOLATE_ATTEMPTS=0.
corfu_replica_dirs_for_attempt() {
    local trial="$1"
    local attempt="$2"
    local dirs="${REPLICA_DISK_DIRS_TEMPLATE//%TRIAL%/$trial}"
    dirs="${dirs//%ATTEMPT%/$attempt}"

    if [[ "$SEQUENCER" == "CORFU" && "$REPLICATION_FACTOR" -gt 1 &&
          "$CORFU_REPLICA_ISOLATE_ATTEMPTS" == "1" ]] &&
          ! corfu_uses_memory_copy_sink; then
        local artifact_key="${BENCHMARK_TAG:-$LOG_DIR}"
        local artifact_hash
        artifact_hash="$(printf '%s' "$artifact_key" | cksum | awk '{print $1}')"
        local suffix="corfu_slots_${artifact_hash}_t${trial}_a${attempt}"
        local -a base_dirs scoped_dirs
        local base_dir
        IFS=',' read -r -a base_dirs <<< "$dirs"
        for base_dir in "${base_dirs[@]}"; do
            [[ -n "$base_dir" ]] && scoped_dirs+=("${base_dir%/}/$suffix")
        done
        (IFS=','; printf '%s' "${scoped_dirs[*]}")
    else
        printf '%s' "$dirs"
    fi
}

if [[ -n "${LOG_DIR:-}" ]]; then
    : # explicit caller choice wins
elif [[ -n "${OUT_BASE:-}" && -n "${BENCHMARK_TAG:-}" ]]; then
    LOG_DIR="$(resolve_project_path "$OUT_BASE")/logs/$BENCHMARK_TAG"
else
    LOG_DIR="$PROJECT_ROOT/multiclient_logs"
fi

baseline_sequencer_binary() {
    case "$SEQUENCER:$CONTROL_TRANSPORT" in
        CORFU:grpc) echo "corfu_global_sequencer" ;;
        CORFU:cxl_mailbox) echo "corfu_mailbox_global_sequencer" ;;
        LAZYLOG:grpc) echo "lazylog_global_sequencer" ;;
        LAZYLOG:cxl_mailbox) echo "lazylog_mailbox_global_sequencer" ;;
        SCALOG:grpc) echo "scalog_global_sequencer" ;;
        SCALOG:cxl_mailbox) echo "scalog_mailbox_global_sequencer" ;;
        *) echo "" ;;
    esac
}

default_broker_ip() {
    if [[ -n "${EMBARCADERO_HEAD_ADDR:-}" ]]; then
        printf '%s\n' "$EMBARCADERO_HEAD_ADDR"
        return 0
    fi
    local host_ip
    host_ip="$(hostname -I 2>/dev/null | awk '{print $1}')"
    if [[ -n "$host_ip" ]]; then
        printf '%s\n' "$host_ip"
    else
        printf '%s\n' "127.0.0.1"
    fi
}

# Corfu has its own ordered WriteOnce chain.  A generic memory-copy selection
# must therefore select an actual in-memory Corfu replica service, not merely
# change client-side ACK accounting while still launching --replicate_to_disk.
corfu_replica_sink_mode() {
    local sink="${EMBARCADERO_CHAIN_REPLICATION_SINK:-}"
    sink="${sink,,}"
    case "$sink" in
        memory-copy|memory_copy|mem-copy|copy) printf '%s' "memory-copy" ;;
        memory-accounting|memory_accounting|mem-accounting|accounting) printf '%s' "memory-accounting" ;;
        disk-durable|disk_durable|disk|'')
            if [[ "${EMBARCADERO_CHAIN_REPLICATION_INMEM:-0}" == "1" ]]; then
                if [[ "${EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY:-0}" == "1" ]]; then
                    printf '%s' "memory-copy"
                else
                    printf '%s' "memory-accounting"
                fi
            else
                printf '%s' "disk-durable"
            fi
            ;;
        *) printf '%s' "invalid" ;;
    esac
}

corfu_uses_memory_copy_sink() {
    [[ "$(corfu_replica_sink_mode)" == "memory-copy" ]]
}

BROKER_IP="$(default_broker_ip)"
export EMBARCADERO_CXL_SHM_NAME="${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}"
export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-metadata}"
# A populated 64 GiB POSIX-shm object can take seconds of CPU time to reclaim.
# Give every trial/attempt a unique object and reclaim it off the next cell's
# critical path; the next broker 0 cannot accidentally attach stale bytes.
BASE_CXL_SHM_NAME="$EMBARCADERO_CXL_SHM_NAME"
# A real-CXL deployment commonly backs this mapping with a 64 GiB POSIX-shm
# object.  Deferring unlinks while changing the name on every retry can retain
# several such objects at once (until every mapping closes) and exhaust
# /dev/shm.  Throughput runs therefore synchronously reclaim one stable name
# by default.  Fault-injection users can explicitly opt back into per-attempt
# names and deferred cleanup when isolation is more important than capacity.
DEFER_CXL_SHM_CLEANUP="${DEFER_CXL_SHM_CLEANUP:-0}"
CXL_SHM_PER_ATTEMPT="${CXL_SHM_PER_ATTEMPT:-0}"

# ---------------------------------------------------------------------------
# Validation
# ---------------------------------------------------------------------------
if [[ "$CONTROL_TRANSPORT" != "grpc" && "$CONTROL_TRANSPORT" != "cxl_mailbox" ]]; then
    echo "ERROR: EMBARCADERO_BASELINE_CONTROL_TRANSPORT must be exactly grpc or cxl_mailbox (got '$CONTROL_TRANSPORT')" >&2
    exit 1
fi
if [[ "$DRY_RUN" != "0" && "$DRY_RUN" != "1" ]]; then
    echo "ERROR: DRY_RUN must be 0 or 1 (got '$DRY_RUN')" >&2
    exit 1
fi
if [[ "$SEQUENCER" == "CORFU" && "$REPLICATION_FACTOR" -gt 1 ]] &&
      ! corfu_uses_memory_copy_sink &&
      [[ -z "${EMBARCADERO_REPLICA_DISK_DIRS:-}" &&
         -z "${EMBARCADERO_REPLICA_DISK_ROOT:-}" ]]; then
    echo "ERROR: Corfu RF>1 requires explicit EMBARCADERO_REPLICA_DISK_DIRS or EMBARCADERO_REPLICA_DISK_ROOT" >&2
    exit 1
fi
if [[ "$CONTROL_TRANSPORT" == "cxl_mailbox" ]]; then
    # Broker 0 owns CreateInPlace over its CXL mapping. Real CXL can be exposed
    # through DAX *or* through the shared mapping whose pages are mbound to a
    # zero-core CXL NUMA node. Do not reject the latter: this machine's default
    # CXL deployment uses it. A dry run must remain hardware-independent.
    if [[ "$DRY_RUN" != "1" && -n "${EMBARCADERO_CXL_DEVICE:-}" && ! -c "${EMBARCADERO_CXL_DEVICE}" ]]; then
        echo "ERROR: EMBARCADERO_CXL_DEVICE must be a DAX character device when set: ${EMBARCADERO_CXL_DEVICE}" >&2
        exit 1
    fi
    if [[ "$DRY_RUN" != "1" && -z "${EMBARCADERO_CXL_DEVICE:-}" && -z "${EMBARCADERO_CXL_SHM_NAME:-}" ]]; then
        echo "ERROR: cxl_mailbox requires EMBARCADERO_CXL_DEVICE or EMBARCADERO_CXL_SHM_NAME" >&2
        exit 1
    fi
    # These publication-layout constants are deliberately fixed.  Changing one
    # role's values would make AttachInPlace interpret a different ring layout.
    for _mailbox_setting in EMBARCADERO_BASELINE_MAILBOX_RECORD_SIZE=512 \
                            EMBARCADERO_BASELINE_MAILBOX_UP_CAPACITY=1024 \
                            EMBARCADERO_BASELINE_MAILBOX_DOWN_CAPACITY=1024; do
        _name="${_mailbox_setting%%=*}"
        _expected="${_mailbox_setting#*=}"
        _actual="${!_name:-$_expected}"
        if [[ "$_actual" != "$_expected" ]]; then
            echo "ERROR: $_name must be $_expected for cxl_mailbox (got $_actual)" >&2
            exit 1
        fi
        export "$_name=$_expected"
    done
fi
if ! [[ "$NUM_CLIENTS" =~ ^[1-9][0-9]*$ ]] || [ "$NUM_CLIENTS" -gt "$MAX_CLIENTS" ]; then
    echo "ERROR: NUM_CLIENTS must be 1–${MAX_CLIENTS}, got '${NUM_CLIENTS}'" >&2
    echo "Usage: NUM_CLIENTS=<1-${MAX_CLIENTS}> $0" >&2
    exit 1
fi

if [[ "$SEQUENCER" == "CORFU" ]] && [[ "$ORDER" != "2" ]]; then
    echo "ERROR: CORFU sequencer requires ORDER=2 (got ORDER=$ORDER)" >&2
    exit 1
fi
if [[ "$SEQUENCER" == "SCALOG" ]] && [[ "$ORDER" != "1" ]]; then
    echo "ERROR: SCALOG baseline requires ORDER=1; ORDER=$ORDER does not start its local-cut sequencer" >&2
    exit 1
fi
if [[ "$SEQUENCER" == "CORFU" ]] && \
   { ! [[ "$EMBARCADERO_CORFU_PROXY_PORT_BASE" =~ ^[1-9][0-9]*$ ]] ||
     (( EMBARCADERO_CORFU_PROXY_PORT_BASE + NUM_BROKERS - 1 > 65535 )); }; then
    echo "ERROR: EMBARCADERO_CORFU_PROXY_PORT_BASE must leave one valid port per broker (got '$EMBARCADERO_CORFU_PROXY_PORT_BASE')." >&2
    exit 1
fi
if [[ "$SEQUENCER" == "LAZYLOG" ]] && [[ "$ORDER" != "2" ]]; then
    echo "ERROR: LAZYLOG baseline requires ORDER=2 (got ORDER=$ORDER)" >&2
    exit 1
fi
if [[ "$SEQUENCER" == "LAZYLOG" ]]; then
    if [[ -n "${EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS:-}" ]]; then
        IFS=',' read -r -a lazylog_metadata_endpoints <<< "$EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS"
        for endpoint in "${lazylog_metadata_endpoints[@]}"; do
            if [[ -z "${endpoint//[[:space:]]/}" ]]; then
                echo "ERROR: EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS contains an empty endpoint" >&2
                exit 1
            fi
        done
        LAZYLOG_METADATA_REPLICA_COUNT=${#lazylog_metadata_endpoints[@]}
        if [[ "$LAZYLOG_METADATA_REPLICA_COUNT" -ne "$REPLICATION_FACTOR" ]]; then
            echo "ERROR: faithful LazyLog requires exactly RF metadata endpoints " \
                 "(RF=$REPLICATION_FACTOR, endpoints=$LAZYLOG_METADATA_REPLICA_COUNT)" >&2
            exit 1
        fi
        LAZYLOG_METADATA_CONTRACT="ack1_append_replicated"
        export EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS
    else
        LAZYLOG_METADATA_CONTRACT="legacy_ack1_ordered_visible"
        if [[ "$REQUIRE_FAITHFUL_LAZYLOG" == "1" ]]; then
            echo "ERROR: REQUIRE_FAITHFUL_LAZYLOG=1 needs EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS" >&2
            exit 1
        fi
    fi
fi

if [[ "$SEQUENCER" == "LAZYLOG" && "$ACK" == "1" ]]; then
    ACK_DURABILITY_CONTRACT="$LAZYLOG_METADATA_CONTRACT"
elif [[ "$SEQUENCER" == "CORFU" && "$ACK" == "2" && "$REPLICATION_FACTOR" -ge 2 ]]; then
    if corfu_uses_memory_copy_sink; then
        ACK_DURABILITY_CONTRACT="ack2_primary_plus_$((REPLICATION_FACTOR - 1))_ordered_memory_copy_replicas"
    else
        ACK_DURABILITY_CONTRACT="ack2_primary_plus_$((REPLICATION_FACTOR - 1))_ordered_media_durable_replicas"
    fi
elif [[ "$SEQUENCER" == "SCALOG" && "$ACK" == "2" && "$REPLICATION_FACTOR" -ge 1 ]]; then
    # Scalog orders from local durable cuts, then ACK2 clamps that order to the
    # minimum media-synced prefix across the configured replica set.
    ACK_DURABILITY_CONTRACT="ack2_minimum_media_durable_replica_prefix"
elif [[ "$SEQUENCER" == "LAZYLOG" && "$ACK" == "2" && "$REPLICATION_FACTOR" -ge 1 ]]; then
    ACK_DURABILITY_CONTRACT="ack2_minimum_media_durable_replica_prefix"
fi

# The metadata replicas are external services. Verify that every configured
# endpoint is accepting TCP connections before starting brokers; otherwise a
# run can appear healthy while every faithful ACK1 remains pinned at zero.
lazylog_metadata_endpoints_ready() {
    local endpoint host port deadline now
    [[ "$SEQUENCER" == "LAZYLOG" ]] || return 0
    [[ -n "${EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS:-}" ]] || return 0
    deadline=$(( $(date +%s) + LAZYLOG_METADATA_READY_TIMEOUT_SEC ))
    IFS=',' read -r -a lazylog_metadata_endpoints <<< "$EMBARCADERO_LAZYLOG_METADATA_ENDPOINTS"
    for endpoint in "${lazylog_metadata_endpoints[@]}"; do
        endpoint="${endpoint//[[:space:]]/}"
        host="${endpoint%:*}"
        port="${endpoint##*:}"
        if [[ -z "$host" || -z "$port" || "$host" == "$port" || ! "$port" =~ ^[0-9]+$ ]]; then
            echo "ERROR: LazyLog metadata endpoint must be host:port (got '$endpoint')" >&2
            return 1
        fi
        while true; do
            if timeout 1 bash -c 'exec 3<>"/dev/tcp/$1/$2"' _ "$host" "$port" 2>/dev/null; then
                break
            fi
            now=$(date +%s)
            if (( now >= deadline )); then
                echo "ERROR: LazyLog metadata endpoint did not become reachable within " \
                     "${LAZYLOG_METADATA_READY_TIMEOUT_SEC}s: $endpoint" >&2
                return 1
            fi
            sleep 0.2
        done
    done
}

# RF>1 Corfu ACK2 is meaningful only after every target service in the
# broker-indexed membership map is listening.  Broker readiness alone covers
# the data plane, not the distinct replica gRPC listeners.
corfu_replica_endpoints_ready() {
    local endpoint spec host port deadline now
    declare -A _corfu_hosts=()
    [[ "$SEQUENCER" == "CORFU" && "$REPLICATION_FACTOR" -gt 1 ]] || return 0
    [[ -n "${EMBARCADERO_CORFU_REPLICA_ENDPOINTS:-}" ]] || return 1
    deadline=$(( $(date +%s) + ${CORFU_REPLICA_READY_TIMEOUT_SEC:-30} ))
    IFS=',' read -r -a _corfu_replica_specs <<< "$EMBARCADERO_CORFU_REPLICA_ENDPOINTS"
    for spec in "${_corfu_replica_specs[@]}"; do
        spec="${spec//[[:space:]]/}"
        endpoint="${spec#*@}"
        host="${endpoint%:*}"; port="${endpoint##*:}"
        if [[ "$spec" != *@* || -z "$host" || -z "$port" || "$host" == "$port" || ! "$port" =~ ^[0-9]+$ ]]; then
            echo "ERROR: Corfu replica entry must be broker_id@host:port (got '$spec')" >&2
            return 1
        fi
        if [[ "${EMBARCADERO_CORFU_REQUIRE_DISTINCT_FAILURE_DOMAINS:-0}" == "1" ]]; then
            if [[ -n "${_corfu_hosts[$host]:-}" ]]; then
                echo "ERROR: Corfu host-failure-tolerant mode requires distinct replica hosts; duplicate host '$host'" >&2
                return 1
            fi
            _corfu_hosts[$host]=1
        fi
        while ! timeout 1 bash -c 'exec 3<>"/dev/tcp/$1/$2"' _ "$host" "$port" 2>/dev/null; do
            now=$(date +%s)
            if (( now >= deadline )); then
                echo "ERROR: Corfu replica endpoint did not become reachable: $spec" >&2
                return 1
            fi
            sleep 0.2
        done
    done
}

corfu_seq_ip_is_loopback() {
    case "${EMBARCADERO_CORFU_SEQ_IP:-}" in
        ""|127.0.0.1|localhost)
            return 0
            ;;
        *)
            return 1
            ;;
    esac
}

corfu_uses_remote_clients() {
    local i host
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        if [[ "$host" != "local" ]]; then
            return 0
        fi
    done
    return 1
}

has_remote_clients() {
    local i host
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        if [[ "$host" != "local" ]]; then
            return 0
        fi
    done
    return 1
}

all_active_clients_are_remote() {
    local i host
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        if [[ "$host" == "local" ]]; then
            return 1
        fi
    done
    return 0
}

# ORDER=5 multi-client striping: do not auto-set EMBARCADERO_ORDER5_HOME_BROKERS.
# Full stripe is the default once ReqReceive is sized for N×THREADS and dead
# preferred queues are fail-closed (2026-07-12). Set HOME_BROKERS explicitly only
# when intentionally biasing striping; PublishThreads then connect only to homes.

# Ensure broker ReqReceive pool can hold every concurrent publish connection
# (NUM_CLIENTS × THREADS_PER_BROKER per broker) plus ACK/precreate headroom.
_min_network_io=$(( NUM_CLIENTS * THREADS_PER_BROKER + 8 ))
if (( _min_network_io > 64 )); then
    _min_network_io=64
fi
if (( EMBARCADERO_NETWORK_IO_THREADS < _min_network_io )); then
    if (( _EMB_NET_IO_USERSET == 1 )); then
        echo "WARNING: EMBARCADERO_NETWORK_IO_THREADS=$EMBARCADERO_NETWORK_IO_THREADS < required floor $_min_network_io (NUM_CLIENTS*THREADS_PER_BROKER+8); raising to avoid SessionOpen starvation" >&2
    fi
    EMBARCADERO_NETWORK_IO_THREADS=$_min_network_io
fi

if [[ "$SEQUENCER" == "CORFU" && "$CONTROL_TRANSPORT" == "grpc" ]] && \
   corfu_uses_remote_clients && corfu_seq_ip_is_loopback; then
    echo "ERROR: CORFU with SSH clients requires EMBARCADERO_CORFU_SEQ_IP to be a routable non-loopback address." >&2
    if [[ -n "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        echo "       Sequencer host: $REMOTE_CORFU_SEQUENCER_HOST" >&2
        echo "       Hint: ssh $REMOTE_CORFU_SEQUENCER_HOST 'hostname -I'" >&2
    else
        echo "       Hint: set EMBARCADERO_CORFU_SEQ_IP to the sequencer host dataplane IP." >&2
        echo "       Local interfaces: $(hostname -I 2>/dev/null || true)" >&2
    fi
    exit 1
fi

# ---------------------------------------------------------------------------
# Source lifecycle helpers (provides broker_local_wait_for_cluster, etc.)
# broker_is_remote_mode() tests for REMOTE_BROKER_HOST; unset it so we stay local
# ---------------------------------------------------------------------------
unset REMOTE_BROKER_HOST
export PROJECT_ROOT
source "$SCRIPT_DIR/lib/broker_lifecycle.sh"
broker_init_paths

embar_default_numa_membind() {
  if command -v numactl >/dev/null 2>&1 && numactl -H 2>/dev/null | grep -qE '^node 2 cpus:'; then
    echo "1,2"
  else
    echo "1"
  fi
}

# Inherited LazyLog sequencer IP breaks all-local broker clusters unless a remote sequencer is in use.
if [[ "${EMBARCADERO_KEEP_LAZYLOG_SEQ_ENV:-0}" != "1" ]] && [[ -z "${REMOTE_LAZYLOG_SEQUENCER_HOST:-}" ]]; then
  unset EMBARCADERO_LAZYLOG_SEQ_IP EMBARCADERO_LAZYLOG_SEQ_PORT
fi
if [[ "$SEQUENCER" == "LAZYLOG" && -z "${REMOTE_LAZYLOG_SEQUENCER_HOST:-}" ]]; then
  EMBARCADERO_LAZYLOG_SEQ_IP="${EMBARCADERO_LAZYLOG_SEQ_IP:-$BROKER_IP}"
fi

_default_mb="$(embar_default_numa_membind)"
EMBARLET_NUMA_BIND="${EMBARLET_NUMA_BIND:-numactl --cpunodebind=1 --membind=${_default_mb}}"
unset _default_mb
[[ "$SEQUENCER" == "CORFU" ]] && EMBARLET_NUMA_BIND=""

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
log() { [ "$QUIET" != "1" ] && echo "$*"; }

START_BROKERS_FAILURE_REASON=""

print_local_resource_snapshot() {
    local mem_available_kb huge_free huge_rsvd huge_surp page_kb huge_free_mb shm_avail_kb
    mem_available_kb="$(awk '/MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    huge_free="$(awk '/HugePages_Free:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    huge_rsvd="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    huge_surp="$(awk '/HugePages_Surp:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    page_kb="$(awk '/Hugepagesize:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    shm_avail_kb="$(df -Pk /dev/shm 2>/dev/null | awk 'NR==2 {print $4}' || echo 0)"
    huge_free_mb=$(( huge_free * page_kb / 1024 ))
    log "Resource snapshot: MemAvailable=$(( mem_available_kb / 1024 ))MB HugePagesFree=${huge_free} (${huge_free_mb}MB) HugePagesRsvd=${huge_rsvd} HugePagesSurp=${huge_surp} /dev/shm_avail=$(( shm_avail_kb / 1024 ))MB"
}

# Wait for hugepage reservations held by dying/crashed brokers to drain.
# HugePages_Free stays high while a torn-down mapping still holds Rsvd pages;
# launching a new broker in that window can SIGBUS at first touch (observed
# in e2_embar5_rf0_n1 trial 2, run 20260711T003924Z: SIGBUS @CXL mmap offset
# 0x2000 with a "full" pool). Bounded wait, warning on timeout, never fatal.
wait_for_hugepage_reservations_released() {
    [[ "${EMBAR_USE_HUGETLB:-1}" == "1" ]] || return 0
    local timeout_sec="${HUGEPAGE_SETTLE_TIMEOUT_SEC:-15}"
    local deadline=$(( $(date +%s) + timeout_sec ))
    local rsvd
    while :; do
        rsvd="$(awk '/HugePages_Rsvd:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
        if [[ "$rsvd" -eq 0 ]]; then
            return 0
        fi
        if [[ "$(date +%s)" -ge "$deadline" ]]; then
            break
        fi
        sleep 0.5
    done
    echo "WARNING: HugePages_Rsvd=${rsvd} still held after ${timeout_sec}s settle — broker may SIGBUS at first touch" >&2
    return 0
}

preflight_local_broker_resources() {
    local mem_available_kb huge_free page_kb huge_free_mb
    mem_available_kb="$(awk '/MemAvailable:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    huge_free="$(awk '/HugePages_Free:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    page_kb="$(awk '/Hugepagesize:/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)"
    huge_free_mb=$(( huge_free * page_kb / 1024 ))
    print_local_resource_snapshot
    if [[ "$mem_available_kb" -lt $(( 8 * 1024 * 1024 )) ]]; then
        echo "WARNING: low MemAvailable before broker start: $(( mem_available_kb / 1024 ))MB" >&2
    fi
    if [[ "${EMBAR_USE_HUGETLB:-1}" == "1" && "$huge_free_mb" -lt 8192 ]]; then
        echo "WARNING: low free hugepages before broker start: ${huge_free_mb}MB" >&2
    fi
    wait_for_hugepage_reservations_released
    return 0
}

classify_broker_startup_failure() {
    local -a pids=( "$@" )
    local idx pid log_file
    for idx in "${!pids[@]}"; do
        pid="${pids[$idx]}"
        log_file="/tmp/broker_${idx}.log"
        if [[ -n "$pid" ]] && ! kill -0 "$pid" 2>/dev/null; then
            if grep -Eqi 'Killed|Out of memory|oom-kill|oom_reaper|std::bad_alloc' "$log_file" 2>/dev/null; then
                printf '%s\n' "infra_broker_killed"
                return
            fi
            printf '%s\n' "infra_broker_exited"
            return
        fi
    done
    printf '%s\n' "infra_broker_startup_timeout"
}

infer_broker_cpu_numa() {
    if [[ "$EMBARLET_NUMA_BIND" =~ --cpunodebind=([0-9]+) ]]; then
        printf '%s\n' "${BASH_REMATCH[1]}"
        return
    fi
    printf '%s\n' "1"
}

resolve_local_client_numa() {
    if [[ -n "${LOCAL_CLIENT_NUMA:-}" ]]; then
        printf '%s\n' "$LOCAL_CLIENT_NUMA"
        return
    fi
    # Publication throughput uses the broker-node local client on NUMA 0 to match
    # the requested study topology and avoid cross-NUMA loopback penalties.
    printf '%s\n' "0"
}

resolve_client_numa() {
    local host="$1"
    local default_numa="$2"
    if [[ "$host" == "local" ]]; then
        resolve_local_client_numa
        return
    fi
    printf '%s\n' "$default_numa"
}

verify_client_binary() {
    local host="$1"
    local client_bin="$BUILD_BIN"
    if [[ "$host" != "local" ]]; then
        client_bin="$REMOTE_CLIENT_BIN_DIR"
    fi
    local remote_cmd="
set -e
cd '$client_bin'
if [ ! -x ./throughput_test ]; then
    echo 'missing throughput_test in $client_bin'
    exit 10
fi
if command -v ldd >/dev/null 2>&1; then
    LD_LIBRARY_PATH=\"$CLIENT_LD_LIBRARY_PATH\" ldd ./throughput_test 2>/dev/null | grep -E 'not found|GLIBC_|GLIBCXX_' && exit 11 || true
fi
"
    if [[ "$host" == "local" ]]; then
        bash -lc "$remote_cmd"
    else
        ssh "$host" "$remote_cmd"
    fi
}

preflight_clients() {
    local failed=0
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        local host="${CLIENT_HOSTS[$i]}"
        if ! verify_client_binary "$host"; then
            echo "ERROR: client preflight failed on host '$host'." >&2
            echo "       Ensure /home/domin/Embarcadero/build/bin/throughput_test exists and is runnable on that host." >&2
            failed=1
        fi
    done
    return "$failed"
}

require_local_executable() {
    local executable="$1"
    if [[ ! -x "$BUILD_BIN/$executable" ]]; then
        echo "ERROR: required local executable is missing or not executable: $BUILD_BIN/$executable" >&2
        echo "       Build the selected configuration with: cmake --build <build-dir> --target $executable" >&2
        return 1
    fi
}

print_dry_run() {
    local seq_bin
    seq_bin="$(baseline_sequencer_binary)"
    echo "DRY_RUN=1: no processes will be started."
    echo "CONTROL_TRANSPORT=$CONTROL_TRANSPORT SEQUENCER=$SEQUENCER RF=$REPLICATION_FACTOR REMOTE_REPLICAS=$(( REPLICATION_FACTOR > 0 ? REPLICATION_FACTOR - 1 : 0 )) ACK=$ACK BATCH_SIZE=$EMBARCADERO_BATCH_SIZE CLIENT_PUB_BATCH_KB=$EMBARCADERO_CLIENT_PUB_BATCH_KB"
    echo "BUILD_BIN=$BUILD_BIN"
    echo "BROKER_COMMAND=$BUILD_BIN/embarlet --broker_id <0..$((NUM_BROKERS - 1))>"
    if [[ -n "$seq_bin" ]]; then
        echo "SEQUENCER_COMMAND=$BUILD_BIN/$seq_bin"
    else
        echo "SEQUENCER_COMMAND=not_applicable"
    fi
    echo "CLIENT_COMMAND=$BUILD_BIN/throughput_test --sequencer $SEQUENCER"
}

probe_tcp_from_host() {
    local host="$1"
    local ip="$2"
    local port="$3"
    local probe_cmd="timeout 1 bash -lc '</dev/tcp/$ip/$port' >/dev/null 2>&1"
    if [[ "$host" == "local" ]]; then
        eval "$probe_cmd"
    else
        ssh "$host" "$probe_cmd" >/dev/null 2>&1
    fi
}

wait_for_broker_reachability() {
    local deadline=$(( $(date +%s) + BROKER_REACHABILITY_TIMEOUT_SEC ))
    local i j host port

    while (( $(date +%s) < deadline )); do
        local all_ok=1
        local missing=""

        for (( i=0; i<NUM_CLIENTS; i++ )); do
            host="${CLIENT_HOSTS[$i]}"
            for (( j=0; j<NUM_BROKERS; j++ )); do
                port=$((BROKER_DATA_PORT_BASE + j))
                if ! probe_tcp_from_host "$host" "$BROKER_IP" "$port"; then
                    all_ok=0
                    missing+=" ${host}:${BROKER_IP}:${port}"
                fi
            done
            if [[ -n "${BROKER_HEARTBEAT_PORT:-}" ]]; then
                if ! probe_tcp_from_host "$host" "$BROKER_IP" "$BROKER_HEARTBEAT_PORT"; then
                    all_ok=0
                    missing+=" ${host}:${BROKER_IP}:${BROKER_HEARTBEAT_PORT}"
                fi
            fi
        done

        if [[ "$all_ok" -eq 1 ]]; then
            return 0
        fi

        log "Waiting for client-side broker reachability:${missing}"
        sleep "$BROKER_REACHABILITY_POLL_SEC"
    done

    return 1
}

# Corfu's publisher obtains a token through the selected broker's membership
# ingress. Verify those endpoints before client launch, rather than letting a
# missing proxy turn into a workload timeout after payload admission begins.
wait_for_corfu_proxy_reachability() {
    [[ "$SEQUENCER" == "CORFU" ]] || return 0
    local deadline=$(( $(date +%s) + BROKER_REACHABILITY_TIMEOUT_SEC ))
    local i j host port
    while (( $(date +%s) < deadline )); do
        local all_ok=1
        for (( i=0; i<NUM_CLIENTS; i++ )); do
            host="${CLIENT_HOSTS[$i]}"
            for (( j=0; j<NUM_BROKERS; j++ )); do
                port=$((EMBARCADERO_CORFU_PROXY_PORT_BASE + j))
                if ! probe_tcp_from_host "$host" "$BROKER_IP" "$port"; then
                    all_ok=0
                fi
            done
        done
        [[ "$all_ok" -eq 1 ]] && return 0
        sleep "$BROKER_REACHABILITY_POLL_SEC"
    done
    echo "ERROR: Corfu membership ingress proxies are not reachable from every client host." >&2
    return 1
}

# Resolve the mailbox backing from the processes that actually participated in
# this attempt.  Intent/configuration is not sufficient evidence: a POSIX
# fallback or a failed standalone sequencer attach would otherwise be recorded
# as a CXL result.  This is deliberately called only after broker readiness,
# when broker 0 has initialized the in-place layout and the mailbox sequencer
# has attached to it.
record_mailbox_runtime_backing() {
    [[ "$CONTROL_TRANSPORT" == "cxl_mailbox" ]] || return 0

    local sequencer_log="/tmp/${SEQUENCER,,}_mailbox_sequencer.log"
    local backing=""
    if grep -q 'cxl_type=Real dax_backed=1' /tmp/broker_0.log 2>/dev/null && \
       grep -q 'BASELINE_MAILBOX_SEQUENCER_ATTACHED backing=dax ' "$sequencer_log" 2>/dev/null; then
        backing="dax"
    elif grep -q 'cxl_type=Real dax_backed=0' /tmp/broker_0.log 2>/dev/null && \
         grep -q 'CXL region bound to NUMA node ' /tmp/broker_0.log 2>/dev/null && \
         grep -q 'BASELINE_MAILBOX_SEQUENCER_ATTACHED backing=shm_numa_cxl ' "$sequencer_log" 2>/dev/null; then
        # A shared mapping explicitly mbound to the CXL NUMA node is this
        # machine's production CXL configuration; it is not the private-SHM
        # smoke fallback prohibited by the evaluation contract.
        backing="shm_numa_cxl"
    fi
    if [[ -z "$backing" ]]; then
        echo "ERROR: mailbox runtime backing could not be verified from broker/sequencer logs" >&2
        return 1
    fi

    local broker
    for (( broker=0; broker<NUM_BROKERS; broker++ )); do
        local marker='BASELINE_MAILBOX_READY'
        [[ "$broker" -eq 0 ]] || marker='BASELINE_MAILBOX_ATTACHED'
        if ! grep -q "$marker" "/tmp/broker_${broker}.log" 2>/dev/null; then
            echo "ERROR: broker $broker did not verify its in-place baseline mailbox attachment" >&2
            return 1
        fi
    done

    # There is one immutable contract per invocation, so replace only the
    # placeholder field rather than trusting a caller-provided label.
    local contract_tmp="${RUN_CONTRACT_CSV}.tmp"
    awk -F',' -v OFS=',' -v backing="$backing" \
        'NR == 1 { print; next } { $4 = backing; print }' \
        "$RUN_CONTRACT_CSV" > "$contract_tmp"
    mv "$contract_tmp" "$RUN_CONTRACT_CSV"
    echo "$backing" > "$LOG_DIR/mailbox_runtime_backing.txt"
    log "Verified mailbox runtime backing: $backing"
}

# The Corfu client records the two client-visible phases itself.  Treat a
# missing/invalid record as a failed correctness cell: successful ACKs alone
# cannot prove that payload admission waited for a token grant.
validate_corfu_phase_evidence() {
    local trial="$1"
    [[ "$SEQUENCER" == "CORFU" ]] || return 0
    local index log_file line requests grants payload violations
    for (( index=0; index<${#CLIENT_LOGS[@]}; index++ )); do
        log_file="${CLIENT_LOGS[$index]}"
        line="$(grep '\[CORFU_TOKEN_PHASE\]' "$log_file" 2>/dev/null | tail -1 || true)"
        if [[ ! "$line" =~ requests=([0-9]+)[[:space:]]+grants=([0-9]+)[[:space:]]+payload_sends=([0-9]+)[[:space:]]+payload_before_grant=([0-9]+) ]]; then
            echo "ERROR: missing or malformed CORFU_TOKEN_PHASE evidence in $log_file" >&2
            return 1
        fi
        requests="${BASH_REMATCH[1]}"; grants="${BASH_REMATCH[2]}"
        payload="${BASH_REMATCH[3]}"; violations="${BASH_REMATCH[4]}"
        if (( requests == 0 || grants != requests || payload != grants || violations != 0 )); then
            echo "ERROR: Corfu phase invariant failed in $log_file: $line" >&2
            return 1
        fi
        echo "$trial,${CLIENT_TAGS[$index]},$requests,$grants,$payload,$violations" >> "$CORFU_PHASE_CSV"
    done
}

wait_for_order5_cluster_convergence() {
    if [[ "$SEQUENCER" != "EMBARCADERO" || "$ORDER" != "5" ]]; then
        return 0
    fi

    local timeout_sec="${ORDER5_CLUSTER_READY_TIMEOUT_SEC}"
    local deadline=$(( $(date +%s) + timeout_sec ))
    local broker_id expected_scanners scanner_lines
    expected_scanners="$NUM_BROKERS"

    log "Waiting for ORDER=5 head convergence (${expected_scanners} scanner(s), timeout: ${timeout_sec}s)..."
    while (( $(date +%s) < deadline )); do
        if [[ ! -f /tmp/broker_0.log ]]; then
            sleep 0.2
            continue
        fi

        if ! kill -0 "${HEAD_BROKER_PID:-0}" 2>/dev/null; then
            echo "ERROR: head broker exited while waiting for ORDER=5 convergence" >&2
            START_BROKERS_FAILURE_REASON="infra_broker_exited"
            return 1
        fi

        scanner_lines="$(grep -Ec 'Started (initial )?BrokerScannerWorker5 for broker|Started BrokerScannerWorker5 for broker' /tmp/broker_0.log 2>/dev/null || true)"
        if [[ "$scanner_lines" -ge "$expected_scanners" ]]; then
            for (( broker_id=0; broker_id<NUM_BROKERS; broker_id++ )); do
                if ! grep -Eq "Started (initial )?BrokerScannerWorker5 for broker ${broker_id}|Started BrokerScannerWorker5 for broker ${broker_id}" /tmp/broker_0.log 2>/dev/null; then
                    scanner_lines=0
                    break
                fi
            done
            if [[ "$scanner_lines" -ne 0 ]]; then
                log "ORDER=5 head convergence complete: scanner workers active for brokers 0..$((NUM_BROKERS - 1))."
                return 0
            fi
        fi

        sleep 0.2
    done

    echo "ERROR: ORDER=5 head convergence did not complete within ${timeout_sec}s" >&2
    tail -n 120 /tmp/broker_0.log >&2 || true
    START_BROKERS_FAILURE_REASON="infra_order5_cluster_convergence_timeout"
    return 1
}

precreate_embarcadero_order5_topic() {
    if [[ "$SEQUENCER" != "EMBARCADERO" || "$ORDER" != "5" ]]; then
        return 0
    fi

    local precreate_log="/tmp/embarcadero_order5_precreate.log"
    log "Precreating ORDER=5 topic metadata and scanner state..."
    (
        cd "$BUILD_BIN"
        EMBARCADERO_RUNTIME_MODE=throughput \
        EMBARCADERO_HEAD_ADDR="$BROKER_IP" \
        EMBARCADERO_CXL_SHM_NAME="$EMBARCADERO_CXL_SHM_NAME" \
        EMBARCADERO_CXL_ZERO_MODE="$EMBARCADERO_CXL_ZERO_MODE" \
        EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR" \
        EMBARCADERO_ORDER0_FAST_PATH="$EMBARCADERO_ORDER0_FAST_PATH" \
        EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES="$EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES" \
        EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE="$EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE" \
        EMBARCADERO_BATCH_SIZE="$EMBARCADERO_BATCH_SIZE" \
        EMBARCADERO_CLIENT_PUB_BATCH_KB="$EMBARCADERO_CLIENT_PUB_BATCH_KB" \
        EMBARCADERO_NETWORK_IO_THREADS="$EMBARCADERO_NETWORK_IO_THREADS" \
        EMBARCADERO_ORDER5_HOME_BROKERS="$EMBARCADERO_ORDER5_HOME_BROKERS" \
        EMBARCADERO_ACK_TIMEOUT_SEC="${EMBARCADERO_ACK_TIMEOUT_SEC:-}" \
        LD_LIBRARY_PATH="$CLIENT_LD_LIBRARY_PATH" \
        EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}" \
        numactl --cpunodebind="$(resolve_local_client_numa)" --membind="$(resolve_local_client_numa)" \
            ./throughput_test \
            --config "$CLIENT_CONFIG_ABS" \
            -n "$THREADS_PER_BROKER" \
            -m "$MESSAGE_SIZE" \
            -s 0 \
            -t "$TEST_TYPE" \
            -o "$ORDER" \
            -a 0 \
            -r "$REPLICATION_FACTOR" \
            --sequencer "$SEQUENCER" \
            -l 0
    ) >"$precreate_log" 2>&1
    local status=$?
    if [[ "$status" -ne 0 ]]; then
        echo "ERROR: ORDER=5 topic precreate failed" >&2
        cat "$precreate_log" >&2 || true
        return 1
    fi
    return 0
}

compute_overlap_throughput_gbps() {
    local trial="$1"
    shift
    local -a files=("$@")
    local tmp_bounds
    tmp_bounds="$(mktemp)"
    local file
    local layer_col="${OVERLAP_LAYER_COL:-Cum_Ack_Bytes}"
    local rate_col="${OVERLAP_RATE_COL:-Ack_GiBps}"

    for file in "${files[@]}"; do
        [[ -s "$file" ]] || continue
        awk -F',' -v layer="$layer_col" '
            NR==1 {
                for (i = 1; i <= NF; ++i) {
                    if ($i == layer) layer_i = i
                    if ($i == "Cum_Ack_Bytes") ack_i = i
                    if ($i == "Cum_Sent_Bytes") sent_i = i
                    if ($i == "Total_GBps") total_i = i
                }
                if (!layer_i) layer_i = (ack_i ? ack_i : (sent_i ? sent_i : 0))
                next
            }
            layer_i > 0 {
                ts = $1 + 0
                bytes = $layer_i + 0
                if (bytes > 0) {
                    if (!seen) { first = ts; first_bytes = bytes; seen = 1 }
                    last = ts
                    last_bytes = bytes
                }
            }
            END {
                if (seen && last > first) {
                    printf "%s,%s,%s,%s,%s\n", FILENAME, first, last, first_bytes, last_bytes
                }
            }
        ' "$file" >> "$tmp_bounds"
    done

    if [[ ! -s "$tmp_bounds" ]]; then
        rm -f "$tmp_bounds"
        return 1
    fi

    local expected_clients=${#files[@]}
    local valid_bounds
    valid_bounds="$(mktemp)"
    awk -F',' '
        ($3 - $2) >= 500 { print; next }
        {
            printf "WARNING: degenerate timeseries window (%d ms) in %s — excluded from overlap\n",
                   ($3 - $2), $1 > "/dev/stderr"
        }
    ' "$tmp_bounds" > "$valid_bounds"
    mv -f "$valid_bounds" "$tmp_bounds"
    local bounds_count
    bounds_count="$(wc -l < "$tmp_bounds")"
    if [[ "$bounds_count" -lt "$expected_clients" ]]; then
        echo "WARNING: overlap aborted — only ${bounds_count}/${expected_clients} clients have usable timeseries" >&2
        rm -f "$tmp_bounds"
        return 1
    fi

    # True wall-clock intersection of active windows (not phase-aligned means).
    local overlap_start overlap_end
    overlap_start="$(awk -F',' 'NR==1{m=$2} {if($2>m)m=$2} END{printf "%.0f", m}' "$tmp_bounds")"
    overlap_end="$(awk -F',' 'NR==1{m=$3} {if($3<m)m=$3} END{printf "%.0f", m}' "$tmp_bounds")"
    if [[ -z "$overlap_start" || -z "$overlap_end" || "$overlap_end" -le "$overlap_start" ]]; then
        rm -f "$tmp_bounds"
        return 1
    fi

    local overlap_window_ms=$((overlap_end - overlap_start))
    # Publication gate: require >= 10s shared steady overlap when available.
    local min_overlap_ms="${MIN_OVERLAP_MS:-10000}"
    if [[ "$overlap_window_ms" -lt 500 ]]; then
        echo "WARNING: overlap window only ${overlap_window_ms} ms (< 500 ms)" >&2
        rm -f "$tmp_bounds"
        return 1
    fi
    if [[ "$overlap_window_ms" -lt "$min_overlap_ms" ]]; then
        echo "WARNING: overlap window ${overlap_window_ms} ms < MIN_OVERLAP_MS=${min_overlap_ms}; reporting anyway with caution" >&2
    fi

    local sum_gibps="0"
    local client_count=0
    local ts_file first_ts last_ts first_bytes last_bytes
    while IFS=',' read -r ts_file first_ts last_ts first_bytes last_bytes; do
        [[ -f "$ts_file" ]] || continue
        local gibps
        gibps="$(awk -F',' -v s="$overlap_start" -v e="$overlap_end" -v layer="$layer_col" '
            NR==1 {
                for (i = 1; i <= NF; ++i) {
                    if ($i == layer) layer_i = i
                    if ($i == "Cum_Ack_Bytes") ack_i = i
                    if ($i == "Cum_Sent_Bytes") sent_i = i
                }
                if (!layer_i) layer_i = (ack_i ? ack_i : sent_i)
                next
            }
            layer_i > 0 {
                ts = $1 + 0
                b = $layer_i + 0
                if (ts >= s && !have_start) { start_b = b; have_start = 1 }
                if (ts <= e) { end_b = b; have_end = 1 }
            }
            END {
                if (have_start && have_end && e > s) {
                    delta = end_b - start_b
                    if (delta < 0) delta = 0
                    printf "%.9f", (delta / ((e - s) / 1000.0)) / (1024.0*1024.0*1024.0)
                }
            }
        ' "$ts_file")"
        if [[ -n "$gibps" ]]; then
            sum_gibps="$(awk -v a="$sum_gibps" -v b="$gibps" 'BEGIN {printf "%.9f", a + b}')"
            client_count=$((client_count + 1))
        fi
    done < "$tmp_bounds"
    rm -f "$tmp_bounds"

    if [[ "$client_count" -le 0 ]]; then
        return 1
    fi

    printf "%s,%s,%s,%s,%s\n" "$trial" "$sum_gibps" "$overlap_window_ms" "$client_count" "$rate_col"
}

shm_cleanup() {
    local path="/dev/shm${EMBARCADERO_CXL_SHM_NAME}"
    [[ -e "$path" ]] || return 0
    if [[ "$DEFER_CXL_SHM_CLEANUP" == "1" ]]; then
        # unlink(2) on a large tmpfs object may synchronously reclaim populated
        # pages. It is safe to defer because every attempt has a distinct name.
        # The background reaper inherits every open descriptor by default.
        # In particular, it must close the harness flock descriptor: otherwise
        # a large deferred unlink keeps RUN_LOCK_FILE locked after this
        # run_multiclient process exits, and the next sweep cell is rejected as
        # a concurrent orchestrator.
        ( exec {RUN_LOCK_FD}>&-; nice -n 19 rm -f "$path" >/dev/null 2>&1 ) &
        # broker_local_cleanup reaps shell jobs at the next attempt; detach
        # this reaper so it can finish reclaiming the prior attempt's pages.
        disown "$!" 2>/dev/null || true
    else
        rm -f "$path" 2>/dev/null || true
    fi
}

wait_for_broker_ports_free() {
    local timeout_sec="${BROKER_PORT_DRAIN_TIMEOUT_SEC:-20}"
    local deadline=$(( $(date +%s) + timeout_sec ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        local listeners=""
        if [ -n "${BROKER_HEARTBEAT_PORT:-}" ]; then
            listeners="$(ss -H -ltn "( sport = :${BROKER_DATA_PORT_BASE} or sport = :${BROKER_HEARTBEAT_PORT} )" 2>/dev/null || true)"
        else
            listeners="$(ss -H -ltn "sport = :${BROKER_DATA_PORT_BASE}" 2>/dev/null || true)"
        fi
        if [ -z "$listeners" ]; then
            return 0
        fi
        sleep 0.2
    done
    echo "WARNING: broker port drain timed out after ${timeout_sec}s; continuing with restart" >&2
    return 0
}

CLEANUP_DONE=0
GLOBAL_SEQUENCER_PID=""
# Exact remote client PID files are populated per attempt.  Never use a broad
# remote `pkill -f throughput_test`: remote machines are shared by independent
# experimenters.
declare -a CLIENT_REMOTE_PID_HOSTS=()
declare -a CLIENT_REMOTE_PID_FILES=()
cleanup() {
    # Trap EXIT also fires after an explicit cleanup(); make teardown idempotent so we
    # do not double-pay port drain / process kill / remote pkill on every trial.
    if [[ "${CLEANUP_DONE}" -eq 1 ]]; then
        return 0
    fi
    CLEANUP_DONE=1
    log "Cleaning up..."
    # The local baseline sequencer is not a broker and has no listener on a
    # broker port, so terminate and reap its exact PID explicitly.  This keeps
    # teardown bounded and avoids leaving a mailbox poller attached to a CXL
    # mapping after the next cell starts.
    if [[ -n "$GLOBAL_SEQUENCER_PID" ]]; then
        kill "$GLOBAL_SEQUENCER_PID" >/dev/null 2>&1 || true
        for _ in $(seq 1 20); do
            kill -0 "$GLOBAL_SEQUENCER_PID" >/dev/null 2>&1 || break
            sleep 0.1
        done
        kill -9 "$GLOBAL_SEQUENCER_PID" >/dev/null 2>&1 || true
        # Do not wait indefinitely after SIGKILL; bounded port/process checks
        # below provide the next-cell safety condition.
        GLOBAL_SEQUENCER_PID=""
    fi
    broker_local_cleanup
    if [[ "$SEQUENCER" == "CORFU" && -n "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        broker_remote_corfu_stop || true
  elif [[ "$SEQUENCER" == "LAZYLOG" && -n "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
        broker_remote_lazylog_stop || true
    fi
    if [[ "$SEQUENCER" == "SCALOG" && -n "$REMOTE_SCALOG_SEQUENCER_HOST" ]]; then
        broker_remote_scalog_stop || true
    fi
    shm_cleanup
    # broker_local_cleanup already drained listeners; a second wait_for_broker_ports_free
    # here used to add up to another BROKER_PORT_DRAIN_TIMEOUT_SEC of polling.
    if [[ "${EMBARCADERO_DISABLE_PATTERN_KILL:-0}" != "1" ]]; then
        for (( _i=0; _i<${#CLIENT_REMOTE_PID_FILES[@]}; _i++ )); do
            _h="${CLIENT_REMOTE_PID_HOSTS[$_i]}"
            _pid_file="${CLIENT_REMOTE_PID_FILES[$_i]}"
            ssh "$_h" "if test -r '$_pid_file'; then read -r _pid < '$_pid_file'; kill -TERM \"\$_pid\" 2>/dev/null || true; sleep 0.1; kill -KILL \"\$_pid\" 2>/dev/null || true; rm -f '$_pid_file'; fi" \
                2>/dev/null || true
        done
    fi
}
trap cleanup EXIT

start_brokers() {
    START_BROKERS_FAILURE_REASON=""
    CLEANUP_DONE=0
    log "Resetting previous broker state..."
    broker_local_cleanup
    shm_cleanup
    # Ports should already be clear after broker_local_drain_ports; keep a short
    # bound so a stuck TIME_WAIT listener cannot burn the full 20s budget every trial.
    BROKER_PORT_DRAIN_TIMEOUT_SEC="${BROKER_PORT_DRAIN_TIMEOUT_SEC:-5}" wait_for_broker_ports_free
    preflight_local_broker_resources

    # Do this before any process is launched.  Previously a missing standalone
    # baseline sequencer was started in the background, leaving brokers to retry
    # a port that could never become live and obscuring the actual artifact error.
    if ! require_local_executable embarlet; then
        START_BROKERS_FAILURE_REASON="infra_missing_local_broker_binary"
        return 1
    fi
    if [[ "$SEQUENCER" == "CORFU" && -z "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        if ! require_local_executable "$(baseline_sequencer_binary)"; then
            START_BROKERS_FAILURE_REASON="infra_missing_local_corfu_sequencer"
            return 1
        fi
    elif [[ "$SEQUENCER" == "LAZYLOG" && -z "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
        if ! require_local_executable "$(baseline_sequencer_binary)"; then
            START_BROKERS_FAILURE_REASON="infra_missing_local_lazylog_sequencer"
            return 1
        fi
    elif [[ "$SEQUENCER" == "SCALOG" ]]; then
        if ! require_local_executable "$(baseline_sequencer_binary)"; then
            START_BROKERS_FAILURE_REASON="infra_missing_local_scalog_sequencer"
            return 1
        fi
    fi

    if [[ "$SEQUENCER" == "CORFU" && -n "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        export REMOTE_CORFU_BUILD_BIN="${REMOTE_CORFU_BUILD_BIN:-$BUILD_BIN}"
        log "Starting remote Corfu sequencer on $REMOTE_CORFU_SEQUENCER_HOST..."
        if ! broker_remote_corfu_start; then
            echo "ERROR: failed to start remote Corfu sequencer on $REMOTE_CORFU_SEQUENCER_HOST" >&2
            return 1
        fi
        sleep 1
    elif [[ "$SEQUENCER" == "LAZYLOG" && -n "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
        export REMOTE_LAZYLOG_BUILD_BIN="${REMOTE_LAZYLOG_BUILD_BIN:-$BUILD_BIN}"
        log "Starting remote LazyLog sequencer on $REMOTE_LAZYLOG_SEQUENCER_HOST..."
        if ! broker_remote_lazylog_start; then
            echo "ERROR: failed to start remote LazyLog sequencer on $REMOTE_LAZYLOG_SEQUENCER_HOST" >&2
            return 1
        fi
        sleep 1
    elif [[ "$SEQUENCER" == "SCALOG" && -n "$REMOTE_SCALOG_SEQUENCER_HOST" ]]; then
        export REMOTE_SCALOG_BUILD_BIN="${REMOTE_SCALOG_BUILD_BIN:-$BUILD_BIN}"
        log "Starting remote Scalog sequencer on $REMOTE_SCALOG_SEQUENCER_HOST..."
        if ! broker_remote_scalog_start; then
            echo "ERROR: failed to start remote Scalog sequencer on $REMOTE_SCALOG_SEQUENCER_HOST" >&2
            return 1
        fi
        sleep 1
    fi

    cd "$BUILD_BIN"

    export EMBAR_USE_HUGETLB="${EMBAR_USE_HUGETLB:-1}"
    export EMBARCADERO_CXL_ZERO_MODE="${EMBARCADERO_CXL_ZERO_MODE:-full}"
    export EMBARCADERO_RUNTIME_MODE="throughput"
    export EMBARCADERO_BASELINE_CONTROL_TRANSPORT="$CONTROL_TRANSPORT"
    export EMBARCADERO_CORFU_PROXY_PORT_BASE
    export EMBARCADERO_REPLICATION_FACTOR="$REPLICATION_FACTOR"
    export EMBARCADERO_HEAD_ADDR="$BROKER_IP"
    export EMBARCADERO_NUM_BROKERS="$NUM_BROKERS"
	# One initial topic needs one CXL log segment per broker.  Ask the broker to
	# reject an impossible real-CXL geometry during startup rather than retrying
	# topic discovery until metadata allocation itself fails.
    export EMBARCADERO_REQUIRED_CXL_SEGMENTS="$NUM_BROKERS"
    local -a corfu_durability_args=()
    if [[ "$SEQUENCER" == "CORFU" && "$REPLICATION_FACTOR" -gt 1 ]]; then
        local corfu_sink
        corfu_sink="$(corfu_replica_sink_mode)"
        if [[ "$corfu_sink" == "memory-copy" ]]; then
            # The Corfu service retains a private payload copy per WriteOnce.
            # Do not pass --replicate_to_disk or let a configured disk path
            # silently turn this labeled memory cell into a media-durable one.
            unset EMBARCADERO_REPLICA_DISK_DIRS EMBARCADERO_REPLICA_DISK_ROOT
            log "Corfu RF membership uses ordered memory-copy replicas"
        elif [[ "$corfu_sink" == "disk-durable" ]]; then
            if [[ -z "${EMBARCADERO_REPLICA_DISK_DIRS:-}" && -z "${EMBARCADERO_REPLICA_DISK_ROOT:-}" ]]; then
                echo "ERROR: Corfu RF>1 disk-durable mode requires explicit replica directories" >&2
                START_BROKERS_FAILURE_REASON="config_corfu_missing_durable_replica_dir"
                return 1
            fi
            corfu_durability_args+=(--replicate_to_disk)
        else
            echo "ERROR: Corfu RF>1 requires disk-durable or memory-copy replication; refusing sink '$corfu_sink'" >&2
            START_BROKERS_FAILURE_REASON="config_corfu_unsupported_replica_sink"
            return 1
        fi
        if [[ -z "$EMBARCADERO_CORFU_REPLICA_ENDPOINTS" ]]; then
            local endpoints=""
            for (( _corfu_broker=0; _corfu_broker<NUM_BROKERS; ++_corfu_broker )); do
                endpoints+="${endpoints:+,}${_corfu_broker}@${BROKER_IP}:$((50053 + _corfu_broker))"
            done
            EMBARCADERO_CORFU_REPLICA_ENDPOINTS="$endpoints"
        fi
        export EMBARCADERO_CORFU_REPLICA_ENDPOINTS
        log "Corfu RF membership: $EMBARCADERO_CORFU_REPLICA_ENDPOINTS"
    fi
    export EMBARCADERO_ORDER0_FAST_PATH
    export EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES
    export EMBARCADERO_NETWORK_IO_THREADS
    if [[ -n "${EMBARCADERO_CXL_COHERENT:-}" ]]; then
        export EMBARCADERO_CXL_COHERENT
    fi
    if [[ -n "${EMBARCADERO_CXL_NT_INGEST:-}" ]]; then
        export EMBARCADERO_CXL_NT_INGEST
    fi
    # Chain replication sink profile (forwarded to remote brokers via broker_lifecycle).
    if [[ -n "${EMBARCADERO_CHAIN_REPLICATION_SINK:-}" ]]; then
        export EMBARCADERO_CHAIN_REPLICATION_SINK
    fi
    if [[ -n "${EMBARCADERO_CHAIN_REPLICATION_INMEM:-}" ]]; then
        export EMBARCADERO_CHAIN_REPLICATION_INMEM
    fi
    if [[ -n "${EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY:-}" ]]; then
        export EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY
    fi
    if [[ -n "${EMBARCADERO_CHAIN_REPLICATION_INMEM_BYTES_PER_SOURCE:-}" ]]; then
        export EMBARCADERO_CHAIN_REPLICATION_INMEM_BYTES_PER_SOURCE
    fi
    if [[ -n "${EMBARCADERO_CHAIN_SYNC_BYTES:-}" ]]; then
        export EMBARCADERO_CHAIN_SYNC_BYTES
    fi
    if [[ -n "${EMBARCADERO_CHAIN_SYNC_INTERVAL_MS:-}" ]]; then
        export EMBARCADERO_CHAIN_SYNC_INTERVAL_MS
    fi
    # Pass epoch tuning to embarlet if set; broker reads it in EpochDriverThread.
    if [[ -n "${EMBAR_ORDER5_EPOCH_US:-}" ]]; then
        export EMBAR_ORDER5_EPOCH_US
    fi
    if [[ -n "${EMBAR_ORDER5_COMMIT_PROFILE:-}" ]]; then
        export EMBAR_ORDER5_COMMIT_PROFILE
    fi
    if [[ -n "${EMBARCADERO_SESSION_LEASE_MS:-}" ]]; then
        export EMBARCADERO_SESSION_LEASE_MS
    fi
    if [[ -n "${EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS:-}" ]]; then
        export EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS
    fi
    if [[ -n "${EMBARCADERO_ORDER5_PHASE_DIAG:-}" ]]; then
        export EMBARCADERO_ORDER5_PHASE_DIAG
    fi
    if [[ "$SEQUENCER" == "LAZYLOG" ]]; then
        export LAZYLOG_CXL_MODE=1
    fi
    if [[ "$SEQUENCER" == "LAZYLOG" && -n "${EMBARCADERO_LAZYLOG_SEQ_IP:-}" ]]; then
        export EMBARCADERO_LAZYLOG_SEQ_IP
    fi
    if [[ "$SEQUENCER" == "LAZYLOG" && -n "${EMBARCADERO_LAZYLOG_SEQ_PORT:-}" ]]; then
        export EMBARCADERO_LAZYLOG_SEQ_PORT
    fi
    if [[ "$SEQUENCER" == "SCALOG" ]]; then
        export SCALOG_CXL_MODE="${SCALOG_CXL_MODE:-1}"
    fi

    log "Starting $NUM_BROKERS broker(s) with NUMA bind: '${EMBARLET_NUMA_BIND}'"
    local -a launched_broker_pids=()
    if [[ "$CONTROL_TRANSPORT" == "grpc" && "$SEQUENCER" == "CORFU" && -z "$REMOTE_CORFU_SEQUENCER_HOST" ]]; then
        ./"$(baseline_sequencer_binary)" > /tmp/corfu_sequencer.log 2>&1 &
        GLOBAL_SEQUENCER_PID=$!
    elif [[ "$CONTROL_TRANSPORT" == "grpc" && "$SEQUENCER" == "LAZYLOG" && -z "$REMOTE_LAZYLOG_SEQUENCER_HOST" ]]; then
        ./"$(baseline_sequencer_binary)" > /tmp/lazylog_sequencer.log 2>&1 &
        GLOBAL_SEQUENCER_PID=$!
    elif [[ "$CONTROL_TRANSPORT" == "grpc" && "$SEQUENCER" == "SCALOG" ]]; then
        ./"$(baseline_sequencer_binary)" > /tmp/scalog_sequencer.log 2>&1 &
        GLOBAL_SEQUENCER_PID=$!
    fi

    # Start the head broker first. Cold CXL full-region zeroing is memory-heavy; launching
    # followers concurrently in that mode can OOM-kill broker 0. Metadata-mode startup (used
    # by throughput cells) only faults pages via a joined prefault on the head — followers
    # just map the existing shm — so overlap their launch with the ~12s prefault instead of
    # serializing follower startup after it.
    local zero_mode="${EMBARCADERO_CXL_ZERO_MODE:-full}"
    local wait_head_before_followers=1
    if [[ "$zero_mode" == "metadata" ]]; then
        wait_head_before_followers=0
    fi
    # A per-broker override lets a bounded RF2 integration smoke delay only the
    # remote media replica.  The global variable remains the production/fault
    # injection default when no indexed override is supplied.
    local head_fdatasync_stall_ms="${EMBARCADERO_FDATASYNC_STALL_MS_BROKER_0:-${EMBARCADERO_FDATASYNC_STALL_MS:-}}"
    if [[ -n "$head_fdatasync_stall_ms" ]]; then
        # shellcheck disable=SC2086
        EMBARCADERO_FDATASYNC_STALL_MS="$head_fdatasync_stall_ms" $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" --head "--${SEQUENCER}" "${corfu_durability_args[@]}" \
            --network_threads "$EMBARCADERO_NETWORK_IO_THREADS" \
            > /tmp/broker_0.log 2>&1 &
    else
        # shellcheck disable=SC2086
        $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" --head "--${SEQUENCER}" "${corfu_durability_args[@]}" \
            --network_threads "$EMBARCADERO_NETWORK_IO_THREADS" \
            > /tmp/broker_0.log 2>&1 &
    fi
    launched_broker_pids+=("$!")
    HEAD_BROKER_PID="${launched_broker_pids[0]}"
    if [[ "$CONTROL_TRANSPORT" == "cxl_mailbox" ]]; then
        # A mailbox sequencer must never create or clear shared backing.  The
        # head broker owns initialization; the precise marker is the handoff.
        local mailbox_deadline=$(( $(date +%s) + BROKER_READY_TIMEOUT_SEC ))
        until grep -q "BASELINE_MAILBOX_READY" /tmp/broker_0.log 2>/dev/null; do
            if (( $(date +%s) >= mailbox_deadline )); then
                echo "ERROR: broker 0 did not report BASELINE_MAILBOX_READY" >&2
                START_BROKERS_FAILURE_REASON="infra_mailbox_not_initialized"
                return 1
            fi
            sleep 0.1
        done
        ./"$(baseline_sequencer_binary)" > "/tmp/${SEQUENCER,,}_mailbox_sequencer.log" 2>&1 &
        GLOBAL_SEQUENCER_PID=$!
        # Do not let broker/client readiness race a mailbox sequencer that has
        # merely been forked but has not attached to the published in-place
        # extent (or has already died on an incompatible header).
        local sequencer_log="/tmp/${SEQUENCER,,}_mailbox_sequencer.log"
        local sequencer_deadline=$(( $(date +%s) + BROKER_READY_TIMEOUT_SEC ))
        until grep -q 'BASELINE_MAILBOX_SEQUENCER_ATTACHED ' "$sequencer_log" 2>/dev/null; do
            if ! kill -0 "$GLOBAL_SEQUENCER_PID" 2>/dev/null; then
                echo "ERROR: mailbox sequencer exited before attaching" >&2
                cat "$sequencer_log" >&2 || true
                START_BROKERS_FAILURE_REASON="infra_mailbox_sequencer_attach_failed"
                return 1
            fi
            if (( $(date +%s) >= sequencer_deadline )); then
                echo "ERROR: mailbox sequencer did not attach before timeout" >&2
                START_BROKERS_FAILURE_REASON="infra_mailbox_sequencer_attach_timeout"
                return 1
            fi
            sleep 0.1
        done
    fi
    if [[ "$wait_head_before_followers" -eq 1 ]]; then
        log "Waiting for head broker readiness before followers (zero_mode=$zero_mode)..."
        if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" 1 "${launched_broker_pids[0]}"; then
            echo "ERROR: head broker did not become ready within ${BROKER_READY_TIMEOUT_SEC}s" >&2
            cat /tmp/broker_0.log >&2
            START_BROKERS_FAILURE_REASON="$(classify_broker_startup_failure "${launched_broker_pids[@]}")"
            return 1
        fi
        if [[ "$BROKER_START_STAGGER_SEC" -gt 0 ]]; then
            sleep "$BROKER_START_STAGGER_SEC"
        fi
    else
        log "Overlapping follower launch with head startup (zero_mode=$zero_mode)..."
        # Head creates/sizes the CXL shm early in CXLManager, then spends ~12s in
        # joined segment prefault before becoming ready. A short grace lets the shm
        # exist so followers can attach, while still overlapping the prefault.
        sleep "${BROKER_HEAD_SHM_GRACE_SEC:-2}"
    fi
    for (( i=1; i<NUM_BROKERS; i++ )); do
        local broker_stall_var="EMBARCADERO_FDATASYNC_STALL_MS_BROKER_${i}"
        local broker_fdatasync_stall_ms="${EMBARCADERO_FDATASYNC_STALL_MS:-}"
        if [[ -n "${!broker_stall_var+x}" ]]; then
            broker_fdatasync_stall_ms="${!broker_stall_var}"
        fi
        if [[ -n "$broker_fdatasync_stall_ms" ]]; then
            # shellcheck disable=SC2086
            EMBARCADERO_FDATASYNC_STALL_MS="$broker_fdatasync_stall_ms" $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" "--${SEQUENCER}" "${corfu_durability_args[@]}" \
                --network_threads "$EMBARCADERO_NETWORK_IO_THREADS" \
                > /tmp/broker_"$i".log 2>&1 &
        else
            # shellcheck disable=SC2086
            $EMBARLET_NUMA_BIND ./embarlet --config "$BROKER_CONFIG_ABS" "--${SEQUENCER}" "${corfu_durability_args[@]}" \
                --network_threads "$EMBARCADERO_NETWORK_IO_THREADS" \
                > /tmp/broker_"$i".log 2>&1 &
        fi
        launched_broker_pids+=("$!")
        if [[ "$BROKER_START_STAGGER_SEC" -gt 0 ]]; then
            sleep "$BROKER_START_STAGGER_SEC"
        fi
    done
    # Global copy for failure injection (BROKER_KILL_AFTER_SEC) — indexed by broker id.
    BROKER_PIDS=("${launched_broker_pids[@]}")

    log "Waiting for $NUM_BROKERS broker(s) to become ready (timeout: ${BROKER_READY_TIMEOUT_SEC}s)..."
    if ! broker_local_wait_for_cluster "$BROKER_READY_TIMEOUT_SEC" "$NUM_BROKERS" "${launched_broker_pids[@]}"; then
        echo "ERROR: Brokers did not become ready within ${BROKER_READY_TIMEOUT_SEC}s" >&2
        cat /tmp/broker_0.log >&2
        START_BROKERS_FAILURE_REASON="$(classify_broker_startup_failure "${launched_broker_pids[@]}")"
        log "Broker startup failure classification: ${START_BROKERS_FAILURE_REASON}"
        return 1
    fi
    # Clear ready sentinels so the next trial's wait starts from a clean state
    rm -f /tmp/embarlet_*_ready 2>/dev/null || true
    if ! wait_for_broker_reachability; then
        echo "ERROR: Brokers are not reachable from client host(s) within ${BROKER_REACHABILITY_TIMEOUT_SEC}s" >&2
        START_BROKERS_FAILURE_REASON="infra_broker_unreachable"
        return 1
    fi
    if ! wait_for_corfu_proxy_reachability; then
        START_BROKERS_FAILURE_REASON="infra_corfu_proxy_unreachable"
        return 1
    fi
    if ! corfu_replica_endpoints_ready; then
        START_BROKERS_FAILURE_REASON="infra_corfu_replica_unreachable"
        return 1
    fi
    if [[ "$BROKER_READY_PROPAGATION_SEC" -gt 0 ]]; then
        log "Waiting ${BROKER_READY_PROPAGATION_SEC}s for cluster state propagation..."
        sleep "$BROKER_READY_PROPAGATION_SEC"
    fi
    # LazyLog's global sequencer can publish broker membership before the
    # broker-port service is fully stable.  A bounded settle phase avoids a
    # transient CreateTopic connection refusal on the first client attempt;
    # it applies only to the baseline and is recorded in the cell log.
    if [[ "$SEQUENCER" == "LAZYLOG" ]]; then
        local lazylog_settle_sec="${LAZYLOG_POST_READY_SETTLE_SEC:-3}"
        if [[ "$lazylog_settle_sec" -gt 0 ]]; then
            log "Waiting ${lazylog_settle_sec}s for LazyLog broker-port stabilization..."
            sleep "$lazylog_settle_sec"
        fi
    fi
    if ! precreate_embarcadero_order5_topic; then
        START_BROKERS_FAILURE_REASON="infra_order5_topic_precreate_failed"
        return 1
    fi
    if ! wait_for_order5_cluster_convergence; then
        return 1
    fi
    if [[ "$SEQUENCER" == "SCALOG" && -n "$REMOTE_SCALOG_SEQUENCER_HOST" ]]; then
        local precreate_settle_sec="${SCALOG_PRECREATE_SETTLE_SEC:-15}"
        if [[ "$precreate_settle_sec" -gt 0 ]]; then
            log "Waiting ${precreate_settle_sec}s for Scalog precreate settle..."
            sleep "$precreate_settle_sec"
        fi
        local precreate_attempt precreate_max_attempts precreate_retry_sleep_sec
        precreate_max_attempts="${SCALOG_PRECREATE_ATTEMPTS:-3}"
        precreate_retry_sleep_sec="${SCALOG_PRECREATE_RETRY_SLEEP_SEC:-3}"
        for (( precreate_attempt=1; precreate_attempt<=precreate_max_attempts; precreate_attempt++ )); do
            log "Precreating Scalog topic metadata (attempt ${precreate_attempt}/${precreate_max_attempts})..."
            (
                cd "$BUILD_BIN"
                EMBARCADERO_RUNTIME_MODE=throughput \
                EMBARCADERO_SCALOG_SEQ_IP="${EMBARCADERO_SCALOG_SEQ_IP:-}" \
                EMBARCADERO_SCALOG_SEQ_PORT="${EMBARCADERO_SCALOG_SEQ_PORT:-}" \
                SCALOG_CXL_MODE="${SCALOG_CXL_MODE:-1}" \
                ./throughput_test \
                    --config "$CLIENT_CONFIG_ABS" \
                    --head_addr "$BROKER_IP" \
                    -n "$NUM_BROKERS" \
                    -m "$MESSAGE_SIZE" \
                    -s 0 \
                    -t 5 \
                    -o "$ORDER" \
                    -a "$ACK" \
                    -r "$REPLICATION_FACTOR" \
                    --sequencer "$SEQUENCER" \
                    -l 0 \
                    >/tmp/scalog_topic_precreate.log 2>&1
            ) && break

            if [[ "$precreate_attempt" -lt "$precreate_max_attempts" ]]; then
                log "Scalog topic precreation attempt ${precreate_attempt} failed; retrying after ${precreate_retry_sleep_sec}s..."
                sleep "$precreate_retry_sleep_sec"
                continue
            fi

            echo "ERROR: Scalog topic precreation failed" >&2
            cat /tmp/scalog_topic_precreate.log >&2 || true
            return 1
        done
        log "Waiting for remote Scalog sequencer readiness..."
        if ! broker_wait_for_remote_scalog_ready "${SCALOG_READY_TIMEOUT_SEC:-30}" "$NUM_BROKERS" "$REPLICATION_FACTOR"; then
            echo "ERROR: remote Scalog sequencer did not reach full readiness" >&2
            return 1
        fi
        if [[ "$REPLICATION_FACTOR" -gt 1 ]]; then
            log "Waiting for local Scalog replication pollers to initialize..."
            if ! broker_wait_for_local_scalog_replication_ready "${SCALOG_REPLICATION_READY_TIMEOUT_SEC:-30}" "$NUM_BROKERS" "$REPLICATION_FACTOR"; then
                echo "ERROR: local Scalog replication did not reach full readiness" >&2
                return 1
            fi
        fi
    fi
    log "All $NUM_BROKERS brokers ready and reachable."
}

# ---------------------------------------------------------------------------
# Derived run-time values
# ---------------------------------------------------------------------------
LOAD_PER_CLIENT=$(( TOTAL_MESSAGE_SIZE / NUM_CLIENTS ))

echo "================================================================"
echo "  Embarcadero Multi-Client Throughput Benchmark"
echo "================================================================"
printf "  %-32s %s\n" "NUM_CLIENTS:"                   "$NUM_CLIENTS  (${CLIENT_HOSTS[*]:0:$NUM_CLIENTS})"
printf "  %-32s %s\n" "NUM_BROKERS:"                   "$NUM_BROKERS"
printf "  %-32s %s\n" "NUM_TRIALS:"                    "$NUM_TRIALS"
printf "  %-32s %s\n" "SEQUENCER / ORDER:"             "$SEQUENCER / $ORDER"
printf "  %-32s %s\n" "ACK / REPLICATION_FACTOR:"      "$ACK / $REPLICATION_FACTOR"
# Resolve chain sink labeling for publication metadata (memory != media_durable).
CHAIN_SINK_MODE="disk-durable"
if [[ -n "${EMBARCADERO_CHAIN_REPLICATION_SINK:-}" ]]; then
    CHAIN_SINK_MODE="${EMBARCADERO_CHAIN_REPLICATION_SINK}"
elif [[ "${EMBARCADERO_CHAIN_REPLICATION_INMEM:-0}" == "1" ]]; then
    if [[ "${EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY:-0}" == "1" ]]; then
        CHAIN_SINK_MODE="memory-copy"
    else
        CHAIN_SINK_MODE="memory-accounting"
    fi
fi
case "$CHAIN_SINK_MODE" in
    memory-copy|memory_copy|memory-accounting|memory_accounting|accounting|copy)
        ACK_CLAIM_LABEL="replicated_ack_emulated"
        # Memory-emulated ACK2: CXL-class client credit + NT ingest headroom.
        if [[ "$ACK" == "2" ]]; then
            export EMBARCADERO_ACK2_OFFERED_RATE_BYTES_PER_SEC="${EMBARCADERO_ACK2_OFFERED_RATE_BYTES_PER_SEC:-12884901888}"
            export EMBARCADERO_CXL_NT_INGEST="${EMBARCADERO_CXL_NT_INGEST:-1}"
            export EMBARCADERO_ACK2_RETENTION="${EMBARCADERO_ACK2_RETENTION:-pool_pin}"
            # Pool-pin depth needs multi-GiB hugepage arena (default pipeline ~0.7 GiB).
            export EMBARCADERO_QUEUE_POOL_MAX_BYTES="${EMBARCADERO_QUEUE_POOL_MAX_BYTES:-8589934592}"
        fi
        ;;
    *)
        if [[ "$ACK" == "2" ]]; then
            ACK_CLAIM_LABEL="media_durable"
            # Disk ACK2: larger sync batches + optional capacity weights (fast disk first).
            export EMBARCADERO_CHAIN_SYNC_BYTES="${EMBARCADERO_CHAIN_SYNC_BYTES:-268435456}"
            export EMBARCADERO_ACK2_RETENTION="${EMBARCADERO_ACK2_RETENTION:-owned_rto_copy}"
            if [[ -n "${EMBARCADERO_REPLICA_DISK_DIRS:-}" && -z "${EMBARCADERO_REPLICA_DISK_WEIGHTS:-}" ]]; then
                # Prefer first listed disk (typically the faster NVMe) 4× vs second.
                IFS=',' read -r -a _replica_dirs <<< "$EMBARCADERO_REPLICA_DISK_DIRS"
                if [[ "${#_replica_dirs[@]}" -ge 2 ]]; then
                    export EMBARCADERO_REPLICA_DISK_WEIGHTS="${EMBARCADERO_REPLICA_DISK_WEIGHTS:-4,1}"
                fi
            fi
        else
            ACK_CLAIM_LABEL="n/a"
        fi
        ;;
esac
printf "  %-32s %s\n" "CHAIN_SINK_MODE:"               "$CHAIN_SINK_MODE"
printf "  %-32s %s\n" "ACK_CLAIM_LABEL:"               "$ACK_CLAIM_LABEL"
if [[ "$ACK" == "2" ]]; then
    printf "  %-32s %s\n" "ACK2_OFFERED_RATE:"             "${EMBARCADERO_ACK2_OFFERED_RATE_BYTES_PER_SEC:-default}"
    printf "  %-32s %s\n" "ACK2_RETENTION:"                "${EMBARCADERO_ACK2_RETENTION:-default}"
    printf "  %-32s %s\n" "QUEUE_POOL_MAX_BYTES:"           "${EMBARCADERO_QUEUE_POOL_MAX_BYTES:-default}"
    printf "  %-32s %s\n" "CHAIN_SYNC_BYTES:"              "${EMBARCADERO_CHAIN_SYNC_BYTES:-default}"
    printf "  %-32s %s\n" "REPLICA_DISK_WEIGHTS:"          "${EMBARCADERO_REPLICA_DISK_WEIGHTS:-}"
    printf "  %-32s %s\n" "CXL_NT_INGEST:"                 "${EMBARCADERO_CXL_NT_INGEST:-}"
fi
printf "  %-32s %s\n" "MESSAGE_SIZE:"                  "$MESSAGE_SIZE B"
printf "  %-32s %s\n" "TOTAL_MESSAGE_SIZE:"            "$TOTAL_MESSAGE_SIZE B  ($(( TOTAL_MESSAGE_SIZE / 1024 / 1024 )) MiB)"
printf "  %-32s %s\n" "LOAD_PER_CLIENT:"               "$LOAD_PER_CLIENT B  ($(( LOAD_PER_CLIENT / 1024 / 1024 )) MiB)"
printf "  %-32s %s\n" "THREADS_PER_BROKER:"            "$THREADS_PER_BROKER"
printf "  %-32s %s\n" "BROKER_IP:"                     "$BROKER_IP"
printf "  %-32s %s\n" "START_DELAY_SEC:"               "$START_DELAY_SEC"
printf "  %-32s %s\n" "ORDER0_FAST_PATH:"              "$EMBARCADERO_ORDER0_FAST_PATH"
printf "  %-32s %s\n" "PAYLOAD_SEND_CHUNK_BYTES:"      "$EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES"
printf "  %-32s %s\n" "ENABLE_PAYLOAD_MSG_MORE:"       "$EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE"
printf "  %-32s %s\n" "BATCH_SIZE:"                    "$EMBARCADERO_BATCH_SIZE"
printf "  %-32s %s\n" "CLIENT_PUB_BATCH_KB:"           "$EMBARCADERO_CLIENT_PUB_BATCH_KB"
printf "  %-32s %s\n" "NETWORK_IO_THREADS:"            "$EMBARCADERO_NETWORK_IO_THREADS"
printf "  %-32s %s\n" "CONTROL_TRANSPORT:"            "$CONTROL_TRANSPORT"
printf "  %-32s %s\n" "ORDER5_HOME_BROKERS:"           "${EMBARCADERO_ORDER5_HOME_BROKERS:-"(unset)"}"
printf "  %-32s %s\n" "ORDER5_EPOCH_US:"               "${EMBAR_ORDER5_EPOCH_US:-"(default=500)"}"
printf "  %-32s %s\n" "LOCAL_CLIENT_NUMA:"             "$(resolve_local_client_numa)"
if [[ "$SEQUENCER" == "CORFU" ]]; then
    printf "  %-32s %s\n" "CORFU_SEQ_IP:"               "${EMBARCADERO_CORFU_SEQ_IP:-"(unset)"}"
    printf "  %-32s %s\n" "CORFU_SEQ_PORT:"             "${EMBARCADERO_CORFU_SEQ_PORT:-"(default)"}"
elif [[ "$SEQUENCER" == "LAZYLOG" ]]; then
    printf "  %-32s %s\n" "LAZYLOG_SEQ_IP:"             "${EMBARCADERO_LAZYLOG_SEQ_IP:-"(unset)"}"
    printf "  %-32s %s\n" "LAZYLOG_SEQ_PORT:"           "${EMBARCADERO_LAZYLOG_SEQ_PORT:-"(default)"}"
    printf "  %-32s %s\n" "LAZYLOG_ACK1_CONTRACT:"      "$LAZYLOG_METADATA_CONTRACT"
    printf "  %-32s %s\n" "LAZYLOG_METADATA_REPLICAS:"  "$LAZYLOG_METADATA_REPLICA_COUNT"
elif [[ "$SEQUENCER" == "SCALOG" ]]; then
    printf "  %-32s %s\n" "SCALOG_SEQ_IP:"              "${EMBARCADERO_SCALOG_SEQ_IP:-"(unset)"}"
    printf "  %-32s %s\n" "SCALOG_SEQ_PORT:"            "${EMBARCADERO_SCALOG_SEQ_PORT:-"(default)"}"
    printf "  %-32s %s\n" "SCALOG_CXL_MODE:"            "${SCALOG_CXL_MODE:-1}"
fi
printf "  %-32s %s\n" "ACK_DURABILITY_CONTRACT:"     "$ACK_DURABILITY_CONTRACT"
echo "================================================================"

if [[ "$DRY_RUN" == "1" ]]; then
    print_dry_run
    exit 0
fi

mkdir -p "$LOG_DIR"
overall_status=0
ATTEMPT_SUMMARY_CSV="$LOG_DIR/attempt_summary.csv"
echo "trial,attempt,result,reason" > "$ATTEMPT_SUMMARY_CSV"
OVERLAP_SUMMARY_CSV="$LOG_DIR/overlap_summary.csv"
echo "trial,overlap_total_gbps,overlap_window_ms,timeseries_clients" > "$OVERLAP_SUMMARY_CSV"
CORFU_PHASE_CSV="$LOG_DIR/corfu_token_phase.csv"
echo "trial,client,requests,grants,payload_sends,payload_before_grant" > "$CORFU_PHASE_CSV"
RUN_CONTRACT_CSV="$LOG_DIR/run_contract.csv"
GIT_COMMIT="$(git -C "$PROJECT_ROOT" rev-parse HEAD 2>/dev/null || echo unknown)"
if [[ -n "$(git -C "$PROJECT_ROOT" status --porcelain 2>/dev/null)" ]]; then
    GIT_DIRTY=true
else
    GIT_DIRTY=false
fi
if [[ "$GIT_DIRTY" == "true" && "${ALLOW_DIRTY_ARTIFACT:-0}" != "1" ]]; then
    echo "ERROR: refusing to create a publication artifact from a dirty revision. Set ALLOW_DIRTY_ARTIFACT=1 only for a developer smoke run." >&2
    exit 1
fi
MAILBOX_BACKING="not_applicable"
CONTROL_TOPOLOGY="direct_global"
if [[ "$SEQUENCER" == "CORFU" ]]; then
    CONTROL_TOPOLOGY="broker_proxy"
fi
if [[ "$CONTROL_TRANSPORT" == "cxl_mailbox" ]]; then
    # This describes the intended production backing.  The CXL manager logs
    # whether it actually obtained DAX; POSIX fallback results must be curated
    # as smoke-only, never as CXL mailbox measurements.
    MAILBOX_BACKING="pending_runtime_verification"
fi
echo "sequencer,control_transport,control_topology,mailbox_backing,cxl_layout_version,order,ack_level,replication_factor,rf_includes_primary,remote_replica_count,ack_durability_contract,lazylog_ack1_contract,lazylog_metadata_replica_count,order5_home_brokers,git_commit,git_dirty" > "$RUN_CONTRACT_CSV"
echo "$SEQUENCER,$CONTROL_TRANSPORT,$CONTROL_TOPOLOGY,$MAILBOX_BACKING,$CXL_LAYOUT_VERSION,$ORDER,$ACK,$REPLICATION_FACTOR,true,$(( REPLICATION_FACTOR > 0 ? REPLICATION_FACTOR - 1 : 0 )),$ACK_DURABILITY_CONTRACT,$LAZYLOG_METADATA_CONTRACT,$LAZYLOG_METADATA_REPLICA_COUNT,${EMBARCADERO_ORDER5_HOME_BROKERS:-},${GIT_COMMIT},${GIT_DIRTY}" >> "$RUN_CONTRACT_CSV"

if ! lazylog_metadata_endpoints_ready; then
    exit 1
fi

if ! preflight_clients; then
    exit 1
fi

# ---------------------------------------------------------------------------
# Trial loop
# ---------------------------------------------------------------------------
for (( trial=1; trial<=NUM_TRIALS; trial++ )); do
    echo ""
    echo "=== Trial $trial / $NUM_TRIALS  ($SEQUENCER  Order=$ORDER  Brokers=$NUM_BROKERS  Clients=$NUM_CLIENTS  msg=${MESSAGE_SIZE}B) ==="
    trial_success=0

    for (( attempt=1; attempt<=TRIAL_MAX_ATTEMPTS; attempt++ )); do
        log "  Attempt $attempt / $TRIAL_MAX_ATTEMPTS"
        if [[ -n "$REPLICA_DISK_DIRS_TEMPLATE" ]] &&
           ! ([[ "$SEQUENCER" == "CORFU" && "$REPLICATION_FACTOR" -gt 1 ]] && corfu_uses_memory_copy_sink); then
            export EMBARCADERO_REPLICA_DISK_DIRS="$(corfu_replica_dirs_for_attempt "$trial" "$attempt")"
            IFS=',' read -r -a _trial_replica_dirs <<< "$EMBARCADERO_REPLICA_DISK_DIRS"
            for _trial_replica_dir in "${_trial_replica_dirs[@]}"; do
                mkdir -p "$_trial_replica_dir"
            done
            unset _trial_replica_dirs _trial_replica_dir
            log "  Corfu replica media: $EMBARCADERO_REPLICA_DISK_DIRS"
        elif [[ "$SEQUENCER" == "CORFU" && "$REPLICATION_FACTOR" -gt 1 ]] && corfu_uses_memory_copy_sink; then
            unset EMBARCADERO_REPLICA_DISK_DIRS EMBARCADERO_REPLICA_DISK_ROOT
            log "  Corfu replica sink: ordered memory-copy (no durable sidecar)"
        fi
        if [[ "$CXL_SHM_PER_ATTEMPT" == "1" ]]; then
            export EMBARCADERO_CXL_SHM_NAME="${BASE_CXL_SHM_NAME}_t${trial}_a${attempt}"
        else
            export EMBARCADERO_CXL_SHM_NAME="$BASE_CXL_SHM_NAME"
        fi

        if ! start_brokers; then
            echo "ERROR: broker startup failed on trial $trial attempt $attempt" >&2
            echo "$trial,$attempt,failed,${START_BROKERS_FAILURE_REASON:-infra_broker_startup}" >> "$ATTEMPT_SUMMARY_CSV"
            for b in $(seq 0 $((NUM_BROKERS - 1))); do
                cp -f "/tmp/broker_${b}.log" "$LOG_DIR/trial${trial}_attempt${attempt}_broker${b}.log" 2>/dev/null || true
            done
            cleanup
            continue
        fi
        if ! record_mailbox_runtime_backing; then
            echo "$trial,$attempt,failed,infra_mailbox_runtime_backing_unverified" >> "$ATTEMPT_SUMMARY_CSV"
            cleanup
            continue
        fi

        # ------------------------------------------------------------------
        # Compute synchronized barrier timestamp (NTP-synced wall clock)
        # All clients spin-wait until this exact millisecond.
        # ------------------------------------------------------------------
        effective_start_delay_sec="$START_DELAY_SEC"
        if has_remote_clients && [[ "$effective_start_delay_sec" -lt "$MIN_REMOTE_START_DELAY_SEC" ]]; then
            log "  START_DELAY_SEC=${START_DELAY_SEC}s too small for SSH clients; using ${MIN_REMOTE_START_DELAY_SEC}s"
            effective_start_delay_sec="$MIN_REMOTE_START_DELAY_SEC"
        fi
        START_TIME_MS=$(( $(date +%s%3N) + effective_start_delay_sec * 1000 ))
        log "  Barrier start time: ${START_TIME_MS} ms  (T+${effective_start_delay_sec}s)"

        # ------------------------------------------------------------------
        # Launch all clients in parallel
        # ------------------------------------------------------------------
        declare -a CLIENT_PIDS=()
        declare -a CLIENT_LOGS=()
        declare -a CLIENT_TS_LOCAL_FILES=()
        declare -a CLIENT_TAGS=()
        declare -a CLIENT_PUSH_HOSTS=()
        declare -a CLIENT_PUSH_READY=()
        declare -a CLIENT_PUSH_GO=()
        CLIENT_REMOTE_PID_HOSTS=()
        CLIENT_REMOTE_PID_FILES=()
        # Publication contract: every throughput run uses one wall-clock axis with
        # push-ready/go so overlap is not polluted by staggered SSH launch.
        use_push_go=1
        if [[ "${EMBARCADERO_DISABLE_PUSH_GO:-0}" == "1" ]]; then
            use_push_go=0
        fi

        for (( i=0; i<NUM_CLIENTS; i++ )); do
            host="${CLIENT_HOSTS[$i]}"
            numa="$(resolve_client_numa "$host" "${CLIENT_NUMAS[$i]}")"
            host_occurrences=0
            for (( j=0; j<NUM_CLIENTS; j++ )); do
                if [[ "${CLIENT_HOSTS[$j]}" == "$host" ]]; then
                    host_occurrences=$((host_occurrences + 1))
                fi
            done
            client_tag="$host"
            if [[ "$host_occurrences" -gt 1 ]]; then
                client_tag="${host}${i}"
            fi
            log_file="$LOG_DIR/trial${trial}_${client_tag}.log"
            remote_build_bin="$BUILD_BIN"
            if [[ "$host" != "local" ]]; then
                remote_build_bin="$REMOTE_CLIENT_BIN_DIR"
            fi
            ts_file="$remote_build_bin/throughput_timeseries_trial${trial}_${client_tag}.csv"
            CLIENT_TAGS+=( "$client_tag" )
            CLIENT_LOGS+=( "$log_file" )
            CLIENT_TS_LOCAL_FILES+=( "$LOG_DIR/trial${trial}_${client_tag}_timeseries.csv" )
            CLIENT_PUSH_HOSTS+=( "$host" )
            remote_pid_file="/tmp/embarcadero_throughput_t${trial}_${client_tag}_$$_pid"
            if [[ "$host" != "local" ]]; then
                CLIENT_REMOTE_PID_HOSTS+=( "$host" )
                CLIENT_REMOTE_PID_FILES+=( "$remote_pid_file" )
            fi

            push_ready_export=""
            push_go_export=""
            if [[ "$use_push_go" -eq 1 ]]; then
                ready_path="/tmp/embarcadero_e4_ready_t${trial}_${client_tag}_$$"
                go_path="/tmp/embarcadero_e4_go_t${trial}_${client_tag}_$$"
                CLIENT_PUSH_READY+=( "$ready_path" )
                CLIENT_PUSH_GO+=( "$go_path" )
                if [[ "$host" == "local" ]]; then
                    rm -f "$ready_path" "$go_path"
                else
                    ssh "$host" "rm -f '$ready_path' '$go_path'" 2>/dev/null || true
                fi
                push_ready_export="export EMBARCADERO_PUSH_READY_FILE=$ready_path"
                push_go_export="export EMBARCADERO_PUSH_GO_FILE=$go_path"
            fi

            # Build the command that will execute on the remote (or local) shell.
            # We use export statements so every env var is properly set regardless
            # of the remote shell's inherited environment.
            EXEC_CMD="$(cat <<ENDINNERSCRIPT
set -e
export EMBARCADERO_HEAD_ADDR=$BROKER_IP
export NUM_BROKERS=$NUM_BROKERS
export EMBARCADERO_CXL_SHM_NAME=$EMBARCADERO_CXL_SHM_NAME
export EMBARCADERO_CXL_ZERO_MODE=${EMBARCADERO_CXL_ZERO_MODE:-full}
export EMBARCADERO_RUNTIME_MODE=throughput
export EMBARCADERO_REPLICATION_FACTOR=$REPLICATION_FACTOR
export EMBARCADERO_ORDER0_FAST_PATH=$EMBARCADERO_ORDER0_FAST_PATH
export EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES=$EMBARCADERO_PAYLOAD_SEND_CHUNK_BYTES
export EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE=$EMBARCADERO_ENABLE_PAYLOAD_MSG_MORE
export EMBARCADERO_BATCH_SIZE=$EMBARCADERO_BATCH_SIZE
export EMBARCADERO_CLIENT_PUB_BATCH_KB=$EMBARCADERO_CLIENT_PUB_BATCH_KB
export EMBARCADERO_NETWORK_IO_THREADS=$EMBARCADERO_NETWORK_IO_THREADS
export EMBARCADERO_ORDER5_HOME_BROKERS=$EMBARCADERO_ORDER5_HOME_BROKERS
if [ -n "${EMBARCADERO_ACK_TIMEOUT_SEC:-}" ]; then export EMBARCADERO_ACK_TIMEOUT_SEC=${EMBARCADERO_ACK_TIMEOUT_SEC:-}; fi
if [ -n "${EMBARCADERO_ACK2_OFFERED_RATE_BYTES_PER_SEC:-}" ]; then export EMBARCADERO_ACK2_OFFERED_RATE_BYTES_PER_SEC=${EMBARCADERO_ACK2_OFFERED_RATE_BYTES_PER_SEC:-}; fi
if [ -n "${EMBARCADERO_OFFERED_RATE_BYTES_PER_SEC:-}" ]; then export EMBARCADERO_OFFERED_RATE_BYTES_PER_SEC=${EMBARCADERO_OFFERED_RATE_BYTES_PER_SEC:-}; fi
if [ -n "${EMBARCADERO_ACK2_RETENTION:-}" ]; then export EMBARCADERO_ACK2_RETENTION=${EMBARCADERO_ACK2_RETENTION:-}; fi
if [ -n "${EMBARCADERO_QUEUE_POOL_MAX_BYTES:-}" ]; then export EMBARCADERO_QUEUE_POOL_MAX_BYTES=${EMBARCADERO_QUEUE_POOL_MAX_BYTES:-}; fi
if [ -n "${EMBARCADERO_CHAIN_REPLICATION_SINK:-}" ]; then export EMBARCADERO_CHAIN_REPLICATION_SINK=${EMBARCADERO_CHAIN_REPLICATION_SINK:-}; fi
if [ -n "${EMBARCADERO_CHAIN_REPLICATION_INMEM:-}" ]; then export EMBARCADERO_CHAIN_REPLICATION_INMEM=${EMBARCADERO_CHAIN_REPLICATION_INMEM:-}; fi
if [ -n "${EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY:-}" ]; then export EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=${EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY:-}; fi
if [ -n "${EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS:-}" ]; then export EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS=${EMBARCADERO_THROUGHPUT_TIMESERIES_INTERVAL_MS:-}; fi
if [ -n "${EMBARCADERO_THROUGHPUT_TIMESERIES_ON_SENT:-}" ]; then export EMBARCADERO_THROUGHPUT_TIMESERIES_ON_SENT=${EMBARCADERO_THROUGHPUT_TIMESERIES_ON_SENT:-}; fi
if [ -n "${EMBARCADERO_SESSION_LEASE_MS:-}" ]; then export EMBARCADERO_SESSION_LEASE_MS=${EMBARCADERO_SESSION_LEASE_MS:-}; fi
if [ -n "${EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS:-}" ]; then export EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=${EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS:-}; fi
if [ -n "${EMBARCADERO_ORDER5_PHASE_DIAG:-}" ]; then export EMBARCADERO_ORDER5_PHASE_DIAG=${EMBARCADERO_ORDER5_PHASE_DIAG:-}; fi
if [ -n "${EMBAR_ORDER5_EPOCH_US:-}" ]; then export EMBAR_ORDER5_EPOCH_US=${EMBAR_ORDER5_EPOCH_US:-}; fi
if [ -n "${EMBAR_ORDER5_COMMIT_PROFILE:-}" ]; then export EMBAR_ORDER5_COMMIT_PROFILE=${EMBAR_ORDER5_COMMIT_PROFILE:-}; fi
if [ -n "${EMBAR_PROFILE_NETWORK_PATH:-}" ]; then export EMBAR_PROFILE_NETWORK_PATH=${EMBAR_PROFILE_NETWORK_PATH:-}; fi
if [ -n "${EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC:-}" ]; then export EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC=${EMBARCADERO_SESSION_OPEN_TIMEOUT_SEC:-}; fi
if [ -n "${EMBARCADERO_PUBLISH_CONNECT_ATTEMPTS:-}" ]; then export EMBARCADERO_PUBLISH_CONNECT_ATTEMPTS=${EMBARCADERO_PUBLISH_CONNECT_ATTEMPTS:-}; fi
if [ -n "${EMBARCADERO_ENABLE_CONNECT_FAIL_SIM:-}" ]; then export EMBARCADERO_ENABLE_CONNECT_FAIL_SIM=${EMBARCADERO_ENABLE_CONNECT_FAIL_SIM:-}; fi
if [ -n "${EMBARCADERO_SIMULATE_CONNECT_FAIL_MOD:-}" ]; then export EMBARCADERO_SIMULATE_CONNECT_FAIL_MOD=${EMBARCADERO_SIMULATE_CONNECT_FAIL_MOD:-}; fi
if [ -n "${EMBARCADERO_SIMULATE_CONNECT_FAIL_REM:-}" ]; then export EMBARCADERO_SIMULATE_CONNECT_FAIL_REM=${EMBARCADERO_SIMULATE_CONNECT_FAIL_REM:-}; fi
export EMBARCADERO_THROUGHPUT_TIMESERIES_FILE=$ts_file
export EMBARCADERO_THROUGHPUT_TIMESERIES_ORIGIN_MS=$START_TIME_MS
$push_ready_export
$push_go_export
rm -f $ts_file
if [ "$SEQUENCER" = "CORFU" ] && [ -n "${EMBARCADERO_CORFU_SEQ_IP:-}" ]; then export EMBARCADERO_CORFU_SEQ_IP=${EMBARCADERO_CORFU_SEQ_IP:-}; fi
if [ "$SEQUENCER" = "CORFU" ] && [ -n "${EMBARCADERO_CORFU_SEQ_PORT:-}" ]; then export EMBARCADERO_CORFU_SEQ_PORT=${EMBARCADERO_CORFU_SEQ_PORT:-}; fi
if [ "$SEQUENCER" = "CORFU" ]; then export EMBARCADERO_CORFU_PROXY_PORT_BASE=$EMBARCADERO_CORFU_PROXY_PORT_BASE; fi
if [ "$SEQUENCER" = "LAZYLOG" ] && [ -n "${EMBARCADERO_LAZYLOG_SEQ_IP:-}" ]; then export EMBARCADERO_LAZYLOG_SEQ_IP=${EMBARCADERO_LAZYLOG_SEQ_IP:-}; fi
if [ "$SEQUENCER" = "LAZYLOG" ] && [ -n "${EMBARCADERO_LAZYLOG_SEQ_PORT:-}" ]; then export EMBARCADERO_LAZYLOG_SEQ_PORT=${EMBARCADERO_LAZYLOG_SEQ_PORT:-}; fi
if [ "$SEQUENCER" = "SCALOG" ] && [ -n "${EMBARCADERO_SCALOG_SEQ_IP:-}" ]; then export EMBARCADERO_SCALOG_SEQ_IP=${EMBARCADERO_SCALOG_SEQ_IP:-}; fi
if [ "$SEQUENCER" = "SCALOG" ] && [ -n "${EMBARCADERO_SCALOG_SEQ_PORT:-}" ]; then export EMBARCADERO_SCALOG_SEQ_PORT=${EMBARCADERO_SCALOG_SEQ_PORT:-}; fi
if [ "$SEQUENCER" = "SCALOG" ]; then export SCALOG_CXL_MODE=${SCALOG_CXL_MODE:-1}; fi
if [ -n "$CLIENT_LD_LIBRARY_PATH" ]; then export LD_LIBRARY_PATH=$CLIENT_LD_LIBRARY_PATH; fi
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
cd $remote_build_bin
# Spin-wait until the synchronized barrier millisecond (requires NTP-synced clocks).
# NOTE: use %s%N/1e6, NOT %s%3N — uutils date (c3) does not support %3N and
# emits nanoseconds, making the comparison always true (client skips the
# barrier entirely; root cause of months of degenerate N>=2 overlap windows).
while [ \$(( \$(date +%s%N) / 1000000 )) -lt $START_TIME_MS ]; do sleep 0.0005; done
__bar_now_ms=\$(( \$(date +%s%N) / 1000000 ))
if [ \$(( __bar_now_ms - $START_TIME_MS )) -gt 2000 ]; then
  echo "WARNING: BARRIER MISSED by \$(( __bar_now_ms - $START_TIME_MS )) ms — host clock skewed vs broker; concurrency of this trial is suspect" >&2
fi
echo \$\$ > $remote_pid_file
exec numactl --cpunodebind=$numa --membind=$numa ./throughput_test --config $CLIENT_CONFIG_ABS -n $THREADS_PER_BROKER -m $MESSAGE_SIZE -s $LOAD_PER_CLIENT -t $TEST_TYPE -o $ORDER -a $ACK -r $REPLICATION_FACTOR --sequencer $SEQUENCER --head_addr $BROKER_IP -l 0 $CLIENT_EXTRA_ARGS
ENDINNERSCRIPT
)"

            log "  Launching client[$i] host=$host  NUMA=$numa  log=$log_file"
            if [[ "$host" == "local" ]]; then
                bash -c "$EXEC_CMD" > "$log_file" 2>&1 &
            else
                ssh "$host" "$EXEC_CMD" > "$log_file" 2>&1 &
            fi
            CLIENT_PIDS+=( $! )
        done

        # ------------------------------------------------------------------
        # Push-ready/go barrier (always when use_push_go=1). Clients block on
        # GO after Init; without this writer they hang forever. Optional E4
        # broker-kill arms after the same GO timestamp.
        # ------------------------------------------------------------------
        BROKER_KILLER_PID=""
        if [[ "$use_push_go" -eq 1 ]]; then
            if [[ "$BROKER_KILL_AFTER_SEC" -gt 0 ]]; then
                if [[ "$BROKER_KILL_ID" -lt 0 || "$BROKER_KILL_ID" -ge "$NUM_BROKERS" ]]; then
                    echo "ERROR: BROKER_KILL_ID=$BROKER_KILL_ID out of range for NUM_BROKERS=$NUM_BROKERS" >&2
                    exit 1
                fi
                kill_target_pid="${BROKER_PIDS[$BROKER_KILL_ID]}"
                kill_record="$LOG_DIR/trial${trial}_broker_kill.csv"
                log "  Armed broker kill: broker $BROKER_KILL_ID (pid $kill_target_pid) SIG${BROKER_KILL_SIGNAL} at push-go+${BROKER_KILL_AFTER_SEC}s (wait up to ${BROKER_KILL_PUSH_WAIT_SEC}s for Init)"
            else
                kill_target_pid=""
                kill_record=""
                log "  Push-go barrier armed (wait up to ${BROKER_KILL_PUSH_WAIT_SEC}s for Init)"
            fi
            (
                push_wait_deadline=$(( $(date +%s) + BROKER_KILL_PUSH_WAIT_SEC ))
                while true; do
                    ready=0
                    for (( ci=0; ci<NUM_CLIENTS; ci++ )); do
                        h="${CLIENT_PUSH_HOSTS[$ci]}"
                        rf="${CLIENT_PUSH_READY[$ci]}"
                        if [[ "$h" == "local" ]]; then
                            [[ -f "$rf" ]] && ready=$((ready + 1))
                        else
                            ssh "$h" "test -f '$rf'" 2>/dev/null && ready=$((ready + 1))
                        fi
                    done
                    if [[ "$ready" -ge "$NUM_CLIENTS" ]]; then
                        break
                    fi
                    if [[ "$(date +%s)" -ge "$push_wait_deadline" ]]; then
                        echo "ERROR: push-ready wait timed out after ${BROKER_KILL_PUSH_WAIT_SEC}s (${ready}/${NUM_CLIENTS} ready)" >&2
                        for (( ci=0; ci<NUM_CLIENTS; ci++ )); do
                            h="${CLIENT_PUSH_HOSTS[$ci]}"
                            gf="${CLIENT_PUSH_GO[$ci]}"
                            if [[ "$h" == "local" ]]; then
                                echo 1 > "$gf"
                            else
                                ssh "$h" "echo 1 > '$gf'" 2>/dev/null || true
                            fi
                        done
                        exit 1
                    fi
                    sleep 0.05
                done
                GO_NS=$(( $(date +%s%N) + 200000000 ))  # +200ms lead
                for (( ci=0; ci<NUM_CLIENTS; ci++ )); do
                    h="${CLIENT_PUSH_HOSTS[$ci]}"
                    gf="${CLIENT_PUSH_GO[$ci]}"
                    if [[ "$h" == "local" ]]; then
                        echo "$GO_NS" > "$gf"
                    else
                        ssh "$h" "echo $GO_NS > '$gf'" &
                    fi
                done
                wait || true
                go_ms=$(( GO_NS / 1000000 ))
                echo "  Push-go fired at ${go_ms}" >&2
                if [[ "${BROKER_KILL_AFTER_SEC:-0}" -gt 0 && -n "$kill_target_pid" ]]; then
                    kill_at_ms=$(( go_ms + BROKER_KILL_AFTER_SEC * 1000 ))
                    echo "  Broker kill: push-go=${go_ms}; killing at ${kill_at_ms} (T+${BROKER_KILL_AFTER_SEC}s)" >&2
                    while [ "$(date +%s%3N)" -lt "$kill_at_ms" ]; do sleep 0.001; done
                    t_kill_ms="$(date +%s%3N)"
                    kill_rc=0
                    kill -"$BROKER_KILL_SIGNAL" "$kill_target_pid" 2>/dev/null || kill_rc=$?
                    {
                        echo "trial,attempt,broker_id,pid,signal,kill_wall_ms,kill_rel_ms,kill_rc"
                        echo "$trial,$attempt,$BROKER_KILL_ID,$kill_target_pid,$BROKER_KILL_SIGNAL,$t_kill_ms,$(( t_kill_ms - START_TIME_MS )),$kill_rc"
                    } > "$kill_record"
                fi
            ) &
            BROKER_KILLER_PID=$!
        fi

        # ------------------------------------------------------------------
        # Wait for all clients to finish
        # ------------------------------------------------------------------
        log "  Waiting for ${#CLIENT_PIDS[@]} client(s)..."
        all_ok=1
        for pid in "${CLIENT_PIDS[@]}"; do
            wait "$pid" || all_ok=0
        done

        # Reap the killer (kill first in case clients exited before T_kill)
        if [[ -n "$BROKER_KILLER_PID" ]]; then
            kill "$BROKER_KILLER_PID" 2>/dev/null || true
            wait "$BROKER_KILLER_PID" 2>/dev/null || true
            BROKER_KILLER_PID=""
        fi

        # Collect per-client throughput timeseries from client hosts.
        for (( i=0; i<NUM_CLIENTS; i++ )); do
            host="${CLIENT_HOSTS[$i]}"
            client_tag="${CLIENT_TAGS[$i]}"
            local_ts="$LOG_DIR/trial${trial}_${client_tag}_timeseries.csv"
            remote_ts="$BUILD_BIN/throughput_timeseries_trial${trial}_${client_tag}.csv"
            if [[ "$host" != "local" ]]; then
                remote_ts="$REMOTE_CLIENT_BIN_DIR/throughput_timeseries_trial${trial}_${client_tag}.csv"
            fi
            if [[ "$host" == "local" ]]; then
                cp "$remote_ts" "$local_ts" 2>/dev/null || true
            else
                scp -o StrictHostKeyChecking=no "$host:$remote_ts" "$local_ts" >/dev/null 2>&1 || true
            fi
        done

        # ------------------------------------------------------------------
        # Validate: every log must contain a "Bandwidth:" line
        # ------------------------------------------------------------------
        logs_ok=1
        for log_file in "${CLIENT_LOGS[@]}"; do
            if ! grep -qi "bandwidth:" "$log_file" 2>/dev/null; then
                echo "WARNING: no Bandwidth line in $log_file" >&2
                tail -n 15 "$log_file" >&2
                logs_ok=0
            fi
        done
        if [[ "$logs_ok" -eq 1 ]] && ! validate_corfu_phase_evidence "$trial"; then
            logs_ok=0
        fi

        if [[ "$all_ok" -eq 1 && "$logs_ok" -eq 1 ]]; then
            overlap_row="$(compute_overlap_throughput_gbps "$trial" "${CLIENT_TS_LOCAL_FILES[@]}" || true)"
            if [[ -n "$overlap_row" ]]; then
                echo "$overlap_row" >> "$OVERLAP_SUMMARY_CSV"
            fi
            trial_success=1
            echo "$trial,$attempt,success,ok" >> "$ATTEMPT_SUMMARY_CSV"
            break
        fi

        echo "WARNING: trial $trial attempt $attempt incomplete — retrying..." >&2
        echo "$trial,$attempt,failed,incomplete_or_missing_bandwidth" >> "$ATTEMPT_SUMMARY_CSV"
        for b in $(seq 0 $((NUM_BROKERS - 1))); do
            cp -f "/tmp/broker_${b}.log" "$LOG_DIR/trial${trial}_attempt${attempt}_broker${b}.log" 2>/dev/null || true
        done
        # Kill any surviving clients before re-attempting
        for pid in "${CLIENT_PIDS[@]}"; do
            kill "$pid" 2>/dev/null || true
        done
        cleanup
        # brief pause so the broker port is released
        sleep 2
    done

    # ------------------------------------------------------------------
    # Per-trial summary
    # ------------------------------------------------------------------
    echo ""
    echo "--- Trial $trial Results ---"
    total_bw_mbs=0
    total_send_done_mbs=0
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        client_tag="${CLIENT_TAGS[$i]:-$host}"
        log_file="$LOG_DIR/trial${trial}_${client_tag}.log"
        # Prefer the test_utils headline that includes Send-done. A later
        # result_writer line ("Publish bandwidth: ...") would otherwise win via
        # tail -1 and hide Send-done / unit parsing.
        bw_line=$(grep -E '\] Bandwidth:.*Send-done:' "$log_file" 2>/dev/null | tail -1 || true)
        if [[ -z "$bw_line" ]]; then
            bw_line=$(grep -E '\] Bandwidth:' "$log_file" 2>/dev/null | tail -1 || echo "")
        fi
        bw_val=$(echo "$bw_line" | grep -oiP 'bandwidth:\s*\K[0-9]+(\.[0-9]+)?' || true)
        bw_unit=$(echo "$bw_line" | awk '{for(i=1;i<=NF;i++) if(tolower($i)=="bandwidth:") {print $(i+2); exit}}' || true)
        sd_val=$(echo "$bw_line" | grep -oiP 'send-done:\s*\K[0-9]+(\.[0-9]+)?' || true)
        poll_line=$(grep -F "[POLL_BREAKDOWN]" "$log_file" 2>/dev/null | tail -1 || echo "")
        poll_join=$(echo "$poll_line" | grep -oiP 'publisher_join_ms=\K[0-9]+(\.[0-9]+)?' || true)
        poll_ack=$(echo "$poll_line" | grep -oiP 'ack_wait_ms=\K[0-9]+(\.[0-9]+)?' || true)
        if [[ -n "$sd_val" ]]; then
            printf "  %-8s → %s %s  (Send-done %s %s" "$client_tag" "${bw_val:-N/A}" "${bw_unit:-}" "$sd_val" "${bw_unit:-MB/s}"
            if [[ -n "$poll_join" || -n "$poll_ack" ]]; then
                printf "; join=%sms ack_wait=%sms" "${poll_join:-?}" "${poll_ack:-?}"
            fi
            printf ")\n"
        else
            printf "  %-8s → %s %s\n" "$client_tag" "${bw_val:-N/A}" "${bw_unit:-}"
        fi
        if [[ "$bw_val" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            total_bw_mbs=$(awk "BEGIN {printf \"%.2f\", $total_bw_mbs + $bw_val}")
        fi
        if [[ "$sd_val" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            total_send_done_mbs=$(awk "BEGIN {printf \"%.2f\", $total_send_done_mbs + $sd_val}")
        fi
    done
    total_gbs=$(awk "BEGIN {printf \"%.3f\", $total_bw_mbs / 1024}")
    echo "  ────────────────────────────────────────"
    printf "  %-8s → %s MB/s  (%s GB/s)  [end-to-end incl. Poll]\n" "TOTAL" "$total_bw_mbs" "$total_gbs"
    if awk "BEGIN {exit !($total_send_done_mbs > 0)}"; then
        total_sd_gbs=$(awk "BEGIN {printf \"%.3f\", $total_send_done_mbs / 1024}")
        printf "  %-8s → %s MB/s  (%s GB/s)  [excludes Poll; prefer for short ORDER=5 runs]\n" \
            "SENDDONE" "$total_send_done_mbs" "$total_sd_gbs"
    fi
    overlap_line="$(awk -F',' -v t="$trial" 'NR>1 && $1==t {print $0; exit}' "$OVERLAP_SUMMARY_CSV" || true)"
    if [[ -n "$overlap_line" ]]; then
        overlap_gbps="$(echo "$overlap_line" | awk -F',' '{print $2}')"
        overlap_window_ms="$(echo "$overlap_line" | awk -F',' '{print $3}')"
        overlap_clients="$(echo "$overlap_line" | awk -F',' '{print $4}')"
        printf "  %-8s → %s GB/s  (window=%sms clients=%s)\n" "OVERLAP" "$overlap_gbps" "$overlap_window_ms" "$overlap_clients"
    fi
    echo "  (naive sum of per-client averages; see OSDI/SOSP note below)"

    if [[ "$trial_success" -ne 1 ]]; then
        echo "ERROR: Trial $trial failed after $TRIAL_MAX_ATTEMPTS attempts." >&2
        if ! awk -F',' -v t="$trial" '$1==t && $3=="success"{found=1} END{exit !found}' "$ATTEMPT_SUMMARY_CSV"; then
            echo "$trial,$TRIAL_MAX_ATTEMPTS,failed,max_attempts_exhausted" >> "$ATTEMPT_SUMMARY_CSV"
        fi
        overall_status=1
    fi

    cleanup
done

# ---------------------------------------------------------------------------
# Grand summary across all trials
# ---------------------------------------------------------------------------
echo ""
echo "================================================================"
echo "  Grand Summary  (naive aggregate MB/s per trial)"
echo "================================================================"
printf "  %-10s %12s %12s %10s\n" "Trial" "Bandwidth" "Send-done" "GB/s(BW)"
for (( trial=1; trial<=NUM_TRIALS; trial++ )); do
    total=0
    total_sd=0
    for (( i=0; i<NUM_CLIENTS; i++ )); do
        host="${CLIENT_HOSTS[$i]}"
        client_tag="$host"
        host_occurrences=0
        for (( j=0; j<NUM_CLIENTS; j++ )); do
            if [[ "${CLIENT_HOSTS[$j]}" == "$host" ]]; then
                host_occurrences=$((host_occurrences + 1))
            fi
        done
        if [[ "$host_occurrences" -gt 1 ]]; then
            client_tag="${host}${i}"
        fi
        log_file="$LOG_DIR/trial${trial}_${client_tag}.log"
        bw_line=$(grep -E '\] Bandwidth:.*Send-done:' "$log_file" 2>/dev/null | tail -1 || true)
        if [[ -z "$bw_line" ]]; then
            bw_line=$(grep -E '\] Bandwidth:' "$log_file" 2>/dev/null | tail -1 || echo "")
        fi
        bw=$(echo "$bw_line" | grep -oiP 'bandwidth:\s*\K[0-9]+(\.[0-9]+)?' || true)
        sd=$(echo "$bw_line" | grep -oiP 'send-done:\s*\K[0-9]+(\.[0-9]+)?' || true)
        if [[ "$bw" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            total=$(awk "BEGIN {printf \"%.2f\", $total + $bw}")
        fi
        if [[ "$sd" =~ ^[0-9]+(\.[0-9]+)?$ ]]; then
            total_sd=$(awk "BEGIN {printf \"%.2f\", $total_sd + $sd}")
        fi
    done
    gbs=$(awk "BEGIN {printf \"%.3f\", $total / 1024}")
    printf "  Trial %-3d:  %9s  %9s  %8s\n" "$trial" "$total" "${total_sd:-n/a}" "$gbs"
done
echo "================================================================"
echo "  Note: Bandwidth includes Poll(); Send-done excludes it."
echo "  After the ORDER=5 join-watchdog fix, short runs should agree;"
echo "  if they diverge, inspect [POLL_BREAKDOWN] publisher_join_ms."
echo "================================================================"
echo ""
echo "  Logs: $LOG_DIR/"
echo ""
echo "  RIGOROUS AGGREGATION NOTE (OSDI/SOSP method):"
echo "  The numbers above are naive sums of each client's average.  For"
echo "  peer-reviewable results, determine the overlapping measurement"
echo "  window from time-series CSVs (one per client) and divide total"
echo "  bytes sent inside that window by its duration."
echo "================================================================"

exit "$overall_status"
