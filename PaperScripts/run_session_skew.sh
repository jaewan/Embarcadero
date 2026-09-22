#!/usr/bin/env bash
# Hot-ingress premise experiment: fixed aggregate load with uniform/Zipfian
# session rates. Compares Embarcadero striping against a favorable load-aware
# CXL-Scalog sticky assignment while all four log servers remain active. Sticky means
# one ordered TCP stream per session; parallel streams to one server are not a
# FIFO-preserving sticky baseline.
#
# Usage:
#   bash PaperScripts/run_session_skew.sh --smoke
#   bash PaperScripts/run_session_skew.sh
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
SMOKE=0
[[ "${1:-}" == "--smoke" ]] && SMOKE=1
[[ $# -le 1 ]] || { echo "usage: $0 [--smoke]" >&2; exit 2; }

CAMPAIGN_ID="${CAMPAIGN_ID:-session_skew_$(date -u +%Y%m%dT%H%M%SZ)}"
OUT_ROOT="${OUT_ROOT:-$ROOT/data/paper_eval/session_skew/$CAMPAIGN_ID}"
[[ ! -e "$OUT_ROOT" ]] || { echo "ERROR: refusing existing OUT_ROOT=$OUT_ROOT" >&2; exit 2; }

GIT_COMMIT="$(git rev-parse HEAD)"
GIT_DIRTY="$(git status --porcelain | wc -l | tr -d ' ')"
if [[ "$GIT_DIRTY" != 0 && "$SMOKE" != 1 && "${ALLOW_DIRTY_ARTIFACT:-0}" != 1 ]]; then
    echo "ERROR: publication campaign requires a clean tree" >&2
    exit 2
fi
mkdir -p "$OUT_ROOT/cells"

# Publication artifacts are invalid if any remote publisher runs a stale
# client. Record every hash and fail before starting the first broker.
LOCAL_CLIENT_SHA256="$(sha256sum build/bin/throughput_test | awk '{print $1}')"
LOCAL_CLIENT_CONFIG_SHA256="$(sha256sum config/client.yaml | awk '{print $1}')"
echo "host,binary_path,binary_sha256,binary_matches_local,config_path,config_sha256,config_matches_local" > "$OUT_ROOT/remote_binary_hashes.csv"
for host in c4 c3 c1; do
    remote_hash="$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" \
      'sha256sum /home/domin/Embarcadero/build/bin/throughput_test' 2>/dev/null | awk '{print $1}')"
    match=false; [[ "$remote_hash" == "$LOCAL_CLIENT_SHA256" ]] && match=true
    remote_config_hash="$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" \
      'sha256sum /home/domin/Embarcadero/config/client.yaml' 2>/dev/null | awk '{print $1}')"
    config_match=false; [[ "$remote_config_hash" == "$LOCAL_CLIENT_CONFIG_SHA256" ]] && config_match=true
    echo "$host,/home/domin/Embarcadero/build/bin/throughput_test,$remote_hash,$match,/home/domin/Embarcadero/config/client.yaml,$remote_config_hash,$config_match" >> "$OUT_ROOT/remote_binary_hashes.csv"
    if [[ "$match" != true || "$config_match" != true ]]; then
        [[ "$match" == true ]] || echo "ERROR: stale/missing throughput_test on $host (local=$LOCAL_CLIENT_SHA256 remote=${remote_hash:-missing})" >&2
        [[ "$config_match" == true ]] || echo "ERROR: stale/missing client.yaml on $host (local=$LOCAL_CLIENT_CONFIG_SHA256 remote=${remote_config_hash:-missing})" >&2
        exit 2
    fi
done

if [[ "$SMOKE" == 1 ]]; then
    SESSION_COUNTS="${SESSION_COUNTS:-4}"
    THETAS="${THETAS:-1.2}"
    MODES="${MODES:-embar_striped scalog_sticky_balanced}"
    TRIALS="${TRIALS:-1}"
    DURATION_SEC="${DURATION_SEC:-2}"
    AGGREGATE_TARGET_MIBPS="${AGGREGATE_TARGET_MIBPS:-1024}"
