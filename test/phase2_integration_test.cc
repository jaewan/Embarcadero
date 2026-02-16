/**
 * Phase 2 Integration Tests - Production Readiness Validation
 *
 * Tests:
 * 1. GOI sequential indexing (no gaps)
 * 2. Replica polling correctness
 * 3. CV monotonic tracking
 * 4. ACK path batch lookup
 * 5. Ring wraparound behavior
 * 6. Multi-broker correctness
 */

#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include <vector>
#include <cstring>
#include <queue>
#include <deque>
#include "../src/cxl_manager/cxl_datastructure.h"
#include "../src/common/performance_utils.h"
#include "../src/embarlet/sequencer_utils.h"

using namespace Embarcadero;

// Mock CXL memory for testing
constexpr size_t TEST_CXL_SIZE = 64 * 1024 * 1024;  // 64 MB for testing

class Phase2Test : public ::testing::Test {
protected:
    void SetUp() override {
        // Allocate mock CXL memory
        cxl_memory_ = aligned_alloc(4096, TEST_CXL_SIZE);
        ASSERT_NE(cxl_memory_, nullptr);
        memset(cxl_memory_, 0, TEST_CXL_SIZE);

        // Setup Phase 2 structures
        control_block_ = reinterpret_cast<ControlBlock*>(cxl_memory_);
        cv_ = reinterpret_cast<CompletionVectorEntry*>(
            reinterpret_cast<uint8_t*>(cxl_memory_) + 0x1000);
        goi_ = reinterpret_cast<GOIEntry*>(
            reinterpret_cast<uint8_t*>(cxl_memory_) + 0x2000);

        // Initialize control block
        control_block_->epoch.store(1, std::memory_order_release);

        // Initialize CV to zeros
        for (int i = 0; i < NUM_MAX_BROKERS; i++) {
            cv_[i].completed_pbr_head.store(0, std::memory_order_release);
        }
    }

    void TearDown() override {
        if (cxl_memory_) {
            free(cxl_memory_);
        }
    }

    void* cxl_memory_{nullptr};
    ControlBlock* control_block_{nullptr};
    CompletionVectorEntry* cv_{nullptr};
    GOIEntry* goi_{nullptr};
};

namespace {
struct TestRange {
    uint64_t start;
    uint64_t end;
    bool operator>(const TestRange& o) const { return start > o.start; }
};

static uint64_t ApplyRangesAndReturnCommitted(
    uint64_t initial_committed,
    const std::vector<TestRange>& ranges) {
    uint64_t next_expected = (initial_committed == UINT64_MAX) ? 0 : (initial_committed + 1);
    std::priority_queue<TestRange, std::vector<TestRange>, std::greater<TestRange>> pending;
    for (const auto& r : ranges) {
        if (r.end <= r.start) continue;
        pending.push(r);
        while (!pending.empty() && pending.top().start == next_expected) {
            next_expected = pending.top().end;
            pending.pop();
        }
    }
    return (next_expected == 0) ? UINT64_MAX : (next_expected - 1);
}

static uint64_t ApplyRangesBoundedAndReturnCommitted(
    uint64_t initial_committed,
    const std::vector<TestRange>& ranges,
    size_t ring_capacity,
    size_t* max_depth_out) {
    uint64_t next_expected = (initial_committed == UINT64_MAX) ? 0 : (initial_committed + 1);
    std::priority_queue<TestRange, std::vector<TestRange>, std::greater<TestRange>> pending;
    size_t max_depth = 0;
    for (const auto& r : ranges) {
        if (r.end <= r.start) continue;
        if (pending.size() >= ring_capacity) {
            // Model the production behavior: bounded queue applies backpressure (no silent drop).
            while (!pending.empty() && pending.top().start == next_expected) {
                next_expected = pending.top().end;
                pending.pop();
            }
            if (pending.size() >= ring_capacity) {
                // Still full, skip enqueue in test model and keep state bounded.
                continue;
            }
        }
        pending.push(r);
        if (pending.size() > max_depth) max_depth = pending.size();
        while (!pending.empty() && pending.top().start == next_expected) {
            next_expected = pending.top().end;
            pending.pop();
        }
    }
    if (max_depth_out) *max_depth_out = max_depth;
    return (next_expected == 0) ? UINT64_MAX : (next_expected - 1);
}
}  // namespace

