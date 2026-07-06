# results/

Single home for **all generated experiment output** — throughput CSVs, latency traces,
baseline sweeps, plots, and run logs.

**This directory is git-ignored** (except this README). Never commit generated data: it bloated
`.git` to multiple GB in the past (see `docs/operations/history-purge-runbook.md`).

## Convention

Point benchmark/experiment scripts here, e.g.:

```
results/
  throughput/<date>/...
  latency/<date>/...
  ablation/<date>/...
```

If a result must be preserved (e.g. a figure for the paper), copy the specific file into the
paper repo or an external artifact store — not into git history here.
