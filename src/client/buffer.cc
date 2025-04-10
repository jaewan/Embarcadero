#include "buffer.h"

Buffer::Buffer(size_t num_buf, size_t num_threads_per_broker, int client_id, size_t message_size, int order) 
    : bufs_(num_buf), 
      num_threads_per_broker_(num_threads_per_broker), 
      order_(order) {
    
    // Initialize message header with provided values
    header_.client_id = client_id;
    header_.size = message_size;
    header_.total_order = 0;
    
    // Calculate padding for alignment
    int padding = message_size % 64;
    if (padding) {
        padding = 64 - padding;
    }
    
    // Set padded size to include message size, padding, and header
    header_.paddedSize = message_size + padding + sizeof(Embarcadero::MessageHeader);
    
    // Initialize other header fields with default values
    header_.segment_header = nullptr;
    header_.logical_offset = static_cast<size_t>(-1); // Sentinel value
    header_.next_msg_diff = 0;
    header_.complete = 0;
    
    VLOG(5) << "Buffer created with " << num_buf << " buffers, " 
            << num_threads_per_broker << " threads per broker, "
            << "message size: " << message_size 
            << ", padded size: " << header_.paddedSize;
}

Buffer::~Buffer() {
    // Free all allocated buffers
    for (size_t i = 0; i < num_buf_; i++) {
        if (bufs_[i].buffer) {
            munmap(bufs_[i].buffer, bufs_[i].len);
            bufs_[i].buffer = nullptr;
        }
    }
}

bool Buffer::AddBuffers(size_t buf_size) {
    // Align buffer size to 64-byte boundary
    size_t aligned_size = ((buf_size + 63) / 64) * 64;
    
    // Ensure minimum buffer size (1GB)
    const static size_t min_size = (1UL << 30);
    if (min_size > aligned_size) {
        buf_size = min_size;
    } else {
        buf_size = aligned_size;
    }
    
    // Get index for the new buffers and increment counter atomically
    size_t idx = num_buf_.fetch_add(num_threads_per_broker_);
    if (idx + num_threads_per_broker_ > bufs_.size()) {
        LOG(ERROR) << "Buffer allocation failed: not enough space in buffer array. "
                   << "Requested index: " << idx 
                   << ", threads per broker: " << num_threads_per_broker_
                   << ", buffer array size: " << bufs_.size();
        return false;
    }
    
    // Allocate memory for each buffer
    for (size_t i = 0; i < num_threads_per_broker_; i++) {
        size_t allocated = 0;
        void* new_buffer = nullptr;
        
        try {
            new_buffer = mmap_large_buffer(buf_size, allocated);
        } catch (const std::exception& e) {
            LOG(ERROR) << "Failed to allocate buffer: " << e.what();
            
            // Clean up any buffers already allocated in this batch
            for (size_t j = 0; j < i; j++) {
                munmap(bufs_[idx + j].buffer, bufs_[idx + j].len);
                bufs_[idx + j].buffer = nullptr;
            }
            return false;
        }
        
        bufs_[idx + i].buffer = new_buffer;
        bufs_[idx + i].len = allocated;
        
#ifdef BATCH_OPTIMIZATION
        // In batch mode, initialize tail to leave space for batch header
        bufs_[idx + i].tail = sizeof(Embarcadero::BatchHeader);
#endif
    }
    
    return true;
}

void Buffer::AdvanceWriteBufId() {
    // Calculate number of brokers
    size_t num_broker = num_buf_ / num_threads_per_broker_;
    
    // Round-robin through buffers and threads
    i_ = (i_ + 1) % num_broker;
    if (i_ == 0) {
        j_ = (j_ + 1) % num_threads_per_broker_;
    }
    
    // Calculate new write buffer ID
    write_buf_id_ = i_ * num_threads_per_broker_ + j_;
}

