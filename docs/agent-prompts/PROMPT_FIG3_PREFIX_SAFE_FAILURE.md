# Agent prompt: Fix Fig3 prefix-safe failure panel to match paper claim

Copy everything below the line into a coding agent on the **cluster** (or laptop with `ssh broker`). This is an investigate → root-cause → fix → commit → sync → re-run loop until Fig3 matches the paper’s expected curves **without weakening Embarcadero’s prefix-safe guarantees**.

---

## TL;DR

Paper Fig3 (`fig:failure_throughput`, Sec 7.3 / `Paper/Text/Sec7_Evaluation.tex`) validates **prefix-safe per-session FIFO under broker kill**: hold gapped sessions (never gap-skip / silent reorder), stall until repair or fence, then resume on survivors — **no global seal**.

Latest campaign (`pass=20260717T024226Z`):
- **Panel (a) `arrival_order` PASS** — looks paper-shaped (survivors keep ACKing).
- **Panel (b) `prefix_safe` FAIL** — ACK drops to ~0 after kill and **never recovers** (~583 s zeros); client exit **124** (timeout); no `failure_events.csv`; fencing/session churn observed; combined PDF is **not** publishable.

Your job: identify **root cause(s)** of the prefix-safe hang / missing recovery spike / missing 3-server resume, fix implementation and/or harness bugs, **preserve correctness** (prefix-safe: never commit past a gap; fence only after unrepaired lease), rebuild+sync to `c4`, re-run Fig3, and iterate until both panels match the paper’s qualitative shape with strong quantitative numbers.

## Why this experiment exists (read first)

### Paper property under test (Q2)

From `Paper/Text/Sec7_Evaluation.tex` §*Failure under the Prefix-Safe Contract* and `Paper/Text/Sec4_Design.tex` `thm:guarantee`:

> Committed batches of a session form a **submission-order prefix**. A batch is either committed at its in-order position or not at all — late/duplicates rejected, **never reordered past a gap**. A gapped session **stalls** (~δ / detection) and is **fenced** if unrepaired. Under full striping a server loss can gap many sessions; each recovers independently **without a global seal**.

Fig3 is the **thesis instrument** for that contract on this CXL testbed — not a generic “does reconnect work?” smoke test, and **not** the E4 stall-CDF suite (`run_failure_suite.sh`).

### Two-panel contrast (what we must show)

| Panel | Mode | What it proves |
|-------|------|----------------|
| **(a)** sensitivity | Holding disabled → `HOLD_MODE=arrival_order` / ORDER=4 | Survivors **never stall on gaps**; aggregate dips only for in-flight loss during detection, then resumes on 3 servers. Isolates that (b)’s zero interval is **the hold**, not CXL/sequencer death. |
| **(b)** contract | Prefix-safe hold → `HOLD_MODE=prefix_safe` / ORDER=5 | In-flight batches to the dead broker leave gaps → survivors’ later batches **enter hold** → sequencer **does not commit past them** → aggregate ACK **falls to ~0 for the detection window** → client **reroutes + retransmits** fill holds → **burst/spike** as held batches release → steady ~**4 GB/s on 3 servers**. Sessions either repair or fence. **No silent reorder.** |

Caption expectations (`failure_combined.pdf`):
- Kill at \(t \approx 1.8\) s
- Shaded band: kill→reroute ≈ **660 ms** (TCP/gRPC detection; CXL lease path is *not* the detector in these traces)
- Spike in (b): held batches released after gaps fill (may clip peak)

### Expected quantitative shape (paper text)

Assume 4 brokers, remote publisher `c4`, 20–21 GiB × 1 KB, kill 1 broker @ ~1.8 s, 100 ms ACK windows:

**(a) holding off**
- Pre-kill ≈ **5.2 GB/s** (≈1.3 GB/s × 4)
- During detect ≈ **5.1 GB/s** (slight dip only)
- Post-reroute ≈ **5.2 GB/s** on 3 servers

**(b) prefix-safe hold**
- Pre-kill ≈ **5.2 GB/s** (striped across 4 — **not** single-broker)
- During detect: aggregate ACK ≈ **0** for ≈ detection window
- After reroute: **spike** (held release), then steady ≈ **4.0 GB/s** on 3 servers
- Stall duration ≈ **detection**, not a multi-round global seal
- Correctness: no GOI commit past a missing predecessor for a live session; fence only if gap outlives session lease

### What “strong results” means (acceptance)

A pass is **not** “client exit 0.” All of the following must hold:

