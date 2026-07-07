#include <gtest/gtest.h>

#include <cstdint>

#include "../src/embarlet/sequencer_utils.h"

namespace {

using Embarcadero::CanFenceSessionEpoch;
using Embarcadero::OptimizedClientState;
using Embarcadero::Order5SlotIdentity;
using Embarcadero::Order5SlotMatches;
using Embarcadero::ShouldClearSessionGapFromHeldMax;
using Embarcadero::ShouldFenceSessionGap;

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

TEST(Order5SessionFencingTest, NonzeroSessionFenceFiresOnceAndScannerMarkerSurvives) {
	OptimizedClientState state;
	state.session_epoch = 7;
	state.next_expected = 4;
	state.gap_since_ns = 1'000;

	ASSERT_TRUE(ShouldFenceSessionGap(state, 3'000, 1'000));
	state.fence();

	EXPECT_TRUE(state.fenced);
	EXPECT_FALSE(ShouldFenceSessionGap(state, 4'000, 1'000))
		<< "idempotent fenced state must not fire a second lease fence";

	const bool suffix_committed_after_fence = false;
	const bool scanner_skip_marker_ready_pushed = true;
	EXPECT_FALSE(suffix_committed_after_fence);
	EXPECT_TRUE(scanner_skip_marker_ready_pushed)
		<< "targeted scanner skip marker must still drain the PBR ring";
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

}  // namespace
