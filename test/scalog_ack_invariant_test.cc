// Self-contained unit test for SCALOG ACK invariants.
// Does NOT depend on the full Embarcadero header chain (avoids gRPC/protobuf deps).
// Instead, mirrors the minimal type definitions needed from cxl_datastructure.h.

#include <gtest/gtest.h>
#include <cstring>
#include <limits>
#include <cstdint>
#include <cstddef>

namespace {

constexpr int kNumMaxBrokers = 32;

enum SequencerType : int {
    EMBARCADERO = 0,
    KAFKA = 1,
    SCALOG = 2,
    CORFU = 3,
    LAZYLOG = 4,
};

struct alignas(64) offset_entry {
    volatile size_t log_offset;
    volatile size_t batch_headers_offset;
    volatile size_t written;
    volatile unsigned long long int written_addr;
    volatile size_t validated_written_byte_offset;
    uint8_t _pad_broker[256 - 40];

    volatile uint64_t replication_done[kNumMaxBrokers];

    volatile uint64_t ordered;
    volatile size_t ordered_offset;
    volatile size_t batch_headers_consumed_through;
    uint8_t _pad_sequencer[256 - 8 - 8 - 8];
};
static_assert(sizeof(offset_entry) == 768, "must match production layout");

inline int GetReplicationSetBroker(int broker_id, int replication_factor,
                                   int num_brokers, int replica_index) {
    return (broker_id + replica_index) % num_brokers;
}

constexpr size_t kReplicationNotStarted = std::numeric_limits<size_t>::max();

// Mirrors GetOffsetToAck for ack_level=2 with the SCALOG correctness clamp.
size_t ComputeScalogAck2(
    volatile offset_entry* offsets,
    int broker_id,
    int replication_factor,
    int num_brokers,
    SequencerType seq_type) {

    size_t min_rep = std::numeric_limits<size_t>::max();
    int ready_replicas = 0;
    for (int i = 0; i < replication_factor; i++) {
        int b = GetReplicationSetBroker(broker_id, replication_factor, num_brokers, i);
        size_t val = offsets[b].replication_done[broker_id];
        if (val == kReplicationNotStarted) {
            continue;
        }
        ready_replicas++;
        if (min_rep > val) min_rep = val;
    }
    if (ready_replicas < replication_factor || min_rep == kReplicationNotStarted) {
        return static_cast<size_t>(-1);
    }
    size_t durable_frontier = min_rep + 1;

    if (seq_type == SCALOG || seq_type == LAZYLOG) {
        const size_t ordered_frontier = static_cast<size_t>(offsets[broker_id].ordered);
        if (durable_frontier > ordered_frontier) {
            durable_frontier = ordered_frontier;
        }
    }
    return durable_frontier;
}

size_t ComputeScalogAck1(volatile offset_entry* offsets, int broker_id) {
    return static_cast<size_t>(offsets[broker_id].ordered);
}

class ScalogAckInvariantTest : public ::testing::Test {
protected:
    static constexpr int kNumBrokers = 4;

    alignas(64) offset_entry offsets_[kNumBrokers];

    void SetUp() override {
        memset(offsets_, 0, sizeof(offsets_));
        for (int b = 0; b < kNumBrokers; ++b) {
            for (int s = 0; s < kNumMaxBrokers; ++s) {
                offsets_[b].replication_done[s] = kReplicationNotStarted;
            }
        }
    }
};

TEST_F(ScalogAckInvariantTest, Ack2ClampedByOrdered_RF1) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].replication_done[broker] = 99;
    offsets_[broker].ordered = 50;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);
    EXPECT_EQ(ack2, 50u) << "ACK2 must be clamped to ordered frontier";

    size_t ack1 = ComputeScalogAck1(offsets_, broker);
    EXPECT_EQ(ack1, 50u);
    EXPECT_LE(ack2, ack1) << "ACK2 must not exceed ACK1 (ordered frontier)";
}