1. **Both panels complete** (exit 0) with `real_time_acked_throughput.csv` + `failure_events.csv`.
2. **(a)** survivors continue ACKing through the kill window (aggregate never stuck at 0 for hundreds of ms unless measurement artifact); post-kill steady on 3 brokers in the **~4.5–5.5 GB/s** band (paper ~5.2).
3. **(b)** shows the **three-phase story**: high pre-kill → **near-zero** during detect → **recovery spike** → **~3.5–4.5 GB/s** steady on 3 brokers (paper ~4.0). Zero-stall must be **bounded by detection (~0.5–1.5 s)**, not multi-minute hang.
4. **Pre-kill striping**: (b) must show load across **multiple brokers** before kill (paper: four servers). If CSV shows 100% on broker 0 while paper claims striping, either fix attribution **or** prove striping via another metric — do not ship a single-broker fake.
5. **Correctness invariants** (non-negotiable):
   - No gap-skip past a missing `batch_seq` for an active ORDER=5 session.
   - No silent reorder of a session’s committed prefix.
   - Fence only after unrepaired gap ≥ session lease (or equivalent designed fence path); after fence, resume only under fresh session from HWM.
   - Do **not** “fix” (b) by setting hold timeout to 0, ORDER=4, or idle-force-expire that gap-skips during the detect window.
6. Combined figure regenerated: `data/paper_eval/fig3/fig3_failure/failure_combined.{pdf,png}` with both panels populated; leave a short `NOTES.md` in the pass dir summarizing root cause → fix → numbers.

## Environment

| Fact | Detail |
|------|--------|
| Repo | `/home/domin/Embarcadero` (discover if needed) |
| Head / CXL | `10.10.10.10` (`broker` / moscxl) |
| Publisher | remote **`c4`** → `~/Embarcadero/build/bin/throughput_test` |
| Paper text | `Paper/Text/Sec7_Evaluation.tex` (~L306–340), `Paper/Text/Sec4_Design.tex` (`thm:guarantee`), `PaperScripts/FIG3.md` |
| Drivers | `PaperScripts/run_fig3_failure.sh` → `scripts/run_failures.sh` → `plot_failure_combined.py` |
| Locks | `/tmp/embarcadero_paper_fig3.lock`, never overlap `/tmp/embarcadero_paper_fig2.lock` |
| Git | **Allowed**: commit focused fixes when a root cause is fixed and validated (user requested). No force-push; no amend of others’ commits; no secrets. |

### Cluster hygiene (mandatory before every run)

```bash
pgrep -af 'run_fig3|run_failures|run_fig2|run_latency|embarlet|throughput_test' || true
# Kill orphan embarlets from failed cleanups if idle and no other campaign owns the lock
fuser /tmp/embarcadero_paper_fig3.lock 2>/dev/null; fuser /tmp/embarcadero_paper_fig2.lock 2>/dev/null
```

If Fig2/Fig3 lock held or another campaign is live: **wait**. Do not parallelize.

## Known failure evidence (start here — verify, then dig)

Pass dir: `data/paper_eval/fig3/fig3_failure/runs/20260717T024226Z/`

| Observation | Path / detail |
|-------------|----------------|
| (a) PASS | `arrival_order/` — kill @1800 ms; reconnects ~1916–1921; pre ~4.95 GB/s balanced; post ~4.8 GB/s on brokers 0/2/3 |
| (b) FAIL rc=124 | `prefix_safe/client_failure_trial1.log` + `logs/.../prefix_safe.log` |
| (b) throughput | Pre ~4.9 GB/s but **CSV attributes ~100% to Broker_0**; after kill → 0 forever (~583 s) |
| (b) events | **Missing** `failure_events.csv` (collect/timeout race) |
| Kill still fired | Wall-clock kill @1800 ms; broker 1 killed; header send fails; some reconnects; later `SessionOpen failed` / `Reconnect Fail Broker 3`; ~3 min later `SESSION_FENCED_OBSERVED` then new session_epoch |
| Harness knobs | `EMBARCADERO_SESSION_LEASE_MS=180000`, `EMBARCADERO_ORDER5_IDLE_FORCE_EXPIRE_MS=180000` (intentional: avoid idle gap-skip during TCP detect) |
| Client timeout | `CLIENT_TIMEOUT=600` in Fig3 wrapper — hung until kill |

**Do not assume a single bug.** Treat as a diagnosis tree (hypotheses below). Prefer evidence from broker logs + client log + GOI/session counters over guessing.

## Investigation protocol (SOTA loop)

Use this structure every iteration. Do not skip “restate claim.”

### Phase 0 — Restate + idle check
1. Quote the paper’s expected (a)/(b) shapes in 5 lines in your working notes.
2. Confirm cluster idle + locks free; clean orphan `embarlet` if leftover from `20260717T024226Z`.

