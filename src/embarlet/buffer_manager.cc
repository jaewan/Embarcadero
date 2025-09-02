#include "buffer_manager.h"
#include "../client/corfu_client.h"
#include "../client/scalog_client.h"
#include <glog/logging.h>
#include <cstring>

namespace Embarcadero {

std::atomic<size_t> BufferManager::scalog_batch_offset_{0};

BufferManager::BufferManager(void* cxl_addr,
                           void* current_segment,
                           std::atomic<unsigned long long int>& log_addr,
                           unsigned long long int batch_headers_addr,
                           int broker_id)
    : cxl_addr_(cxl_addr),
      current_segment_(current_segment),
      log_addr_(log_addr),
      batch_headers_(batch_headers_addr),
      broker_id_(broker_id) {}

bool BufferManager::CheckSegmentBoundary(void*& log, size_t msg_size, SegmentMetadata& metadata) {
    if (!segment_manager_) {
        metadata.is_new_segment = false;
        return true;
    }

    return segment_manager_->CheckSegmentBoundary(log, msg_size, metadata);
}

BufferManager::BufferAllocation BufferManager::AllocateKafkaBuffer(
    BatchHeader& batch_header,
    const char topic[TOPIC_NAME_SIZE],
    size_t& logical_offset_counter) {
    
    BufferAllocation allocation;
    size_t start_logical_offset;

    {
        absl::MutexLock lock(&kafka_mutex_);
        
        allocation.log_address = reinterpret_cast<void*>(log_addr_.fetch_add(batch_header.total_size));
        allocation.logical_offset = logical_offset_counter;
        allocation.segment_header = current_segment_;
        start_logical_offset = logical_offset_counter;
        logical_offset_counter += batch_header.num_msg;

        if (reinterpret_cast<unsigned long long int>(current_segment_) + SEGMENT_SIZE <= log_addr_) {
            LOG(ERROR) << "!!!!!!!!! Increase the Segment Size: " << SEGMENT_SIZE;
        }
    }

    // Create completion callback
    allocation.completion_callback = [this, start_logical_offset](void* log_ptr, size_t logical_offset) {
        absl::MutexLock lock(&kafka_mutex_);

        if (kafka_logical_offset_.load() != start_logical_offset) {
            written_messages_range_[start_logical_offset] = logical_offset;
        } else {
            size_t start = start_logical_offset;
            bool has_next_messages_written = false;

            do {
                has_next_messages_written = false;
                
                reinterpret_cast<MessageHeader*>(log_ptr)->logical_offset = static_cast<size_t>(-1);
                kafka_logical_offset_.store(logical_offset + 1);

                if (written_messages_range_.contains(logical_offset + 1)) {
                    start = logical_offset + 1;
                    logical_offset = written_messages_range_[start];
                    written_messages_range_.erase(start);
                    has_next_messages_written = true;
                }
            } while (has_next_messages_written);
        }
    };

    return allocation;
}

BufferManager::BufferAllocation BufferManager::AllocateCorfuBuffer(
    BatchHeader& batch_header,
    const char topic[TOPIC_NAME_SIZE],
    int replication_factor,
    Corfu::CorfuReplicationClient* replication_client) {
    
    BufferAllocation allocation;
    
    const unsigned long long int segment_metadata = 
        reinterpret_cast<unsigned long long int>(current_segment_);
    const size_t msg_size = batch_header.total_size;
    BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);

    allocation.log_address = reinterpret_cast<void*>(log_addr_.load() + batch_header.log_idx);
    
    CheckSegmentBoundary(allocation.log_address, msg_size);

    batch_header_log[batch_header.batch_seq].batch_seq = batch_header.batch_seq;
    batch_header_log[batch_header.batch_seq].total_size = batch_header.total_size;
    batch_header_log[batch_header.batch_seq].broker_id = broker_id_;
    batch_header_log[batch_header.batch_seq].ordered = 0;
    batch_header_log[batch_header.batch_seq].batch_off_to_export = 0;
    batch_header_log[batch_header.batch_seq].log_idx = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(allocation.log_address) - reinterpret_cast<uintptr_t>(cxl_addr_)
    );

    // Create replication callback
    if (replication_factor > 0 && replication_client) {
        allocation.completion_callback = [this, batch_header, allocation, replication_client](void* log_ptr, size_t) {
            BatchHeader* batch_header_log = reinterpret_cast<BatchHeader*>(batch_headers_);
            MessageHeader* header = static_cast<MessageHeader*>(allocation.log_address);
            
            while (header->next_msg_diff == 0) {
                std::this_thread::yield();
            }

            replication_client->ReplicateData(
                batch_header.log_idx,
                batch_header.total_size,
                allocation.log_address
            );

            batch_header_log[batch_header.batch_seq].ordered = 1;
        };
    }

    return allocation;
}

BufferManager::BufferAllocation BufferManager::AllocateScalogBuffer(
    BatchHeader& batch_header,
    const char topic[TOPIC_NAME_SIZE],
    int replication_factor,
    Scalog::ScalogReplicationClient* replication_client) {
    
    BufferAllocation allocation;
    
    batch_header.log_idx = scalog_batch_offset_.fetch_add(batch_header.total_size);
    
    const unsigned long long int segment_metadata = 
        reinterpret_cast<unsigned long long int>(current_segment_);
    const size_t msg_size = batch_header.total_size;

    allocation.log_address = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
    
    CheckSegmentBoundary(allocation.log_address, msg_size);

    // Create replication callback
    if (replication_factor > 0 && replication_client) {
        allocation.completion_callback = [batch_header, allocation, replication_client](void* log_ptr, size_t) {
            replication_client->ReplicateData(
                batch_header.log_idx,
                batch_header.total_size,
                batch_header.num_msg,
                allocation.log_address
            );
        };
    }

    return allocation;
}

