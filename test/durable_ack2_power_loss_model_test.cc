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

// Models the common Scalog/LazyLog media-durability publication rule used by
// both the RPC writer and the CXL primary/replica pollers.  `local_cut` and
// `replication_done` are deliberately separate: neither may move until the
// same batch has completed pwrite and fdatasync.
struct MediaReplicaLedger {
	uint64_t pwrite_prefix{0};
	uint64_t synced_prefix{0};
	uint64_t local_cut{0};
	uint64_t replication_done{0};

	bool CompleteBatch(uint64_t end_offset, bool pwrite_ok, bool fdatasync_ok) {
		if (!pwrite_ok) return false;
		pwrite_prefix = end_offset;
		if (!fdatasync_ok) return false;
		synced_prefix = end_offset;
		local_cut = end_offset;
		replication_done = end_offset;
		return true;
	}

	MediaReplicaLedger Restart() const {
		MediaReplicaLedger recovered;
		recovered.pwrite_prefix = synced_prefix;
		recovered.synced_prefix = synced_prefix;
		recovered.local_cut = synced_prefix;
		recovered.replication_done = synced_prefix;
		return recovered;
	}
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

TEST(DurableAck2PowerLossModelTest, FailedPwriteOrSyncDoesNotPublishAnyFrontier) {
	MediaReplicaLedger replica;
	EXPECT_FALSE(replica.CompleteBatch(/*end_offset=*/10, /*pwrite_ok=*/false,
	                                   /*fdatasync_ok=*/true));
	EXPECT_EQ(replica.pwrite_prefix, 0u);
	EXPECT_EQ(replica.local_cut, 0u);
	EXPECT_EQ(replica.replication_done, 0u);

	EXPECT_FALSE(replica.CompleteBatch(/*end_offset=*/10, /*pwrite_ok=*/true,
	                                   /*fdatasync_ok=*/false));
	EXPECT_EQ(replica.pwrite_prefix, 10u);
	EXPECT_EQ(replica.synced_prefix, 0u);
	EXPECT_EQ(replica.local_cut, 0u);
	EXPECT_EQ(replica.replication_done, 0u);
}

TEST(DurableAck2PowerLossModelTest, RpcSuccessAndCxlFrontiersFollowSyncNotPwrite) {
	MediaReplicaLedger primary;
	MediaReplicaLedger replica;

	// A successful RPC is the return value of CompleteBatch.  A primary or
	// replica CXL poller must make exactly the same decision before publishing
	// its local cut / replication_done slot.
	EXPECT_FALSE(primary.CompleteBatch(/*end_offset=*/8, /*pwrite_ok=*/true,
	                                   /*fdatasync_ok=*/false));
	EXPECT_EQ(primary.local_cut, 0u);
	EXPECT_EQ(primary.replication_done, 0u);
	EXPECT_FALSE(replica.CompleteBatch(/*end_offset=*/8, /*pwrite_ok=*/true,
	                                   /*fdatasync_ok=*/false));
	EXPECT_EQ(replica.local_cut, 0u);
	EXPECT_EQ(replica.replication_done, 0u);

	EXPECT_TRUE(primary.CompleteBatch(/*end_offset=*/8, /*pwrite_ok=*/true,
	                                  /*fdatasync_ok=*/true));
	EXPECT_TRUE(replica.CompleteBatch(/*end_offset=*/8, /*pwrite_ok=*/true,
	                                  /*fdatasync_ok=*/true));
	EXPECT_EQ(primary.local_cut, 8u);
	EXPECT_EQ(primary.replication_done, 8u);
	EXPECT_EQ(replica.local_cut, 8u);
	EXPECT_EQ(replica.replication_done, 8u);
}

TEST(DurableAck2PowerLossModelTest, RestartRequiresRedriveOfUnsyncedSuffix) {
	MediaReplicaLedger replica;
	ASSERT_TRUE(replica.CompleteBatch(/*end_offset=*/10, /*pwrite_ok=*/true,
	                                  /*fdatasync_ok=*/true));
	ASSERT_FALSE(replica.CompleteBatch(/*end_offset=*/20, /*pwrite_ok=*/true,
	                                   /*fdatasync_ok=*/false));

	MediaReplicaLedger recovered = replica.Restart();
	EXPECT_EQ(recovered.local_cut, 10u);
	EXPECT_EQ(recovered.replication_done, 10u);
	EXPECT_TRUE(recovered.CompleteBatch(/*end_offset=*/20, /*pwrite_ok=*/true,
	                                    /*fdatasync_ok=*/true));
	EXPECT_EQ(recovered.local_cut, 20u);
	EXPECT_EQ(recovered.replication_done, 20u);
}

TEST(DurableAck2PowerLossModelTest, Ack2PolicyRequiresRemoteReplica) {
	EXPECT_GE(kMinReplicationFactorForAck2, 2);
	EXPECT_FALSE(IsValidAckReplicationPolicy(/*ack=*/2, /*rf=*/1));
	EXPECT_TRUE(IsValidAckReplicationPolicy(/*ack=*/2, /*rf=*/2));
}

}  // namespace
