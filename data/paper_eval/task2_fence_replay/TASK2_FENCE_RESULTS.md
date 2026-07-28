# Task 2 fence path — SESSION_FENCED -> reopen -> replay, apply-order audited
Config: ORDER=5, 4 brokers, RF=2, ACK=1, 2 sessions, co-located, kv_ycsb_bench --fifo_valid.
Injected predecessor delay 10000ms > session lease 6000ms -> held gap unrepaired past lease ->
broker FENCES the session; client reopens (new epoch) + replays the retained unacked suffix.
Harness: PaperScripts/run_task2_fence_replay.sh.
Current paper artifact:
`EMBARCADERO_retrans_hardening_paper_20260728T173814Z/`.  This rerun uses the
hardened predecessor-only retransmission policy and the fail-closed campaign
runner; its manifest records six valid session rows and no failed trials.

## 3/3 trials PASS (per session; 2 sessions each)
valid=1, applied==published=1,510,000, session_reorders=0, key_reorders=0, final_mismatch_keys=0,
failed_checks=none. Fence markers per trial: SESSION_FENCED fired for both sessions; both reopened
(SESSION_OPEN_ACK with a new session_epoch / committed_prefix), client replayed, run COMPLETED.
=> The explicit prefix-safe recovery (gap -> hold -> SESSION_FENCED@committed-HWM -> reopen
under new epoch -> replay unacked suffix -> deliver -> apply) is demonstrated END-TO-END with the
apply-order checker passing: zero inversions, zero missing committed ops, correct final state, and
both sessions complete (the global gap knob delays every publisher, so both sessions were fenced and reopened;
this run has no unaffected control session—cross-session isolation is a separate experiment). Combined with the follower-kill result
(contract survives broker failure via replica), Task 2's Q2xQ3 thesis is demonstrated across BOTH
recovery modes.

## Lease sensitivity

A matched 500 ms lease / 1,500 ms gap campaign also passes 3/3 trials for both
sessions: all 1,510,000 operations apply with zero ordering or final-state
violations. Across the six session executions, `SESSION_FENCED` is observed
499.576--499.866 ms after gap injection. This isolates the implemented
session-fence timer; it is not a measurement of the unimplemented CXL failure
detector.
