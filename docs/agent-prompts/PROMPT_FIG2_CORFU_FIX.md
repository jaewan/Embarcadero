# Agent prompt: Fix Fig2 Corfu RF2 baseline failures

Copy everything below the line into a coding agent on your **personal laptop**.

---

## TL;DR

Fig2 Corfu cells (`fig2_corfu_o2_ack2_rf2_mem`) all fail before any latency is measured. Root cause is almost certainly **missing `EMBARCADERO_CORFU_REPLICA_ENDPOINTS`** on the latency/Fig2 path (multiclient auto-builds it; Fig2/`run_latency.sh` do not). Fix the harness so Corfu RF=2 memory-copy topic creation succeeds, then run a minimal Corfu latency smoke + optionally refill Fig2 Corfu points. **No git / no commits.** Work on the cluster via `ssh broker` and ship files with `rsync`/`scp`.

## Environment (your laptop)

| Fact | Detail |
|------|--------|
| Cluster access | `ssh broker` → moscxl (Embarcadero head / CXL node) |
| Repo on cluster | typically `~/Embarcadero` or `/home/domin/Embarcadero` — discover with `ssh broker 'ls ~/Embarcadero'` |
| Client node | Fig2 publisher is **`c4`** (remote `throughput_test`) |
| Git | **Unavailable / do not use.** No `git commit`, `git push`, `git checkout`, etc. |
| File sync | `rsync` / `scp` laptop ↔ `broker:` (and `broker` → `c4` if you rebuild client bits; Corfu fix is likely **scripts-only**) |
| Parallelism | **Never** run a second Fig2 / overnight / multiclient campaign while another owns `/tmp/embarcadero_paper_fig2.lock` or while `embarlet`/`throughput_test` are live unless you are sure the cluster is idle |

### Suggested workflow

1. On laptop: edit locally (or edit over SSH). Prefer a local clone or a working tree synced from broker.
2. `rsync` changed scripts to `broker:~/Embarcadero/...`
3. Run validation **on broker** via `ssh broker '...'` (or an interactive SSH session).
4. If you change C++ (unlikely for this bug): build on broker, `scp`/`rsync` binaries as needed; do **not** assume laptop can compile the CXL stack.
5. Leave a short `NOTES.md` or log under `data/paper_eval/fig2/...` on the cluster describing what you changed and smoke results. Do not commit.

## Mission

Make **Corfu ORDER=2 ACK=2 RF=2 memory-copy** work under the **Fig2 / `run_latency.sh` path**, matching the membership setup that `scripts/run_multiclient.sh` already does for Corfu RF>1.

Success = at least one Corfu cell completes with:
- topic create OK (no FATAL in `broker_0.log`)
- `pub_ack_p50_us` / `pub_ack_p99_us` present in trial results
- status `ok` in Fig2 `results.csv` (or equivalent latency `trial_results.csv`)

## Non-goals

- Do not redesign Corfu token / WriteOnce semantics.
- Do not change Embar ORDER=5 path, Scalog, or disk-durable Corfu unless required for the mem RF2 fix.
- Do not start subscriber-delivery optimization work.
- Do not kill an in-progress Fig2/overnight run; wait for idle + lock free.
- Do not use git.

## Known diagnosis (verify, then fix)

Failed cells (both passes):

- `data/paper_eval/fig2/fig2_append_latency/logs/*/fig2_corfu_o2_ack2_rf2_mem_l*.log`

Client symptom:

```text
Failed to create topic: Socket closed
Benchmark setup failed: topic creation did not succeed.
```

True root cause in `broker_0.log` (trial brokers under the cell’s `raw/.../brokers/`):

```text
E... topic.cc:56] RF>1 Corfu requires EMBARCADERO_CORFU_REPLICA_ENDPOINTS as broker_id@endpoint entries for every participating broker
F... topic.cc:667] invalid Corfu ordered chain configuration
```

Brokers then abort → gRPC `Socket closed` on CreateTopic. **Corfu never reached append.**

### Why Fig2 hits this