else
    SESSION_COUNTS="${SESSION_COUNTS:-4 8 16}"
    THETAS="${THETAS:-0.0 0.99 1.2}"
    MODES="${MODES:-embar_striped scalog_sticky_balanced}"
    TRIALS="${TRIALS:-3}"
    DURATION_SEC="${DURATION_SEC:-3}"
    AGGREGATE_TARGET_MIBPS="${AGGREGATE_TARGET_MIBPS:-14336}"
fi

MESSAGE_SIZE="${MESSAGE_SIZE:-4096}"
BROKERS="${BROKERS:-4}"
PACE_QUANTUM_BYTES="${PACE_QUANTUM_BYTES:-262144}"
STRIPED_THREADS_PER_BROKER="${STRIPED_THREADS_PER_BROKER:-1}"
QUEUE_POOL_MAX_BYTES="${QUEUE_POOL_MAX_BYTES:-268435456}"
PUBLISH_BATCH_BYTES="${PUBLISH_BATCH_BYTES:-524288}"
(( PUBLISH_BATCH_BYTES >= MESSAGE_SIZE && PUBLISH_BATCH_BYTES % 1024 == 0 )) || {
    echo "ERROR: PUBLISH_BATCH_BYTES must be >= MESSAGE_SIZE and KiB-aligned" >&2
    exit 2
}
SEGMENT_SIZE_BYTES="${SEGMENT_SIZE_BYTES:-51539607552}"
echo "cell,mode,placement,sessions,theta,trials,duration_sec,offered_mibps,status,result_csv" > "$OUT_ROOT/results.csv"
campaign_fail=0