#ifdef BATCH_OPTIMIZATION
bool Buffer::Write(size_t client_order, char* msg, size_t len, size_t paddedSize) {
    static const size_t header_size = sizeof(Embarcadero::MessageHeader);

    void* buffer;
    size_t head, tail;
    
    // Critical section for buffer access
    {
        size_t lockedIdx = write_buf_id_;
        buffer = bufs_[write_buf_id_].buffer;
        head = bufs_[write_buf_id_].writer_head;
        tail = bufs_[write_buf_id_].tail;
        
        // Check if buffer is full and needs to be wrapped
        if (tail + header_size + paddedSize + paddedSize /*buffer margin*/ > bufs_[lockedIdx].len) {
            LOG(INFO) << "Buffer:" << write_buf_id_ << " full. Size:" << bufs_[write_buf_id_].len 
                   << ". Wrapping buffer. (Note: this may be buggy as it doesn't check if new head has been read)";
            
            // Seal current batch before moving to next buffer
            Embarcadero::BatchHeader* batch_header = 
                reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[write_buf_id_].buffer + head);
            
            batch_header->start_logical_offset = 0;
            batch_header->batch_seq = batch_seq_.fetch_add(1);
            batch_header->total_size = bufs_[write_buf_id_].tail - head - sizeof(Embarcadero::BatchHeader);
            batch_header->num_msg = bufs_[write_buf_id_].num_msg;

            // Reset buffer state for new batch
            bufs_[write_buf_id_].num_msg = 0;
            bufs_[write_buf_id_].writer_head = 0;
            bufs_[write_buf_id_].tail = sizeof(Embarcadero::BatchHeader);

            // Move to next buffer
            AdvanceWriteBufId();
            
            // Recursive call to write to the new buffer
            return Write(client_order, msg, len, paddedSize);
        }
        
        // Update buffer state
        bufs_[write_buf_id_].tail += paddedSize;
        bufs_[write_buf_id_].num_msg++;
        
        // Check if current batch has reached BATCH_SIZE and seal it
        if ((bufs_[write_buf_id_].tail - head) >= BATCH_SIZE) {
            Embarcadero::BatchHeader* batch_header = 
                reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[write_buf_id_].buffer + head);
            
            batch_header->start_logical_offset = bufs_[write_buf_id_].tail;
            batch_header->batch_seq = batch_seq_.fetch_add(1);
            batch_header->total_size = bufs_[write_buf_id_].tail - head - sizeof(Embarcadero::BatchHeader);
            batch_header->num_msg = bufs_[write_buf_id_].num_msg;

            // Update buffer state for next batch
            bufs_[write_buf_id_].num_msg = 0;
            bufs_[write_buf_id_].writer_head = bufs_[write_buf_id_].tail;
            bufs_[write_buf_id_].tail += sizeof(Embarcadero::BatchHeader);

            // Move to next buffer
            AdvanceWriteBufId();
        }
    }

    // Update header with current message info
    header_.paddedSize = paddedSize;
    header_.size = len;
    header_.client_order = client_order;
    
    // Write header and message to buffer
    memcpy(static_cast<void*>((uint8_t*)buffer + tail), &header_, header_size);
    memcpy(static_cast<void*>((uint8_t*)buffer + tail + header_size), msg, len);
    
    // Memory barrier to ensure data is visible to readers
    //std::atomic_thread_fence(std::memory_order_release);
    
    return true;
}