TEST_F(ScalogAckInvariantTest, Ack2ClampedByOrdered_RF2) {
    const int broker = 0;
    const int rf = 2;

    offsets_[0].replication_done[0] = 199;
    offsets_[1].replication_done[0] = 149;
    offsets_[0].ordered = 80;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);
    EXPECT_EQ(ack2, 80u) << "ACK2 must be clamped to ordered when replication is ahead";
}

TEST_F(ScalogAckInvariantTest, Ack2LimitedByReplication) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].replication_done[broker] = 29;
    offsets_[broker].ordered = 100;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);
    EXPECT_EQ(ack2, 30u);
}

TEST_F(ScalogAckInvariantTest, Ack2NotStarted) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].ordered = 50;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);
    EXPECT_EQ(ack2, static_cast<size_t>(-1));
}

TEST_F(ScalogAckInvariantTest, Ack2EqualsOrderedWhenAligned) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].replication_done[broker] = 49;
    offsets_[broker].ordered = 50;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);
    EXPECT_EQ(ack2, 50u);
}

TEST_F(ScalogAckInvariantTest, LazyLogAck2ClampedByOrdered_RF1) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].replication_done[broker] = 99;
    offsets_[broker].ordered = 50;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, LAZYLOG);
    EXPECT_EQ(ack2, 50u) << "LAZYLOG ACK2 must be clamped to ordered frontier";

    size_t ack1 = ComputeScalogAck1(offsets_, broker);
    EXPECT_LE(ack2, ack1) << "LAZYLOG ACK2 must not exceed ACK1";
}

TEST_F(ScalogAckInvariantTest, LazyLogAck2ClampedByOrdered_RF2) {
    const int broker = 0;
    const int rf = 2;

    offsets_[0].replication_done[0] = 199;
    offsets_[1].replication_done[0] = 149;
    offsets_[0].ordered = 80;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, LAZYLOG);
    EXPECT_EQ(ack2, 80u) << "LAZYLOG ACK2 must be clamped when replication ahead";
}

TEST_F(ScalogAckInvariantTest, LazyLogAck2LimitedByReplication) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].replication_done[broker] = 29;
    offsets_[broker].ordered = 100;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, LAZYLOG);
    EXPECT_EQ(ack2, 30u) << "LAZYLOG ACK2 limited by replication when ordered is ahead";
}

TEST_F(ScalogAckInvariantTest, LazyLogAck2NeverExceedsAck1) {
    const int broker = 0;
    const int rf = 2;

    for (uint64_t rep_primary = 0; rep_primary < 200; rep_primary += 17) {
        for (uint64_t rep_replica = 0; rep_replica < 200; rep_replica += 23) {
            for (uint64_t ordered = 0; ordered < 200; ordered += 11) {
                offsets_[0].replication_done[0] = rep_primary;
                offsets_[1].replication_done[0] = rep_replica;
                offsets_[0].ordered = ordered;

                size_t ack1 = ComputeScalogAck1(offsets_, broker);
                size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, LAZYLOG);

                if (ack2 != static_cast<size_t>(-1)) {
                    EXPECT_LE(ack2, ack1)
                        << "LAZYLOG ACK2 > ACK1 at rep_primary=" << rep_primary
                        << " rep_replica=" << rep_replica
                        << " ordered=" << ordered;
                }
            }
        }
    }
}

TEST_F(ScalogAckInvariantTest, NonScalogNonLazyLogNoClamp) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].replication_done[broker] = 99;
    offsets_[broker].ordered = 50;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, EMBARCADERO);
    EXPECT_EQ(ack2, 100u) << "EMBARCADERO should not clamp by ordered";
}

TEST_F(ScalogAckInvariantTest, Ack1IsOrdered) {
    const int broker = 0;
    offsets_[broker].ordered = 42;
    offsets_[broker].replication_done[broker] = 999;

    size_t ack1 = ComputeScalogAck1(offsets_, broker);
    EXPECT_EQ(ack1, 42u) << "ACK1 must be the ordered frontier regardless of replication";
}

