#pragma once

#include <functional>
#include "../common/common.h"

namespace Embarcadero {

/**
 * Interface for buffer allocation
 */
class IBufferAllocator {
public:
    virtual ~IBufferAllocator() = default;
    
    virtual void GetCXLBuffer(BatchHeader& batch_header,
                             void*& log,
                             size_t& logical_offset,
                             std::function<void(size_t, size_t)>& callback) = 0;
};

/**
 * Interface for message ordering/sequencing
 */
class IMessageSequencer {
public:
    virtual ~IMessageSequencer() = default;
    
    virtual void StartSequencer(SequencerType seq_type, int order, const std::string& topic_name) = 0;
    virtual void StopSequencer() = 0;
    virtual size_t GetOrderedCount() const = 0;
};

/**
 * Interface for replication
 */
class IReplicationManager {
public:
    virtual ~IReplicationManager() = default;
    
    virtual bool Initialize() = 0;
    virtual void ReplicateCorfuData(size_t log_idx, size_t total_size, void* data) = 0;
    virtual void ReplicateScalogData(size_t log_idx, size_t total_size, size_t num_msg, void* data) = 0;
    virtual void UpdateReplicationDone(size_t last_offset, std::function<int()> get_num_brokers) = 0;
};

/**
 * Interface for message export/subscriber support
 */
class IMessageExporter {
public:
    virtual ~IMessageExporter() = default;
    
    virtual bool GetMessageAddr(size_t& last_offset,
                               void*& last_addr,
                               void*& messages,
                               size_t& messages_size) = 0;
    
    virtual bool GetBatchToExport(size_t& expected_batch_offset,
                                 void*& batch_addr,
                                 size_t& batch_size) = 0;
};

/**
 * Interface for segment management
 */
class ISegmentManager {
public:
    virtual ~ISegmentManager() = default;
    
    virtual void* GetNewSegment(size_t size, size_t msg_size, size_t& segment_size, SegmentMetadata& metadata) = 0;
    virtual bool CheckSegmentBoundary(void* log, size_t msg_size, SegmentMetadata& metadata) = 0;
};

} // namespace Embarcadero
