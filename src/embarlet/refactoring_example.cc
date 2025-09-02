/**
 * Example demonstrating the refactored modular architecture
 * This shows how the original monolithic Topic class can be replaced
 * with specialized, testable components
 */

#include "topic_refactored.h"
#include "buffer_manager.h"
#include "message_ordering.h"
#include "replication_manager.h"
#include "message_export.h"
#include "segment_manager.h"
#include <glog/logging.h>

namespace Embarcadero {

/**
 * Example of how to use the refactored components
 */
class RefactoringExample {
public:
    static void DemonstrateModularArchitecture() {
        // Setup parameters
        std::string topic_name = "example_topic";
        void* cxl_addr = /* CXL memory address */;
        TInode* tinode = /* Topic inode */;
        TInode* replica_tinode = /* Replica inode */;
        int broker_id = 0;
        SequencerType seq_type = KAFKA;
        int order = 4;
        int ack_level = 1;
        int replication_factor = 3;

        // 1. Create the refactored topic using modular components
        auto topic = std::make_unique<TopicRefactored>(
            topic_name,
            cxl_addr,
            tinode,
            replica_tinode,
            broker_id,
            seq_type,
            order,
            ack_level,
            replication_factor
        );

        // 2. Set up callbacks
        topic->SetGetNewSegmentCallback(
            [](size_t size, size_t msg_size, size_t& segment_size, SegmentMetadata& metadata) {
                // Custom segment allocation logic
                void* new_segment = /* allocate segment */;
                segment_size = /* calculated size */;
                return new_segment;
            }
        );

        topic->SetGetNumBrokersCallback([]() {
            return 3; // Example: 3 brokers in cluster
        });

        topic->SetGetRegisteredBrokersCallback(
            [](absl::btree_set<int>& brokers, TInode* tinode) {
                brokers.insert({0, 1, 2});
                return true;
            }
        );

        // 3. Initialize and start
        if (!topic->Initialize()) {
            LOG(ERROR) << "Failed to initialize topic";
            return;
        }

        topic->Start();

        // 4. Example: Allocate buffer for a batch
        BatchHeader batch_header;
        batch_header.num_msg = 10;
        batch_header.total_size = 1024;
        
        void* log;
        size_t logical_offset;
        std::function<void(size_t, size_t)> callback;
        
        topic->GetCXLBuffer(batch_header, log, logical_offset, callback);

        // 5. Example: Get messages for subscribers
        size_t last_offset = 0;
        void* last_addr = nullptr;
        void* messages;
        size_t messages_size;
        
        if (topic->GetMessageAddr(last_offset, last_addr, messages, messages_size)) {
            LOG(INFO) << "Retrieved " << messages_size << " bytes of messages";
        }

        // 6. Clean shutdown
        topic->Stop();
    }

    /**
     * Example showing direct use of individual components
     * This demonstrates the flexibility of the modular design
     */
    static void DemonstrateDirectComponentUsage() {
        void* cxl_addr = /* CXL memory address */;
        TInode* tinode = /* Topic inode */;
        int broker_id = 0;

        // Create individual components
        auto segment_manager = std::make_shared<SegmentManager>(cxl_addr, 1024 * 1024);
        
        auto buffer_manager = std::make_unique<BufferManager>(
            cxl_addr,
            /* current_segment */ nullptr,
            /* log_addr */ std::atomic<unsigned long long int>{0},
            /* batch_headers_addr */ 0,
            broker_id
        );
        buffer_manager->SetSegmentManager(segment_manager);

        auto message_ordering = std::make_unique<MessageOrdering>(
            cxl_addr,
            tinode,
            broker_id
        );

        auto replication_manager = std::make_unique<ReplicationManager>(
            "test_topic",
            broker_id,
            3, // replication_factor
            CORFU,
            tinode,
            nullptr
        );

        // Use components independently
        replication_manager->Initialize();
        message_ordering->StartSequencer(CORFU, 4, "test_topic");

        // Components can be tested individually
        // This makes unit testing much easier
    }

    /**
     * Example showing how to create a mock for testing
     */
    class MockBufferAllocator : public IBufferAllocator {
    public:
        void GetCXLBuffer(BatchHeader& batch_header,
                         void*& log,
                         size_t& logical_offset,
                         std::function<void(size_t, size_t)>& callback) override {
            // Mock implementation for testing
            log = test_buffer_;
            logical_offset = test_offset_++;
            callback = [](size_t start, size_t end) {
                LOG(INFO) << "Mock callback: " << start << " - " << end;
            };
        }

    private:
        void* test_buffer_ = nullptr;
        size_t test_offset_ = 0;
    };

    /**
     * Example unit test using mocked components
     */
    static void DemonstrateTestability() {
        // Create mock components for testing
        auto mock_buffer_allocator = std::make_unique<MockBufferAllocator>();
        
        // Test buffer allocation without needing full system
        BatchHeader header;
        void* log;
        size_t offset;
        std::function<void(size_t, size_t)> callback;
        
        mock_buffer_allocator->GetCXLBuffer(header, log, offset, callback);
        
        // Verify behavior
        assert(offset == 0);
        if (callback) {
            callback(0, 10);
        }
    }
};

} // namespace Embarcadero

/**
 * Benefits of the refactored architecture:
 * 
 * 1. **Separation of Concerns**: Each component has a single, well-defined responsibility
 *    - BufferManager: Buffer allocation strategies
 *    - MessageOrdering: Sequencing and ordering logic
 *    - ReplicationManager: Replication to secondary nodes
 *    - MessageExport: Subscriber message delivery
 *    - SegmentManager: Segment boundary management
 * 
 * 2. **Testability**: Components can be tested in isolation using interfaces
 *    - Mock implementations for unit tests
 *    - No need to set up entire system for component testing
 * 
 * 3. **Maintainability**: Smaller, focused classes are easier to understand and modify
 *    - Changes to ordering logic don't affect buffer management
 *    - New sequencer types can be added without touching export logic
 * 
 * 4. **Reusability**: Components can be used independently
 *    - BufferManager can be used by other systems needing buffer allocation
 *    - MessageOrdering can be extracted for other ordering requirements
 * 
 * 5. **Flexibility**: Easy to swap implementations
 *    - Different replication strategies can be plugged in
 *    - Alternative ordering algorithms can be tested
 * 
 * 6. **Performance**: No overhead from modularization
 *    - Interfaces use virtual functions only where necessary
 *    - Components maintain the same performance characteristics
 */