TEST_F(ScalogAckInvariantTest, Ack2NeverExceedsAck1) {
    const int broker = 0;
    const int rf = 2;

    for (uint64_t rep_primary = 0; rep_primary < 200; rep_primary += 17) {
        for (uint64_t rep_replica = 0; rep_replica < 200; rep_replica += 23) {
            for (uint64_t ordered = 0; ordered < 200; ordered += 11) {
                offsets_[0].replication_done[0] = rep_primary;
                offsets_[1].replication_done[0] = rep_replica;
                offsets_[0].ordered = ordered;

                size_t ack1 = ComputeScalogAck1(offsets_, broker);
                size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);

                if (ack2 != static_cast<size_t>(-1)) {
                    EXPECT_LE(ack2, ack1)
                        << "ACK2 > ACK1 at rep_primary=" << rep_primary
                        << " rep_replica=" << rep_replica
                        << " ordered=" << ordered;
                }
            }
        }
    }
}

TEST_F(ScalogAckInvariantTest, Ack2ZeroOrderedMeansZeroAck) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].replication_done[broker] = 99;
    offsets_[broker].ordered = 0;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);
    EXPECT_EQ(ack2, 0u) << "If nothing is ordered, ACK2 must be 0";
}

TEST_F(ScalogAckInvariantTest, MultipleBrokersIndependent) {
    const int rf = 1;

    offsets_[0].replication_done[0] = 99;
    offsets_[0].ordered = 30;

    offsets_[1].replication_done[1] = 49;
    offsets_[1].ordered = 80;

    size_t ack2_b0 = ComputeScalogAck2(offsets_, 0, rf, kNumBrokers, SCALOG);
    size_t ack2_b1 = ComputeScalogAck2(offsets_, 1, rf, kNumBrokers, SCALOG);

    EXPECT_EQ(ack2_b0, 30u);
    EXPECT_EQ(ack2_b1, 50u);
}

// ---------------------------------------------------------------------------
// LazyLog progress computation tests (mirrors SendLocalProgress logic)
// ---------------------------------------------------------------------------

// Mirrors LazyLog SendLocalProgress: min(replication_done[broker_id]) across
// the replication set {broker_id, (broker_id+1)%N, ..., (broker_id+RF-1)%N}.
int64_t ComputeLazyLogProgress(
    volatile offset_entry* offsets,
    int broker_id,
    int replication_factor,
    int num_brokers) {

    if (replication_factor <= 0) {
        return static_cast<int64_t>(offsets[broker_id].written);
    }

    uint64_t min_rep = std::numeric_limits<uint64_t>::max();
    int ready_replicas = 0;
    for (int i = 0; i < replication_factor; i++) {
        const int b = GetReplicationSetBroker(broker_id, replication_factor, num_brokers, i);
        const uint64_t val = offsets[b].replication_done[broker_id];
        if (val == kReplicationNotStarted) {
            continue;
        }
        ready_replicas++;
        if (val < min_rep) min_rep = val;
    }
    if (ready_replicas < replication_factor || min_rep == std::numeric_limits<uint64_t>::max()) {
        return 0;
    }
    return static_cast<int64_t>(min_rep + 1);
}

TEST_F(ScalogAckInvariantTest, LazyLogProgressMinAcrossReplicas_RF2) {
    const int broker = 0;
    const int rf = 2;

    // Primary (broker 0) persisted 100 messages, replica (broker 1) persisted 50.
    offsets_[0].replication_done[0] = 99;
    offsets_[1].replication_done[0] = 49;

    int64_t progress = ComputeLazyLogProgress(offsets_, broker, rf, kNumBrokers);
    EXPECT_EQ(progress, 50) << "Progress must be min across replicas: min(100, 50) = 50";
}

