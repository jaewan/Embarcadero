#!/bin/bash

# Shared broker lifecycle helpers for local and remote orchestration.

broker_is_remote_mode() {
  [ -n "${REMOTE_BROKER_HOST:-}" ]
}

broker_resolve_path() {
  local root="$1"
  local path="$2"
  if [ -z "$path" ]; then
    printf '%s\n' "$root"
  elif [[ "$path" = /* ]]; then
    printf '%s\n' "$path"
  else
    printf '%s/%s\n' "$root" "$path"
  fi
}

broker_init_paths() {
  BROKER_CONFIG="${BROKER_CONFIG:-config/embarcadero.yaml}"
  CLIENT_CONFIG="${CLIENT_CONFIG:-config/client.yaml}"
  BROKER_CONFIG_ABS="$(broker_resolve_path "$PROJECT_ROOT" "$BROKER_CONFIG")"
  CLIENT_CONFIG_ABS="$(broker_resolve_path "$PROJECT_ROOT" "$CLIENT_CONFIG")"
  BROKER_DATA_PORT_BASE="$(grep -oP '^\s*port:\s*\K[0-9]+' "$BROKER_CONFIG_ABS" 2>/dev/null | head -1)"
  BROKER_HEARTBEAT_PORT="$(grep -oP '^\s*broker_port:\s*\K[0-9]+' "$BROKER_CONFIG_ABS" 2>/dev/null | head -1)"
  BROKER_DATA_PORT_BASE="${BROKER_DATA_PORT_BASE:-1214}"
  BROKER_HEARTBEAT_PORT="${BROKER_HEARTBEAT_PORT:-12140}"

  if broker_is_remote_mode; then
    REMOTE_PROJECT_ROOT="${REMOTE_PROJECT_ROOT:-$PROJECT_ROOT}"
    REMOTE_PROJECT_ROOT="$(broker_resolve_path "$PROJECT_ROOT" "$REMOTE_PROJECT_ROOT")"
    REMOTE_BUILD_BIN="${REMOTE_BUILD_BIN:-$REMOTE_PROJECT_ROOT/build/bin}"
    REMOTE_BUILD_BIN="$(broker_resolve_path "$REMOTE_PROJECT_ROOT" "$REMOTE_BUILD_BIN")"
    REMOTE_BROKER_STATE_DIR="${REMOTE_BROKER_STATE_DIR:-/tmp/embarcadero_broker_state}"
    REMOTE_HEAD_ADDR="${EMBARCADERO_HEAD_ADDR:-$REMOTE_BROKER_HOST}"
  else
    REMOTE_PROJECT_ROOT=""
    REMOTE_BUILD_BIN=""
    REMOTE_BROKER_STATE_DIR=""
    REMOTE_HEAD_ADDR=""
  fi
}

broker_remote_ssh() {
  ssh "$REMOTE_BROKER_HOST" "$@"
}

broker_remote_host_tokens() {
  broker_remote_ssh bash -s <<'EOF'
{
  hostname -f 2>/dev/null || true
  hostname -s 2>/dev/null || true
  hostname -I 2>/dev/null || true
} | tr ' ' '\n' | awk 'NF' | sort -u
EOF
}

broker_local_host_tokens() {
  {
    hostname -f 2>/dev/null || true
    hostname -s 2>/dev/null || true
    hostname -I 2>/dev/null || true
  } | tr ' ' '\n' | awk 'NF' | sort -u
}

broker_remote_host_is_local() {
  local local_tokens remote_tokens token
  local_tokens="$(broker_local_host_tokens)" || return 1
  remote_tokens="$(broker_remote_host_tokens)" || return 1
  for token in $remote_tokens; do
    case " $local_tokens " in
      *" $token "*) return 0 ;;
    esac
  done
  return 1
}

# Corfu gRPC sequencer on a cluster node (may differ from REMOTE_BROKER_HOST, e.g. c2 vs c3).
broker_remote_corfu_sequencer_host() {
  if [ -n "${REMOTE_CORFU_SEQUENCER_HOST:-}" ]; then
    printf '%s\n' "${REMOTE_CORFU_SEQUENCER_HOST}"
  elif broker_is_remote_mode; then
    printf '%s\n' "${REMOTE_BROKER_HOST}"
  else
    printf '%s\n' ""
  fi
}

broker_remote_corfu_start() {
  local host
  host="$(broker_remote_corfu_sequencer_host)"
  [ -n "$host" ] || return 0
  local bin="${REMOTE_CORFU_BUILD_BIN:-$REMOTE_BUILD_BIN}"
  [ -n "$bin" ] || return 1
  echo "Remote Corfu sequencer: starting on $host (cd $bin)"
  ssh -o BatchMode=yes "$host" bash -s -- "$bin" <<'EOF'
set -euo pipefail
build_bin=$1
cd "$build_bin"
for pid in $(pgrep -f "corfu_global_sequencer" 2>/dev/null || true); do
  kill "$pid" 2>/dev/null || true
done
sleep 0.3
for pid in $(pgrep -f "corfu_global_sequencer" 2>/dev/null || true); do
  kill -9 "$pid" 2>/dev/null || true
done
nohup ./corfu_global_sequencer >>/tmp/corfu_sequencer.log 2>&1 &
printf '%s\n' "$!" >/tmp/corfu_global_sequencer.pid
EOF
}

broker_remote_corfu_stop() {
  local host
  host="$(broker_remote_corfu_sequencer_host)"
  [ -n "$host" ] || return 0
  ssh -o BatchMode=yes "$host" bash -s -- <<'EOF'
for pid in $(pgrep -f "corfu_global_sequencer" 2>/dev/null || true); do
  kill "$pid" 2>/dev/null || true
done
sleep 0.2
for pid in $(pgrep -f "corfu_global_sequencer" 2>/dev/null || true); do
  kill -9 "$pid" 2>/dev/null || true
done
rm -f /tmp/corfu_global_sequencer.pid
EOF
}

broker_remote_scalog_sequencer_host() {
  if [ -n "${REMOTE_SCALOG_SEQUENCER_HOST:-}" ]; then
    printf '%s\n' "${REMOTE_SCALOG_SEQUENCER_HOST}"
  elif broker_is_remote_mode; then
    printf '%s\n' "${REMOTE_BROKER_HOST}"
  else
    printf '%s\n' ""
  fi
}

broker_remote_scalog_start() {
  if [ "${SKIP_REMOTE_SCALOG_SEQUENCER:-0}" = "1" ]; then
    echo "Remote Scalog sequencer: skipped (SKIP_REMOTE_SCALOG_SEQUENCER=1; sequencer must already be running)"
    return 0
  fi
  local host
  host="$(broker_remote_scalog_sequencer_host)"
  [ -n "$host" ] || return 0
  local bin="${REMOTE_SCALOG_BUILD_BIN:-$REMOTE_BUILD_BIN}"
  [ -n "$bin" ] || return 1
  echo "Remote Scalog sequencer: starting on $host (cd $bin)"
  ssh -o BatchMode=yes "$host" bash -s -- "$bin" "${EMBARCADERO_SCALOG_SEQ_IP:-}" "${EMBARCADERO_SCALOG_SEQ_PORT:-}" <<'EOF'
set -euo pipefail
build_bin=$1
scalog_seq_ip=${2-}
scalog_seq_port=${3-}
if [ -z "$scalog_seq_port" ]; then
  scalog_seq_port=50051
fi
cd "$build_bin"
for pid in $(pgrep -f "scalog_global_sequencer" 2>/dev/null || true); do
  kill "$pid" 2>/dev/null || true
done
sleep 0.3
for pid in $(pgrep -f "scalog_global_sequencer" 2>/dev/null || true); do
  kill -9 "$pid" 2>/dev/null || true
done
if [ -n "$scalog_seq_ip" ]; then export EMBARCADERO_SCALOG_SEQ_IP="$scalog_seq_ip"; fi
if [ -n "$scalog_seq_port" ]; then export EMBARCADERO_SCALOG_SEQ_PORT="$scalog_seq_port"; fi
: >/tmp/scalog_sequencer.log
printf '=== scalog start %s ===\n' "$(date -u +%Y%m%dT%H%M%SZ)" >>/tmp/scalog_sequencer.log
nohup ./scalog_global_sequencer >>/tmp/scalog_sequencer.log 2>&1 &
printf '%s\n' "$!" >/tmp/scalog_global_sequencer.pid
for _ in $(seq 1 100); do
  if ss -H -ltn "sport = :$scalog_seq_port" 2>/dev/null | grep -q .; then
    exit 0
  fi
  sleep 0.1
done
echo "Remote Scalog sequencer failed to bind port $scalog_seq_port" >&2
tail -n 80 /tmp/scalog_sequencer.log >&2 || true
exit 1
EOF
}

broker_remote_scalog_stop() {
  if [ "${SKIP_REMOTE_SCALOG_SEQUENCER:-0}" = "1" ]; then
    return 0
  fi
  local host
  host="$(broker_remote_scalog_sequencer_host)"
  [ -n "$host" ] || return 0
  ssh -o BatchMode=yes "$host" bash -s -- <<'EOF'
for pid in $(pgrep -f "scalog_global_sequencer" 2>/dev/null || true); do
  kill "$pid" 2>/dev/null || true
done
sleep 0.2
for pid in $(pgrep -f "scalog_global_sequencer" 2>/dev/null || true); do
  kill -9 "$pid" 2>/dev/null || true
done
rm -f /tmp/scalog_global_sequencer.pid
EOF
}

broker_wait_for_remote_scalog_ready() {
  local timeout_sec="${1:?timeout_sec}"
  local expected_brokers="${2:?expected_brokers}"
  local replication_factor="${3:?replication_factor}"
  local host
  host="$(broker_remote_scalog_sequencer_host)"
  [ -n "$host" ] || return 0
  ssh -o BatchMode=yes "$host" bash -s -- "$timeout_sec" "$expected_brokers" "$replication_factor" <<'EOF'
set -euo pipefail
timeout_sec=$1
expected_brokers=$2
replication_factor=$3
expected_streams=$((expected_brokers * replication_factor))
deadline=$(( $(date +%s) + timeout_sec ))
while [ "$(date +%s)" -lt "$deadline" ]; do
  regs=$(grep -c 'registered broker=' /tmp/scalog_sequencer.log 2>/dev/null || true)
  started=$(grep -c 'starting cut distribution thread' /tmp/scalog_sequencer.log 2>/dev/null || true)
  streams=$(awk '
    /received local cut broker=/ {
      broker="";
      replica="";
      for (i = 1; i <= NF; ++i) {
        if ($i ~ /^broker=/) {
          split($i, a, "=");
          broker=a[2];
        }
        if ($i ~ /^replica=/) {
          split($i, a, "=");
          replica=a[2];
        }
      }
      if (broker != "" && replica != "") {
        seen[broker ":" replica] = 1;
      }
    }
    END {
      count = 0;
      for (k in seen) {
        count++;
      }
      print count + 0;
    }
  ' /tmp/scalog_sequencer.log 2>/dev/null || true)
  nonzero_cuts=$(awk -v expected="$expected_brokers" '
    BEGIN { seen = 0 }
    /Scalog global cut send/ {
      count = 0;
      nonzero = 0;
      for (i = 1; i <= NF; ++i) {
        if ($i ~ /^b[0-9]+=/) {
          split($i, a, "=");
          count++;
          if ((a[2] + 0) > 0) {
            nonzero++;
          }
        }
      }
      if (count >= expected && nonzero >= expected) {
        seen = 1;
      }
    }
    END { print seen + 0 }
  ' /tmp/scalog_sequencer.log 2>/dev/null || true)
  if [ "$regs" -ge "$expected_brokers" ] && [ "$started" -ge 1 ] && [ "$streams" -ge "$expected_streams" ] && [ "$nonzero_cuts" -ge 1 ]; then
    exit 0
  fi
  sleep 0.2
done
echo "Scalog readiness timeout: regs=$regs/$expected_brokers streams=$streams/$expected_streams started=$started nonzero_cuts=$nonzero_cuts" >&2
tail -n 80 /tmp/scalog_sequencer.log >&2 || true
exit 1
EOF
}

broker_ready_file_count_local() {
  local ready_files=(/tmp/embarlet_*_ready)
  if [ -e "${ready_files[0]}" ]; then
    printf '%s\n' "${#ready_files[@]}"
  else
    printf '0\n'
  fi
}

broker_ready_file_count_remote() {
  local expected="$1"
  broker_remote_ssh bash -s -- "$BROKER_DATA_PORT_BASE" "$BROKER_HEARTBEAT_PORT" "$expected" <<'EOF'
set -euo pipefail
data_base=$1
heartbeat_port=$2
expected=$3
count=0
for idx in $(seq 0 $((expected - 1))); do
  port=$((data_base + idx))
  if [ "$idx" -eq 0 ]; then
    if ss -H -ltn "( sport = :$port or sport = :$heartbeat_port )" 2>/dev/null | grep -q .; then
      count=$((count + 1))
    fi
  elif ss -H -ltn "sport = :$port" 2>/dev/null | grep -q .; then
    count=$((count + 1))
  fi
done
printf '%s\n' "$count"
EOF
}

broker_count_ready() {
  if broker_is_remote_mode; then
    broker_ready_file_count_remote "$NUM_BROKERS"
  else
    broker_ready_file_count_local
  fi
}

broker_remote_status() {
  local idx="$1"
  broker_remote_ssh bash -s -- "$BROKER_DATA_PORT_BASE" "$BROKER_HEARTBEAT_PORT" "$idx" <<'EOF'
set -euo pipefail
data_base=$1
heartbeat_port=$2
idx=$3
data_port=$((data_base + idx))
if [ "$idx" -eq 0 ]; then
  if ss -H -ltn "( sport = :$data_port or sport = :$heartbeat_port )" 2>/dev/null | grep -q .; then
    echo "healthy:$data_port:$heartbeat_port"
    exit 0
  fi
else
  if ss -H -ltn "sport = :$data_port" 2>/dev/null | grep -q .; then
    echo "healthy:$data_port"
    exit 0
  fi
fi
echo "not-ready:$data_port"
exit 1
EOF
}

broker_remote_cleanup() {
  broker_remote_ssh bash -s -- "$REMOTE_BROKER_STATE_DIR" "$REMOTE_BUILD_BIN" "$BROKER_CONFIG_ABS" "$BROKER_DATA_PORT_BASE" "$BROKER_HEARTBEAT_PORT" "$NUM_BROKERS" "${EMBARCADERO_CXL_SHM_NAME:-}" <<'EOF'
set -euo pipefail
state_dir=$1
build_bin=$2
config=$3
data_base=$4
heartbeat_port=$5
num_brokers=$6
shm_name=${7:-}
if [ -d "$state_dir" ]; then
  for pid_file in "$state_dir"/broker_*.pid; do
    [ -e "$pid_file" ] || continue
    pid=$(cat "$pid_file" 2>/dev/null || true)
    if [ -n "$pid" ]; then
      kill "$pid" 2>/dev/null || true
      sleep 0.05
      kill -9 "$pid" 2>/dev/null || true
    fi
  done
  rm -f "$state_dir"/broker_*.pid "$state_dir"/broker_*.ready "$state_dir"/broker_*.log
fi

# Kill any orphaned broker processes from earlier failed starts, even if they never
# reached the point of binding a data port or writing a current pid file.
for pid in $(pgrep -x embarlet 2>/dev/null || true); do
  kill "$pid" 2>/dev/null || true
  sleep 0.05
  kill -9 "$pid" 2>/dev/null || true
done

# Catch any orphaned listeners on the broker data/heartbeat ports.
for idx in $(seq 0 $((num_brokers - 1))); do
  port=$((data_base + idx))
  if [ "$idx" -eq 0 ]; then
    listeners=$(ss -H -ltnp "( sport = :$port or sport = :$heartbeat_port )" 2>/dev/null || true)
  else
    listeners=$(ss -H -ltnp "sport = :$port" 2>/dev/null || true)
  fi
  if [ -n "$listeners" ]; then
    pids=$(printf '%s\n' "$listeners" | sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' | sort -u)
    for pid in $pids; do
      kill "$pid" 2>/dev/null || true
      sleep 0.05
      kill -9 "$pid" 2>/dev/null || true
    done
  fi
done

for pid in $(pgrep -f "$build_bin/corfu_global_sequencer" 2>/dev/null || true); do
  kill "$pid" 2>/dev/null || true
  sleep 0.05
  kill -9 "$pid" 2>/dev/null || true
done
for pid in $(pgrep -f "$build_bin/scalog_global_sequencer" 2>/dev/null || true); do
  kill "$pid" 2>/dev/null || true
  sleep 0.05
  kill -9 "$pid" 2>/dev/null || true
done

rm -f /tmp/embarlet_*_ready 2>/dev/null || true
if [ -n "$shm_name" ]; then
  shm_unlink "$shm_name" 2>/dev/null || true
  rm -f "/dev/shm${shm_name}" 2>/dev/null || true
fi
EOF
}

broker_local_cleanup() {
  pkill -9 -f "./embarlet" >/dev/null 2>&1 || true
  pkill -9 -f "throughput_test" >/dev/null 2>&1 || true
  pkill -9 -f "corfu_global_sequencer" >/dev/null 2>&1 || true
  pkill -9 -f "scalog_global_sequencer" >/dev/null 2>&1 || true
  rm -f /tmp/embarlet_*_ready 2>/dev/null || true
}

broker_cleanup() {
  if broker_is_remote_mode; then
    broker_remote_cleanup
    # If Corfu runs on a dedicated host (REMOTE_CORFU_SEQUENCER_HOST), kill it there; if
    # collocated with brokers, broker_remote_cleanup already killed it on REMOTE_BROKER_HOST.
    if [ -n "${REMOTE_CORFU_SEQUENCER_HOST:-}" ]; then
      broker_remote_corfu_stop || true
    fi
    if [ -n "${REMOTE_SCALOG_SEQUENCER_HOST:-}" ]; then
      broker_remote_scalog_stop || true
    fi
  else
    broker_local_cleanup
  fi
}

broker_remote_launch() {
  local idx="$1"
  local role="$2"
  local sequence="$3"
  local head_addr="${REMOTE_HEAD_ADDR:-${REMOTE_BROKER_HOST}}"
  local poll_interval="${BROKER_POLL_INTERVAL_SEC:-0.1}"

  # SSH does not forward the orchestrator shell's environment; pass replication and cluster
  # size explicitly so remote embarlet matches local runs (Scalog CXL / ACK paths depend on this).
  local _remote_rep="${REPLICATION_FACTOR:-${EMBARCADERO_REPLICATION_FACTOR:-0}}"
  local _remote_nb="${NUM_BROKERS:-4}"

  broker_remote_ssh bash -s -- \
    "$REMOTE_BROKER_STATE_DIR" \
    "$REMOTE_BUILD_BIN" \
    "$BROKER_CONFIG_ABS" \
    "$head_addr" \
    "$idx" \
    "$role" \
    "$sequence" \
    "$poll_interval" \
    "${EMBARCADERO_ACK_SEND_MIN_INTERVAL_US:-}" \
    "${EMBARCADERO_ORDER0_FAST_PATH:-}" \
    "${EMBARCADERO_ACK_STALL_SLEEP_US:-}" \
    "${EMBARCADERO_CORFU_SEQ_IP:-}" \
    "${EMBARCADERO_SCALOG_SEQ_IP:-}" \
    "${EMBARCADERO_SCALOG_SEQ_PORT:-}" \
    "$_remote_rep" \
    "$_remote_nb" <<'EOF'
set -euo pipefail
state_dir=$1
build_bin=$2
config=$3
head_addr=$4
idx=$5
role=$6
sequence=$7
poll_interval=$8
ack_send_min_interval_us=${9-}
order0_fast_path=${10-}
ack_stall_sleep_us=${11-}
corfu_seq_ip=${12-}
scalog_seq_ip=${13-}
scalog_seq_port=${14-}
replication_factor_cfg=${15:-0}
num_brokers_cfg=${16:-4}
mkdir -p "$state_dir"
rm -f "$state_dir/broker_${idx}.pid" "$state_dir/broker_${idx}.ready" "$state_dir/broker_${idx}.log"
nohup bash -s -- "$state_dir" "$build_bin" "$config" "$head_addr" "$idx" "$role" "$sequence" "$poll_interval" "$ack_send_min_interval_us" "$order0_fast_path" "$ack_stall_sleep_us" "$corfu_seq_ip" "$scalog_seq_ip" "$scalog_seq_port" "$replication_factor_cfg" "$num_brokers_cfg" <<'INNER' >/tmp/embarcadero_broker_${idx}_manager.log 2>&1 &
set -euo pipefail
state_dir=$1
build_bin=$2
config=$3
head_addr=$4
idx=$5
role=$6
sequence=$7
poll_interval=$8
ack_send_min_interval_us=${9-}
order0_fast_path=${10-}
ack_stall_sleep_us=${11-}
corfu_seq_ip=${12-}
scalog_seq_ip=${13-}
scalog_seq_port=${14-}
replication_factor_cfg=${15:-0}
num_brokers_cfg=${16:-4}
export EMBAR_USE_HUGETLB=${EMBAR_USE_HUGETLB:-1}
export EMBARCADERO_CXL_ZERO_MODE=${EMBARCADERO_CXL_ZERO_MODE:-full}
export EMBARCADERO_RUNTIME_MODE=${EMBARCADERO_RUNTIME_MODE:-throughput}
export EMBARCADERO_REPLICATION_FACTOR="${replication_factor_cfg:-0}"
export NUM_BROKERS="${num_brokers_cfg:-4}"
export EMBARCADERO_CXL_SHM_NAME=${EMBARCADERO_CXL_SHM_NAME:-/CXL_SHARED_EXPERIMENT_${UID}}
if [ "$sequence" = "SCALOG" ]; then
  export SCALOG_CXL_MODE=${SCALOG_CXL_MODE:-1}
fi
if [ -n "$ack_send_min_interval_us" ]; then
  export EMBARCADERO_ACK_SEND_MIN_INTERVAL_US="$ack_send_min_interval_us"
fi
if [ -n "$order0_fast_path" ]; then export EMBARCADERO_ORDER0_FAST_PATH="$order0_fast_path"; fi
if [ -n "$ack_stall_sleep_us" ]; then export EMBARCADERO_ACK_STALL_SLEEP_US="$ack_stall_sleep_us"; fi
if [ -n "$corfu_seq_ip" ]; then export EMBARCADERO_CORFU_SEQ_IP="$corfu_seq_ip"; fi
if [ -n "$scalog_seq_ip" ]; then export EMBARCADERO_SCALOG_SEQ_IP="$scalog_seq_ip"; fi
if [ -n "$scalog_seq_port" ]; then export EMBARCADERO_SCALOG_SEQ_PORT="$scalog_seq_port"; fi
log_file="$state_dir/broker_${idx}.log"
cd "$build_bin"
export EMBARCADERO_HEAD_ADDR="$head_addr"
cmd=(./embarlet --config "$config")
if [ "$role" = "head" ]; then
  cmd+=(--head)
fi
cmd+=(--"$sequence")
if [ "$sequence" != "CORFU" ] && command -v numactl >/dev/null 2>&1 && numactl --hardware 2>/dev/null | grep -q 'node 1'; then
  if numactl --hardware 2>/dev/null | grep -q 'node 2'; then
    cmd=(numactl --cpunodebind=1 --membind=1,2 "${cmd[@]}")
  else
    cmd=(numactl --cpunodebind=1 --membind=1 "${cmd[@]}")
  fi
fi
"${cmd[@]}" >"$log_file" 2>&1 &
pid=$!
printf '%s\n' "$pid" > "$state_dir/broker_${idx}.pid"
for _ in $(seq 1 1200); do
  if [ -f "/tmp/embarlet_${pid}_ready" ]; then
    printf 'ready\n' > "$state_dir/broker_${idx}.ready"
    exit 0
  fi
  if ! kill -0 "$pid" 2>/dev/null; then
    exit 1
  fi
  sleep "$poll_interval"
done
exit 1
INNER
EOF
}

broker_remote_wait_for_cluster() {
  local timeout_s="$1"
  local expected="$2"
  local stability_s="${BROKER_READY_STABILITY_SEC:-1}"
  local start_ts
  start_ts=$(date +%s)
  local last_report=0
  local stable_since=0
  while true; do
    local healthy=0
    local missing=()
    for ((i=0; i<expected; i++)); do
      if broker_remote_status "$i" >/dev/null 2>&1; then
        healthy=$((healthy + 1))
      else
        missing+=("$i")
      fi
    done
    if [ "$healthy" -ge "$expected" ]; then
      local now
      now=$(date +%s)
      if [ "$stable_since" -eq 0 ]; then
        stable_since="$now"
      fi
      if [ $((now - stable_since)) -ge "$stability_s" ]; then
        return 0
      fi
    else
      stable_since=0
    fi
    local now
    now=$(date +%s)
    if [ $((now - start_ts)) -ge "$timeout_s" ]; then
      echo "ERROR: remote brokers on $REMOTE_BROKER_HOST did not become ready within ${timeout_s}s" >&2
      echo "  healthy=${healthy}/${expected} missing_ids=${missing[*]:-none}" >&2
      return 1
    fi
    if [ $((now - last_report)) -ge 1 ]; then
      echo "Waiting on $REMOTE_BROKER_HOST: healthy=${healthy}/${expected} missing_ids=${missing[*]:-none}"
      last_report="$now"
    fi
    sleep "${BROKER_POLL_INTERVAL_SEC:-0.1}"
  done
}

broker_local_wait_for_cluster() {
  local timeout_s="$1"
  local expected="$2"
  local stability_s="${BROKER_READY_STABILITY_SEC:-1}"
  local start_ts
  start_ts=$(date +%s)
  local stable_since=0
  echo "Waiting for $expected brokers to signal readiness..."
  while true; do
    local ready
    ready="$(broker_ready_file_count_local)"
    if [ "$ready" -ge "$expected" ]; then
      local now
      now=$(date +%s)
      if [ "$stable_since" -eq 0 ]; then
        stable_since="$now"
      fi
      if [ $((now - stable_since)) -ge "$stability_s" ]; then
        return 0
      fi
    else
      stable_since=0
    fi
    if [ $(( $(date +%s) - start_ts )) -ge "$timeout_s" ]; then
      echo "ERROR: local brokers did not become ready within ${timeout_s}s" >&2
      echo "  ready=${ready}/${expected}" >&2
      return 1
    fi
    sleep "${BROKER_POLL_INTERVAL_SEC:-0.1}"
  done
}

broker_wait_for_cluster() {
  local timeout_s="$1"
  local expected="$2"
  if broker_is_remote_mode; then
    broker_remote_wait_for_cluster "$timeout_s" "$expected"
  else
    broker_local_wait_for_cluster "$timeout_s" "$expected"
  fi
}

broker_ensure_cluster() {
  local expected="$1"
  local timeout_s="$2"
  local sequence="$3"
  local force_restart="${FORCE_RESTART_BROKERS:-0}"

  if broker_is_remote_mode; then
    if [ "$force_restart" = "1" ]; then
      echo "Force-restarting remote brokers on $REMOTE_BROKER_HOST"
  broker_remote_cleanup
    fi

    if [[ "$sequence" == "CORFU" ]]; then
      if ! broker_remote_corfu_start; then
        echo "ERROR: failed to start Corfu sequencer on $(broker_remote_corfu_sequencer_host)" >&2
        return 1
      fi
      sleep 1
    elif [[ "$sequence" == "SCALOG" ]]; then
      if ! broker_remote_scalog_start; then
        echo "ERROR: failed to start Scalog sequencer on $(broker_remote_scalog_sequencer_host)" >&2
        return 1
      fi
      sleep 1
    fi

    local healthy=0
    local missing=()
    for ((i=0; i<expected; i++)); do
      if broker_remote_status "$i" >/dev/null 2>&1; then
        healthy=$((healthy + 1))
      else
        missing+=("$i")
      fi
    done

    if [ "$healthy" -eq "$expected" ]; then
      echo "Reusing healthy remote brokers on $REMOTE_BROKER_HOST (${healthy}/${expected})"
      return 0
    fi

    echo "Remote brokers on $REMOTE_BROKER_HOST: healthy=${healthy}/${expected}; starting missing ids=${missing[*]:-none}"
    for idx in "${missing[@]}"; do
      if [ "$idx" -eq 0 ]; then
        broker_remote_launch "$idx" "head" "$sequence"
      else
        broker_remote_launch "$idx" "follower" "$sequence"
      fi
    done
    broker_remote_wait_for_cluster "$timeout_s" "$expected"
    return $?
  fi

  if [ "$force_restart" = "1" ]; then
    broker_local_cleanup
  fi
  return 0
}
