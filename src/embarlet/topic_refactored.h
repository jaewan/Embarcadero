#pragma once

#include <memory>
#include <functional>
#include <string>
#include "../common/common.h"
#include "buffer_manager.h"
#include "message_ordering.h"
#include "replication_manager.h"
#include "message_export.h"

namespace Embarcadero {

/**
 * Refactored Topic class using modular components
 * This demonstrates how the Topic class can be simplified by delegating
 * responsibilities to specialized components
 */
class TopicRefactored {
public:
    // Callback function types
    using GetNewSegmentFunc = std::function<void*(size_t, size_t, size_t&, SegmentMetadata&)>;
    using GetNumBrokersFunc = std::function<int()>;
    using GetRegisteredBrokersFunc = std::function<bool(absl::btree_set<int>&, TInode*)>;

    TopicRefactored(const std::string& topic_name,
                    void* cxl_addr,
                    TInode* tinode,
                    TInode* replica_tinode,
                    int broker_id,
                    SequencerType seq_type,
                    int order,
                    int ack_level,
                    int replication_factor);
    ~TopicRefactored();

    // Initialize all components
    bool Initialize();

    // Start processing
    void Start();

    // Stop processing
    void Stop();

    // Buffer allocation interface (delegates to BufferManager)
    void GetCXLBuffer(BatchHeader& batch_header,
                     void*& log,
                     size_t& logical_offset,
                     std::function<void(size_t, size_t)>& callback);

    // Message export interface (delegates to MessageExport)
    bool GetMessageAddr(size_t& last_offset,
                       void*& last_addr,
                       void*& messages,
                       size_t& messages_size);

    bool GetBatchToExport(size_t& expected_batch_offset,
                         void*& batch_addr,
                         size_t& batch_size);

    // Set callbacks
    void SetGetNewSegmentCallback(GetNewSegmentFunc func) {
        buffer_manager_->SetGetNewSegmentCallback(func);
    }
    
    void SetGetNumBrokersCallback(GetNumBrokersFunc func) {
        get_num_brokers_callback_ = func;
    }
    
    void SetGetRegisteredBrokersCallback(GetRegisteredBrokersFunc func) {
        get_registered_brokers_callback_ = func;
    }

    // Statistics
    size_t GetOrderedCount() const {
        return message_ordering_ ? message_ordering_->GetOrderedCount() : 0;
    }

private:
    // Basic properties
    std::string topic_name_;
    void* cxl_addr_;
    TInode* tinode_;
    TInode* replica_tinode_;
    int broker_id_;
    SequencerType seq_type_;
    int order_;
    int ack_level_;
    int replication_factor_;

    // Modular components
    std::unique_ptr<BufferManager> buffer_manager_;
    std::unique_ptr<MessageOrdering> message_ordering_;
    std::unique_ptr<ReplicationManager> replication_manager_;
    std::unique_ptr<MessageExport> message_export_;
    std::unique_ptr<MessageCombiner> message_combiner_;

    // Callbacks
    GetNumBrokersFunc get_num_brokers_callback_;
    GetRegisteredBrokersFunc get_registered_brokers_callback_;

    // First message address (calculated during initialization)
    void* first_message_addr_{nullptr};
};

} // namespace Embarcadero
