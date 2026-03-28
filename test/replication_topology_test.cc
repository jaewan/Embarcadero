#include <gtest/gtest.h>

#include "common/performance_utils.h"

namespace {

TEST(ReplicationTopology, SetBrokerModuloRing) {
	EXPECT_EQ(Embarcadero::GetReplicationSetBroker(0, 2, 4, 0), 0);
	EXPECT_EQ(Embarcadero::GetReplicationSetBroker(0, 2, 4, 1), 1);
	EXPECT_EQ(Embarcadero::GetReplicationSetBroker(3, 2, 4, 0), 3);
	EXPECT_EQ(Embarcadero::GetReplicationSetBroker(3, 2, 4, 1), 0);
}

TEST(ReplicationTopology, ChainRoleRf2FourBrokers) {
	// Publication topology: RF=2 chain for source S is {S, (S+1)%N}.
	const int n = 4;
	const int rf = 2;
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(0, 0, rf, n), 0);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(1, 0, rf, n), 1);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(2, 0, rf, n), -1);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(3, 0, rf, n), -1);

	EXPECT_EQ(Embarcadero::GetReplicationChainRole(1, 1, rf, n), 0);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(2, 1, rf, n), 1);

	EXPECT_EQ(Embarcadero::GetReplicationChainRole(2, 2, rf, n), 0);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(3, 2, rf, n), 1);

	EXPECT_EQ(Embarcadero::GetReplicationChainRole(3, 3, rf, n), 0);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(0, 3, rf, n), 1);
}

TEST(ReplicationTopology, ChainRoleRf1IsHeadOnly) {
	const int n = 4;
	const int rf = 1;
	for (int src = 0; src < n; ++src) {
		for (int local = 0; local < n; ++local) {
			const int r = Embarcadero::GetReplicationChainRole(local, src, rf, n);
			if (local == src) {
				EXPECT_EQ(r, 0) << "local=" << local << " src=" << src;
			} else {
				EXPECT_EQ(r, -1) << "local=" << local << " src=" << src;
			}
		}
	}
}

TEST(ReplicationTopology, ChainRoleInvalidArgs) {
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(0, 0, 0, 4), -1);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(0, 0, 2, 0), -1);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(-1, 0, 2, 4), -1);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(0, -1, 2, 4), -1);
	EXPECT_EQ(Embarcadero::GetReplicationChainRole(0, 4, 2, 4), -1);
}

TEST(ReplicationTopology, ChainRoleMatchesSetBrokerMembership) {
	const int n = 8;
	for (int rf = 1; rf <= 4; ++rf) {
		for (int src = 0; src < n; ++src) {
			for (int local = 0; local < n; ++local) {
				const int role = Embarcadero::GetReplicationChainRole(local, src, rf, n);
				bool in_set = false;
				for (int i = 0; i < rf; ++i) {
					if (Embarcadero::GetReplicationSetBroker(src, rf, n, i) == local) {
						in_set = true;
						EXPECT_EQ(role, i);
						break;
					}
				}
				if (!in_set) {
					EXPECT_EQ(role, -1);
				}
			}
		}
	}
}

} // namespace
