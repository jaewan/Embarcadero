# Paper and Evaluation Hardening Plan

This plan turns every evaluation claim into one of three things: a result backed
by a versioned artifact, a deliberately scoped interpretation, or an explicitly
stated limitation. Work proceeds in priority order so that later experiments do
not paper over defects in analysis or exposition.

## P0 — Make the paper agree with the evidence

- Correct the testbed description to match the actual NUMA-bound process setup.
- Use one throughput metric within each ablation comparison.
- Correct the failure timeline and separate observed ACK recovery from
  end-to-end exactly-once application semantics.
- Scope latency conclusions to the tested load range and remove unsupported
  causal and novelty claims.
- State Q3 as a semantic-contract test: FIFO can be recovered by serialization,
  but Embarcadero provides it without a client-side ordering wait.
- Remove quantitative microbenchmark claims that lack a traceable artifact.

Exit criterion: every number and causal statement in the evaluation can be
mapped to a result file and is no stronger than that result.

## P1 — Make analysis reproducible

- Add validators/summarizers for the throughput path ablation, failure timeline,
  session-isolation experiment, and baseline latency results.
- Require complete trials, matching commits/configurations, and the metric named
  by the paper before producing paper-facing summaries.
- Generate machine-readable manifests containing inputs, hashes, configuration,
  aggregation rules, and outputs.

Exit criterion: figures and tables can be regenerated from versioned raw or
canonical data without manual transcription.

## P2 — Close the remaining empirical gaps

- Add an apply-side failure audit if the paper retains a hardware exactly-once
  claim; otherwise keep that property as model-checked and report only the
  observed ACK/fence behavior in hardware.
- Re-run cells only when validation finds an unstable or incomparable result.
- Treat LazyLog slow-replica measurements as optional unless the main text makes
  an empirical cross-system claim about LazyLog.

Exit criterion: all headline claims have either direct hardware evidence or
clearly identified formal/architectural support.

## P3 — Publication and artifact hygiene

- Regenerate all affected figures and tables, rebuild the PDF, and check page
  count, references, captions, and consistency across abstract/introduction,
  evaluation, and limitations.
- Curate canonical data and manifests under `data/paper_eval`; keep transient
  logs out of the submission artifact.
- Commit code, scripts, documentation, and curated results in reviewable units.

Exit criterion: a clean checkout reproduces the submitted evaluation artifacts
and contains no stale `FILL`, contradictory number, or undocumented manual step.
