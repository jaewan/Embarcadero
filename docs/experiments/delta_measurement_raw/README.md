# δ-measurement raw audit data

Small CSV/metadata subset of the open-load append→ack sweep behind
`../delta_measurement_results.md` and the D1 v2 §5.2 δ constants
(`kDeltaFloorMs=1.7`, `kDeltaCapMs=12`).

**Source:** moscxl `~/Embarcadero-sessions/s2-work` run
`run_20260707T024651Z` (branch `delta-measure`, base `s2` @ 9901b93).
The full run dir was ~2.7 GB (broker/run logs); only the auditable
artifacts are archived here (~200 KB): per-trial `pub_latency_stats.csv`,
`pub_cdf_latency_us.csv`, `stage_latency_summary.csv`, `run_metadata.txt`,
plus per-point `summary.csv` / `trial_results.csv` and the top-level
`benchmark_contract.md`.

**Layout:** `run_.../points/00N_<load>mbps/raw/burst/target_<load>mbps/<cfg>_trial{1,2,3}/`.

**The 4.951 ms anchor is auditable here.** In
`points/003_4000mbps/.../trial1/pub_latency_stats.csv`, the
`append_send_to_ack_batch_latency` row shows p99.9 = max = **4951 µs** —
i.e. the per-trial p99.9 IS the single largest sample (n=545, nearest-rank
`latency_stats.h:46`). The other two 4000-point trials give 755 and 1143 µs
(median across trials = 1143). This single-sample fragility is exactly why
the adversarial review raised the cap 10→12 ms for honest ≥2× margin.

**Follow-up (D2-phase, non-blocking):** re-run the 4000-point in isolation,
add a `SCENARIO=remote` real-wire point and a multi-client point.
