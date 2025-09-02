#pragma once

#include <atomic>
#include <memory>
#include <functional>
#include <absl/synchronization/mutex.h>
#include "common/config.h"
#include "common/performance_utils.h"
#include "zero_copy_buffer.h"
#include "segment_manager.h"

namespace Embarcadero {

// Forward declarations
namespace Corfu {
    class CorfuReplicationClient;
}
namespace Scalog {
    class ScalogReplicationClient;
}

/**
 * BufferManager handles buffer allocation for different sequencer types
 * Extracted from Topic class to separate buffer management concerns
 */
class BufferManager : public IBufferAllocator {
public:
    // Callback type for buffer completion
    using CompletionCallback = std::function<void(void*, size_t)>;
    
    // Segment manager
    std::shared_ptr<ISegmentManager> segment_manager_;

    // Buffer allocation result
    struct BufferAllocation {
        void* log_address;
        void* segment_header;
        size_t logical_offset;
        CompletionCallback completion_callback;
    };

    // Set segment manager
    void SetSegmentManager(std::shared_ptr<ISegmentManager> segment_manager) {
        segment_manager_ = segment_manager;
    };

    BufferManager(void* cxl_addr, 
                  void* current_segment,
                  std::atomic<unsigned long long int>& log_addr,
                  unsigned long long int batch_headers_addr,
                  int broker_id);

    // Buffer allocation methods for different sequencer types
    BufferAllocation AllocateKafkaBuffer(BatchHeader& batch_header,
                                        const char topic[TOPIC_NAME_SIZE],
                                        size_t& logical_offset_counter);

    BufferAllocation AllocateCorfuBuffer(BatchHeader& batch_header,
                                        const char topic[TOPIC_NAME_SIZE],
                                        int replication_factor,
                                        Corfu::CorfuReplicationClient* replication_client);

    BufferAllocation AllocateScalogBuffer(BatchHeader& batch_header,
                                         const char topic[TOPIC_NAME_SIZE],
                                         int replication_factor,
                                         Scalog::ScalogReplicationClient* replication_client);

    BufferAllocation AllocateEmbarcaderoBuffer(BatchHeader& batch_header,
                                              const char topic[TOPIC_NAME_SIZE]);

    BufferAllocation AllocateOrder3Buffer(BatchHeader& batch_header,
                                         const char topic[TOPIC_NAME_SIZE],
                                         std::function<int()> get_num_brokers);

    BufferAllocation AllocateOrder4Buffer(BatchHeader& batch_header,
                                         const char topic[TOPIC_NAME_SIZE],
                                         size_t& logical_offset_counter);

    // Segment boundary checking
    void CheckSegmentBoundary(void* log, size_t msg_size);

    // Update tracking for Kafka-style ordering
    void UpdateKafkaTracking(size_t start_logical_offset, 
                           size_t end_logical_offset,
                           void* log_ptr,
                           void* current_segment,
                           std::function<void(size_t, unsigned long long int)> update_tinode);

    // IBufferAllocator interface
    void GetCXLBuffer(BatchHeader& batch_header,
                      void*& log,
                      size_t& logical_offset,
                      std::function<void(size_t, size_t)>& callback) override;

private:
    void* cxl_addr_;
    void* current_segment_;
    std::atomic<unsigned long long int>& log_addr_;
    unsigned long long int batch_headers_;
    int broker_id_;

    // Kafka-specific tracking
    absl::Mutex kafka_mutex_;
    std::atomic<size_t> kafka_logical_offset_{0};
    absl::flat_hash_map<size_t, size_t> written_messages_range_;

    // Order3-specific tracking
    absl::Mutex order3_mutex_;
    absl::flat_hash_map<size_t, size_t> order3_client_batch_;
    absl::flat_hash_map<size_t, absl::flat_hash_map<size_t, void*>> skipped_batch_;

    // Scalog batch offset
    static std::atomic<size_t> scalog_batch_offset_;
};

} // namespace Embarcadero
