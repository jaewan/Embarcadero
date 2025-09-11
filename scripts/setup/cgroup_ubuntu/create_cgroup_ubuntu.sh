#!/usr/bin/env bash
set -euo pipefail

# Ubuntu 24.04 defaults to cgroup v2 (unified). This script assumes that.
# It creates cgroups under /sys/fs/cgroup/emb and namespaces ns0..nsN-1
# plus a Linux bridge br-emb to connect them.
#
# Usage:
#   sudo ./create_cgroup_ubuntu.sh setup N          # create N groups + namespaces
#   sudo ./create_cgroup_ubuntu.sh run  i -- <cmd>  # run <cmd> in ns<i> AND cgroup i
#   sudo ./create_cgroup_ubuntu.sh test-net i j     # run iperf3 server in ns<j> & client in ns<i>
#   sudo ./create_cgroup_ubuntu.sh verify           # print current settings
#   sudo ./create_cgroup_ubuntu.sh cleanup          # tear everything down
#
# ENV overrides:
#   RATE=10gbit                # per-namespace bandwidth cap (both directions)
#   IO_TOTAL_RBPS=2400M        # (optional) total READ BW; split evenly across groups (K/M/G or plain bytes)
#   IO_TOTAL_WBPS=1200M        # (optional) total WRITE BW; split evenly across groups

RATE="${RATE:-10gbit}"
IO_TOTAL_RBPS="${IO_TOTAL_RBPS:-}"
IO_TOTAL_WBPS="${IO_TOTAL_WBPS:-}"

CGROOT="/sys/fs/cgroup"
ROOT="${CGROOT}/emb"
BR="br-emb"
NSP="ns"   # namespace prefix
SUBNET="10.200.0.0/24"
GW="10.200.0.1"

need() { command -v "$1" >/dev/null 2>&1 || { echo "Missing: $1" >&2; exit 1; }; }
ensure_htb() { modprobe sch_htb 2>/dev/null || true; }
ensure_blk_throttle() { modprobe blk-throttle 2>/dev/null || true; }

ensure_root() {
  if [[ $EUID -ne 0 ]]; then
    echo "Please run as root." >&2
    exit 1
  fi
}

enable_controllers() {
  # enable controllers at root and emb level (idempotent)
  for d in "${CGROOT}"; do
    [[ -w "${d}/cgroup.subtree_control" ]] || continue
    for c in cpu cpuset memory io; do
      grep -qw "$c" "${d}/cgroup.controllers" 2>/dev/null || continue
      grep -qw "\\+${c}" "${d}/cgroup.subtree_control" 2>/dev/null || echo "+${c}" > "${d}/cgroup.subtree_control" || true
    done
  done

  mkdir -p "${ROOT}"
  if [[ -f "${ROOT}/cpuset.cpus" ]]; then
    cat "${CGROOT}/cpuset.cpus.effective" > "${ROOT}/cpuset.cpus" 2>/dev/null || true
  fi
  if [[ -f "${ROOT}/cpuset.mems" ]]; then
    cat "${CGROOT}/cpuset.mems.effective" > "${ROOT}/cpuset.mems" 2>/dev/null || true
  fi

  if [[ -w "${ROOT}/cgroup.subtree_control" ]]; then
    for c in cpu cpuset memory io; do
      grep -qw "\\+${c}" "${ROOT}/cgroup.subtree_control" 2>/dev/null || echo "+${c}" > "${ROOT}/cgroup.subtree_control" || true
    done
  fi
}

cpu_chunks() {
  local N="$1" I="$2" total start len rem per
  total="$(nproc --all)"
  per=$(( total / N ))
  rem=$(( total % N ))
  if (( I < rem )); then
    len=$(( per + 1 ))
    start=$(( I * (per + 1) ))
  else
    len=$(( per ))
    start=$(( rem * (per + 1) + (I - rem) * per ))
  fi
  if (( len == 0 )); then
    echo ""
    return
  fi
  local end=$(( start + len - 1 ))
  echo "${start}-${end}"
}

# -------------------------
# IO helpers for disk split
# -------------------------

root_dev_majmin() {
  local SRC DEV BASE

  SRC="$(findmnt -no SOURCE /)" || return 1
  DEV="${SRC#/dev/}"

  if [[ -n "${IO_DEV:-}" && -e "/sys/class/block/${IO_DEV}/dev" ]]; then
    cat "/sys/class/block/${IO_DEV}/dev"
    return 0
  fi

  if [[ "$DEV" =~ ^(nvme[0-9]+n[0-9]+)p[0-9]+$ ]]; then
    BASE="${BASH_REMATCH[1]}"
  elif [[ "$DEV" =~ ^([a-z]+)[0-9]+$ ]]; then
    BASE="${BASH_REMATCH[1]}"
  else
    BASE="$DEV"
  fi

  if [[ -e "/sys/class/block/${BASE}/dev" ]]; then
    cat "/sys/class/block/${BASE}/dev"
    return 0
  fi

  [[ -e "/sys/class/block/${DEV}/dev" ]] || return 1
  cat "/sys/class/block/${DEV}/dev"
}

