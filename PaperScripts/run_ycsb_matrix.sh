#!/usr/bin/env bash
# PaperScripts/run_ycsb_matrix.sh
#
# Preregistered YCSB matrix sweep
# (docs/experiments/YCSB_DISTRIBUTED_KV_PLAN.md, Section 4/gate step 5).
# Iterates SYSTEMS x WORKLOADS x N_CLIENTS_LIST x TRIALS_PER_CELL, invoking
# PaperScripts/run_ycsb_distributed.sh once per (cell, trial, attempt), and
# writes campaign_manifest.json + results.csv under
# data/paper_eval/ycsb/<campaign_id>/, per the handoff's artifact layout.
#
# Fault-isolating: a failed attempt is retried up to TRIAL_MAX_ATTEMPTS times
# (matching run_multiclient.sh's convention); a cell that still fails after
# that is recorded as failed in results.csv/attempt_summary.csv without
# aborting the rest of the sweep (matching run_overnight_eval.sh's
# never-propagate-failure convention) — no cell is silently dropped.
#
# Refuses to start from a dirty tree (own check, in addition to
# run_ycsb_distributed.sh's) so the recorded commit is stable for the whole
# campaign even if a cell's own dirty-check races with a later edit.
#
# Usage:
#   bash PaperScripts/run_ycsb_matrix.sh
#   SYSTEMS="EMBARCADERO" WORKLOADS="A" N_CLIENTS_LIST="1" TRIALS_PER_CELL=1 \
#     RECORD_COUNT_TOTAL=4000 OPERATION_COUNT_PER_CLIENT=4000 \
#     bash PaperScripts/run_ycsb_matrix.sh   # small rehearsal

set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

SYSTEMS="${SYSTEMS:-EMBARCADERO CORFU SCALOG}"
WORKLOADS="${WORKLOADS:-A F}"
N_CLIENTS_LIST="${N_CLIENTS_LIST:-1 2 3}"
TRIALS_PER_CELL="${TRIALS_PER_CELL:-3}"
TRIAL_MAX_ATTEMPTS="${TRIAL_MAX_ATTEMPTS:-3}"
CELL_TIMEOUT_SEC="${CELL_TIMEOUT_SEC:-900}"

RECORD_COUNT_TOTAL="${RECORD_COUNT_TOTAL:-1000000}"
OPERATION_COUNT_PER_CLIENT="${OPERATION_COUNT_PER_CLIENT:-300000}"
VALUE_SIZE="${VALUE_SIZE:-100}"
ZIPF_THETA="${ZIPF_THETA:-0.99}"
REPLICATION_FACTOR="${REPLICATION_FACTOR:-2}"
ACK="${ACK:-2}"
NUM_BROKERS="${NUM_BROKERS:-4}"
CLIENT_NODES_CSV="${CLIENT_NODES_CSV:-c4,c3,c1}"

GIT_COMMIT="$(git rev-parse HEAD)"
GIT_DIRTY_COUNT="$(git status --porcelain | wc -l)"
if [[ "$GIT_DIRTY_COUNT" -ne 0 && "${ALLOW_DIRTY_ARTIFACT:-0}" != "1" ]]; then
    echo "ERROR: refusing to start a campaign from a dirty tree ($GIT_DIRTY_COUNT changed files)." >&2
    echo "       Set ALLOW_DIRTY_ARTIFACT=1 only for a non-publication rehearsal." >&2
    exit 1
fi

CAMPAIGN_ID="${CAMPAIGN_ID:-$(date -u +%Y%m%dT%H%M%SZ)_ycsb}"
CAMPAIGN_ROOT="$PROJECT_ROOT/data/paper_eval/ycsb/$CAMPAIGN_ID"
mkdir -p "$CAMPAIGN_ROOT/trials"

CAMPAIGN_START_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
RESULTS_CSV="$CAMPAIGN_ROOT/results.csv"
ATTEMPT_LOG="$CAMPAIGN_ROOT/attempt_summary.csv"
echo "system,workload,n_clients,trial,attempts_used,verdict,commit,start_ts,end_ts,out_dir" > "$RESULTS_CSV"
echo "system,workload,n_clients,trial,attempt,result,reason,ts" > "$ATTEMPT_LOG"

