# Campaign curation note

This clean campaign ran the four append-to-ACK mechanism cells and four epoch
cells three times each at commit `fd1a36ced573ba1066237a6e82ba327aeff86756`.
The extra primary 250 MB/s cell is a campaign control and is not consumed by
the mechanism/epoch validator.

The local campaign checkout was clean. The remote publisher's source checkout
contained unrelated edits, which `client_git_state.txt` records rather than
hiding. The executed remote `throughput_test` SHA-256 is identical to the local
binary, and both remote configuration-file hashes match the local files; those
binary and configuration identities, rather than the unused remote source
checkout, define the executed client.

The paper-facing metric is publisher submit-to-ACK latency. Two trials
completed all publisher ACKs at the configured offered rate but the concurrent
downstream delivery drain did not finish before its timeout. Their CSV rows
carry `saturated_e2e_lt_50pct_target`; the manifest counts these soft faults
explicitly. They are retained because this table does not report delivery
latency or delivery throughput, and the medians do not depend on treating the
downstream timeout as an ACK failure.

The 8.5 GiB `latency/` directory is transient per-run diagnostic output and is
not part of the curated artifact. The versioned inputs are `results.csv`, the
campaign contract and provenance, from which `mechanism_epoch_summary.csv` and
`mechanism_epoch_manifest.json` are regenerated.
