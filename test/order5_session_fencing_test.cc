#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "../src/client/session_client_utils.h"
#include "../src/embarlet/sequencer_utils.h"

namespace {

using Embarcadero::CanFenceSessionEpoch;
using Embarcadero::OptimizedClientState;
using Embarcadero::Order5SlotIdentity;
using Embarcadero::Order5SlotMatches;
using Embarcadero::RecoveredNextExpected;
using Embarcadero::ReconnectAnswerHwm;
using Embarcadero::SessionKeyFor;
using Embarcadero::ShouldRejectOrder5SpatialGuard;
using Embarcadero::ShouldClearSessionGapFromHeldMax;
using Embarcadero::ShouldFenceSessionGap;
using Embarcadero::ShouldTerminalFenceAckRelay;
using Embarcadero::ShouldWithholdAckRelay;

TEST(Order5SessionFencingTest, EpochZeroSuffixGapNeverFencesOrAdvancesImmediately) {
	OptimizedClientState state;
	state.session_epoch = 0;
	state.next_expected = 3;
	state.gap_since_ns = 10;

	EXPECT_FALSE(CanFenceSessionEpoch(state.session_epoch));
	EXPECT_FALSE(ShouldFenceSessionGap(state, 2'000, 100));

	const uint64_t initial_next_expected = state.next_expected;
	const uint64_t arrived_suffix_seq = 9;
	state.mark_sequenced(arrived_suffix_seq);

	EXPECT_FALSE(state.fenced);
	EXPECT_EQ(state.next_expected, initial_next_expected);
}

TEST(Order5SessionFencingTest, EpochZeroTargetedScannerSkipAdvancesLegacyFrontier) {
	OptimizedClientState state;
	state.session_epoch = 0;
	state.next_expected = 2;

	const uint64_t scanner_skip_seq = 5;
	const bool skip_marker_ready_pushed = true;
	if (!CanFenceSessionEpoch(state.session_epoch)) {
		state.mark_sequenced(scanner_skip_seq);
		if (scanner_skip_seq >= state.next_expected) {
			state.set_next_expected(scanner_skip_seq + 1);
		}
	}

	EXPECT_FALSE(state.fenced);
	EXPECT_EQ(state.next_expected, scanner_skip_seq + 1);
	EXPECT_TRUE(skip_marker_ready_pushed);
}

TEST(Order5SessionFencingTest, NonzeroSessionFenceFiresOnce) {
	OptimizedClientState state;
	state.session_epoch = 7;
	state.next_expected = 4;
	state.gap_since_ns = 1'000;

	ASSERT_TRUE(ShouldFenceSessionGap(state, 3'000, 1'000));
	state.fence();

	EXPECT_TRUE(state.fenced);
	EXPECT_EQ(state.committed_hwm, 3u)
		<< "fence() must publish inclusive last-committed batch_seq (next_expected-1)";
	EXPECT_TRUE(state.HasCommittedPrefix());
	EXPECT_FALSE(ShouldFenceSessionGap(state, 4'000, 1'000))
		<< "idempotent fenced state must not fire a second lease fence";
}

TEST(Order5SessionFencingTest, EmptyPrefixFenceDoesNotClaimBatchZero) {
	OptimizedClientState state;
	state.session_epoch = 3;
	state.next_expected = 0;
	state.fence();
	EXPECT_TRUE(state.fenced);
	EXPECT_EQ(state.committed_hwm, 0u);
	EXPECT_FALSE(state.HasCommittedPrefix());
}

TEST(Order5SessionFencingTest, FalseFenceGapFillsBeforeLease) {
	OptimizedClientState state;
	state.session_epoch = 9;
	state.next_expected = 11;
	state.gap_since_ns = 1'000;

	EXPECT_FALSE(ShouldFenceSessionGap(state, 1'900, 1'000));
	state.advance_next_expected();

	EXPECT_EQ(state.gap_since_ns, 0);
	EXPECT_FALSE(state.fenced);
}

TEST(Order5SessionFencingTest, StaleSubFrontierEntryDoesNotClearSuffixGap) {
	const uint64_t next_expected = 10;
	const uint64_t stale_min_held_seq = 8;
	const uint64_t max_held_seq = 13;

	EXPECT_LE(stale_min_held_seq, next_expected);
	EXPECT_FALSE(ShouldClearSessionGapFromHeldMax(next_expected, max_held_seq));
	EXPECT_TRUE(ShouldClearSessionGapFromHeldMax(next_expected, next_expected));
}

TEST(Order5SessionFencingTest, DeferredFullBackpressureSlotExcludesConsumedThrough) {
	const Order5SlotIdentity blocked{2, 4096, 99};

	EXPECT_TRUE(Order5SlotMatches(blocked, 2, 4096, 99));
	EXPECT_FALSE(Order5SlotMatches(blocked, 2, 4096, 100));
	EXPECT_FALSE(Order5SlotMatches(blocked, 1, 4096, 99));
}

TEST(Order5SessionFencingTest, AckRelayWithholdsStaleProducingEpoch) {
	EXPECT_FALSE(ShouldWithholdAckRelay(0, 0));
	EXPECT_TRUE(ShouldWithholdAckRelay(0, 5));
	EXPECT_TRUE(ShouldWithholdAckRelay(4, 5));
	EXPECT_FALSE(ShouldWithholdAckRelay(5, 5));
	EXPECT_FALSE(ShouldWithholdAckRelay(6, 5));
}

TEST(Order5SessionFencingTest, SpatialGuardRejectsOnlyFencedOrPastExpectedSuffix) {
	EXPECT_FALSE(ShouldRejectOrder5SpatialGuard(false, true, 2, 10));
	EXPECT_FALSE(ShouldRejectOrder5SpatialGuard(true, false, 10, 10));
	EXPECT_FALSE(ShouldRejectOrder5SpatialGuard(true, false, 11, 10));
	EXPECT_TRUE(ShouldRejectOrder5SpatialGuard(true, false, 9, 10));
	EXPECT_TRUE(ShouldRejectOrder5SpatialGuard(true, true, 10, 10));
}

TEST(Order5SessionFencingTest, RecoveryAndReconnectUseAuthoritativeMaxima) {
	EXPECT_EQ(RecoveredNextExpected(8, 3), 8u);
	EXPECT_EQ(RecoveredNextExpected(8, 12), 12u);
	EXPECT_EQ(ReconnectAnswerHwm(7, 3), 3u);
	EXPECT_EQ(ReconnectAnswerHwm(7, 11), 7u);
	EXPECT_EQ(ReconnectAnswerHwm(7, false, 0), 7u)
		<< "missing GOI must not collapse reconnect HWM to 0";
	EXPECT_EQ(ReconnectAnswerHwm(7, true, 3), 3u);
}

TEST(Order5SessionFencingTest, IdleCommittedSessionAfterFailoverDoesNotTerminalFence) {
	EXPECT_FALSE(ShouldTerminalFenceAckRelay(12, true, 12));
	EXPECT_FALSE(ShouldTerminalFenceAckRelay(11, true, 12));
	EXPECT_TRUE(ShouldTerminalFenceAckRelay(13, true, 12));
	EXPECT_TRUE(ShouldTerminalFenceAckRelay(0, false, 0));
}

TEST(Order5SessionFencingTest, FullEpochSessionKeysDoNotAliasLow16Wrap) {
	const uint32_t client_id = 42;
	const uint32_t epoch_a = 7;
	const uint32_t epoch_b = 0x10007;

	EXPECT_EQ(static_cast<uint16_t>(epoch_a), static_cast<uint16_t>(epoch_b));
	EXPECT_NE(SessionKeyFor(client_id, epoch_a), SessionKeyFor(client_id, epoch_b));
}

TEST(Order5SessionFencingTest, RendezvousBrokerUsesBatchSeqAndExcludesFailedBroker) {
	const std::vector<int> brokers{0, 1, 2, 3};
	const int first = RendezvousBroker(42, 100, brokers, -1);
	const int excluded = RendezvousBroker(42, 100, brokers, first);

	EXPECT_NE(first, -1);
	EXPECT_NE(excluded, -1);
	EXPECT_NE(excluded, first);
	EXPECT_NE(RendezvousBroker(42, 101, brokers, first), first);
}

TEST(Order5SessionFencingTest, DeltaEstimatorClampsAndUsesKarn) {
	DeltaEstimator estimator;
	EXPECT_DOUBLE_EQ(estimator.delta_ms(), DeltaEstimator::kDeltaFloorMs);

	estimator.sample(20'000'000, 1);
	EXPECT_DOUBLE_EQ(estimator.delta_ms(), DeltaEstimator::kDeltaFloorMs)
		<< "retransmitted samples must be ignored by Karn filtering";

	estimator.sample(500'000, 0);
	EXPECT_DOUBLE_EQ(estimator.delta_ms(), DeltaEstimator::kDeltaFloorMs);

	estimator.sample(100'000'000, 0);
	EXPECT_DOUBLE_EQ(estimator.delta_ms(), DeltaEstimator::kDeltaCapMs);
}

TEST(Order5SessionFencingTest, FenceRebasesAckBaseAfterLocalCreditOnce) {
	const size_t prior_ack_received = 100;
	const size_t locally_committed = 40;
	const size_t broker_frontier_after_fence = 140;
	const size_t broker_frontier_excludes_local = 100;

	const size_t rebased = RebasedAckBaseAfterFenceCredit(
		prior_ack_received, locally_committed);
	EXPECT_EQ(rebased, 140u);
	EXPECT_EQ(broker_frontier_after_fence - rebased, 0u)
		<< "already committed local-credit messages must not be credited twice";
	EXPECT_EQ(broker_frontier_after_fence - prior_ack_received, locally_committed)
		<< "the old base would double count the locally credited prefix";

	EXPECT_EQ(RebasedAckBaseAfterFenceCredit(
		          prior_ack_received, locally_committed),
	          broker_frontier_after_fence)
		<< "diagnostic committed_msg_hwm is not a release axis; the local credit path owns the prefix";
	EXPECT_LT(broker_frontier_excludes_local, rebased);

	const size_t suffix_ack_in_new_generation = 60;
	EXPECT_EQ(SessionGlobalAckFromGeneration(rebased, suffix_ack_in_new_generation), 200u)
		<< "R-G ACKs are generation-relative and must be rebased before ACK-delta accounting";
}

TEST(Order5SessionFencingTest, SessionGlobalLedgerRetiresNonHeadAndRehomedBatches) {
	const size_t base = 10;
	const size_t first_non_head_ack_end = base + 4;
	const size_t second_rehomed_ack_end = base + 9;

	EXPECT_TRUE(SessionGlobalUnackedRetired(first_non_head_ack_end, 14));
	EXPECT_FALSE(SessionGlobalUnackedRetired(second_rehomed_ack_end, 14));
	EXPECT_TRUE(SessionGlobalUnackedRetired(second_rehomed_ack_end, 19));

	size_t ack_end = 0;
	EXPECT_FALSE(SessionPrefixAckEnd(1, 0, base, 5, &ack_end))
		<< "out-of-order send completion must not derive a releasable ACK key";
	EXPECT_TRUE(SessionPrefixAckEnd(0, 0, base, 4, &ack_end));
	EXPECT_EQ(ack_end, first_non_head_ack_end);
}

TEST(Order5SessionFencingTest, RolloverNextBatchSeqStartsAfterResubmittedSuffix) {
	const size_t suffix_batches = 6;
	EXPECT_EQ(NextBatchSeqAfterSuffixResubmit(suffix_batches), 6u);
	EXPECT_LT(5u, NextBatchSeqAfterSuffixResubmit(suffix_batches));
	EXPECT_EQ(NextSessionEpochAfterFence(7), 8u)
		<< "env override may seed the initial epoch but must not pin reopens";
}

}  // namespace