1. `PaperScripts/run_fig2_latency_vs_load.sh` → `clear_rf2_ambient` / `apply_rf2_mem_env` **unsets** `EMBARCADERO_CORFU_REPLICA_ENDPOINTS`.
2. Fig2 launches Corfu with RF=2 mem but only passes `EMBARCADERO_CORFU_SEQ_IP` — **no replica membership rebuild**.
3. `scripts/run_latency.sh` starts `corfu_global_sequencer` and brokers but **never sets** `EMBARCADERO_CORFU_REPLICA_ENDPOINTS` (grep is empty today).
4. `scripts/run_multiclient.sh` **does** auto-build when unset:

```bash
# pattern to mirror (ports match CorfuReplicationManager bind):
for (( i=0; i<NUM_BROKERS; ++i )); do
  endpoints+="${endpoints:+,}${i}@${BROKER_IP}:$((50053 + i))"
done
# example N=4, BROKER_IP=10.10.10.10:
# 0@10.10.10.10:50053,1@10.10.10.10:50054,2@10.10.10.10:50055,3@10.10.10.10:50056
```

Code that FATAL-fails without membership:

- `src/embarlet/topic.cc` — `ResolveCorfuChainTargets()` (~L43+)
- Topic ctor FATAL: `invalid Corfu ordered chain configuration` (~L667)

## Required fix (preferred)

**Harness-only** (prefer no C++ change):

1. **`scripts/run_latency.sh`** (and/or shared helper used by it): when `SEQUENCER=CORFU` and `REPLICATION_FACTOR>1`, if `EMBARCADERO_CORFU_REPLICA_ENDPOINTS` is empty, auto-build the multiclient-equivalent map from `NUM_BROKERS` + broker listen IP (`BROKER_LISTEN_ADDR` / `EMBARCADERO_HEAD_ADDR` / `10.10.10.10`). Export it into **broker** env (same place other `EMBARCADERO_*` are passed to `embarlet`).
2. **`PaperScripts/run_fig2_latency_vs_load.sh`**: after `apply_rf2_mem_env` for Corfu cells (or inside the Corfu `run_series_loads` extra env), ensure endpoints are set **after** the unset in `clear_rf2_ambient`. Either:
   - pass explicit `EMBARCADERO_CORFU_REPLICA_ENDPOINTS=...` in the Corfu series invocation, or
   - rely on `run_latency.sh` auto-build (better single source of truth).
3. Confirm endpoints reach remote brokers if any broker is started via SSH wrappers (`scripts/lib/broker_lifecycle.sh` already forwards `EMBARCADERO_CORFU_REPLICA_ENDPOINTS` when set — keep that path working).
4. Optional hardening: if RF>1 Corfu and endpoints still missing after auto-build, **fail fast** in the script with a clear error instead of letting `embarlet` FATAL.

Do **not** weaken `topic.cc` to allow empty membership for RF>1 — that check is correct.

## Cluster idle / lock rules

Before any broker launch:

```bash
ssh broker 'pgrep -af "run_fig2_latency|run_latency_vs_load|run_multiclient|run_overnight|embarlet|throughput_test" || true'
ssh broker 'fuser /tmp/embarcadero_paper_fig2.lock 2>/dev/null; ls -l /tmp/embarcadero_paper_fig2.lock'
```

If Fig2 lock is held or brokers are running for another campaign: **wait**. Do not parallelize.

## Validation plan

### A. Smoke (mandatory)

On broker, after syncing scripts, single Corfu latency point (adjust paths if home differs):

```bash
ssh broker 'bash -lc '"'"'
set -euo pipefail
cd ~/Embarcadero   # or absolute path
# ensure cluster idle first

export EMBARCADERO_CHAIN_REPLICATION_SINK=memory-copy
export EMBARCADERO_CHAIN_REPLICATION_INMEM=1
export EMBARCADERO_CHAIN_REPLICATION_INMEM_COPY=1
unset EMBARCADERO_REPLICA_DISK_DIRS
export EMBARCADERO_CORFU_SEQ_IP=10.10.10.10
# Endpoints may be auto-built by your fix; if testing manually:
# export EMBARCADERO_CORFU_REPLICA_ENDPOINTS="0@10.10.10.10:50053,1@10.10.10.10:50054,2@10.10.10.10:50055,3@10.10.10.10:50056"

NUM_TRIALS=1 NUM_BROKERS=4 REPLICATION_FACTOR=2 ACK_LEVEL=2 \
ORDER=2 SEQUENCER=CORFU SCENARIO=remote REMOTE_CLIENT_HOST=c4 \
EMBARCADERO_HEAD_ADDR=10.10.10.10 BROKER_LISTEN_ADDR=10.10.10.10 \
MSG_SIZE=1024 TOTAL_MESSAGE_SIZE=$((1*1024*1024*1024)) \
TARGET_MBPS=100 PACING_MODE=steady \
EMBARCADERO_CXL_ZERO_MODE=metadata EMBARCADERO_CXL_SIZE=77309411328 \
EMBARCADERO_RUNTIME_MODE=latency \
EMBARCADERO_LATENCY_ACK_PRIMARY=1 \
OUT_BASE=/tmp/corfu_fig2_smoke \
bash scripts/run_latency.sh
'"'"''
```

