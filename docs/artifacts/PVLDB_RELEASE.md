# PVLDB review release

PVLDB is single-blind, so the paper package includes author names and
affiliations. The review materials are split deliberately:

1. `Paper/scripts/build_vldb_submission.sh` builds the PVLDB PDF and paper
   source archive from tracked current-paper files and the official pinned
   VLDB template. It checks the 12-page non-reference limit and fails closed
   unless `VLDB_ALLOW_OVERPAGE=1` is explicitly set for draft builds.
2. `scripts/publication/build_anonymous_artifact.sh` builds a sanitized code,
   benchmark, test, plotting, and TLA+ archive. Sanitization is retained to
   avoid leaking machine-local paths, hostnames, or private testbed addresses;
   it is not intended to anonymize the single-blind paper.
3. Full raw traces are not embedded in either archive. The current
   `data/paper_eval` tree is approximately 35 GiB and contains machine-local
   provenance. Sanitize it and publish it through a stable artifact host before
   placing the resulting URL in `\vldbavailabilityurl`.

Both builders refuse to overwrite an existing output and emit SHA-256
checksums. Before publishing raw traces, run the same deny-list over the
extracted upload tree:

```bash
grep -aERn '(/home/|/Users/|moscxl|10[.]10[.]10[.]|jaewan|domin)' RAW_TREE
```

Do not publish the developer repository directly: its Git history,
machine-local experiment logs, and internal review notes are outside the
review artifact.
