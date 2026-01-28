#ifndef EMBARCADERO_STAGING_POOL_H_
#define EMBARCADERO_STAGING_POOL_H_

#include <memory>
#include <vector>
#include <atomic>
#include <cstddef>
#include "folly/MPMCQueue.h"

namespace Embarcadero {

/**
 * StagingPool manages a pool of fixed-size buffers for staging incoming message batches
 * before they are copied to CXL memory. Uses lock-free MPMC queue for thread-safe allocation.
 *
 * Purpose: Decouple fast socket draining from slow CXL allocation (mutex contention)
 *
 * Design:
 * - Pre-allocated buffers (default: 32 × 2MB = 64MB total)
 * - Lock-free allocation/release via folly::MPMCQueue
 * - Returns nullptr when exhausted (triggers backpressure)
 * - Thread-safe for multiple PublishReceiveThreads and CXLAllocationWorkers
 */
class StagingPool {
public:
    /**
     * Constructor
     * @param buffer_size Size of each staging buffer in bytes (default: 2MB)
     * @param num_buffers Number of buffers in the pool (default: 32)
     */
    explicit StagingPool(size_t buffer_size = 2 * 1024 * 1024, size_t num_buffers = 32);

    /**
     * Destructor - cleans up all allocated buffers
     */
    ~StagingPool();

    /**
     * Allocate a staging buffer from the pool
     * @param size Requested size (must be <= buffer_size_)
     * @return Pointer to buffer, or nullptr if pool exhausted or size too large
     *
     * Thread-safe. Non-blocking. Returns immediately.
     */
    void* Allocate(size_t size);

    /**
     * Release a staging buffer back to the pool
     * @param buf Pointer returned from Allocate()
     *
     * Thread-safe. Non-blocking.
     * IMPORTANT: buf must have been allocated from this pool
     */
    void Release(void* buf);

    /**
     * Get current pool utilization as percentage (0-100)
     * @return Percentage of buffers currently allocated
     *
     * Used for monitoring and backpressure decisions
     */
    size_t GetUtilization() const;

    /**
     * Get total number of buffers in pool
     */
    size_t GetTotalBuffers() const { return num_buffers_; }

    /**
     * Get size of each buffer
     */
    size_t GetBufferSize() const { return buffer_size_; }

private:
    // Configuration
    const size_t buffer_size_;      // Size of each buffer (2MB default)
    const size_t num_buffers_;      // Total number of buffers (32 default)

    // Lock-free queue of available buffers
    std::unique_ptr<folly::MPMCQueue<void*>> free_buffers_;

    // Storage for allocated buffers (ownership)
    // Using vector of unique_ptr to ensure proper cleanup
    std::vector<std::unique_ptr<uint8_t[]>> buffer_storage_;

    // Statistics
    std::atomic<size_t> allocated_count_{0};  // Number of buffers currently in use

    // Prevent copying
    StagingPool(const StagingPool&) = delete;
    StagingPool& operator=(const StagingPool&) = delete;
};

} // namespace Embarcadero

#endif // EMBARCADERO_STAGING_POOL_H_