### Phase 1 — Reproduce cheaply
Prefer a **short** prefix_safe smoke before full 20 GiB:

```bash
HOLD_MODE=prefix_safe SCENARIO=remote \
  TOTAL_MESSAGE_SIZE=$((2*1024*1024*1024)) \
  FAILURE_AFTER_MS=1000 CLIENT_TIMEOUT=180 \
  FAILURE_DATA_DIR=data/paper_eval/fig3/debug/prefix_safe_smoke \
  bash scripts/run_failures.sh
```

Collect: client log, all `broker_*.log`, throughput CSV, failure_events, `panel_metadata.txt`.

Also one arrival_order smoke of the same size to keep the contrast honest.

### Phase 2 — Differential diagnosis

Work the tree **top-down**. Log which hypothesis is confirmed/rejected with evidence.

| ID | Hypothesis | How to confirm | If true, fix direction |
|----|------------|----------------|------------------------|
| H1 | Client reconnect/rendezvous broken under ORDER=5 (dead broker excluded, HRW, unacked resubmit) | After kill, no successful SessionOpen on survivors for gapped sessions; unacked buffer not draining | Fix publisher reconnect / `RendezvousBroker` / resubmit path (`publisher.cc`, D1 design) |
| H2 | Sequencer holds forever / never releases after retransmit (classify, next_expected, R1 park) | Retransmits arrive in broker logs but hold buffer never drains; no GOI advance | Fix Order5 classify/release / reconstruct-before-accept |
| H3 | Premature fence or epoch fencing stalls ACK forever | `SESSION_FENCED` before repair; epoch mismatch; ACK withhold without terminal bound | Align fence with lease; fix ACK epoch guard / HWM resume |
| H4 | Idle-force / hold-timeout mis-knobs for Fig3 | Env not exported to remote brokers/SSH; or opposite — expire too aggressive | Ensure lease/hold env reach **embarlet** and client; do not gap-skip |
| H5 | Throughput attribution bug (looks like broker0-only) | Wire taps / per-broker send counters show stripe but CSV is 0 elsewhere | Fix failure real-time accounting; don’t confuse with protocol bug |
| H6 | Artifact collect / timeout: trial actually recovered but harness killed it | Throughput resumes in CSV late; events missing because scp after kill | Fix `CLIENT_TIMEOUT`, remote env for events path, cleanup order |
| H7 | Wrong kill target / sticky routing so “kill broker 1” doesn’t create the intended gaps | Pre-kill traffic not striped; kill broker with no in-flight | Fix striping for ORDER=5 failure test; kill a broker that actually carries in-flight batches |
| H8 | Harness-only: remote missing `EMBARCADERO_FAILURE_AFTER_MS` / lease env / binary skew | md5 mismatch head vs c4; env not in SSH | Rebuild+sync; forward all Fig3 env vars like Fig2 |

Read before editing (minimum):
- `Paper/Text/Sec7_Evaluation.tex` (failure subsection)
- `PaperScripts/FIG3.md`, `scripts/run_failures.sh`, `PaperScripts/run_fig3_failure.sh`
- `src/client/publisher.cc` (failure kill, reconnect, unacked, SessionOpen)
- Order5 hold / fence paths in `src/embarlet/topic.cc` (session lease, idle force expire, hold release)
- D1 notes: `docs/design/D1_SESSION_FIFO_DESIGN_v2.md` §§1–6 (guarantee, fence, R1, rendezvous)
- Prior stall work: `docs/experiments/ORDER5_STALL_INVESTIGATION_BRIEF.md`, `order5_stall_investigation.md` (use as clues, not gospel)

### Phase 3 — Fix with correctness bar

Allowed:
- Real bugs in reconnect, hold release, fencing, ACK withhold terminal bound, harness env/sync/timeouts, throughput accounting, artifact collection.
- Tuning **detection/lease/δ** only if consistent with paper (stall ≈ detection; lease ≫ detect so repair wins; memory-speed lease is future work — do not claim CXL lease detector unless you implement it).

Forbidden “cheats” (reject your own PR if you do these):
- Disabling hold / ORDER=5 → ORDER=4 for panel (b)
- Setting hold/idle expire ≤ detection window so gaps are skipped during the kill band
- Lowering session lease so fence fires instead of repair **as the success path**
- Changing the plotter to hide zeros / fabricate a spike
- Killing fewer sessions by pinning all traffic to survivors before kill

### Phase 4 — Build, sync, commit

