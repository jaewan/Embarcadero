# Scalog sticky-routing Q3 control

- Date: 2026-07-28
- Host: `moscxl`
- Git commit: `151e0376482d4a6dc2029485e02129dc4c40d7b2`
- Worktree recorded by every trial: clean
- Trials: 3
- Median throughput: 1,129,327 apply ops/s
- Validity: 3/3 valid; each trial published and applied 1,050,000 entries,
  with zero session/key inversions, zero final/untouched-key mismatches, and
  digest `1d3bc69f5cdeb2f0`.

The control runs one client session against one active Scalog log server. It
is a favorable implementation of sticky placement: the other servers incur
no control-plane overhead, but the session cannot stripe across their ingress
paths. It is therefore a correctness/design-space control, not a four-server
throughput comparison.

Command:

```bash
OUT_ROOT=build/results/smr_fifo_scalog_sticky_paper_151e0376_20260728 \
SMR_FIFO_SEQUENCERS=SCALOG \
SMR_FIFO_MODES=sticky \
SMR_FIFO_NUM_TRIALS=3 \
BROKER_READY_TIMEOUT_SEC=300 \
BENCH_TIMEOUT_SCALOG=3600 \
bash benchmarks/kv_store/run_smr_fifo_eval.sh
```

The raw per-trial logs remain in the run directory named above. The committed
`summary.csv` is copied verbatim from its aggregate output.