BufferManager::BufferAllocation BufferManager::AllocateEmbarcaderoBuffer(
    BatchHeader& batch_header,
    const char topic[TOPIC_NAME_SIZE]) {
    
    BufferAllocation allocation;
    
    const unsigned long long int segment_metadata = 
        reinterpret_cast<unsigned long long int>(current_segment_);
    const size_t msg_size = batch_header.total_size;

    allocation.log_address = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
    
    CheckSegmentBoundary(allocation.log_address, msg_size);
    
    allocation.completion_callback = nullptr;
    
    return allocation;
}

BufferManager::BufferAllocation BufferManager::AllocateOrder3Buffer(
    BatchHeader& batch_header,
    const char topic[TOPIC_NAME_SIZE],
    std::function<int()> get_num_brokers) {
    
    BufferAllocation allocation;
    
    absl::MutexLock lock(&order3_mutex_);
    
    static size_t num_brokers = get_num_brokers();
    
    // Check if this batch was previously skipped
    if (skipped_batch_.contains(batch_header.client_id)) {
        auto& client_batches = skipped_batch_[batch_header.client_id];
        auto it = client_batches.find(batch_header.batch_seq);
        
        if (it != client_batches.end()) {
            allocation.log_address = it->second;
            client_batches.erase(it);
            allocation.completion_callback = nullptr;
            return allocation;
        }
    }
    
    // Initialize client tracking if needed
    if (!order3_client_batch_.contains(batch_header.client_id)) {
        order3_client_batch_.emplace(batch_header.client_id, broker_id_);
    }
    
    // Handle all skipped batches
    auto& client_seq = order3_client_batch_[batch_header.client_id];
    while (client_seq < batch_header.batch_seq) {
        void* skipped_addr = reinterpret_cast<void*>(log_addr_.load());
        skipped_batch_[batch_header.client_id].emplace(client_seq, skipped_addr);
        log_addr_ += batch_header.total_size;
        client_seq += num_brokers;
    }
    
    // Allocate space for this batch
    allocation.log_address = reinterpret_cast<void*>(log_addr_.load());
    log_addr_ += batch_header.total_size;
    client_seq += num_brokers;
    
    allocation.completion_callback = nullptr;
    
    return allocation;
}

BufferManager::BufferAllocation BufferManager::AllocateOrder4Buffer(
    BatchHeader& batch_header,
    const char topic[TOPIC_NAME_SIZE],
    size_t& logical_offset_counter) {
    
    BufferAllocation allocation;
    
    const unsigned long long int segment_metadata = 
        reinterpret_cast<unsigned long long int>(current_segment_);
    const size_t msg_size = batch_header.total_size;
    void* batch_headers_log;

    {
        absl::MutexLock lock(&order3_mutex_);
        
        allocation.log_address = reinterpret_cast<void*>(log_addr_.fetch_add(msg_size));
        
        batch_headers_log = reinterpret_cast<void*>(batch_headers_);
        batch_headers_ += sizeof(BatchHeader);
        allocation.logical_offset = logical_offset_counter;
        logical_offset_counter += batch_header.num_msg;
    }

    CheckSegmentBoundary(allocation.log_address, msg_size);

    batch_header.start_logical_offset = allocation.logical_offset;
    batch_header.broker_id = broker_id_;
    batch_header.ordered = 0;
    batch_header.total_order = 0;
    batch_header.log_idx = static_cast<size_t>(
        reinterpret_cast<uintptr_t>(allocation.log_address) - reinterpret_cast<uintptr_t>(cxl_addr_)
    );

    memcpy(batch_headers_log, &batch_header, sizeof(BatchHeader));
    
    allocation.completion_callback = nullptr;
    
    return allocation;
}

void BufferManager::UpdateKafkaTracking(
    size_t start_logical_offset,
    size_t end_logical_offset,
    void* log_ptr,
    void* current_segment,
    std::function<void(size_t, unsigned long long int)> update_tinode) {
    
    absl::MutexLock lock(&kafka_mutex_);
    
    if (kafka_logical_offset_.load() != start_logical_offset) {
        written_messages_range_[start_logical_offset] = end_logical_offset;
    } else {
        size_t start = start_logical_offset;
        bool has_next_messages_written = false;
        
        do {
            has_next_messages_written = false;
            
            reinterpret_cast<MessageHeader*>(log_ptr)->logical_offset = static_cast<size_t>(-1);
            
            update_tinode(
                end_logical_offset,
                static_cast<unsigned long long int>(
                    reinterpret_cast<uint8_t*>(log_ptr) - reinterpret_cast<uint8_t*>(cxl_addr_))
            );
            
            *reinterpret_cast<unsigned long long int*>(current_segment) =
                static_cast<unsigned long long int>(
                    reinterpret_cast<uint8_t*>(log_ptr) - 
                    reinterpret_cast<uint8_t*>(current_segment)
                );
            
            kafka_logical_offset_.store(end_logical_offset + 1);
            
            if (written_messages_range_.contains(end_logical_offset + 1)) {
                start = end_logical_offset + 1;
                end_logical_offset = written_messages_range_[start];
                written_messages_range_.erase(start);
                has_next_messages_written = true;
            }
        } while (has_next_messages_written);
    }
}

} // namespace Embarcadero
