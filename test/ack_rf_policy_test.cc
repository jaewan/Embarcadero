#include <gtest/gtest.h>

#include "common/ack_rf_policy.h"

using Embarcadero::InclusiveBatchHwm;
using Embarcadero::IsValidAckReplicationPolicy;
using Embarcadero::ValidateAckReplicationPolicy;

TEST(AckRfPolicyTest, AcceptsValidPairs) {
	EXPECT_TRUE(IsValidAckReplicationPolicy(0, 0));
	EXPECT_TRUE(IsValidAckReplicationPolicy(0, 1));
	EXPECT_TRUE(IsValidAckReplicationPolicy(1, 0));
	EXPECT_TRUE(IsValidAckReplicationPolicy(1, 1));
	EXPECT_TRUE(IsValidAckReplicationPolicy(1, 2));
	EXPECT_TRUE(IsValidAckReplicationPolicy(2, 2));
	EXPECT_TRUE(IsValidAckReplicationPolicy(2, 3));
}

TEST(AckRfPolicyTest, RejectsAck2WithInsufficientRf) {
	EXPECT_FALSE(IsValidAckReplicationPolicy(2, 0));
	EXPECT_FALSE(IsValidAckReplicationPolicy(2, 1));

	const auto zero = ValidateAckReplicationPolicy(2, 0);
	ASSERT_FALSE(zero.ok);
	EXPECT_NE(zero.error.find("replication_factor>=2"), std::string::npos);

	const auto one = ValidateAckReplicationPolicy(2, 1);
	ASSERT_FALSE(one.ok);
	EXPECT_NE(one.error.find("replication_factor>=2"), std::string::npos);
}

TEST(AckRfPolicyTest, RejectsInvalidAckLevelAndNegativeRf) {
	EXPECT_FALSE(IsValidAckReplicationPolicy(-1, 1));
	EXPECT_FALSE(IsValidAckReplicationPolicy(3, 2));
	EXPECT_FALSE(IsValidAckReplicationPolicy(1, -1));
}

TEST(AckRfPolicyTest, InclusiveBatchHwmTyping) {
	const auto empty = InclusiveBatchHwm::FromExclusiveNext(0);
	EXPECT_FALSE(empty.has_prefix);
	EXPECT_EQ(empty.ExclusiveNext(), 0u);

	const auto from_next = InclusiveBatchHwm::FromExclusiveNext(4);
	EXPECT_TRUE(from_next.has_prefix);
	EXPECT_EQ(from_next.value, 3u);
	EXPECT_EQ(from_next.ExclusiveNext(), 4u);

	const auto from_inclusive = InclusiveBatchHwm::FromInclusive(0);
	EXPECT_TRUE(from_inclusive.has_prefix);
	EXPECT_EQ(from_inclusive.value, 0u);
	EXPECT_EQ(from_inclusive.ExclusiveNext(), 1u);
}
