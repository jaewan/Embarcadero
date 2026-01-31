#pragma once

#include <atomic>
#include "../common/common.h"

namespace Embarcadero {

/**
 * MessageExport handles exporting messages to subscribers
 * Extracted from Topic class to separate export/subscriber concerns
 */
class MessageExport {
public:
    MessageExport(void* cxl_addr,
                  void* first_message_addr,
                  TInode* tinode,
                  int broker_id,
                  int order,
                  int ack_level,
                  int replication_factor,
                  heartbeat_system::SequencerType seq_type);

    /**
     * Get message address and size for topic subscribers
     * @param last_offset Last fetched message offset
     * @param last_addr Last fetched message address
     * @param messages Output: pointer to messages
     * @param messages_size Output: total size of messages
     * @return true if more messages are available
     */
    bool GetMessageAddr(size_t& last_offset,
                       void*& last_addr,
                       void*& messages,
                       size_t& messages_size);

    /**
     * Get batch to export (for Order 4)
     * @param expected_batch_offset Expected batch offset
     * @param batch_addr Output: batch address
     * @param batch_size Output: batch size
     * @return true if batch is available
     */
    bool GetBatchToExport(size_t& expected_batch_offset,
                         void*& batch_addr,
                         size_t& batch_size);

    // Set written state (updated by combiner)
    void SetWrittenState(size_t logical_offset, void* physical_addr) {
        written_logical_offset_ = logical_offset;
        written_physical_addr_ = physical_addr;
    }

private:
    void* cxl_addr_;
    void* first_message_addr_;
    TInode* tinode_;
    int broker_id_;
    int order_;
    int ack_level_;
    int replication_factor_;
    heartbeat_system::SequencerType seq_type_;

    // Tracking for non-ordered messages
    std::atomic<size_t> written_logical_offset_{static_cast<size_t>(-1)};
    std::atomic<void*> written_physical_addr_{nullptr};
};

} // namespace Embarcadero
