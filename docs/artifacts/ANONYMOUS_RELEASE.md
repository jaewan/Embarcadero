# Anonymous artifact release

The review artifact is split deliberately:

1. `Paper/scripts/build_anonymous_submission.sh` builds the anonymous PDF and
   paper-source archive from tracked current-paper files only.
2. `scripts/publication/build_anonymous_artifact.sh` builds the anonymous code,
   benchmark, test, plotting, and TLA+ archive. It strips Git metadata and
   rewrites machine-local paths, hostnames, and private testbed addresses to
   documented placeholders.
3. Full raw traces are not embedded in either archive. The current
   `data/paper_eval` tree is approximately 35 GiB and contains machine-local
   provenance. Upload it only through the selected venue's anonymous artifact
   service after applying that service's size and disclosure rules.

Both builders are fail-closed: they refuse to overwrite an existing output and
reject known local identity markers. Each emits SHA-256 checksums. Before
uploading raw traces, run the same deny-list over the extracted upload tree:

```bash
grep -aERn '(/home/|/Users/|moscxl|10[.]10[.]10[.]|jaewan|domin)' RAW_TREE
```

Do not upload the developer repository itself: its Git remote, historical
experiment logs, and internal review notes are intentionally outside the
anonymous release.
