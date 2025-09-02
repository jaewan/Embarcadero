#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include "../common/performance_utils.h"

namespace Embarcadero {

// Zero-copy buffer pool for efficient message passing
class ZeroCopyBufferPool {
public:
    struct Buffer {
        void* data;
        size_t size;
        std::atomic<bool> in_use{false};
        
        Buffer() : data(nullptr), size(0) {}
        Buffer(void* d, size_t s) : data(d), size(s) {}
    };
    
    ZeroCopyBufferPool(void* base_addr, size_t total_size, size_t buffer_size)
        : base_addr_(base_addr), total_size_(total_size), buffer_size_(buffer_size) {
        
        num_buffers_ = total_size / buffer_size;
        buffers_ = new Buffer[num_buffers_];
        
        // Initialize buffers
        for (size_t i = 0; i < num_buffers_; ++i) {
            buffers_[i].data = static_cast<char*>(base_addr_) + (i * buffer_size_);
            buffers_[i].size = buffer_size_;
        }
    }
    
    ~ZeroCopyBufferPool() {
        delete[] buffers_;
    }
    
    // Try to acquire a buffer without copying
    Buffer* TryAcquire() {
        for (size_t i = 0; i < num_buffers_; ++i) {
            bool expected = false;
            if (buffers_[i].in_use.compare_exchange_weak(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed)) {
                return &buffers_[i];
            }
        }
        return nullptr;
    }
    
    // Release a buffer back to the pool
    void Release(Buffer* buffer) {
        buffer->in_use.store(false, std::memory_order_release);
    }
    
    // Get a zero-copy view of data at offset
    ZeroCopyBuffer GetView(size_t offset, size_t size) const {
        if (offset + size > total_size_) {
            throw std::out_of_range("Buffer view out of range");
        }
        return ZeroCopyBuffer(static_cast<char*>(base_addr_) + offset, size);
    }

private:
    void* base_addr_;
    size_t total_size_;
    size_t buffer_size_;
    size_t num_buffers_;
    Buffer* buffers_;
};

// Optimized batch processing with zero-copy
class ZeroCopyBatchProcessor {
public:
    using ProcessCallback = std::function<void(const ZeroCopyBuffer&)>;
    
    // Process messages in batch without copying
    static void ProcessBatch(void* batch_start, size_t batch_size, 
                           size_t message_count, ProcessCallback callback) {
        char* current = static_cast<char*>(batch_start);
        char* end = current + batch_size;
        
        for (size_t i = 0; i < message_count && current < end; ++i) {
            // Assume first 8 bytes contain message size
            size_t msg_size = *reinterpret_cast<size_t*>(current);
            
            // Create zero-copy view of the message
            ZeroCopyBuffer msg_view(current + sizeof(size_t), msg_size);
            
            // Process without copying
            callback(msg_view);
            
            // Move to next message (aligned to 8 bytes)
            current += sizeof(size_t) + ((msg_size + 7) & ~7);
        }
    }
    
    // Scatter-gather I/O support for zero-copy disk writes
    struct IoVector {
        void* base;
        size_t len;
    };
    
    static void GatherBuffers(const ZeroCopyBuffer* buffers, size_t count,
                            IoVector* iovecs) {
        for (size_t i = 0; i < count; ++i) {
            iovecs[i].base = const_cast<void*>(buffers[i].Data());
            iovecs[i].len = buffers[i].Size();
        }
    }
};

} // namespace Embarcadero
