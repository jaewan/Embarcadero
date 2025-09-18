#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include "../../src/embarlet/buffer_manager.h"
#include "../../src/embarlet/segment_manager.h"

using namespace Embarcadero;
using ::testing::_;
using ::testing::Return;
using ::testing::Invoke;

class MockSegmentManager : public ISegmentManager {
public:
    MOCK_METHOD(void*, GetNewSegment, (size_t size, size_t msg_size, size_t& segment_size, SegmentMetadata& metadata), (override));
    MOCK_METHOD(bool, CheckSegmentBoundary, (void* log, size_t msg_size, SegmentMetadata& metadata), (override));
};

class BufferManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Setup test memory
        test_memory_ = aligned_alloc(64, memory_size_);
        memset(test_memory_, 0, memory_size_);
        
        // Initialize atomic log address
        log_addr_ = reinterpret_cast<unsigned long long int>(test_memory_) + 1024;
        
        // Create buffer manager
        buffer_manager_ = std::make_unique<BufferManager>(
            test_memory_,
            test_memory_,
            log_addr_,
            0, // batch_headers_addr
            0  // broker_id
        );
        
        // Create and set mock segment manager
        mock_segment_manager_ = std::make_shared<MockSegmentManager>();
        buffer_manager_->SetSegmentManager(mock_segment_manager_);
    }
    
    void TearDown() override {
        free(test_memory_);
    }

    static constexpr size_t memory_size_ = 1024 * 1024; // 1MB
    void* test_memory_;
    std::atomic<unsigned long long int> log_addr_;
    std::unique_ptr<BufferManager> buffer_manager_;
    std::shared_ptr<MockSegmentManager> mock_segment_manager_;
};

TEST_F(BufferManagerTest, KafkaBufferAllocation) {
    BatchHeader batch_header;
    batch_header.total_size = 256;
    batch_header.num_msg = 5;
    
    void* log;
    size_t logical_offset;
    std::function<void(size_t, size_t)> callback;
    
    // Expect segment boundary check
    EXPECT_CALL(*mock_segment_manager_, CheckSegmentBoundary(_, 256, _))
        .WillOnce(Return(true));
    
    // Allocate buffer
    buffer_manager_->GetCXLBuffer(batch_header, log, logical_offset, callback);
    
    // Verify allocation
    EXPECT_NE(log, nullptr);
    EXPECT_EQ(logical_offset, 0);
    EXPECT_TRUE(callback);
    
    // Test callback
    bool callback_called = false;
    callback = [&callback_called](size_t start, size_t end) {
        callback_called = true;
        EXPECT_EQ(start, 0);
        EXPECT_EQ(end, 5);
    };
    callback(0, 5);
    EXPECT_TRUE(callback_called);
}

TEST_F(BufferManagerTest, SegmentBoundaryHandling) {
    BatchHeader batch_header;
    batch_header.total_size = 512;
    
    void* log;
    size_t logical_offset;
    std::function<void(size_t, size_t)> callback;
    
    // Mock segment boundary exceeded
    SegmentMetadata new_segment_metadata;
    new_segment_metadata.is_new_segment = true;
    new_segment_metadata.segment_start = reinterpret_cast<uint8_t*>(test_memory_) + 2048;
    new_segment_metadata.segment_size = 4096;
    
    EXPECT_CALL(*mock_segment_manager_, CheckSegmentBoundary(_, 512, _))
        .WillOnce(Invoke([&](void* log, size_t msg_size, SegmentMetadata& metadata) {
            metadata = new_segment_metadata;
            return true;
        }));
    
    // Allocate buffer
    buffer_manager_->GetCXLBuffer(batch_header, log, logical_offset, callback);
    
    // Verify new segment was used
    EXPECT_EQ(log, new_segment_metadata.segment_start);
}

TEST_F(BufferManagerTest, ConcurrentAllocation) {
    const int num_threads = 4;
    const int allocations_per_thread = 100;
    std::vector<std::thread> threads;
    std::atomic<int> successful_allocations{0};
    
    // Allow all segment boundary checks
    EXPECT_CALL(*mock_segment_manager_, CheckSegmentBoundary(_, _, _))
        .WillRepeatedly(Return(true));
    
    // Launch threads
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([this, &successful_allocations, t]() {
            for (int i = 0; i < allocations_per_thread; ++i) {
                BatchHeader batch_header;
                batch_header.total_size = 64;
                batch_header.client_id = t;
                batch_header.batch_seq = i;
                
                void* log;
                size_t logical_offset;
                std::function<void(size_t, size_t)> callback;
                
                buffer_manager_->GetCXLBuffer(batch_header, log, logical_offset, callback);
                
                if (log != nullptr) {
                    successful_allocations++;
                }
            }
        });
    }
    
    // Wait for completion
    for (auto& thread : threads) {
        thread.join();
    }
    
    // Verify all allocations succeeded
    EXPECT_EQ(successful_allocations, num_threads * allocations_per_thread);
}