to_bytes_per_sec() {
  local v="$1"
  if [[ "$v" =~ ^[0-9]+$ ]]; then echo "$v"; return 0; fi
  local num unit; num="$(echo "$v" | sed -E 's/[^0-9]//g')"; unit="$(echo "$v" | sed -E 's/[0-9]+//g')"
  case "$unit" in
    K|k|KB|kb) echo $(( num * 1000 ));;
    M|m|MB|mb) echo $(( num * 1000 * 1000 ));;
    G|g|GB|gb) echo $(( num * 1000 * 1000 * 1000 ));;
    KiB|KIB|Ki|ki) echo $(( num * 1024 ));;
    MiB|MIB|Mi|mi) echo $(( num * 1024 * 1024 ));;
    GiB|GIB|Gi|gi) echo $(( num * 1024 * 1024 * 1024 ));;
    *) echo "$num";;
  esac
}

bytes_to_cgroup_rate() {
  echo "$1"
}

compute_io_split() {
  # Only use provided env vars; if unset, no disk caps will be applied.
  local total_r_bps="" total_w_bps=""
  if [[ -n "$IO_TOTAL_RBPS" ]]; then
    total_r_bps="$(to_bytes_per_sec "$IO_TOTAL_RBPS")"
  fi
  if [[ -n "$IO_TOTAL_WBPS" ]]; then
    total_w_bps="$(to_bytes_per_sec "$IO_TOTAL_WBPS")"
  fi

  echo "${total_r_bps:-} ${total_w_bps:-}"
}

setup_cgroups() {
  local N="$1"
  enable_controllers
  ensure_blk_throttle

  local mem_kb per_kb
  mem_kb="$(awk '/MemTotal:/ {print $2}' /proc/meminfo)"
  per_kb=$(( (mem_kb * 90 / 100) / N ))
  local per_bytes=$(( per_kb * 1024 ))

  local _TOTAL_R_BPS="" _TOTAL_W_BPS="" _IO_TOTALS_CALC_DONE=""
  local MAJMIN=""
  if MAJMIN="$(root_dev_majmin)"; then
    read -r _TOTAL_R_BPS _TOTAL_W_BPS <<<"$(compute_io_split)"
    if [[ -n "$_TOTAL_R_BPS" || -n "$_TOTAL_W_BPS" ]]; then
      echo "ℹ️  Disk limits from env: device ${MAJMIN}, totals R=${_TOTAL_R_BPS:-0} B/s W=${_TOTAL_W_BPS:-0} B/s (N=${N})."
    else
      echo "ℹ️  No IO totals provided; skipping disk caps."
    fi
  fi

  for ((i=0;i<N;i++)); do
    local G="${ROOT}/g${i}"
    mkdir -p "${G}"

    local cpus; cpus="$(cpu_chunks "${N}" "${i}")"
    [[ -n "$cpus" ]] || { echo "Warning: no CPUs for group $i"; }
    echo "$cpus" > "${G}/cpuset.cpus"
    cat "${ROOT}/cpuset.mems" > "${G}/cpuset.mems"

    echo "${per_bytes}" > "${G}/memory.max"
    echo 100 > "${G}/cpu.weight"

    # Evenly split disk bandwidth if totals are provided
    if [[ -n "${MAJMIN}" && ( -n "${_TOTAL_R_BPS}" || -n "${_TOTAL_W_BPS}" ) ]]; then
      local PER_R="" PER_W=""

      # Ceil division to avoid 0; skip a direction if total is 0/empty.
      if [[ -n "${_TOTAL_R_BPS}" && "${_TOTAL_R_BPS}" -gt 0 ]]; then
        PER_R=$(( (_TOTAL_R_BPS + N - 1) / N ))
      fi
      if [[ -n "${_TOTAL_W_BPS}" && "${_TOTAL_W_BPS}" -gt 0 ]]; then
        PER_W=$(( (_TOTAL_W_BPS + N - 1) / N ))
      fi

      if [[ -n "$PER_R" || -n "$PER_W" ]]; then
        local line="${MAJMIN}"
        [[ -n "$PER_R" ]] && line+=" rbps=$(bytes_to_cgroup_rate "$PER_R")"
        [[ -n "$PER_W" ]] && line+=" wbps=$(bytes_to_cgroup_rate "$PER_W")"
        if ! echo "$line" > "${G}/io.max" 2>/dev/null; then
          echo "⚠️  Failed to write io.max for ${G} with '${line}'. Skipping disk cap for this group."
        fi
      fi
    fi
  done
}

