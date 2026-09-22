#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <thread>
#include "disk_manager/chain_replication.h"
#include "common/performance_utils.h"

TEST(ChainShutdown, MissingPredecessorCancelsWithoutFalseDurability) {
    using namespace Embarcadero;
    setenv("EMBARCADERO_CHAIN_REPLICATION_SINK", "memory-accounting", 1);
    CXL::SetExplicitFlushRequired(false);
    alignas(128) ControlBlock control{};
    alignas(128) GOIEntry goi[2]{};
    alignas(128) CompletionVectorEntry completion[NUM_MAX_BROKERS]{};
    control.epoch.store(1);
    control.committed_seq.store(0);
    goi[0].global_seq = 0;
    goi[0].broker_id = 0;
    goi[0].epoch_sequenced = 1;
    goi[0].payload_size = 64;
    goi[0].message_count = 1;
    goi[0].cumulative_message_count = 1;
    // Tail owns role 1; absent predecessor never advances the initial token.
    ChainReplicationManager manager(1, 2, 1, 2, &control, goi, completion, "unused");
    manager.Start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto start = std::chrono::steady_clock::now();
    manager.Stop();
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    EXPECT_EQ(goi[0].num_replicated.load(), 0u);
    EXPECT_EQ(completion[0].completed_logical_offset.load(), 0u);
    unsetenv("EMBARCADERO_CHAIN_REPLICATION_SINK");
}
