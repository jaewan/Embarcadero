#include <gtest/gtest.h>

#include "../src/embarlet/sequencer_utils.h"
#include "../src/common/ack_rf_policy.h"

namespace {

using Embarcadero::InclusiveBatchHwm;
using Embarcadero::IsValidAckReplicationPolicy;
using Embarcadero::OptimizedClientState;
using Embarcadero::RecoveredNextExpected;
using Embarcadero::ReconnectAnswerHwm;

// Property-style checks for restart/reconnect frontiers without a live cluster.
TEST(Order5RecoveryPropertyTest, RecoveredNextExpectedIsMonotoneMax) {
	EXPECT_EQ(RecoveredNextExpected(0, 0), 0u);
	EXPECT_EQ(RecoveredNextExpected(5, 3), 5u);
	EXPECT_EQ(RecoveredNextExpected(5, 9), 9u);
}

TEST(Order5RecoveryPropertyTest, InclusiveCommittedPrefixFromExclusiveNext) {
	const auto empty = InclusiveBatchHwm::FromExclusiveNext(0);
	EXPECT_FALSE(empty.has_prefix);

	const auto hwm = InclusiveBatchHwm::FromExclusiveNext(7);
	EXPECT_TRUE(hwm.has_prefix);
	EXPECT_EQ(hwm.value, 6u);

	OptimizedClientState state;
	state.next_expected = 7;
	state.fence();
	EXPECT_EQ(state.committed_hwm, 6u);
	EXPECT_TRUE(state.HasCommittedPrefix());
}

TEST(Order5RecoveryPropertyTest, ReconnectNeverCollapsesWhenGoiMissing) {
	EXPECT_EQ(ReconnectAnswerHwm(12, false, 0), 12u);
	EXPECT_EQ(ReconnectAnswerHwm(12, true, 4), 4u);
}

TEST(Order5RecoveryPropertyTest, Ack2StillRequiresRemoteReplica) {
	EXPECT_FALSE(IsValidAckReplicationPolicy(2, 1));
	EXPECT_TRUE(IsValidAckReplicationPolicy(2, 2));
	EXPECT_TRUE(IsValidAckReplicationPolicy(1, 1));
}

}  // namespace
