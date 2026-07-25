# Task 2 — prefix-safe FIFO through broker failure, apply-order audited (commit 844b9def, PBR-lock build)
Config: ORDER=5, 4 brokers, RF=2 (payload survives), ACK=1 (ordering completion
within the active sequencer epoch), 2 sessions, co-located,
kv_ycsb_bench --fifo_valid. Follower broker 2 (port 1216) kill -9'd
+8s into RUN. Harness: PaperScripts/run_task2_kill_applyorder.sh.

## Follower-kill: 3/3 trials PASS (per session; 2 sessions each)
valid=1, applied==published=2,010,000, session_reorders=0, key_reorders=0, final_mismatch_keys=0,
failed_checks=none. stale_pbr_publish_rejects==0.
=> Prefix-safe FIFO SURVIVES a broker failure end-to-end: kill -> client reconnect + RF=2 replica
failover -> complete delivery + state-machine apply with ZERO apply-order inversions and correct
final state, on both sessions (both striped across the dead broker; neither is an unaffected control). Recovery path here was
reconnect+replica (8 reconnect + SESSION_OPEN_ACK; the RF=2 replica preserved the dead broker's
data so no suffix replay was needed) — i.e. the contract held WITHOUT a session fence.
NOTE: this REVERSES the earlier Panel B commit-recovery-stall pessimism — on the current build the
ORDER=5 RF=2 follower-kill recovery completes (no committed_seq freeze). Q3 apply-order + Q2 broker
kill are now demonstrated in a SINGLE execution.

## Earlier head-kill probe — INCONCLUSIVE (harness mis-target, retained for provenance)
KILL_PORT=1214 (broker 0/head): the port->pid extraction returned a wrong pid (2744253);
broker 0's real process (glog pid 2743253) survived ("1 listener after kill"), so no real head
kill occurred and the pass is a no-op. The explicit SESSION_FENCED -> HWM -> reopen -> replay path
was therefore not demonstrated by that probe; the follower
recovery above used reconnect+RF2-replica (no fence). Later campaigns use
validated PID/cmdline and listener evidence; this note remains so the no-op
probe cannot be mistaken for evidence.

## Validated head-kill probe — expected prototype limitation

Campaign `EMBARCADERO_head_negative_20260726` targets broker 0 via its
`--head` command line, records PID 2830178 and port 1214 before the signal, and
confirms both process and listener absent afterward (`kill_verified=1`).
No replacement sequencer appeared and neither session produced a completion
summary before the 60 s benchmark timeout. This is a negative result, not a
correctness pass: the prototype does not implement replacement-sequencer
election, so head loss stalls. The paper now states this explicitly and does
not present the full-design scan/re-election protocol as implemented behavior.
Raw client logs, driver log, the exact failure event, provenance, and a
losslessly compressed broker-log archive are retained with the campaign.
