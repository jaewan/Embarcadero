# Task 1 results — T/tau vs T/R (measured; commit 7ff2c402, co-located RF1/ACK1, 4 brokers, 2 sessions)

Raw: data/paper_eval/tr_tau_task1/campaign/tau*_trial*/tr.pid*.t*  (3 trials per tau)
Harness: PaperScripts/run_tr_tau_skew.sh   Analysis: PaperScripts/analyze_tr_tau.py
Summary: data/paper_eval/tr_tau_task1/campaign/tr_tau_summary.csv

## Load-bearing measurements (robust)
- tau (epoch seal interval) tracks EMBAR_ORDER5_EPOCH_US EXACTLY:
  set 250us -> meas P50 250.0 (P95 253, P99 276);  set 500 -> P50 500.0 (P95 502, P99 ~510);
  set 1000 -> P50 1000.0 (P95 1002, P99 1003).  tau IS the seal cadence, tight.
- Scanner OBSERVATION period P (mean, from surviving cumulative counter): 0.49-0.58 us
  across all cells -> P << tau (tau/P ~= 430-900x). Observation is far faster than release.
- Release is SEAL-GATED (code trace + empirical): every gap_release coincides with an
  epoch-index advance; seals_during_gap = hold_duration/tau exactly (median 110 at tau=250
  for 27.5ms holds; 673 at tau=500 for 337ms holds; 700 at tau=1000 for 704ms holds).
  A held suffix is NEVER released between seals.
- Both sessions carry the injected striped traffic and see holds; this campaign
  therefore does not provide a stall-free control session. Cross-session
  isolation is measured by the separate targeted-gap experiment.

## Correction for the paper
- P (scan-pass, observation) and tau (seal, release/commit) are DIFFERENT periods; Sec2:32
  conflates them ("R = poll-and-commit cycle ... seals and commits ... tens of us").
- Release/commit opportunities in a skew window T = seals in T = T/tau, NOT T/P.
  Measured tau=500us => for the paper's P99 skew T~=1.5ms, T/tau ~= 3, NOT ~30.
  The "~30" counted the observation/poll rate, not release opportunities.

## Honest caveat (operating point)
- In this co-located instrumentation campaign, holds are large (27ms-700ms) and grow with tau because at larger tau the
  seal rate drops, the striped hold buffer backs up, and commits slow (throughput-collapse,
  NOT the ~1.5ms network skew of the paper). This does not affect the load-bearing facts
  (tau tracks the knob; release is seal-gated; P<<tau); the T/tau~=3 for T=1.5ms is arithmetic
  from the measured tau. The campaign does not extrapolate a remote hold
  distribution from these values.
