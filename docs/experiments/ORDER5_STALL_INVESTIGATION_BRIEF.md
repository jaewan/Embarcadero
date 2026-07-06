# Handoff brief: root-cause the ORDER=5 non-head-broker ACK stall (for the moscxl-side agent)

**From:** Track 01 (`session/01-core-protocol`). **Date:** 2026-07-05.
**You are:** a coding agent running on **moscxl** (the broker/CXL testbed).
**Goal:** determine the root cause of, and attribute, the ORDER=5 non-head-broker ACK stall; then produce a fair real-wire throughput number from **c3**. **Attribution first — do not assume it is our regression.**

---

## 0. TL;DR of the symptom

Under **ORDER=5, ACK=1, `THREADS_PER_BROKER=4`**, the publisher times out waiting for ACKs: the **head broker B0 ACKs fully, but B1/B2/B3 return 0 ACKs** (`short=<all>`), despite each broker having received its data (`sent=~3.9M` each). Reproduces on **loopback** (singlenode) and **real-wire** (c1→moscxl). ORDER=0 is unaffected. ORDER=5 **single-thread** (`THREADS_PER_BROKER=1`) completes fine.

Representative failure line:
```
[Publisher ACK Per-Broker]: B0=11860979(sent=3932656) B1=0(sent=3932656,short=3932656) B2=0(...) B3=0(...)
```

**This may NOT be our regression.** Evidence it could be pre-existing/environmental:
- Track-01's code changes are broker-side but **do not touch the non-head CV/ACK frontier** (see §3).
- ORDER=5 single-thread completes on the modified build (W1.1 latency run: 4.2M msgs delivered clean).
- `docs/experiments/LATENCY_EXPERIMENT_CHECKLIST.md` documents this exact ORDER=5 4-thread ACK shortfall **at baseline** ("Step 27 burn-in: 8/10 pass, 2/10 fail ... lagging broker tail"; "Step 26 tail-progress hardening").
- The client kernel send buffers were **untuned** in our runs (`net.core.wmem_max` capped → `SO_SNDBUF capped: requested 256MB got ~416KB`), which throttles the ACK-bearing path under 4-thread load.

**So step 1 is a clean A/B: does the UNMODIFIED code also stall?**

---

## 1. Environment facts (save yourself the discovery time)

**Node addressing / reachability (verified 2026-07-05):**
| Node | mgmt (ssh) | dataplane to moscxl | client build present | notes |
|------|-----------|---------------------|---------------------|-------|
| moscxl | — | is the broker | — | dataplane IPs: `10.10.10.10`, `192.168.60.8`, `192.168.60.7`, `192.168.30.22` |
| c1 | yes | **10.10.10.11 → moscxl 10.10.10.10 (works)** | yes (`~/Embarcadero-sessions/01-core-protocol/build/bin`) | weaker CPU |
| c2 | yes | **DOWN** (no 10.10.10.x addr) | yes | unusable |
| c3 (`mos181`) | yes | **192.168.60.181 → moscxl 192.168.60.8 (works)** | **no — deploy needed** | powerful CPU; the standard client |
| c4 | yes | **DOWN** | no | unusable |