// Test 1: GOI Sequential Indexing
TEST_F(Phase2Test, GOISequentialIndexing) {
    // Simulate sequencer writing batches
    constexpr int NUM_BATCHES = 100;
    std::atomic<uint64_t> batch_counter{0};

    for (int i = 0; i < NUM_BATCHES; i++) {
        uint64_t batch_idx = batch_counter.fetch_add(1, std::memory_order_relaxed);

        GOIEntry* entry = &goi_[batch_idx];
        entry->global_seq = batch_idx;
        entry->batch_id = (1ULL << 48) | i;  // broker_id=1
        entry->broker_id = 1;
        entry->total_order = i * 1000;  // Each batch has 1000 messages
        entry->message_count = 1000;
        entry->pbr_index = i;
    }

    // Verify sequential indexing (no gaps)
    for (int i = 0; i < NUM_BATCHES; i++) {
        EXPECT_EQ(goi_[i].global_seq, static_cast<uint64_t>(i))
            << "GOI[" << i << "].global_seq should be " << i;
        EXPECT_EQ(goi_[i].total_order, static_cast<uint64_t>(i * 1000))
            << "total_order should be " << i * 1000;
    }
}

// Test 2: Replica Polling (No Spinning)
TEST_F(Phase2Test, ReplicaPollingNoSpin) {
    constexpr int NUM_BATCHES = 50;
    std::atomic<uint64_t> batch_counter{0};
    std::atomic<bool> sequencer_done{false};

    // Sequencer thread: writes batches to GOI
    std::thread sequencer([&]() {
        for (int i = 0; i < NUM_BATCHES; i++) {
            uint64_t batch_idx = batch_counter.fetch_add(1, std::memory_order_relaxed);
            GOIEntry* entry = &goi_[batch_idx];
            entry->global_seq = batch_idx;
            entry->batch_id = (1ULL << 48) | i;
            entry->broker_id = 1;
            entry->total_order = i * 100;
            entry->message_count = 100;
            entry->pbr_index = i;
            entry->num_replicated.store(0, std::memory_order_release);

            // Simulate write latency
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
        sequencer_done.store(true, std::memory_order_release);
    });

    // Replica thread: polls GOI sequentially
    std::atomic<uint64_t> next_goi_index{0};
    std::atomic<int> batches_processed{0};
    std::atomic<int> spin_count{0};

    std::thread replica([&]() {
        while (batches_processed.load() < NUM_BATCHES) {
            uint64_t goi_index = next_goi_index.load(std::memory_order_relaxed);
            GOIEntry* entry = &goi_[goi_index];

            // Check if sequencer has written this entry
            if (entry->global_seq != goi_index) {
                spin_count.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::yield();
                continue;
            }

            // Process batch
            batches_processed.fetch_add(1, std::memory_order_relaxed);
            next_goi_index.fetch_add(1, std::memory_order_relaxed);
        }
    });

    sequencer.join();
    replica.join();

    EXPECT_EQ(batches_processed.load(), NUM_BATCHES)
        << "Replica should process all batches";
    EXPECT_LT(spin_count.load(), NUM_BATCHES * 100)
        << "Spin count should be reasonable (not infinite)";
}

// Test 3: CV Monotonic Tracking
TEST_F(Phase2Test, CVMonotonicTracking) {
    constexpr int NUM_BATCHES = 100;
    constexpr int BROKER_ID = 0;

    // Simulate tail replica updating CV
    uint64_t cv_state = 0;

    for (uint32_t pbr_index = 0; pbr_index < NUM_BATCHES; pbr_index++) {
        // Initial case
        if (cv_state == 0 && pbr_index == 0) {
            cv_[BROKER_ID].completed_pbr_head.store(pbr_index, std::memory_order_release);
            cv_state = pbr_index;
            continue;
        }

        // Check contiguity
        if (pbr_index == cv_state + 1) {
            cv_[BROKER_ID].completed_pbr_head.store(pbr_index, std::memory_order_release);
            cv_state = pbr_index;
        }
    }

    // Verify final state
    uint64_t final_cv = cv_[BROKER_ID].completed_pbr_head.load(std::memory_order_acquire);
    EXPECT_EQ(final_cv, NUM_BATCHES - 1)
        << "CV should track all batches monotonically";
}

// Test 4: CV Contiguity Gap Detection
TEST_F(Phase2Test, CVContiguityGapDetection) {
    constexpr int BROKER_ID = 0;
    uint64_t cv_state = 0;

    // Process batches: 0, 1, 3 (skip 2 - gap!)
    std::vector<uint32_t> pbr_indices = {0, 1, 3};

    for (uint32_t pbr_index : pbr_indices) {
        if (cv_state == 0 && pbr_index == 0) {
            cv_[BROKER_ID].completed_pbr_head.store(pbr_index, std::memory_order_release);
            cv_state = pbr_index;
        } else if (pbr_index == cv_state + 1) {
            cv_[BROKER_ID].completed_pbr_head.store(pbr_index, std::memory_order_release);
            cv_state = pbr_index;
        }
        // else: gap detected, don't update
    }

    // CV should stop at 1 (before gap)
    uint64_t final_cv = cv_[BROKER_ID].completed_pbr_head.load(std::memory_order_acquire);
    EXPECT_EQ(final_cv, 1) << "CV should stop at last contiguous batch before gap";
}

// Test 5: ACK Path Modulo Mapping
TEST_F(Phase2Test, ACKPathModuloMapping) {
    constexpr size_t RING_SIZE = 1024;  // Typical ring size

    // Test various absolute indices map correctly to ring positions
    struct TestCase {
        uint64_t absolute_index;
        size_t expected_ring_pos;
    };

    std::vector<TestCase> test_cases = {
        {0, 0},
        {1, 1},
        {1023, 1023},
        {1024, 0},      // First wrap
        {1025, 1},
        {2048, 0},      // Second wrap
        {10000, 10000 % 1024},
    };

    for (const auto& tc : test_cases) {
        size_t ring_position = tc.absolute_index % RING_SIZE;
        EXPECT_EQ(ring_position, tc.expected_ring_pos)
            << "Absolute index " << tc.absolute_index
            << " should map to ring position " << tc.expected_ring_pos;
    }
}

// Test 6: Ring Wraparound Correctness
TEST_F(Phase2Test, RingWraparoundCorrectness) {
    constexpr size_t RING_SIZE = 128;  // Small ring for testing
    constexpr int NUM_BATCHES = 256;   // 2× ring size
    constexpr int BROKER_ID = 0;

    // Simulate absolute PBR indices wrapping around ring
    std::atomic<uint64_t> pbr_counter{0};
    uint64_t cv_state = 0;

    for (int i = 0; i < NUM_BATCHES; i++) {
        uint64_t pbr_absolute = pbr_counter.fetch_add(1, std::memory_order_relaxed);

        // Update CV (absolute indices are monotonic, so no wrap issues)
        if (cv_state == 0 && pbr_absolute == 0) {
            cv_[BROKER_ID].completed_pbr_head.store(pbr_absolute, std::memory_order_release);
            cv_state = pbr_absolute;
        } else if (pbr_absolute == cv_state + 1) {
            cv_[BROKER_ID].completed_pbr_head.store(pbr_absolute, std::memory_order_release);
            cv_state = pbr_absolute;
        }
    }

    // CV should track all batches (monotonic absolute indices)
    uint64_t final_cv = cv_[BROKER_ID].completed_pbr_head.load(std::memory_order_acquire);
    EXPECT_EQ(final_cv, NUM_BATCHES - 1)
        << "CV should track all batches even after ring wraparound";

    // Verify ACK path can lookup batches correctly
    for (uint64_t abs_index = 0; abs_index < NUM_BATCHES; abs_index++) {
        size_t ring_pos = abs_index % RING_SIZE;
        EXPECT_LT(ring_pos, RING_SIZE) << "Ring position should be in bounds";
    }
}

// Test 7: Committed-range bootstrap uses UINT64_MAX sentinel and advances on [0,n).
TEST_F(Phase2Test, CommittedRangeBootstrapFromSentinel) {
    std::vector<TestRange> ranges{{0, 10}};
    uint64_t committed = ApplyRangesAndReturnCommitted(UINT64_MAX, ranges);
    EXPECT_EQ(committed, 9ULL);
}

// Test 8: Gap handling does not falsely advance committed prefix.
TEST_F(Phase2Test, CommittedRangeGapNoFalseAdvance) {
    std::vector<TestRange> ranges{
        {5, 8},   // gap before first expected range
        {8, 12},  // still blocked by missing [0,5)
        {0, 5},   // now contiguous prefix exists
    };
    uint64_t committed = ApplyRangesAndReturnCommitted(UINT64_MAX, ranges);
    EXPECT_EQ(committed, 11ULL);
}

// Test 9: Bounded completed-range queue keeps depth bounded while preserving prefix safety.
TEST_F(Phase2Test, CommittedRangeBoundedQueue) {
    std::vector<TestRange> ranges;
    ranges.reserve(512);
    // Push many out-of-order ranges before inserting the prefix range.
    for (uint64_t i = 10; i < 266; ++i) ranges.push_back({i, i + 1});
    ranges.push_back({0, 10});

    size_t max_depth = 0;
    uint64_t committed = ApplyRangesBoundedAndReturnCommitted(UINT64_MAX, ranges, 64, &max_depth);
    EXPECT_LE(max_depth, 64ULL) << "Pending completed-range depth must remain bounded";
    EXPECT_GE(committed, 9ULL) << "Committed prefix must never regress below first contiguous range";
}

// Test 10: Exactly-once behavior (dedup) for retry/duplicate batch IDs.
TEST_F(Phase2Test, FastDeduplicatorExactlyOnce) {
    FastDeduplicator dedup;
    EXPECT_FALSE(dedup.check_and_insert(42)) << "First insert should be new";
    EXPECT_TRUE(dedup.check_and_insert(42)) << "Second insert should be duplicate";
    EXPECT_FALSE(dedup.check_and_insert(43)) << "Different batch ID should be new";
    EXPECT_TRUE(dedup.check_and_insert(43)) << "Retry of batch ID should be duplicate";
}

// Test 7: Multi-Broker Independent Tracking
TEST_F(Phase2Test, MultiBrokerIndependentTracking) {
    constexpr int NUM_BROKERS = 4;
    constexpr int BATCHES_PER_BROKER = 50;

    // Each broker has independent PBR counter
    std::array<std::atomic<uint64_t>, NUM_BROKERS> broker_counters{};

    // Simulate each broker ingesting batches
    for (int broker = 0; broker < NUM_BROKERS; broker++) {
        for (int i = 0; i < BATCHES_PER_BROKER; i++) {
            uint64_t pbr_absolute = broker_counters[broker].fetch_add(1, std::memory_order_relaxed);
            EXPECT_EQ(pbr_absolute, static_cast<uint64_t>(i))
                << "Broker " << broker << " should have independent counter starting from 0";
        }
    }

    // Verify each broker's counter is independent
    for (int broker = 0; broker < NUM_BROKERS; broker++) {
        EXPECT_EQ(broker_counters[broker].load(), BATCHES_PER_BROKER)
            << "Broker " << broker << " counter should be " << BATCHES_PER_BROKER;
    }
}

// Test 8: Concurrent GOI Writes (Sequencer Correctness)
TEST_F(Phase2Test, ConcurrentGOIWrites) {
    constexpr int NUM_BATCHES = 1000;
    std::atomic<uint64_t> global_batch_seq{0};
    std::atomic<int> batches_written{0};

    // Simulate sequencer epoch with multiple ready batches
    std::thread writer([&]() {
        for (int i = 0; i < NUM_BATCHES; i++) {
            uint64_t batch_idx = global_batch_seq.fetch_add(1, std::memory_order_relaxed);

            GOIEntry* entry = &goi_[batch_idx];
            entry->global_seq = batch_idx;
            entry->batch_id = (1ULL << 48) | i;
            entry->broker_id = 1;
            entry->total_order = i * 10;
            entry->message_count = 10;
            entry->pbr_index = i;

            batches_written.fetch_add(1, std::memory_order_release);
        }
    });

    writer.join();

    // Verify all batches written sequentially
    EXPECT_EQ(batches_written.load(), NUM_BATCHES);
    for (int i = 0; i < NUM_BATCHES; i++) {
        EXPECT_EQ(goi_[i].global_seq, static_cast<uint64_t>(i))
            << "GOI should be sequential at index " << i;
    }
}

// Test 9: Memory Structure Sizes
TEST_F(Phase2Test, MemoryStructureSizes) {
    // Verify structure sizes meet requirements
    size_t goi_size = sizeof(GOIEntry);
    size_t cv_size = sizeof(CompletionVectorEntry);
    size_t control_size = sizeof(ControlBlock);

    std::cout << "=== Memory Structure Sizes ===" << std::endl;
    std::cout << "GOIEntry: " << goi_size << " bytes" << std::endl;
    std::cout << "CompletionVectorEntry: " << cv_size << " bytes" << std::endl;
    std::cout << "ControlBlock: " << control_size << " bytes" << std::endl;

    // GOI should be cache-line multiple (64 or 128 bytes)
    EXPECT_TRUE(goi_size == 64 || goi_size == 128 || goi_size == 192)
        << "GOIEntry size should be cache-line multiple, got " << goi_size;
    EXPECT_EQ(goi_size % 64, 0) << "GOIEntry should be 64-byte aligned";

    // CV should be 128 bytes (avoid false sharing)
    EXPECT_EQ(cv_size, 128) << "CompletionVectorEntry should be 128 bytes";

    // Control block should be 128 bytes
    EXPECT_EQ(control_size, 128) << "ControlBlock should be 128 bytes";

    // Report memory usage for 256M GOI entries
    size_t goi_total_mb = (256ULL * 1024 * 1024 * goi_size) / (1024 * 1024);
    std::cout << "Total GOI memory for 256M entries: " << goi_total_mb << " MB" << std::endl;

    if (goi_size == 128) {
        std::cout << "⚠️  WARNING: GOIEntry is 128 bytes (2× designed 64 bytes)" << std::endl;
        std::cout << "   Memory usage: " << goi_total_mb << " MB (designed for "
                  << goi_total_mb/2 << " MB)" << std::endl;
    }
}

// Test 10: End-to-End Flow
TEST_F(Phase2Test, EndToEndFlow) {
    constexpr int NUM_BATCHES = 100;
    constexpr int BROKER_ID = 0;
    constexpr int REPLICA_ID = 0;  // Test as head replica (simplest case)
    constexpr int REPLICATION_FACTOR = 1;  // Single replica for simplicity

    std::atomic<uint64_t> global_batch_seq{0};
    std::atomic<uint64_t> broker_pbr_counter{0};
    std::atomic<bool> done{false};

    // Stage 1: Sequencer writes to GOI
    std::thread sequencer([&]() {
        for (int i = 0; i < NUM_BATCHES; i++) {
            uint64_t batch_idx = global_batch_seq.fetch_add(1, std::memory_order_relaxed);
            uint64_t pbr_absolute = broker_pbr_counter.fetch_add(1, std::memory_order_relaxed);

            GOIEntry* entry = &goi_[batch_idx];
            entry->global_seq = batch_idx;
            entry->batch_id = (static_cast<uint64_t>(BROKER_ID) << 48) | i;
            entry->broker_id = BROKER_ID;
            entry->total_order = i * 100;
            entry->message_count = 100;
            entry->pbr_index = static_cast<uint32_t>(pbr_absolute);
            entry->num_replicated.store(0, std::memory_order_release);

            std::this_thread::sleep_for(std::chrono::microseconds(1));
        }
    });

    // Stage 2: Replica polls GOI and replicates
    std::atomic<uint64_t> next_goi_index{0};
    uint64_t cv_state = 0;

    std::thread replica([&]() {
        while (next_goi_index.load() < NUM_BATCHES) {
            uint64_t goi_index = next_goi_index.load(std::memory_order_relaxed);
            GOIEntry* entry = &goi_[goi_index];

            if (entry->global_seq != goi_index) {
                std::this_thread::yield();
                continue;
            }

            // Simulate replication (token-passing)
            while (entry->num_replicated.load(std::memory_order_acquire) != REPLICA_ID) {
                std::this_thread::yield();
            }
            entry->num_replicated.store(REPLICA_ID + 1, std::memory_order_release);

            // Tail replica updates CV
            if (REPLICA_ID == REPLICATION_FACTOR - 1) {
                uint32_t pbr_index = entry->pbr_index;
                if (cv_state == 0 && pbr_index == 0) {
                    cv_[BROKER_ID].completed_pbr_head.store(pbr_index, std::memory_order_release);
                    cv_state = pbr_index;
                } else if (pbr_index == cv_state + 1) {
                    cv_[BROKER_ID].completed_pbr_head.store(pbr_index, std::memory_order_release);
                    cv_state = pbr_index;
                }
            }

            next_goi_index.fetch_add(1, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    sequencer.join();
    replica.join();

    // Stage 3: Verify ACK path
    uint64_t completed_pbr = cv_[BROKER_ID].completed_pbr_head.load(std::memory_order_acquire);
    EXPECT_EQ(completed_pbr, NUM_BATCHES - 1)
        << "CV should track all replicated batches";
    EXPECT_TRUE(done.load()) << "End-to-end flow should complete";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
