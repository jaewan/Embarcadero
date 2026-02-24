#pragma once

/**
 * QueueBuffer: queue-of-pointers + batch buffer pool (see docs/BUFFER_QUEUE_MIGRATION_DESIGN.md).
 *
 * Replaces the in-place ring with:
 * - A pool of batch-sized buffers (hugepage-backed), each holding one batch (BatchHeader + payload).
 * - N SPSC queues (Folly ProducerConsumerQueue<BatchHeader*>); producer pushes batch pointers,
 *   consumers pop and must call ReleaseBatch() when done to return the buffer to the pool.
 *
 * Same public API as Buffer batch path plus ReleaseBatch(void* batch).
 * @threading Single producer (Write/Seal/SealAll); N consumers (Read + ReleaseBatch per bufIdx).
 */

#include "common.h"
#include "../cxl_manager/cxl_datastructure.h"
#include "folly/ProducerConsumerQueue.h"
#include "folly/MPMCQueue.h"
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#ifdef COLLECT_LATENCY_STATS
#include <chrono>
#include <unordered_map>
#endif

class QueueBuffer {
public:
	/**
	 * Constructor (same args as Buffer for drop-in testing).
	 * @param num_buf Number of consumer queues / round-robin slots
	 * @param num_threads_per_broker Threads per broker (for stats)
	 * @param client_id Client identifier
	 * @param message_size Message payload size
	 * @param order Order level (e.g. 5 for batch path)
	 */
	QueueBuffer(size_t num_buf, size_t num_threads_per_broker, int client_id, size_t message_size, int order = 0);

	~QueueBuffer();

	/**
	 * Allocate batch buffer pool (hugepage-backed, same as buffer.cc via mmap_large_buffer).
	 * Pool size is at least 12 GB (kPoolSizeBytes) for microbenchmarks, or enough slots for num_queues_*kQueueCapacity+1.
	 * @param buf_size Ignored; pool size and queue capacity derived from config and num_buf
	 * @return true on success
	 */
	bool AddBuffers(size_t buf_size);

	/**
	 * Append one message to current batch; round-robins by write_buf_id_. Seals batch when >= BATCH_SIZE.
	 * @param sealed_out Number of messages sealed by this call (for caller's client_order_ update).
	 * @return true on success.
	 */
	bool Write(size_t client_order, char* msg, size_t len, size_t paddedSize, size_t& sealed_out);

	/**
	 * Dequeue next batch for consumer bufIdx. Returns BatchHeader* or nullptr if empty (or shutdown).
	 * Consumer must call ReleaseBatch(batch) when done with the batch.
	 */
	void* Read(int bufIdx);

	/**
	 * Seal current batch (push to current queue, advance to next, acquire new buffer from pool).
	 */
	void Seal();

	/**
	 * Seal the current batch (if any) so no data is left unqueued.
	 * @return number of messages in the batch sealed (0 if none).
	 */
	size_t SealAll();

	/**
	 * Signal that no more Write() will be called; Read() may return nullptr when queue empty.
	 */
	void WriteFinished();

	/**
	 * Signal shutdown; readers should exit when they see empty + WriteFinished.
	 */
	void ReturnReads();

	/**
	 * Set number of queues actually in use (for round-robin). Use min(active_count, num_queues).
	 * Call when consumer count changes (e.g. after AddPublisherThreads) so producer does not push to ghost queues.
	 * @param active_count Current number of consumer threads (PublishThreads)
	 */
	void SetActiveQueues(size_t active_count);

	/**
	 * Mark a queue as inactive (e.g. after a broker failure).
	 * The producer will stop pushing new batches to this queue.
	 */
	void MarkQueueInactive(size_t queue_idx);

	/**
	 * Pre-touch pool memory (hugepage regions) to fault pages in and reduce measurement variance.
	 * Call explicitly after AddBuffers() and before the hot path. Not done inside AddBuffers()
	 * so callers control when/how (e.g. after thread binding, or skip in tests that don't care).
	 */
	void WarmupBuffers();

