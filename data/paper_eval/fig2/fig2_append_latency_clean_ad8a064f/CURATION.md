# Campaign curation note

The requested primary sweep completed all 21 Embarcadero cells (seven loads,
three trials each) at commit `ad8a064f2e9f25efbb0ecc2a8dd740ac6baef790`.

After pass 3, the long-running shell read a concurrently edited tail of its
driver and began an unrequested Scalog cell even though the recorded contract
sets `INCLUDE_BASELINES=0`. That cell was interrupted and produced only a
synthetic `status=fail` row with no measurements. The row was removed from the
curated `results.csv`; no successful or measured performance row was removed.
The campaign is used only for the primary Embarcadero cell, and
`summarize_fig2_primary.py` validates exactly three successful rows at every
declared load.

Publication campaigns must run from an immutable committed driver. The paper
scripts now reject dirty worktrees by default.