- `10.10.10.10` is moscxl's address that also **routes via `lo`** for single-node (per `docs/perf/multinode_throughput_findings.md`); it is NOT `10.10.10.143` (that address does not exist on this moscxl).
- For a **c3 client**, use `EMBARCADERO_HEAD_ADDR=192.168.60.8`. For **c1**, use `10.10.10.10`. (One `BROKER_IP` per run; c1 and c3 are on different subnets so they can't be combined into one multi-client run.)

**Build:**
- moscxl gRPC cache: `-DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero-main-ab/build/_deps/grpc-src`
- c-node gRPC cache: `-DFETCHCONTENT_SOURCE_DIR_GRPC=/home/domin/Embarcadero/build/_deps/grpc-src`
- Track-01's build used `-DCOLLECT_LATENCY_STATS=ON`. **For a fair A/B, build the baseline with the SAME flag** (it adds latency instrumentation that can cost throughput).
- Build clients with only `--target throughput_test` to save time.

**Kernel buffers (LIKELY RELEVANT):** `net.core.wmem_max`/`rmem_max` were **212992 (208 KB)** on moscxl and default on c-nodes. Track 01 raised moscxl's to 256 MB via `sudo -n sysctl` (passwordless sudo works). **Raise them on the CLIENT node(s) too** (`scripts/tune_kernel_buffers.sh`, or `sudo sysctl -w net.core.wmem_max=268435456 net.core.rmem_max=268435456`) before benchmarking — the `SO_SNDBUF capped` warnings indicate the client couldn't get large send buffers, which stresses the ACK path.

**Config parity:** everything uses `config/embarcadero.yaml` (batch_size=2MB, batch_headers_size=10MB, io_threads=6, `cxl.numa_node:2` = real CXL). Same file for baseline and modified.

---

## 2. Testbed discipline (learned the hard way)

- **Serialize exclusive runs with the lock:** `flock -w 2400 ~/Embarcadero-sessions/testbed.lock -c "..."`. Log `START`/`END` to `~/Embarcadero-sessions/activity.log`. Compile OUTSIDE the lock.
- **Cleanup pitfall:** do **NOT** `pkill -f embarlet` — your own ssh command line often contains "embarlet", so it self-matches and kills your shell (ssh exit 255) while leaving brokers alive. **Kill by explicit PID** (`pgrep -x embarlet` then `kill -9 <pids>`), or kill the flock holder's **process group** (`kill -9 -<pgid>`). `fuser -k` on the shared lock is blocked (could kill another session).
- run_multiclient's broker-start **retry loop respawns embarlet** — kill the run_multiclient/flock parent FIRST, then stray brokers.
- **Never touch** `~/Embarcadero` (human's live tree/data) or other sessions' dirs. Stay in `~/Embarcadero-sessions/01-core-protocol/`.
- After each run, verify `LOCK_FREE` + `pgrep -x embarlet` empty + c-node clients cleared.

---

## 3. What Track 01 changed (so you can reason about / bisect)

Files: `src/embarlet/topic.cc`, `src/embarlet/topic.h` only. Full patch: `docs/experiments/track01_topic_changes.diff` (my hunks are tagged `[E1]/[E2]/[E3]/[[W1.2]]`; the untagged hunks — include-path `../` removals and `hold_export_queues_ push_back→q[pbr]=ex` — are the human's pre-existing reorg edits, keep those).

| Tag | What | Could it plausibly cause non-head ACK stall? |
|-----|------|----------------------------------------------|
| C1 | `committed_this_epoch` `std::array<size_t,8>`→`NUM_MAX_BROKERS` | No (fixes OOB; feeds force-expiry counter) — but **worth a look**: it changes values fed to `sequencer_committed_batches_[b]` which gates idle force-expiry; verify the fix didn't alter force-expiry timing at 4 brokers. |
| C3 | reorder `he.batch = std::move` after meta capture (4 sites) | No (behavior-identical for POD). |
| C4 | export descriptor `publish_commit` source | No (export readiness uses `ordered`). |
| **E1** | **guard `hold_export_queues_[b]` inserts with `if (b==broker_id_)`** | **PRIME SUSPECT to rule out** — it changes the export-descriptor bookkeeping for non-head brokers. Verified: non-head brokers export via the CXL ring (written unconditionally, untouched) and `hold_export_queues_[b!=broker_id_]` is process-local DRAM never read by other processes. But since the symptom is non-head-specific, **A/B this one specifically.** |
| **E2** | compute ORDER=5 batch size once, reuse | Low, but it feeds export size for non-head ring descriptors — verify sizes still correct. |
| **W1.2** | per-session commit-order assert (B0/sequencer only) + `commit_order_last_seq_` map insert per committed batch | Runs only on B0 (which ACKs fine), but it's **new hot-path work in `CommitEpoch`** — if it slows the sequencer, non-head CV advancement (also done in CommitEpoch) could lag. **Measure CommitEpoch cost with/without** (`EMBAR_ASSERT_COMMIT_ORDER` only gates the CHECK, not the map — the map runs always; consider a quick build that #ifdef's it out for the A/B). |

**Non-head ACK1 path (for your mental model):** non-head broker B's ORDER=5 ACK1 frontier = `CV[B].sequencer_logical_offset`, advanced by B0's sequencer in `CommitEpoch` via `AccumulateCVUpdate`/`FlushAccumulatedCV`. None of the Track-01 edits modify `AccumulateCVUpdate`/`FlushAccumulatedCV`. If B1/B2/B3 CV never advances, either (a) B0's sequencer isn't committing their batches, (b) CV flush isn't happening, or (c) the client isn't reading/attributing their ACKs. Instrument `EMBAR_FRONTIER_TRACE=1` (exists) and the `[SEQ5_IDLE_DIAG]` logs.

---

## 4. Investigation plan (attribution-first)

**Phase A — Is it our regression? (clean A/B)**
1. Build **baseline** in a separate tree on moscxl: take `~/Embarcadero-sessions/01-core-protocol`, reverse-apply *only* the `[E1]/[E2]/[E3]/[[W1.2]]/C1/C3/C4`-tagged hunks from `track01_topic_changes.diff` (keep the reorg hunks), build with `-DCOLLECT_LATENCY_STATS=ON`. (Alternative baseline: the human's `~/Embarcadero` at `chore/repo-reorg` — but note it lacks the reorg include-path + `hold_export_queues_` btree edits; call that out if you use it.)
2. Run the **same** config on both (baseline vs modified), 3+ trials each, with **client kernel buffers tuned**:
   - Repro (loopback, fastest): `NUM_BROKERS=4 TEST_TYPE=5 ORDER=5 ACK=1 TOTAL_MESSAGE_SIZE=10737418240 MESSAGE_SIZE=1024 THREADS_PER_BROKER=4 SEQUENCER=EMBARCADERO bash scripts/singlenode_run_throughput.sh`
   - Watch per-broker ACKs in `build/bin/broker_*.log` / publisher output.
3. **Verdict:**
   - Baseline **also** stalls B1/B2/B3 → **pre-existing / environmental** (not our regression). Then characterize (kernel buffers? tail-stall?) and fix if cheap.
   - Baseline **passes**, modified **stalls** → **our regression**. Bisect by re-adding hunks one at a time (E1 first, then W1.2, then E2) until it breaks.

**Phase B — root cause + fix** (once attributed). If environmental: quantify the kernel-buffer effect (tuned vs untuned) and the documented tail-stall interaction. If ours: identify the exact hunk + minimal correct fix, re-verify W1.2 (0 commit-order violations) and throughput.

**Phase C — fair real-wire number from c3.**
1. Tune kernel buffers on c3. Deploy Track-01 source to `c3:~/Embarcadero-sessions/01-core-protocol/` (rsync from moscxl, exclude build/data/.git), build `throughput_test` (c-node gRPC cache).
2. Run (broker = moscxl modified build), under flock, log activity.log:
   ```
   CLIENT_HOSTS_CSV=c3 NUM_CLIENTS=1 NUM_TRIALS=3 \
   REMOTE_CLIENT_BIN_DIR=/home/domin/Embarcadero-sessions/01-core-protocol/build/bin \
   EMBARCADERO_HEAD_ADDR=192.168.60.8 NUM_BROKERS=4 TEST_TYPE=5 ACK=1 MESSAGE_SIZE=1024 \
   THREADS_PER_BROKER=4 TOTAL_MESSAGE_SIZE=16106127360 SEQUENCER=EMBARCADERO REPLICATION_FACTOR=0 \
   ORDER=<0|5> bash scripts/run_multiclient.sh
   ```
3. Compare O0 to the documented single-client baseline **~6.7 GB/s** (that was c4/Sapphire-Rapids; c3 may differ). O5 only meaningful once Phase A/B resolves the stall.

---

## 5. Artifacts already produced (on moscxl, in the session tree / /tmp)
- Track-01 findings: `docs/experiments/w1_findings.md`, `docs/experiments/code_review_w1.md`.
- My change patch: `docs/experiments/track01_topic_changes.diff`.
- Prior run logs: `/tmp/01_thru_o0.log` (loopback O0 12.46 GB/s), `/tmp/01_thru_o5.log` (O5 12.28 on retry), `/tmp/01_sc_o0.log` (c1 real-wire O0 ~4.8), `/tmp/01_sc_o5.log` (c1 O5 stall), `/tmp/01_w12.log` (W1.2 stress), `/tmp/01_mc_o0.log` (c1+c2 attempt — c2 unreachable).
- W1.2 confirmed: 0 `order5_commit_order_violations` across all brokers under 4-thread stress with `EMBAR_ASSERT_COMMIT_ORDER=1`.

## 6. Deliverable
A short report: (1) **attribution verdict** (ours vs pre-existing/env, with the A/B evidence); (2) if ours, the culprit hunk + fix; if env, the mechanism (kernel buffers / tail-stall) + mitigation; (3) **c3 real-wire O0/O5 numbers** vs the ~6.7 GB/s baseline; (4) whether the Track-01 fixes hold up (W1.2=0, throughput not regressed). Do not git-commit; leave changes staged and describe them.