1. Build on head: `embarlet`, `throughput_test` (whatever your fix touches).
2. Sync **same** `throughput_test` (+ libs if needed) to `c4`; verify md5 match.
3. Sync scripts if harness changed.
4. **Commit** when a coherent fix is in (user requested). Style: concise why-focused message, e.g. `fix order5 reconnect so Fig3 prefix-safe recovers after broker kill`. Group related files; do not commit CSV blobs / anomaly dumps / secrets.
5. If commit hooks fail: fix and make a **new** commit (do not amend unless the usual amend safety rules all hold).

### Phase 5 — Validate (iterate)

**A. Smoke (mandatory after each fix)**  
Short prefix_safe + arrival_order as in Phase 1. Pass criteria: (b) recovers within ~2× detection after kill; spike or clear step-up; exit 0; events CSV present.

**B. Full Fig3**  
```bash
# after idle check; preferably FIG3_SKIP_BUILD=1 only if md5s already synced
bash PaperScripts/run_fig3_failure.sh
```
Or contract-only while debugging: `SKIP_NOHOLD=1 ...` then full two-panel before declaring done.

**C. Score against acceptance**  
Fill a table in `data/paper_eval/fig3/fig3_failure/runs/<PASS>/NOTES.md`:

```text
pass_id:
root_cause:
fix_summary:
(a) pre_gbps / detect / post_gbps:
(b) pre_gbps / zero_stall_ms / spike_peak / post_gbps:
striping_ok: yes/no
correctness_checks: (gap-skip? fence-only-after-lease? events?)
vs_paper: match / partial / fail
files_touched:
commit: <sha>
```

If (b) still hangs: **do not** declare victory on (a) alone. Open the next hypothesis with new log evidence.

### Phase 6 — Stop conditions

**Done when** acceptance criteria (1)–(6) all pass on a fresh full two-panel run, NOTES.md written, figure regenerated, orphan brokers cleaned.

**Stop and report** (do not thrash) if after 2–3 well-evidenced fix iterations you hit an architectural gap requiring design change beyond D1 (e.g. missing durable SessionEntry). Deliver: confirmed hypotheses, failing invariant, minimal repro, proposed design fix — still commit any partial harness hardenings that are clearly correct.

## Code / harness map

| Path | Role |
|------|------|
| `Paper/Text/Sec7_Evaluation.tex` | Ground-truth expected figure story |
| `PaperScripts/FIG3.md` / `run_fig3_failure.sh` | Campaign wrapper, locks, timeouts |
| `scripts/run_failures.sh` | HOLD_MODE→ORDER/lease; remote SSH env; kill; collect |
| `scripts/publication/plot_failure_combined.py` | Two-panel figure |
| `src/client/publisher.cc` | `EMBARCADERO_FAILURE_AFTER_MS`, reconnect, unacked, events |
| `src/client/test_utils.cc` | Failure trial orchestration, CSV export |
| `src/embarlet/topic.cc` | Order5 hold, lease, idle force expire, fence |
| `docs/design/D1_SESSION_FIFO_DESIGN_v2.md` | Spec for correct repair/fence |

## Failure taxonomy (quick)

| Symptom | Likely | Action |
|---------|--------|--------|
| Exit 124, zeros zeros forever | H1/H2/H3 hang | Diff logs around kill+1s and kill+10s |
| `SESSION_FENCED` before reroute completes | H3 / lease too short / false fence | Trace fence predicate |
| Recovered late then killed by timeout | H6 | Raise timeout **after** proving recovery bound; fix events collect |
| Broker0-only pre-kill CSV | H5/H7 | Compare send vs ACK accounting |
| (a) good (b) bad | Protocol/hold path, not CXL fabric | Focus Order5 + client repair |
| Both bad | Cluster/harness/binary skew | Sync md5; clean orphans |

## Deliverables

1. Root-cause writeup in pass `NOTES.md` (evidence-linked).
2. Code/harness fixes committed on the working branch.
3. Head+c4 binaries synced (md5 noted).
4. Passing Fig3 pass_id under `data/paper_eval/fig3/fig3_failure/runs/<id>/` with both panels + `failure_combined.pdf`.
5. Explicit confirmation that prefix-safe guarantees were **not** weakened to obtain the curve.

## Non-goals

- Do not start Fig2, subscriber-delivery opt, or E4 stall-CDF work.
- Do not implement CXL lease detector unless required and scoped — paper already says traces use TCP/gRPC detection.
- Do not “optimize” (a) by breaking (b), or vice versa.
- Do not rewrite the paper numbers until the implementation matches the contract; if true MTTR differs slightly (e.g. 4.8 vs 5.2 GB/s) but shape+invariants hold, document deltas in NOTES — do not fake the CSV.
