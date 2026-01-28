#include "staging_pool.h"
#include <glog/logging.h>
#include <cstring>

namespace Embarcadero {

StagingPool::StagingPool(size_t buffer_size, size_t num_buffers)
    : buffer_size_(buffer_size),
      num_buffers_(num_buffers),
      free_buffers_(std::make_unique<folly::MPMCQueue<void*>>(num_buffers)) {

    // Validate parameters
    if (buffer_size_ == 0 || num_buffers_ == 0) {
        LOG(FATAL) << "StagingPool: Invalid parameters (buffer_size="
                   << buffer_size_ << ", num_buffers=" << num_buffers_ << ")";
    }

    // Pre-allocate all buffers
    buffer_storage_.reserve(num_buffers_);

    for (size_t i = 0; i < num_buffers_; ++i) {
        // Allocate aligned buffer (64-byte alignment for cache line optimization)
        auto buffer = std::unique_ptr<uint8_t[]>(new (std::align_val_t(64)) uint8_t[buffer_size_]);

        if (!buffer) {
            LOG(FATAL) << "StagingPool: Failed to allocate buffer " << i
                       << " of size " << buffer_size_;
        }

        // Zero-initialize for safety
        std::memset(buffer.get(), 0, buffer_size_);

        void* buf_ptr = buffer.get();
        buffer_storage_.push_back(std::move(buffer));

        // Add to free queue
        if (!free_buffers_->write(buf_ptr)) {
            LOG(FATAL) << "StagingPool: Failed to enqueue buffer " << i
                       << " to free_buffers_ queue";
        }
    }

    LOG(INFO) << "StagingPool initialized: " << num_buffers_ << " buffers × "
              << (buffer_size_ / 1024 / 1024) << " MB = "
              << (num_buffers_ * buffer_size_ / 1024 / 1024) << " MB total";
}

StagingPool::~StagingPool() {
    // Verify all buffers were returned
    size_t leaked = allocated_count_.load(std::memory_order_relaxed);
    if (leaked > 0) {
        LOG(WARNING) << "StagingPool destructor: " << leaked
                     << " buffers still allocated (possible leak)";
    }

    // buffer_storage_ unique_ptrs will auto-cleanup
    LOG(INFO) << "StagingPool destroyed";
}

void* StagingPool::Allocate(size_t size) {
    // Validate size
    if (size > buffer_size_) {
        LOG(ERROR) << "StagingPool::Allocate: Requested size " << size
                   << " exceeds buffer_size " << buffer_size_;
        return nullptr;
    }

    // Try to get buffer from free queue
    void* buf = nullptr;
    if (!free_buffers_->read(buf)) {
        // Pool exhausted
        VLOG(2) << "StagingPool::Allocate: Pool exhausted (utilization="
                << GetUtilization() << "%)";
        return nullptr;
    }

    // Update statistics
    allocated_count_.fetch_add(1, std::memory_order_relaxed);

    VLOG(3) << "StagingPool::Allocate: buffer=" << buf << ", size=" << size
            << ", utilization=" << GetUtilization() << "%";

    return buf;
}

void StagingPool::Release(void* buf) {
    if (!buf) {
        LOG(WARNING) << "StagingPool::Release: Attempted to release nullptr";
        return;
    }

    // Validate buffer belongs to this pool (optional debug check)
    bool valid = false;
    for (const auto& owned_buf : buffer_storage_) {
        if (owned_buf.get() == buf) {
            valid = true;
            break;
        }
    }

    if (!valid) {
        LOG(ERROR) << "StagingPool::Release: Buffer " << buf
                   << " does not belong to this pool";
        return;
    }

    // Return to free queue
    if (!free_buffers_->write(buf)) {
        LOG(FATAL) << "StagingPool::Release: Failed to enqueue buffer " << buf
                   << " (queue full - should never happen)";
    }

    // Update statistics
    size_t prev_count = allocated_count_.fetch_sub(1, std::memory_order_relaxed);
    if (prev_count == 0) {
        LOG(WARNING) << "StagingPool::Release: allocated_count_ underflow "
                     << "(double-free of buffer " << buf << "?)";
    }

    VLOG(3) << "StagingPool::Release: buffer=" << buf
            << ", utilization=" << GetUtilization() << "%";
}

size_t StagingPool::GetUtilization() const {
    size_t allocated = allocated_count_.load(std::memory_order_relaxed);

    // Clamp to valid range (handle potential race conditions)
    if (allocated > num_buffers_) {
        allocated = num_buffers_;
    }

    return (allocated * 100) / num_buffers_;
}

} // namespace Embarcadero
