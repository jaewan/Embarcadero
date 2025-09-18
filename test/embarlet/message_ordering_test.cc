#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "../../src/embarlet/message_ordering.h"
#include <chrono>

using namespace Embarcadero;
using namespace std::chrono_literals;

class MessageOrderingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup test memory
        test_memory_ = aligned_alloc(64, memory_size_);
        memset(test_memory_, 0, memory_size_);
        
        // Setup TInode
        tinode_ = reinterpret_cast<TInode*>(test_memory_);
        tinode_->offsets[0].log_offset = 1024;
        tinode_->offsets[0].batch_headers_offset = 2048;
        
        // Create message ordering
        message_ordering_ = std::make_unique<MessageOrdering>(
            test_memory_,
            tinode_,
            0 // broker_id
        );
    }
    
    void TearDown() override {
        message_ordering_.reset();
        free(test_memory_);
    }

    static constexpr size_t memory_size_ = 1024 * 1024; // 1MB
    void* test_memory_;
    TInode* tinode_;
    std::unique_ptr<MessageOrdering> message_ordering_;
};

TEST_F(MessageOrderingTest, StartStopSequencer) {
    // Start sequencer
    message_ordering_->StartSequencer(CORFU, 0, "test_topic");
    
    // Give it time to start
    std::this_thread::sleep_for(10ms);
    
    // Stop sequencer
    message_ordering_->StopSequencer();
    
    // Should complete without hanging
    SUCCEED();
}

TEST_F(MessageOrderingTest, OrderedCountInitiallyZero) {
    EXPECT_EQ(message_ordering_->GetOrderedCount(), 0);
}

TEST_F(MessageOrderingTest, MessageCombinerBasic) {
    void* first_msg_addr = reinterpret_cast<uint8_t*>(test_memory_) + 1024 + CACHELINE_SIZE;
    
    MessageCombiner combiner(
        test_memory_,
        first_msg_addr,
        tinode_,
        nullptr, // replica_tinode
        0        // broker_id
    );
    
    // Start combiner
    combiner.Start();
    
    // Initial state
    EXPECT_EQ(combiner.GetLogicalOffset(), 0);
    EXPECT_EQ(combiner.GetWrittenLogicalOffset(), static_cast<size_t>(-1));
    
    // Stop combiner
    combiner.Stop();
}

TEST_F(MessageOrderingTest, CombinerMessageProcessing) {
    void* first_msg_addr = reinterpret_cast<uint8_t*>(test_memory_) + 1024 + CACHELINE_SIZE;
    
    // Setup a test message
    MessageHeader* msg = reinterpret_cast<MessageHeader*>(first_msg_addr);
    msg->paddedSize = 128;
    msg->complete = 0;
    
    MessageCombiner combiner(
        test_memory_,
        first_msg_addr,
        tinode_,
        nullptr,
        0
    );
    
    combiner.Start();
    
    // Wait a bit
    std::this_thread::sleep_for(10ms);
    
    // Mark message as complete
    msg->complete = 1;
    
    // Wait for processing
    std::this_thread::sleep_for(50ms);
    
    // Check if message was processed
    EXPECT_EQ(combiner.GetLogicalOffset(), 1);
    EXPECT_EQ(combiner.GetWrittenLogicalOffset(), 0);
    EXPECT_EQ(combiner.GetWrittenPhysicalAddr(), msg);
    
    combiner.Stop();
}

// Test for batch ordering logic
class BatchOrderingTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup test batch headers
        for (int i = 0; i < 10; ++i) {
            BatchHeader header;
            header.client_id = i % 3;
            header.batch_seq = i / 3;
            header.num_msg = 5;
            header.log_idx = i * 512;
            batches_.push_back(header);
        }
    }
    
    std::vector<BatchHeader> batches_;
};

TEST_F(BatchOrderingTest, BatchSequenceOrdering) {
    // Test that batches are processed in sequence order
    std::vector<size_t> expected_order = {0, 3, 6, 1, 4, 7, 2, 5, 8};
    std::vector<size_t> actual_order;
    
    // Simulate ordering logic
    std::map<size_t, std::map<size_t, size_t>> client_batches;
    
    for (size_t i = 0; i < batches_.size(); ++i) {
        const auto& batch = batches_[i];
        client_batches[batch.client_id][batch.batch_seq] = i;
    }
    
    // Process in order
    for (const auto& [client_id, batch_map] : client_batches) {
        for (const auto& [batch_seq, index] : batch_map) {
            actual_order.push_back(index);
        }
    }
    
    EXPECT_EQ(actual_order, expected_order);
}