echo "=== YCSB Matrix Campaign: $CAMPAIGN_ID ==="
echo "  commit=$GIT_COMMIT dirty=$GIT_DIRTY_COUNT"
echo "  systems=[$SYSTEMS] workloads=[$WORKLOADS] n_clients=[$N_CLIENTS_LIST] trials_per_cell=$TRIALS_PER_CELL"
echo "  record_count_total=$RECORD_COUNT_TOTAL operation_count_per_client=$OPERATION_COUNT_PER_CLIENT rf=$REPLICATION_FACTOR ack=$ACK"
echo "  client_nodes=$CLIENT_NODES_CSV num_brokers=$NUM_BROKERS"
echo "  campaign_root=$CAMPAIGN_ROOT"

total_cells=0
total_pass=0
total_fail=0

run_cell() {
    local system="$1" workload="$2" n="$3" trial="$4"
    local cell_tag="${system}_${workload}_n${n}"
    local trial_dir="$CAMPAIGN_ROOT/trials/${cell_tag}/trial${trial}"
    mkdir -p "$trial_dir"

    local attempt=1 verdict="failed" start_ts end_ts rc reason out_dir=""
    start_ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

    while [[ "$attempt" -le "$TRIAL_MAX_ATTEMPTS" ]]; do
        echo ""
        echo ">>> $cell_tag trial=$trial attempt=$attempt/$TRIAL_MAX_ATTEMPTS ($(date -u +%H:%M:%SZ))"
        out_dir="$trial_dir/attempt${attempt}"

        NUM_CLIENTS="$n" SYSTEM="$system" WORKLOAD="$workload" \
        REPLICATION_FACTOR="$REPLICATION_FACTOR" ACK="$ACK" \
        RECORD_COUNT_TOTAL="$RECORD_COUNT_TOTAL" OPERATION_COUNT_PER_CLIENT="$OPERATION_COUNT_PER_CLIENT" \
        VALUE_SIZE="$VALUE_SIZE" ZIPF_THETA="$ZIPF_THETA" NUM_BROKERS="$NUM_BROKERS" \
        CLIENT_NODES_CSV="$CLIENT_NODES_CSV" \
        RUN_TAG="${cell_tag}_t${trial}_a${attempt}" \
        OUT_ROOT="$out_dir" \
        timeout "$CELL_TIMEOUT_SEC" bash PaperScripts/run_ycsb_distributed.sh \
            > "$trial_dir/attempt${attempt}.log" 2>&1
        rc=$?

        if [[ "$rc" -eq 0 ]]; then
            verdict="pass"
            echo "$system,$workload,$n,$trial,$attempt,pass,-,$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$ATTEMPT_LOG"
            echo "  PASS"
            break
        else
            reason="exit_${rc}"
            echo "$system,$workload,$n,$trial,$attempt,failed,$reason,$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$ATTEMPT_LOG"
            echo "  FAILED (exit=$rc) — see $trial_dir/attempt${attempt}.log"
            tail -n 15 "$trial_dir/attempt${attempt}.log" 2>/dev/null | sed 's/^/    /'
            attempt=$((attempt + 1))
        fi
    done

    end_ts="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "$system,$workload,$n,$trial,$((attempt > TRIAL_MAX_ATTEMPTS ? TRIAL_MAX_ATTEMPTS : attempt)),$verdict,$GIT_COMMIT,$start_ts,$end_ts,$out_dir" >> "$RESULTS_CSV"

    total_cells=$((total_cells + 1))
    if [[ "$verdict" == "pass" ]]; then
        total_pass=$((total_pass + 1))
    else
        total_fail=$((total_fail + 1))
        echo "  CELL FAILED after $TRIAL_MAX_ATTEMPTS attempts: $cell_tag trial=$trial" >&2
    fi
}

for system in $SYSTEMS; do
    for workload in $WORKLOADS; do
        for n in $N_CLIENTS_LIST; do
            for trial in $(seq 1 "$TRIALS_PER_CELL"); do
                run_cell "$system" "$workload" "$n" "$trial"
            done
        done
    done