setup_bridge() {
  ip link show "${BR}" >/dev/null 2>&1 || {
    ip link add "${BR}" type bridge
    ip addr add "${GW}/24" dev "${BR}"
    ip link set "${BR}" up
  }
}

setup_netns() {
  local N="$1"
  setup_bridge
  ensure_htb

  for ((i=0;i<N;i++)); do
    local NS="${NSP}${i}"
    ip netns list | grep -qw "${NS}" || ip netns add "${NS}"

    ip link show "veth${i}" >/dev/null 2>&1 || {
      ip link add "veth${i}" type veth peer name "veth${i}ns"
      ip link set "veth${i}" master "${BR}"
      ip link set "veth${i}" up
      ip link set "veth${i}ns" netns "${NS}"
    }

    ip -n "${NS}" link set lo up
    ip -n "${NS}" addr show dev "veth${i}ns" | grep -q 'inet ' || ip -n "${NS}" addr add "10.200.0.$((10+i))/24" dev "veth${i}ns"
    ip -n "${NS}" link set "veth${i}ns" up
    ip -n "${NS}" route replace default via "${GW}" dev "veth${i}ns" onlink

    # TC: cap BOTH directions to RATE (shape egress on each end)
    # Increase r2q to reduce "quantum is big" warnings
    tc qdisc del dev "veth${i}" root 2>/dev/null || true
    tc qdisc add dev "veth${i}" root handle 1: htb default 10 r2q 10000
    tc class add dev "veth${i}" parent 1: classid 1:10 htb rate "${RATE}" ceil "${RATE}"

    tc -n "${NS}" qdisc del dev "veth${i}ns" root 2>/dev/null || true
    tc -n "${NS}" qdisc add dev "veth${i}ns" root handle 1: htb default 10 r2q 10000
    tc -n "${NS}" class add dev "veth${i}ns" parent 1: classid 1:10 htb rate "${RATE}" ceil "${RATE}"
  done
}

cmd_setup() {
  local N="${1:?N required}"
  need ip; need tc; ensure_root
  setup_cgroups "${N}"
  setup_netns "${N}"
  echo "✅ Set up ${N} cgroups under ${ROOT} and ${N} namespaces ${NSP}0..${NSP}$((N-1)) bridged on ${BR} @ ${GW}. Rate=${RATE}."
}

cmd_run() {
  local I="${1:?group index required}"
  shift
  [[ "${1:-}" == "--" ]] && shift || true
  local G="${ROOT}/g${I}"
  local NS="${NSP}${I}"
  [[ -d "${G}" ]] || { echo "Group ${I} not found. Did you run setup?"; exit 1; }
  ip netns list | grep -qw "${NS}" || { echo "Namespace ${NS} not found."; exit 1; }

  bash -c 'echo $$ > "'"${G}"'/cgroup.procs"; exec ip netns exec "'"${NS}"'" '"$*"''
}

cmd_test_net() {
  local I="${1:?client index}"; local J="${2:?server index}"
  ensure_root
  need iperf3 || { echo "Install iperf3: apt-get install -y iperf3"; exit 1; }

  local NSC="${NSP}${I}" NSS="${NSP}${J}" SIP="10.200.0.$((10+J))"

  ip netns exec "${NSS}" pkill -f '^iperf3( |$)' >/dev/null 2>&1 || true
  ip netns exec "${NSC}" pkill -f '^iperf3( |$)' >/dev/null 2>&1 || true

  local PORT CAND
  for CAND in 5201 5202 5203 5204 5205 5300 5400 5500; do
    if ! ip netns exec "${NSS}" bash -lc "ss -ltn | awk '{print \$4}' | grep -qE '[:.]${CAND}\$'"; then
      PORT="${CAND}"
      break
    fi
  done
  [[ -n "${PORT:-}" ]] || { echo "No free port found for iperf3 server."; exit 1; }

  echo "Starting iperf3 server in ${NSS} on port ${PORT} …"
  ip netns exec "${NSS}" bash -lc "iperf3 -s -1 -p ${PORT}" >/dev/null 2>&1 &
  local srv_pid=$!

  for _ in $(seq 1 20); do
    if ip netns exec "${NSS}" bash -lc "ss -ltn | awk '{print \$4}' | grep -qE '[:.]${PORT}\$'"; then
      break
    fi
    sleep 0.1
  done
  if ! ip netns exec "${NSS}" bash -lc "ss -ltn | awk '{print \$4}' | grep -qE '[:.]${PORT}\$'"; then
    echo "iperf3 server failed to bind on ${NSS}:${PORT}"; kill "${srv_pid}" 2>/dev/null || true; exit 1;
  fi

  echo "Running client in ${NSC} to ${SIP}:${PORT} (expect ~${RATE} cap)…"
  ip netns exec "${NSC}" bash -lc "iperf3 -J -c ${SIP} -p ${PORT}"
}

