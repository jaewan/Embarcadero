#include <gtest/gtest.h>

#include "../src/common/ack_rf_policy.h"

namespace {

using Embarcadero::IsValidAckReplicationPolicy;
using Embarcadero::kMinReplicationFactorForAck2;

// Models the durable-sync gate: writes may complete (pwrite) before media sync.
// ACK2 may advance only after sync_generation covers the write offset.
struct DurableSyncLedger {
	uint64_t written_offset{0};
	uint64_t synced_offset{0};
	uint64_t durable_prefix{0};

	void CompletePwrite(uint64_t end_offset) {
		if (end_offset > written_offset) written_offset = end_offset;
	}

	void CompleteFdatasync() {
		synced_offset = written_offset;
		durable_prefix = synced_offset;
	}

	uint64_t Ack2VisiblePrefix() const { return durable_prefix; }
};

TEST(DurableAck2PowerLossModelTest, PrefetchPwriteDoesNotAdvanceAck2) {
	DurableSyncLedger led;
	led.CompletePwrite(4096);
	EXPECT_EQ(led.Ack2VisiblePrefix(), 0u);
	led.CompleteFdatasync();
	EXPECT_EQ(led.Ack2VisiblePrefix(), 4096u);
}

TEST(DurableAck2PowerLossModelTest, KillBetweenPwriteAndSyncLosesUnsyncedBytes) {
	DurableSyncLedger led;
	led.CompletePwrite(8192);
	// Simulated process kill before fdatasync: restart recovers only synced_offset.
	DurableSyncLedger after_restart;
	after_restart.synced_offset = led.synced_offset;
	after_restart.durable_prefix = led.synced_offset;
	EXPECT_EQ(after_restart.Ack2VisiblePrefix(), 0u);
	EXPECT_LT(after_restart.Ack2VisiblePrefix(), led.written_offset);
}

TEST(DurableAck2PowerLossModelTest, Ack2PolicyRequiresRemoteReplica) {
	EXPECT_GE(kMinReplicationFactorForAck2, 2);
	EXPECT_FALSE(IsValidAckReplicationPolicy(/*ack=*/2, /*rf=*/1));
	EXPECT_TRUE(IsValidAckReplicationPolicy(/*ack=*/2, /*rf=*/2));
}

}  // namespace
