#include "message_export.h"
#include <glog/logging.h>
#include <thread>
#include "../common/performance_utils.h"

namespace Embarcadero {

MessageExport::MessageExport(void* cxl_addr,
                           void* first_message_addr,
                           TInode* tinode,
                           int broker_id,
                           int order,
                           int ack_level,
                           int replication_factor,
                           heartbeat_system::SequencerType seq_type)
    : cxl_addr_(cxl_addr),
      first_message_addr_(first_message_addr),
      tinode_(tinode),
      broker_id_(broker_id),
      order_(order),
      ack_level_(ack_level),
      replication_factor_(replication_factor),
      seq_type_(seq_type) {}

bool MessageExport::GetMessageAddr(size_t& last_offset,
                                  void*& last_addr,
                                  void*& messages,
                                  size_t& messages_size) {
    // Determine current read position based on order
    size_t combined_offset;
    void* combined_addr;

    if (order_ > 0) {
        combined_offset = tinode_->offsets[broker_id_].ordered;
        combined_addr = reinterpret_cast<uint8_t*>(cxl_addr_) + 
            tinode_->offsets[broker_id_].ordered_offset;
        
        if (ack_level_ == 2 && replication_factor_ > 0) {
            // [[PHASE_2]] ACK Level 2: Wait for full replication before exposing messages to subscribers
            // Use CompletionVector (8 bytes) instead of replication_done array (256 bytes)

            if (seq_type_ == heartbeat_system::EMBARCADERO) {
                // [[PHASE_2_CV_PATH]] Use shared helper to read CV and get replicated position
                size_t replicated_last_offset;
                if (!GetReplicatedLastOffsetFromCV(cxl_addr_, tinode_, broker_id_, replicated_last_offset)) {
                    return false;  // No replication progress yet
                }

                // Adjust combined_offset to not exceed replicated position
                if (combined_offset > replicated_last_offset) {
                    // Back up to replicated position
                    combined_addr = reinterpret_cast<uint8_t*>(combined_addr) -
                        (reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize * (combined_offset - replicated_last_offset));
                    combined_offset = replicated_last_offset;
                }
            } else {
                // [[PHASE_1]] Legacy path: Use replication_done array for non-EMBARCADERO sequencers
                int num_brokers = NUM_MAX_BROKERS;
                size_t r[replication_factor_];
                size_t min = static_cast<size_t>(-1);
                for (int i = 0; i < replication_factor_; i++) {
                    int b = GetReplicationSetBroker(broker_id_, replication_factor_, num_brokers, i);
                    volatile uint64_t* rep_done_ptr = &tinode_->offsets[b].replication_done[broker_id_];
                    CXL::flush_cacheline(const_cast<const void*>(
                        reinterpret_cast<const volatile void*>(rep_done_ptr)));
                    r[i] = *rep_done_ptr;
                    if (min > r[i]) {
                        min = r[i];
                    }
                }
                CXL::load_fence();

                if (min == static_cast<size_t>(-1)) {
                    return false;
                }
                if (combined_offset != min) {
                    combined_addr = reinterpret_cast<uint8_t*>(combined_addr) -
                        (reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize * (combined_offset - min));
                    combined_offset = min;
                }
            }
        }
    } else {
        combined_offset = written_logical_offset_;
        combined_addr = written_physical_addr_;
    }

    // Check if we have new messages
    if (combined_offset == static_cast<size_t>(-1) ||
        (last_addr != nullptr && combined_offset <= last_offset)) {
        return false;
    }

    // Find start message location
    MessageHeader* start_msg_header;

    if (last_addr != nullptr) {
        start_msg_header = static_cast<MessageHeader*>(last_addr);

        // Wait for message to be combined if necessary
        while (start_msg_header->next_msg_diff == 0) {
            std::this_thread::yield();
        }

        // Move to next message
        start_msg_header = reinterpret_cast<MessageHeader*>(
            reinterpret_cast<uint8_t*>(start_msg_header) + start_msg_header->next_msg_diff
        );
    } else {
        // Start from first message
        if (combined_addr <= last_addr) {
            LOG(ERROR) << "GetMessageAddr: Invalid address relationship";
            return false;
        }
        start_msg_header = static_cast<MessageHeader*>(first_message_addr_);
    }

    // Verify message is valid
    if (start_msg_header->paddedSize == 0) {
        return false;
    }

    // Set output message pointer
    messages = static_cast<void*>(start_msg_header);

#ifdef MULTISEGMENT
    // Multi-segment logic for determining message size and last offset
    unsigned long long int* segment_offset_ptr = 
        static_cast<unsigned long long int*>(start_msg_header->segment_header);

    MessageHeader* last_msg_of_segment = reinterpret_cast<MessageHeader*>(
        reinterpret_cast<uint8_t*>(segment_offset_ptr) + *segment_offset_ptr
    );

    if (combined_addr < last_msg_of_segment) {
        // Last message is not fully ordered yet
        messages_size = reinterpret_cast<uint8_t*>(combined_addr) -
            reinterpret_cast<uint8_t*>(start_msg_header) +
            reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize;
        last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
        last_addr = combined_addr;
    } else {
        // Return entire segment of messages
        messages_size = reinterpret_cast<uint8_t*>(last_msg_of_segment) -
            reinterpret_cast<uint8_t*>(start_msg_header) +
            last_msg_of_segment->paddedSize;
        last_offset = last_msg_of_segment->logical_offset;
        last_addr = static_cast<void*>(last_msg_of_segment);
    }
#else
    // Single-segment logic for determining message size and last offset
    messages_size = reinterpret_cast<uint8_t*>(combined_addr) -
        reinterpret_cast<uint8_t*>(start_msg_header) +
        reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize;

    last_offset = reinterpret_cast<MessageHeader*>(combined_addr)->logical_offset;
    last_addr = combined_addr;
#endif

    return true;
}

bool MessageExport::GetBatchToExport(size_t& expected_batch_offset,
                                    void*& batch_addr,
                                    size_t& batch_size) {
    static BatchHeader* start_batch_header = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(cxl_addr_) + tinode_->offsets[broker_id_].batch_headers_offset);
    
    BatchHeader* header = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(start_batch_header) + sizeof(BatchHeader) * expected_batch_offset);
    
    if (header->ordered == 0) {
        return false;
    }
    
    header = reinterpret_cast<BatchHeader*>(
        reinterpret_cast<uint8_t*>(header) + static_cast<int>(header->batch_off_to_export));
    
    batch_size = header->total_size;
    batch_addr = header->log_idx + reinterpret_cast<uint8_t*>(cxl_addr_);
    expected_batch_offset++;

    return true;
}

} // namespace Embarcadero