void* Buffer::Read(int bufIdx) {
    Embarcadero::BatchHeader* batch_header = 
        reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[bufIdx].buffer + bufs_[bufIdx].reader_head);
    
    // Check if batch header contains valid data
    if (batch_header->total_size != 0 && batch_header->num_msg != 0) {
        // Valid batch found, update reader head and return batch
        bufs_[bufIdx].reader_head = batch_header->start_logical_offset;
        return static_cast<void*>(batch_header);
    } else {
        // No valid batch yet, check if we need to wait
        if (!seal_from_read_) {
            bool seal_exist = true;
            
            // Wait for data to become available
            while (batch_header->total_size == 0 || batch_header->num_msg == 0) {
                if (seal_from_read_) {
                    seal_exist = false;
                    break;
                } else {
                    std::this_thread::yield();
                }
            }
            
            if (seal_exist) {
                // Valid batch became available, update reader head and return batch
                bufs_[bufIdx].reader_head = batch_header->start_logical_offset;
                return static_cast<void*>(batch_header);
            }
        }
        
        // Handle end-of-queue conditions or create a final batch from remaining data
        size_t head, tail, num_msg, batch_seq;
        {
            // Check again in case data became available
            if (batch_header->batch_seq != 0 && batch_header->total_size != 0 && batch_header->num_msg != 0) {
                bufs_[bufIdx].reader_head = batch_header->start_logical_offset;
                return static_cast<void*>(batch_header);
            }
            
            // Get current buffer state
            head = bufs_[bufIdx].writer_head;
            tail = bufs_[bufIdx].tail;
            num_msg = bufs_[bufIdx].num_msg;
            
            // No messages, nothing to return
            if (num_msg == 0) {
                return nullptr;
            }
            
            // Prepare for final batch
            bufs_[bufIdx].reader_head = tail;
            bufs_[bufIdx].writer_head = tail;
            bufs_[bufIdx].tail = tail + sizeof(Embarcadero::BatchHeader);
            bufs_[bufIdx].num_msg = 0;
            batch_seq = batch_seq_.fetch_add(1);
        }
        
        // Create final batch header
        Embarcadero::BatchHeader* final_batch_header = 
            reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[bufIdx].buffer + head);
        final_batch_header->batch_seq = batch_seq;
        final_batch_header->total_size = tail - head - sizeof(Embarcadero::BatchHeader);
        final_batch_header->num_msg = num_msg;
        final_batch_header->start_logical_offset = tail;
        
        return static_cast<void*>(final_batch_header);
    }
}
#else
bool Buffer::Write(int bufIdx, size_t client_order, char* msg, size_t len, size_t paddedSize) {
    static const size_t header_size = sizeof(Embarcadero::MessageHeader);
    
    // Update header with current message info
    header_.paddedSize = paddedSize;
    header_.size = len;
    header_.client_order = client_order;
    
    // Check if writing would overflow the buffer
    if (bufs_[bufIdx].tail + header_size + paddedSize > bufs_[bufIdx].len) {
        LOG(ERROR) << "tail:" << bufs_[bufIdx].tail 
                   << " write size:" << paddedSize 
                   << " will go over buffer:" << bufs_[bufIdx].len;
        return false;
    }
    
    // Write header and message to buffer
    memcpy(static_cast<void*>((uint8_t*)bufs_[bufIdx].buffer + bufs_[bufIdx].tail), &header_, header_size);
    memcpy(static_cast<void*>((uint8_t*)bufs_[bufIdx].buffer + bufs_[bufIdx].tail + header_size), msg, len);
    
    // Memory barrier to ensure data is visible to readers
    //std::atomic_thread_fence(std::memory_order_release);
    
    // Update tail position
    bufs_[bufIdx].tail += paddedSize;
    
    return true;
}

void* Buffer::Read(int bufIdx, size_t& len) {
    if (order_ == 3) {
        // For order level 3, read fixed-size batches
        while (!shutdown_ && bufs_[bufIdx].tail - bufs_[bufIdx].reader_head < BATCH_SIZE) {
            std::this_thread::yield();
        }
        
        size_t head = bufs_[bufIdx].reader_head;
        
        // Memory barrier to ensure we see the latest data
        //std::atomic_thread_fence(std::memory_order_acquire);
        
        len = bufs_[bufIdx].tail - head;
        if (len == 0) {
            return nullptr;
        }
        
        // For order level 3, always return a fixed batch size
        len = BATCH_SIZE;
        bufs_[bufIdx].reader_head += BATCH_SIZE;
        
        return static_cast<void*>((uint8_t*)bufs_[bufIdx].buffer + head);
    } else {
        // For other order levels, read all available data
        while (!shutdown_ && bufs_[bufIdx].tail <= bufs_[bufIdx].reader_head) {
            std::this_thread::yield();
        }
				/*
				 * Better version. Test this later
				int spin_count = 0;
				const int MAX_SPIN = 1000;
				const int YIELD_THRESHOLD = 10;

				while (!shutdown_ && bufs_[bufIdx].tail <= bufs_[bufIdx].reader_head) {
						if (spin_count < YIELD_THRESHOLD) {
								// Fast path: CPU spin
								for (volatile int i = 0; i < 10; i++) {}
						} else if (spin_count < MAX_SPIN) {
								// Medium path: yield to other threads
								std::this_thread::yield();
						} else {
								// Slow path: sleep briefly
								std::this_thread::sleep_for(std::chrono::microseconds(1));
						}
						spin_count++;
				}
				*/
        
        size_t head = bufs_[bufIdx].reader_head;
        
        // Memory barrier to ensure we see the latest data
        //std::atomic_thread_fence(std::memory_order_acquire);
        
        size_t tail = bufs_[bufIdx].tail;
        len = tail - head;
        
        // Update reader head to current tail
        bufs_[bufIdx].reader_head = tail;
        
        return static_cast<void*>((uint8_t*)bufs_[bufIdx].buffer + head);
    }
}
#endif

void Buffer::ReturnReads() {
    shutdown_ = true;
}

void Buffer::WriteFinished() {
    seal_from_read_ = true;
}