Pass criteria:

- No `invalid Corfu ordered chain configuration` in broker logs
- Client exit 0 (or ACK-primary soft path with pub ACK present)
- `pub_latency_stats.csv` / trial summary contains append→ack percentiles

### B. Fig2 Corfu gap-fill (if smoke passes and cluster still idle)

```bash
ssh broker 'bash -lc '"'"'
cd ~/Embarcadero
# Only Corfu cells; skip already-ok Embar/Scalog
ONLY_CELLS=fig2_corfu_o2_ack2_rf2_mem \
TARGET_TRIALS=1 NUM_TRIALS=1 \
CAMPAIGN_ID=fig2_append_latency \
PASS_ID=$(date -u +%Y%m%dT%H%M%SZ)_corfu_fix \
bash PaperScripts/run_fig2_latency_vs_load.sh
'"'"''
```

(If `ONLY_CELLS` is not implemented, use whatever Fig2 filter exists, or run the Corfu `run_series_loads` block manually. Check `should_run_cell` / env filters in `run_fig2_latency_vs_load.sh`.)

Expect `ok` rows for loads `100 250 500 1000 2000` (or campaign default `BASELINE_LOAD_MBPS`).

### C. Sanity vs Embar/Scalog

After Corfu works, briefly compare ACK p50 at 100 and 1000 MB/s to existing Embar/Scalog rows in:

`data/paper_eval/fig2/fig2_append_latency/results.csv`

Do not reinterpret remaining large gaps as bugs without evidence; Corfu token-before-write is expected to be slower than Embar ORDER=5.

## Code map

| Path | Role |
|------|------|
| `PaperScripts/run_fig2_latency_vs_load.sh` | Unsets endpoints; launches Corfu series |
| `scripts/run_latency.sh` | Starts local `corfu_global_sequencer`, brokers, remote client — **needs auto-build** |
| `scripts/run_multiclient.sh` ~1383–1391 | Reference auto-build of endpoints |
| `scripts/lib/broker_lifecycle.sh` | Forwards `EMBARCADERO_CORFU_REPLICA_ENDPOINTS` when set |
| `src/embarlet/topic.cc` | `ResolveCorfuChainTargets`; FATAL if missing |
| `src/disk_manager/corfu_replication_manager.cc` | Replica gRPC listen `50053+broker_id`; memory-copy sink |

## Failure taxonomy

| Symptom | Likely cause | Action |
|---------|--------------|--------|
| `requires EMBARCADERO_CORFU_REPLICA_ENDPOINTS` | Fix not applied / not exported to embarlet | Fix env plumbing |
| Endpoints set but wrong ports | Port base ≠ `50053+id` | Match multiclient |
| Topic OK then ACK hang/timeout | Different bug (replica path / sequencer) | Capture broker+client logs; do not claim Fig2 fixed |
| `topic creation` fails with other message | Sequencer down / proxy | Check `/tmp/corfu_sequencer.log`, proxy `50100+` |
| Cluster busy / lock held | Another campaign | Wait; never force |

## Deliverables (on cluster, not git)

1. Patched scripts under `~/Embarcadero` (rsync’d from laptop if edited locally).
2. Smoke log path + 3–6 line summary of root cause → fix → ACK p50 at one load.
3. If gap-fill run: note new `pass_id` and which Corfu loads are `ok` in `results.csv`.
4. Explicit list of files touched.

## Stop conditions

- Smoke passes and at least one Fig2 Corfu load is `ok` → done.
- If after correct endpoints Corfu still FATAL/hangs: stop, paste broker+client log excerpts, and propose next hypothesis (do not thrash Embar/Scalog configs).