cmd_verify() {
  echo "# Cgroups:"
  find "${ROOT}" -maxdepth 1 -type d -name 'g*' | sort | while read -r G; do
    i="${G##*/}"
    echo "== ${i} =="
    echo -n "cpuset.cpus: "; cat "${G}/cpuset.cpus"
    echo -n "memory.max : "; cat "${G}/memory.max"
    [[ -f "${G}/io.max" ]] && { echo -n "io.max     : "; cat "${G}/io.max"; } || true
    echo
  done

  echo "# Namespaces + tc:"
  ip -br link show type bridge | grep -E "${BR}" || true
  ip -br addr show | grep -E "10\.200\.0\." || true

  for l in $(ip -o link show | awk -F': ' '/veth[0-9]+@/{print $2}' | cut -d'@' -f1 | sort); do
    echo "qdisc on ${l}:"
    tc qdisc show dev "${l}" || true
    tc class show dev "${l}" || true
  done

  for ns in $(ip netns list | awk '{print $1}'); do
    i="${ns#ns}"
    dev="veth${i}ns"
    echo "qdisc in ${ns} on ${dev}:"
    tc -n "${ns}" qdisc show dev "${dev}" 2>/dev/null || true
    tc -n "${ns}" class show dev "${dev}" 2>/dev/null || true
  done
}

cmd_cleanup() {
  ensure_root
  for ns in $(ip netns list | awk '{print $1}'); do
    ip netns exec "${ns}" bash -lc 'pkill iperf3 || true'
  done

  for ns in $(ip netns list | awk '{print $1}'); do
    ip netns del "${ns}" || true
  done

  for v in $(ip -o link show | awk -F': ' '/veth[0-9]+@/{print $2}' | cut -d'@' -f1); do
    tc qdisc del dev "${v}" root 2>/dev/null || true
    ip link del "${v}" 2>/dev/null || true
  done

  ip link set "${BR}" down 2>/dev/null || true
  ip link del "${BR}" 2>/dev/null || true

  if [[ -d "${ROOT}" ]]; then
    for G in "${ROOT}"/g*; do
      [[ -d "$G" ]] || continue
      rmdir "$G" 2>/dev/null || {
        while read -r pid; do
          echo "$pid" > "${CGROOT}/cgroup.procs" 2>/dev/null || true
        done < <(cat "${G}/cgroup.procs" 2>/dev/null || true)
        rmdir "$G" 2>/dev/null || true
      }
    done
    rmdir "${ROOT}" 2>/dev/null || true
  fi
  echo "🧹 Cleaned up."
}

main() {
  local cmd="${1:-}"
  case "$cmd" in
    setup)   shift; cmd_setup "${1:?Usage: $0 setup N}";;
    run)     shift; cmd_run "$@";;
    test-net)shift; cmd_test_net "${1:?i}" "${2:?j}";;
    verify)  cmd_verify;;
    cleanup) cmd_cleanup;;
    *)
      cat <<EOF
Usage:
  sudo $0 setup N           # create N groups + namespaces, limit to RATE (${RATE})
  sudo $0 run i -- <cmd>    # run <cmd> in namespace i inside its cgroup
  sudo $0 test-net i j      # iperf3 test: client in i -> server in j
  sudo $0 verify            # show cgroup + tc state
  sudo $0 cleanup           # remove everything
ENV:
  RATE=${RATE}                 (e.g., 1gbit, 500mbit, 10gbit)
  IO_TOTAL_RBPS=${IO_TOTAL_RBPS}  (e.g., 2400M)
  IO_TOTAL_WBPS=${IO_TOTAL_WBPS}  (e.g., 1200M)
Notes:
  * Disk BW caps apply to the block device backing '/'. If your workload is on /data,
    set IO_DEV (e.g., IO_DEV=dm-0) or adapt root_dev_majmin().
  * If IO_TOTAL_RBPS/IO_TOTAL_WBPS are unset, no disk caps will be applied.
EOF
      ;;
  esac
}
main "$@"