make_plan() {
    local mode="$1" sessions="$2" theta="$3" plan="$4"
    python3 - "$mode" "$sessions" "$theta" "$TRIALS" "$DURATION_SEC" \
      "$AGGREGATE_TARGET_MIBPS" "$MESSAGE_SIZE" "$BROKERS" "$SEGMENT_SIZE_BYTES" \
      "$PACE_QUANTUM_BYTES" "$STRIPED_THREADS_PER_BROKER" \
      "$QUEUE_POOL_MAX_BYTES" "$PUBLISH_BATCH_BYTES" "$plan" <<'PY'
import json, math, sys
mode, ns, theta, trials, duration, total_rate, msg, brokers, segment_size, pace_quantum, striped_threads, pool_cap, batch_size, out = sys.argv[1:]
n, theta, trials, duration = int(ns), float(theta), int(trials), int(duration)
total_rate, msg, brokers, segment_size = int(total_rate), int(msg), int(brokers), int(segment_size)
pace_quantum = int(pace_quantum)
striped_threads, pool_cap, batch_size = int(striped_threads), int(pool_cap), int(batch_size)
if striped_threads <= 0: raise SystemExit("striped thread count must be positive")
weights = [1.0 / ((i + 1) ** theta) for i in range(n)]
raw = [total_rate * w / sum(weights) for w in weights]
rates = [max(1, int(math.floor(x))) for x in raw]
for i in sorted(range(n), key=lambda j: raw[j] - math.floor(raw[j]), reverse=True)[:total_rate-sum(rates)]: rates[i] += 1
if sum(rates) != total_rate: raise SystemExit("rate rounding failed")
# Repeated sessions on one publisher host alternate NUMA nodes.  This is part
# of the experimental contract: placing both c4 sessions on node 1 exhausted
# that node's HugeTLB pool and produced SIGBUS rather than a workload result.
host_names = ["c4", "c3", "c1"]
preferred_numa = {"c4": 1, "c3": 1, "c1": 0}
occurrences = {h: 0 for h in host_names}
hosts = []
for i in range(n):
    h = host_names[i % len(host_names)]
    numa = preferred_numa[h] if occurrences[h] % 2 == 0 else 1 - preferred_numa[h]
    occurrences[h] += 1
    hosts.append((h, numa))
host_counts = {h: sum(1 for x, _ in hosts if x == h) for h, _ in hosts}
tags = [f"{h}{i}" if host_counts[h] > 1 else h for i, (h, _) in enumerate(hosts)]
placement = "striped" if mode == "embar_striped" else ("sticky_hash" if mode.endswith("hash") else "sticky_balanced")
assigned = [None] * n
if placement == "sticky_hash": assigned = [i % brokers for i in range(n)]
elif placement == "sticky_balanced":
    load = [0] * brokers
    for i in sorted(range(n), key=lambda j: rates[j], reverse=True):
        b = min(range(brokers), key=lambda x: (load[x], x)); assigned[i] = b; load[b] += rates[i]
sessions = []
for i in range(n):
    total_bytes = rates[i] * duration * 1024 * 1024
    total_bytes -= total_bytes % msg
    sessions.append({"id": i, "host": hosts[i][0], "numa": hosts[i][1], "client_tag": tags[i],
                     "target_mibps": rates[i], "total_bytes": total_bytes, "broker": assigned[i]})
if placement != "striped":
    assigned_bytes = [sum(s["total_bytes"] for s in sessions if s["broker"] == b) for b in range(brokers)]
    if max(assigned_bytes) > int(segment_size * 0.75):
        raise SystemExit(f"planned sticky bytes {max(assigned_bytes)} exceed 75% of segment {segment_size}")
plan = {"cell": f"{mode}_s{n}_z{theta:g}", "mode": mode, "placement": placement,
        "sessions": n, "theta": theta, "trials": trials, "duration_sec": duration,
        "aggregate_target_mibps": total_rate, "message_size": msg, "brokers": brokers,
        # Native-policy contract: Embarcadero may pipeline several streams per
        # server because its sequencer reconstructs session FIFO. CXL-Scalog's
        # correct sticky control uses one ordered stream per session; parallel
        # independent TCP streams would reintroduce arrival-order inversions.
        "threads_per_broker": striped_threads if placement == "striped" else 1,
        "data_connections_total": n * brokers * striped_threads if placement == "striped" else n,
        "pace_quantum_bytes": pace_quantum,
        # QueueBuffer allocates at least 32 slots per data queue plus one.
        # The publisher currently creates queues for all configured servers,
        # including sticky cells whose routing allowlist uses only one server.
        "publisher_batch_size_bytes": batch_size,
        "queue_slot_size_bytes": math.ceil((batch_size + 128) / 64) * 64,
        "queue_pool_cap_bytes": pool_cap,
        "segment_size_bytes": segment_size,
        "session_plan": sessions}
hugepage = 2 * 1024 * 1024
# Mirror QueueBuffer::AddBuffers so preflight covers the actual mapping, not
# merely the configured cap (the queue-count minimum may exceed that cap).
threads = plan["threads_per_broker"]
queues = threads * brokers
min_slots = max(256, queues * 32 + 1)
queue_hint = min(threads * brokers * 256 * 1024 * 1024, 4 * 1024**3) // threads
hint_bytes = min(max(256 * 1024**2, queue_hint), pool_cap)
slots_for_hint = max(1, hint_bytes // plan["queue_slot_size_bytes"])
max_slots = max(min_slots, pool_cap // plan["queue_slot_size_bytes"])
plan["queue_slots_per_client"] = min(max_slots, max(min_slots, slots_for_hint))
plan["planned_hugetlb_bytes_per_client"] = math.ceil(
    plan["queue_slot_size_bytes"] * plan["queue_slots_per_client"] / hugepage
) * hugepage
with open(out, "w") as f: json.dump(plan, f, indent=2)
PY
}

preflight_client_hugepages() {
    local plan="$1" report="$2"
    echo "host,numa,clients,planned_bytes,free_bytes,headroom_bytes,status" > "$report"
    while IFS=, read -r host numa clients planned; do
        local page_kb free_pages free_bytes headroom status
        # -n is essential: otherwise ssh consumes the remaining plan rows from
        # this while loop's stdin and only the first NUMA group is validated.
        read -r page_kb free_pages < <(ssh -n -o BatchMode=yes -o ConnectTimeout=10 "$host" \
          "awk '/Hugepagesize:/ {p=\$2} END {print p}' /proc/meminfo; cat /sys/devices/system/node/node${numa}/hugepages/hugepages-2048kB/free_hugepages" | xargs)
        [[ "$page_kb" =~ ^[0-9]+$ && "$free_pages" =~ ^[0-9]+$ ]] || {
            echo "ERROR: cannot read node${numa} HugeTLB availability on $host" >&2
            return 1
        }
        free_bytes=$((page_kb * 1024 * free_pages))
        # Preserve 64 MiB per process for small auxiliary HugeTLB mappings and
        # reject a plan that would consume the NUMA node to its last page.
        headroom=$((clients * 64 * 1024 * 1024))
        status=pass
        if (( planned + headroom > free_bytes )); then status=fail; fi
        echo "$host,$numa,$clients,$planned,$free_bytes,$headroom,$status" >> "$report"
        if [[ "$status" != pass ]]; then
            echo "ERROR: $host/node$numa HugeTLB plan needs $planned + $headroom headroom; only $free_bytes free" >&2
            return 1
        fi
    done < <(python3 - "$plan" <<'PY'
import collections, json, sys
p=json.load(open(sys.argv[1]))
groups=collections.Counter((s["host"], int(s["numa"])) for s in p["session_plan"])
per=int(p["planned_hugetlb_bytes_per_client"])
for (host,numa),clients in sorted(groups.items()):
    print(f"{host},{numa},{clients},{clients * per}")
PY
    )
}

run_cell() {
    local mode="$1" sessions="$2" theta="$3"
    local cell="${mode}_s${sessions}_z${theta}" cell_root="$OUT_ROOT/cells/${mode}_s${sessions}_z${theta}"
    local plan="$cell_root/plan.json" logs="$cell_root/logs" result="$cell_root/result.csv"
    mkdir -p "$cell_root"
    make_plan "$mode" "$sessions" "$theta" "$plan"
    if ! preflight_client_hugepages "$plan" "$cell_root/client_hugepage_preflight.csv"; then
        echo "$cell,$mode,unknown,$sessions,$theta,$TRIALS,$DURATION_SEC,$AGGREGATE_TARGET_MIBPS,fail,$result" >> "$OUT_ROOT/results.csv"
        campaign_fail=$((campaign_fail + 1))
        return
    fi
    local hosts numas loads rates allowlists sequencer order threads extra=()
    mapfile -t vals < <(python3 - "$plan" <<'PY'
import json, sys
p=json.load(open(sys.argv[1])); ss=p["session_plan"]
print(",".join(s["host"] for s in ss)); print(",".join(str(s["numa"]) for s in ss))
print("|".join(str(s["total_bytes"]) for s in ss)); print("|".join(str(s["target_mibps"]) for s in ss))
print("|".join(str(s["broker"]) for s in ss) if p["placement"] != "striped" else "")
PY
    )
    hosts="${vals[0]}"; numas="${vals[1]}"; loads="${vals[2]}"; rates="${vals[3]}"; allowlists="${vals[4]:-}"
    if [[ "$mode" == embar_* ]]; then
        sequencer=EMBARCADERO; order=5
    else
        sequencer=SCALOG; order=1
        extra+=(SKIP_REMOTE_SCALOG_SEQUENCER=1 EMBARCADERO_SCALOG_SEQ_IP="${BROKER_IP:-10.10.10.10}")
    fi
    threads="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["threads_per_broker"])' "$plan")"
    echo ">>> $cell"
    set +e
    env ALLOW_DIRTY_ARTIFACT="$([[ "$SMOKE" == 1 ]] && echo 1 || echo "${ALLOW_DIRTY_ARTIFACT:-0}")" \
      LOG_DIR="$logs" NUM_CLIENTS="$sessions" NUM_BROKERS="$BROKERS" NUM_TRIALS="$TRIALS" \
      TRIAL_MAX_ATTEMPTS=2 CLIENT_HOSTS_CSV="$hosts" CLIENT_NUMAS_CSV="$numas" \
      CLIENT_LOAD_BYTES_PIPE="$loads" CLIENT_TARGET_MBPS_PIPE="$rates" \
      CLIENT_PUBLISH_BROKER_ALLOWLISTS_PIPE="$allowlists" \
      TOTAL_MESSAGE_SIZE=1 MESSAGE_SIZE="$MESSAGE_SIZE" THREADS_PER_BROKER="$threads" \
      TEST_TYPE=5 ORDER="$order" ACK=1 REPLICATION_FACTOR=0 SEQUENCER="$sequencer" \
      EMBARCADERO_CXL_SIZE=274877906944 EMBARCADERO_CXL_ZERO_MODE=metadata \
      EMBARCADERO_CXL_MAP_POPULATE=0 EMBAR_USE_HUGETLB=1 BROKER_READY_TIMEOUT_SEC=300 \
      EMBARCADERO_QUEUE_POOL_MAX_BYTES="$QUEUE_POOL_MAX_BYTES" \
      EMBARCADERO_TCP_USER_TIMEOUT_MS=30000 \
      EMBARCADERO_HEADER_SEND_TIMEOUT_MS=30000 \
      EMBARCADERO_SESSION_RTO_MIN_MS=2000 \
      EMBARCADERO_THROUGHPUT_PACE_QUANTUM_BYTES="$PACE_QUANTUM_BYTES" \
      EMBARCADERO_SEGMENT_SIZE="$SEGMENT_SIZE_BYTES" EMBARCADERO_REQUIRED_CXL_SEGMENTS=4 \
      EMBARCADERO_CLIENT_PUB_BATCH_KB="$((PUBLISH_BATCH_BYTES / 1024))" \
      EMBARCADERO_BATCH_SIZE="$PUBLISH_BATCH_BYTES" \
      "${extra[@]}" bash scripts/run_multiclient.sh > "$cell_root/runner.log" 2>&1
    local rc=$?
    set -e
    if [[ "$rc" == 0 ]] && python3 PaperScripts/analyze_session_skew_cell.py \
        --plan "$plan" --log-dir "$logs" --out "$result" >> "$cell_root/runner.log" 2>&1; then
        echo "$cell,$mode,$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["placement"])' "$plan"),$sessions,$theta,$TRIALS,$DURATION_SEC,$AGGREGATE_TARGET_MIBPS,pass,$result" >> "$OUT_ROOT/results.csv"
    else
        echo "$cell,$mode,unknown,$sessions,$theta,$TRIALS,$DURATION_SEC,$AGGREGATE_TARGET_MIBPS,fail,$result" >> "$OUT_ROOT/results.csv"
        campaign_fail=$((campaign_fail + 1))
        tail -n 30 "$cell_root/runner.log" >&2 || true
    fi
}

for sessions in $SESSION_COUNTS; do
  for theta in $THETAS; do
    for mode in $MODES; do run_cell "$mode" "$sessions" "$theta"; done
  done
done

python3 - "$OUT_ROOT" "$CAMPAIGN_ID" "$GIT_COMMIT" "$GIT_DIRTY" "$SMOKE" <<'PY'
import csv, hashlib, json, os, sys
root,cid,commit,dirty,smoke=sys.argv[1:]
rows=list(csv.DictReader(open(root+"/results.csv")))
remote_hashes=list(csv.DictReader(open(root+"/remote_binary_hashes.csv")))
manifest={"campaign_id":cid,"commit":commit,"dirty_files":int(dirty),"smoke":bool(int(smoke)),
 "remote_client_hashes":remote_hashes,"matrix":rows,
 "summary":{"cells":len(rows),"pass":sum(r["status"]=="pass" for r in rows),"fail":sum(r["status"]!="pass" for r in rows)}}
for binary in ("build/bin/embarlet","build/bin/throughput_test","build/bin/scalog_global_sequencer"):
 try: manifest.setdefault("sha256",{})[binary]=hashlib.sha256(open(binary,"rb").read()).hexdigest()
 except FileNotFoundError: manifest.setdefault("sha256",{})[binary]=None
json.dump(manifest,open(root+"/campaign_manifest.json","w"),indent=2)
PY

if [[ "$campaign_fail" != 0 ]]; then
    echo "ERROR: $campaign_fail required cells failed; see $OUT_ROOT" >&2
    exit 1
fi
echo "PASS: $OUT_ROOT"
