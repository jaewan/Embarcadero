# Validated head-kill negative result

This campaign is deliberately a negative scope test.

- `failure_event.log` records the exact `embarlet --head` PID and command line,
  `kill -9` timestamp, process disappearance, listener disappearance, and
  `kill_verified=1`.
- `results.csv` contains `NOSUMMARY` / `stall_or_broken`: no replacement
  sequencer was elected before the bounded 60 s benchmark timeout.
- `EMBARCADERO_pipe_trial1_s{0,1}.log` retain the two client/apply traces.
- `driver.log` and `provenance.txt` retain the execution contract and binary
  identity.
- `broker_logs.tar.zst` losslessly stores both broker-log snapshots. SHA-256:
  `79b5df5c777a86b67bd03f0808a83fbb01a92df40d27b74eac92f60bc77fd176`.

Interpretation: the prototype has no replacement-sequencer election path.
This result must not be counted as a FIFO-valid trial or as evidence for the
full-design sequencer recovery protocol.
