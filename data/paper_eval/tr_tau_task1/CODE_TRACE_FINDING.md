# Task 1 code trace — T/tau vs T/R (root repo 7a82b315 + PBR-lock dirty.patch)

## Production release path (ORDER=5), file src/embarlet/topic.cc
1. Predecessor publication -> PBR slot; publish_commit set = readiness barrier.
2. BrokerScannerWorker5 (7652) scans PBR ring each PASS (period P). On a
   publish-committed slot it pushes the batch and increments
   scanner_pushed_batches_[b] (8229). Push targets the CURRENT COLLECTING epoch
   buffer epoch_buffers_[epoch_index_ % 3].
3. Epoch seal: EpochDriverThread (4714) is a fixed tau=500us metronome
   (kEpochUs=500; EMBAR_ORDER5_EPOCH_US in [100,5000]); sleeps to
   next_seal_deadline, cur_buf.seal(), advance by exactly epoch_duration.
4. EpochSequencerThread (5699) busy-waits for a SEALED buffer, then per seal:
   partition level0/level5 (6166) -> ProcessLevel5Batches(level5,ready) (6175)
   -> CommitEpoch(ready,...) (6225) writes GOI. Held suffixes are re-examined
   ONLY here (once per sealed epoch).
5. Hold/release: ProcessLevel5BatchesShard (6761) holds a batch whose predecessor
   (next_expected) is absent; releases it to `ready` when the predecessor appears
   in a SEALED epoch's level5.

## Central question — can a newly-arrived predecessor release a held suffix
## BEFORE the next epoch seal?  NO.
- Scanner push only makes a batch visible to the sealed-epoch processor after the
  collecting buffer SEALS. A missing predecessor is not processable until its
  epoch seals.
- Fast-seal (scanner-triggered early seal, 8232-8276) fires ONLY when
  order5_steady_state_==true, i.e. total_hold_size_==0 (set at 6233). During a
  gap (held suffix), steady_state is FALSE -> fast-seal DISABLED -> sealing is at
  the tau metronome. The disconnect-drain fast path (4758) is broker-death only.
- Therefore: P (scanner pass) bounds OBSERVATION; tau (epoch seal) bounds
  RELEASE/COMMIT. Release opportunities during skew window T = seals in T = T/tau.
  At tau=500us, T=1.5ms -> T/tau ~= 3, NOT T/P ~= 30.

## Paper implication
"R ~= 50us" and "~30 revisits" (Sec2) equate a scanner pass with a commit round.
The scanner may pass ~30x during T, but there are only ~T/tau ~= 3 release/commit
opportunities. Define P and tau separately; P bounds observation, tau bounds
release/commit; report T/tau as the empirical capacity parameter (~=3 at 500us).
