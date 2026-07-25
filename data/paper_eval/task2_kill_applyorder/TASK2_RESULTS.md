# Task 2 — prefix-safe FIFO through broker failure, apply-order audited (commit 844b9def, PBR-lock build)
Config: ORDER=5, 4 brokers, RF=2 (payload survives), ACK=1 (failover-stable), 2 sessions
(affected+control), co-located, kv_ycsb_bench --fifo_valid. Follower broker 2 (port 1216) kill -9'd
+8s into RUN. Harness: PaperScripts/run_task2_kill_applyorder.sh.

## Follower-kill: 3/3 trials PASS (per session; 2 sessions each)
valid=1, applied==published=2,010,000, session_reorders=0, key_reorders=0, final_mismatch_keys=0,
failed_checks=none. stale_pbr_publish_rejects==0.
=> Prefix-safe FIFO SURVIVES a broker failure end-to-end: kill -> client reconnect + RF=2 replica
failover -> complete delivery + state-machine apply with ZERO apply-order inversions and correct
final state, on BOTH the affected and the independent control session. Recovery path here was
reconnect+replica (8 reconnect + SESSION_OPEN_ACK; the RF=2 replica preserved the dead broker's
data so no suffix replay was needed) — i.e. the contract held WITHOUT a session fence.
NOTE: this REVERSES the earlier Panel B commit-recovery-stall pessimism — on the current build the
ORDER=5 RF=2 follower-kill recovery completes (no committed_seq freeze). Q3 apply-order + Q2 broker
kill are now demonstrated in a SINGLE execution.

## Head-kill probe — INCONCLUSIVE (harness mis-target, honest note)
KILL_PORT=1214 (broker 0/head): the port->pid extraction returned a wrong pid (2744253);
broker 0's real process (glog pid 2743253) survived ("1 listener after kill"), so no real head
kill occurred and the pass is a no-op. The explicit SESSION_FENCED -> HWM -> reopen -> replay path
(sequencer failover) is therefore NOT yet demonstrated with the apply-order checker; the follower
recovery above used reconnect+RF2-replica (no fence). To demonstrate the fence/replay path end-to-end
with --fifo_valid, fix the head-pid targeting (broker 0 owns 1214 AND the sequencer; kill the exact
process, and expect a failover window) OR drive the Fig3/E4a prefix-safe client-kill path with the
apply-order checker. FOLLOWER-kill result (3/3, reconnect+replica) stands as the Q2xQ3 demonstration.
