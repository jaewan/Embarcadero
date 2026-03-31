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

    if (seq_type == SCALOG) {
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

TEST_F(ScalogAckInvariantTest, NonScalogNoClamp) {
    const int broker = 0;
    const int rf = 1;

    offsets_[broker].replication_done[broker] = 99;
    offsets_[broker].ordered = 50;

    size_t ack2 = ComputeScalogAck2(offsets_, broker, rf, kNumBrokers, EMBARCADERO);
    EXPECT_EQ(ack2, 100u) << "Non-SCALOG should not clamp by ordered";
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

}  // namespace
