# 500 ms session-fence sensitivity

This campaign repeats the end-to-end gap, fence, reopen, replay, deliver, and
apply audit with a 500 ms session lease and a 1,500 ms injected predecessor
delay. Both sessions are intentionally gapped; neither is an unaffected
control.

Across three trials, both sessions apply all 1,510,000 published operations
with zero session-order inversions, key-order inversions, or final-state
mismatches. `SESSION_FENCED` is observed 499.576--499.866 ms after gap
injection across the six session executions.

This measures the implemented session-fence timer under controlled gaps. It
does not measure the unimplemented CXL failure detector or sequencer election.
The lossless client and broker logs remain in this campaign directory in the
working artifact; the committed compact record contains results, timing, and
binary provenance.