done

CAMPAIGN_END_TS="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

python3 - "$CAMPAIGN_ROOT" "$CAMPAIGN_ID" "$GIT_COMMIT" "$GIT_DIRTY_COUNT" \
    "$CAMPAIGN_START_TS" "$CAMPAIGN_END_TS" "$CLIENT_NODES_CSV" \
    "$RECORD_COUNT_TOTAL" "$OPERATION_COUNT_PER_CLIENT" "$VALUE_SIZE" "$ZIPF_THETA" \
    "$REPLICATION_FACTOR" "$ACK" "$NUM_BROKERS" "$TRIALS_PER_CELL" "$TRIAL_MAX_ATTEMPTS" \
    "$SYSTEMS" "$WORKLOADS" "$N_CLIENTS_LIST" <<'PYEOF'
import csv, json, sys, subprocess

(campaign_root, campaign_id, commit, dirty, start_ts, end_ts, client_nodes_csv,
 record_count_total, op_count, value_size, zipf_theta, rf, ack, num_brokers,
 trials_per_cell, trial_max_attempts, systems, workloads, n_clients_list) = sys.argv[1:20]

client_nodes = client_nodes_csv.split(",")
numa_by_host = {"c4": 1, "c3": 1, "c1": 0}

results_path = f"{campaign_root}/results.csv"
cells = []
with open(results_path, newline="") as f:
    for row in csv.DictReader(f):
        cells.append(row)

def bin_hash(path):
    try:
        out = subprocess.run(["sha256sum", path], capture_output=True, text=True, check=True)
        return out.stdout.split()[0]
    except Exception:
        return None

manifest = {
    "campaign_id": campaign_id,
    "commit": commit,
    "dirty": int(dirty) != 0,
    "start_time": start_ts,
    "end_time": end_ts,
    "host_roster": {
        "broker": "moscxl",
        "clients": [{"host": h, "numa": numa_by_host.get(h)} for h in client_nodes],
    },
    "knobs": {
        "record_count_total": int(record_count_total),
        "operation_count_per_client": int(op_count),
        "value_size": int(value_size),
        "zipf_theta": float(zipf_theta),
        "replication_factor": int(rf),
        "ack": int(ack),
        "num_brokers": int(num_brokers),
        "trials_per_cell": int(trials_per_cell),
        "trial_max_attempts": int(trial_max_attempts),
        "systems": systems.split(),
        "workloads": workloads.split(),
        "n_clients": [int(x) for x in n_clients_list.split()],
        "replication_label": "DRAM replica completion (RF>=2/ACK=2) — never 'durable' without the disk-durable sink",
    },
    "binary_hashes": {
        "kv_ycsb_bench": bin_hash("build/bin/kv_ycsb_bench"),
        "embarlet": bin_hash("build/bin/embarlet"),
        "corfu_global_sequencer": bin_hash("build/bin/corfu_global_sequencer"),
        "scalog_global_sequencer": bin_hash("build/bin/scalog_global_sequencer"),
    },
    "seed_note": "kv_ycsb_bench has no --seed CLI flag; the whole benchmark "
                 "shares one hardcoded std::mt19937_64 rng(42) "
                 "(kv_bench_main.cc:505) — identical for every cell/trial by construction.",
    "cells": cells,
    "summary": {
        "total_cells": len(cells),
        "pass": sum(1 for c in cells if c["verdict"] == "pass"),
        "fail": sum(1 for c in cells if c["verdict"] != "pass"),
    },
}

with open(f"{campaign_root}/campaign_manifest.json", "w") as f:
    json.dump(manifest, f, indent=2)

print(f"Wrote {campaign_root}/campaign_manifest.json")
print(f"Summary: {manifest['summary']}")
PYEOF

echo ""
echo "=== Campaign $CAMPAIGN_ID complete ==="
echo "  total_cells=$total_cells pass=$total_pass fail=$total_fail"
echo "  results: $RESULTS_CSV"
echo "  manifest: $CAMPAIGN_ROOT/campaign_manifest.json"

if [[ "$total_fail" -gt 0 ]]; then
    exit 1
fi
exit 0
