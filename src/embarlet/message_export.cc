#include "message_export.h"
#include <glog/logging.h>
#include <thread>

namespace Embarcadero {

MessageExport::MessageExport(void* cxl_addr,
                           void* first_message_addr,
                           TInode* tinode,
                           int broker_id,
                           int order,
                           int ack_level,
                           int replication_factor)
    : cxl_addr_(cxl_addr),
      first_message_addr_(first_message_addr),
      tinode_(tinode),
      broker_id_(broker_id),
      order_(order),
      ack_level_(ack_level),
      replication_factor_(replication_factor) {}

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
        
        if (ack_level_ == 2) {
            size_t r[replication_factor_];
            size_t min = static_cast<size_t>(-1);
            for (int i = 0; i < replication_factor_; i++) {
                int b = (broker_id_ + NUM_MAX_BROKERS - i) % NUM_MAX_BROKERS;
                r[i] = tinode_->offsets[b].replication_done[broker_id_];
                if (min > r[i]) {
                    min = r[i];
                }
            }
            if (min == static_cast<size_t>(-1)) {
                return false;
            }
            if (combined_offset != min) {
                combined_addr = reinterpret_cast<uint8_t*>(combined_addr) -
                    (reinterpret_cast<MessageHeader*>(combined_addr)->paddedSize * (combined_offset - min));
                combined_offset = min;
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