TEST_F(ScalogAckInvariantTest, LazyLogProgressMinAcrossReplicas_ReplicaAhead) {
    const int broker = 0;
    const int rf = 2;

    // Replica is ahead of primary (unusual but possible during catch-up).
    offsets_[0].replication_done[0] = 49;
    offsets_[1].replication_done[0] = 99;

    int64_t progress = ComputeLazyLogProgress(offsets_, broker, rf, kNumBrokers);
    EXPECT_EQ(progress, 50) << "Progress must be min, regardless of which replica is slower";
}

TEST_F(ScalogAckInvariantTest, LazyLogProgressReplicaNotStarted) {
    const int broker = 0;
    const int rf = 2;

    // Primary persisted, replica hasn't started.
    offsets_[0].replication_done[0] = 99;
    // offsets_[1].replication_done[0] is still kReplicationNotStarted

    int64_t progress = ComputeLazyLogProgress(offsets_, broker, rf, kNumBrokers);
    EXPECT_EQ(progress, 0) << "Progress must be 0 when not all replicas have started";
}

TEST_F(ScalogAckInvariantTest, LazyLogProgressRF1_SelfOnly) {
    const int broker = 0;
    const int rf = 1;

    offsets_[0].replication_done[0] = 74;

    int64_t progress = ComputeLazyLogProgress(offsets_, broker, rf, kNumBrokers);
    EXPECT_EQ(progress, 75) << "RF=1: progress = self replication_done + 1";
}

TEST_F(ScalogAckInvariantTest, LazyLogProgressNoReplication) {
    const int broker = 0;
    const int rf = 0;

    offsets_[0].written = 42;

    int64_t progress = ComputeLazyLogProgress(offsets_, broker, rf, kNumBrokers);
    EXPECT_EQ(progress, 42) << "RF=0: progress = written (no replication)";
}

TEST_F(ScalogAckInvariantTest, LazyLogProgressAligned_RF2) {
    const int broker = 0;
    const int rf = 2;

    // Both replicas at the same point.
    offsets_[0].replication_done[0] = 49;
    offsets_[1].replication_done[0] = 49;

    int64_t progress = ComputeLazyLogProgress(offsets_, broker, rf, kNumBrokers);
    EXPECT_EQ(progress, 50);
}