	/**
	 * Return a batch buffer to the pool after consumer is done. Must be called with the pointer
	 * returned by Read(bufIdx). No use-after-free after this. Buffer contents are not cleared
	 * (internal use only; broker receives exact total_size; padding is never read).
	 * @param batch The BatchHeader* returned by Read()
	 */
	void ReleaseBatch(void* batch);

#ifdef COLLECT_LATENCY_STATS
	/**
	 * Lookup producer-side submit timestamp for a dequeued batch.
	 * Returns true if timestamp metadata exists.
	 */
	bool GetBatchSubmitTime(void* batch, std::chrono::steady_clock::time_point* out_time);
#endif

private:
	size_t num_queues_;
	// Number of queues actually used for round-robin (min(num_queues_, num_consumers)). Avoids pushing to ghost queues.
	size_t active_queues_;
	size_t num_threads_per_broker_;
	int order_;
	int client_id_;
	size_t message_size_;
	bool use_blog_header_{false};

	// N SPSC queues (producer pushes to queues_[write_buf_id_], consumer bufIdx pops from queues_[bufIdx])
	std::vector<std::unique_ptr<folly::ProducerConsumerQueue<Embarcadero::BatchHeader*>>> queues_;
	// Per-queue wait primitives for event-driven consumer wakeup when queues are empty.
	std::unique_ptr<std::mutex[]> queue_wait_mutexes_;
	std::unique_ptr<std::condition_variable[]> queue_wait_cvs_;
	std::unique_ptr<std::atomic<uint64_t>[]> queue_epochs_;
	std::unique_ptr<std::atomic<bool>[]> queue_active_;
	// Per-queue capacity; pool sized so 1 + num_queues_*kQueueCapacity slots exist.
	// [[ROOT_CAUSE_FIX]] 256 was too small for 10GB test. 512 helped; 1024 gives more headroom when broker recv is slow (docs/NEW_BUFFER_BANDWIDTH_INVESTIGATION.md).
	static constexpr size_t kQueueCapacity = 1024;

	// Batch buffer pool (MPMC: producer acquires, consumers release via ReleaseBatch)
	std::unique_ptr<folly::MPMCQueue<Embarcadero::BatchHeader*>> pool_;
	std::vector<std::pair<void*, size_t>> batch_buffers_region_;  // (base, size) for munmap
	size_t slot_size_{0};   // sizeof(BatchHeader) + batch_size_cached_, 64B aligned
	size_t pool_slots_{0};
	// Cached BATCH_SIZE once in AddBuffers; avoid GetConfig().get() + getenv() on every Write().
	// Atomic so producer (Write) sees value set by cluster-probe thread (AddBuffers).
	std::atomic<size_t> batch_size_cached_{0};

	size_t write_buf_id_{0};
	std::atomic<size_t> batch_seq_{0};
	// [[CACHE_LINE]] Separate producer-hot (batch_seq_) from consumer-read (write_finished_/shutdown_)
	// to avoid false sharing: producer fetch_add on batch_seq_ would otherwise invalidate the
	// cache line that consumers load when Read() returns empty.
	static constexpr size_t kCacheLineBytes = 64;
	static_assert(sizeof(std::atomic<size_t>) <= kCacheLineBytes, "padding assumes atomic<size_t> fits in one cache line");
	char _pad_batch_seq_[kCacheLineBytes - sizeof(std::atomic<size_t>)]{};
	std::atomic<bool> write_finished_{false};
	std::atomic<bool> shutdown_{false};
	// Current batch being filled (producer only)
	Embarcadero::BatchHeader* current_batch_{nullptr};
	size_t current_batch_tail_{0};  // bytes written in current slot (start at sizeof(BatchHeader))
	size_t current_batch_num_msg_{0};  // message count for current batch (for BatchHeader::num_msg)
#ifdef COLLECT_LATENCY_STATS
	std::chrono::steady_clock::time_point current_batch_first_submit_time_{};
#endif

	// Message header templates (same as Buffer)
	Embarcadero::MessageHeader header_;
	Embarcadero::BlogMessageHeader blog_header_;

	void AdvanceWriteBufId();
	void NotifyQueueDataReady(size_t queue_idx);
	void NotifyAllWaiters();
	/** Seal current batch and push to queues_[write_buf_id_], then advance and get new buffer. Returns num_msg in batch sealed (0 if none). */
	size_t SealCurrentAndAdvance();
	/** Debug: true iff batch is a slot base in one of our regions (for ReleaseBatch validation). */
	bool IsValidPoolPointer(void* batch) const;

#ifdef COLLECT_LATENCY_STATS
	mutable std::mutex batch_submit_time_mutex_;
	std::unordered_map<void*, std::chrono::steady_clock::time_point> batch_submit_time_;
#endif
};