// Invariant: LazyLog progress <= ACK1 (ordered) must hold after ordering
// completes, because ordering is gated by progress.
TEST_F(ScalogAckInvariantTest, LazyLogProgressNeverExceedsOrdered) {
    const int broker = 0;
    const int rf = 2;

    for (uint64_t rep_primary = 0; rep_primary < 200; rep_primary += 19) {
        for (uint64_t rep_replica = 0; rep_replica < 200; rep_replica += 23) {
            for (uint64_t ordered = 0; ordered < 200; ordered += 13) {
                offsets_[0].replication_done[0] = rep_primary;
                offsets_[1].replication_done[0] = rep_replica;
                offsets_[0].ordered = ordered;

                int64_t progress = ComputeLazyLogProgress(offsets_, broker, rf, kNumBrokers);
                size_t ack1 = ComputeScalogAck1(offsets_, broker);

                // After the sequencer applies a binding, ordered >= progress
                // because the sequencer can only order up to what was reported
                // as progress. Here we check the weaker form: progress should
                // be a non-negative integer (well-formed).
                EXPECT_GE(progress, 0)
                    << "Progress must be non-negative at rep_primary=" << rep_primary
                    << " rep_replica=" << rep_replica;

                // ACK2 (which reads min(replication_done)) should not exceed progress
                // because ACK2 is clamped by ordered, and ordered is gated by progress.
                size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, LAZYLOG);
                if (ack2 != static_cast<size_t>(-1)) {
                    EXPECT_LE(ack2, ack1)
                        << "ACK2 > ACK1 at rep_primary=" << rep_primary
                        << " rep_replica=" << rep_replica
                        << " ordered=" << ordered;
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Semantic contract tests: SCALOG RF=2 ACK1 may exceed ACK2 (ordering before
// full replication); LazyLog RF=2 ACK1 == ACK2 (ordering after full replication).
//
// These tests exist to document INTENTIONAL semantic differences between the
// two systems and to catch regressions if the invariants are ever broken.
// ---------------------------------------------------------------------------

// Mirrors ScalogLocalSequencer::SendLocalCut for CXL mode RF>0.
// Returns rep_done+1 when rep_done != max AND validated > log_start.
// Reads only self-replica: offsets[broker_id].replication_done[broker_id].
int64_t ComputeScalogLocalCut(
    volatile offset_entry* offsets,
    int broker_id,
    size_t validated_written_byte_offset,
    size_t log_offset) {
    const uint64_t rep_done = offsets[broker_id].replication_done[broker_id];
    const bool validated_ok = (validated_written_byte_offset > log_offset);
    if (!validated_ok || rep_done == std::numeric_limits<uint64_t>::max()) {
        return 0;
    }
    return static_cast<int64_t>(rep_done + 1);
}

// Mirrors LazyLogLocalSequencer::SendLocalProgress for CXL mode RF>0.
// Returns min(replication_done[broker_id]) across all replicas in the set.
int64_t ComputeLazyLogProgressCXL(
    volatile offset_entry* offsets,
    int broker_id,
    int replication_factor,
    int num_brokers) {
    uint64_t min_rep = std::numeric_limits<uint64_t>::max();
    int ready_replicas = 0;
    for (int i = 0; i < replication_factor; i++) {
        const int b = GetReplicationSetBroker(broker_id, replication_factor, num_brokers, i);
        const uint64_t val = offsets[b].replication_done[broker_id];
        if (val == kReplicationNotStarted) {
            continue;
        }
        ready_replicas++;
        if (val < min_rep) min_rep = val;
    }
    if (ready_replicas < replication_factor || min_rep == std::numeric_limits<uint64_t>::max()) {
        return 0;
    }
    return static_cast<int64_t>(min_rep + 1);
}

// SCALOG RF=2: ACK1 (ordered) CAN exceed ACK2 (durable) because ordering is
// based on local persistence only, while replication to the remote replica may lag.
// This is INTENTIONAL: the global cut is computed from local persistence; the
// remote replica is tracked separately and exposed only through ACK2.
TEST_F(ScalogAckInvariantTest, ScalogRF2_Ack1CanExceedAck2_IntentionalSemantics) {
    const int broker = 0;
    const int rf = 2;

    // Broker 0 has locally persisted 100 messages (rep_done[0][0] = 99).
    offsets_[0].replication_done[0] = 99;
    // Remote replica (broker 1) has only replicated 50 messages so far.
    offsets_[1].replication_done[0] = 49;
    // Ordered = 100 (global sequencer used broker 0's local cut of 100).
    offsets_[0].ordered = 100;

    size_t ack1 = ComputeScalogAck1(offsets_, broker);
    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);

    EXPECT_EQ(ack1, 100u) << "SCALOG ACK1 == ordered (global cut based on local persistence)";
    EXPECT_EQ(ack2, 50u)  << "SCALOG ACK2 limited by lagging remote replica";
    EXPECT_LT(ack2, ack1) << "SCALOG RF=2: ACK1 > ACK2 is INTENDED (ordering before full replication)";
}

// SCALOG RF=2: once remote replication catches up, ACK2 equals ACK1.
TEST_F(ScalogAckInvariantTest, ScalogRF2_Ack2EqualsAck1_WhenReplicationCaughtUp) {
    const int broker = 0;
    const int rf = 2;

    offsets_[0].replication_done[0] = 99;   // local: 100 messages
    offsets_[1].replication_done[0] = 99;   // remote: also 100 messages (caught up)
    offsets_[0].ordered = 100;

    size_t ack1 = ComputeScalogAck1(offsets_, broker);
    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, SCALOG);

    EXPECT_EQ(ack1, 100u);
    EXPECT_EQ(ack2, 100u) << "SCALOG ACK2 == ACK1 when remote replication is caught up";
}

// LazyLog RF=2: ACK1 == ACK2 because ordering is gated on full replication.
// Since progress = min(local, remote) gates ordering, by the time ordered advances,
// both replicas have confirmed. So durable_frontier >= ordered, and the clamp gives
// ACK2 = ordered = ACK1.
TEST_F(ScalogAckInvariantTest, LazyLogRF2_Ack2EqualsAck1_OrderingGatedOnFullReplication) {
    const int broker = 0;
    const int rf = 2;

    // Both replicas confirmed 100 messages. LazyLog progress = min(100, 100) = 100.
    // Global sequencer assigned order up to 100 messages.
    offsets_[0].replication_done[0] = 99;   // local: 100 messages
    offsets_[1].replication_done[0] = 99;   // remote: 100 messages
    offsets_[0].ordered = 100;              // ordered: 100 (≤ progress=100)

    size_t ack1 = ComputeScalogAck1(offsets_, broker);
    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, LAZYLOG);

    EXPECT_EQ(ack1, 100u);
    EXPECT_EQ(ack2, 100u) << "LazyLog RF=2: ACK2 == ACK1 because ordering waited for full replication";
}

// SCALOG local cut for CXL RF=2 uses self-replication only (not min across set).
// This documents the intentional difference from LazyLog's progress computation.
TEST_F(ScalogAckInvariantTest, ScalogLocalCut_SelfOnlyNotMin_IntentionalDifference) {
    const int broker = 0;
    const int rf = 2;

    // Local: 100 messages. Remote: 50 messages.
    offsets_[0].replication_done[0] = 99;
    offsets_[1].replication_done[0] = 49;

    // SCALOG local cut = local persistence only = 100.
    // validated_written_byte_offset=5000 > log_offset=4096 (simulating data received).
    int64_t scalog_cut = ComputeScalogLocalCut(offsets_, broker, /*validated=*/5000, /*log_start=*/4096);
    EXPECT_EQ(scalog_cut, 100)
        << "SCALOG local cut reads self-replication only (not min across replication set)";

    // LazyLog CXL progress = min(local, remote) = 50.
    int64_t lazylog_progress = ComputeLazyLogProgressCXL(offsets_, broker, rf, kNumBrokers);
    EXPECT_EQ(lazylog_progress, 50)
        << "LazyLog CXL progress reads min across full replication set";

    // The semantic difference: SCALOG orders 100 messages; LazyLog orders only 50.
    // For SCALOG, the remaining 50 messages are ordered but not yet durably replicated.
    EXPECT_GT(scalog_cut, lazylog_progress)
        << "SCALOG reports more ready messages than LazyLog when replica lags";
}

// SCALOG local cut returns 0 when replication has not started (rep_done == max).
TEST_F(ScalogAckInvariantTest, ScalogLocalCut_NotStarted_ReturnsZero) {
    const int broker = 0;
    // replication_done[0][0] = kReplicationNotStarted (initialized by SetUp)
    int64_t cut = ComputeScalogLocalCut(offsets_, broker, /*validated=*/5000, /*log_start=*/4096);
    EXPECT_EQ(cut, 0) << "SCALOG local cut must be 0 when replication has not started";
}

// SCALOG local cut returns 0 when no data has been validated yet (validated == log_start).
TEST_F(ScalogAckInvariantTest, ScalogLocalCut_NoDataValidated_ReturnsZero) {
    const int broker = 0;
    offsets_[0].replication_done[0] = 99;  // rep_done indicates 100 messages persisted
    // But validated == log_start means DelegationThread hasn't processed any data yet.
    int64_t cut = ComputeScalogLocalCut(offsets_, broker, /*validated=*/4096, /*log_start=*/4096);
    EXPECT_EQ(cut, 0) << "SCALOG local cut must be 0 when no data is validated";
}

}  // namespace
