/**
 * QueueBuffer: queue-of-pointers + batch buffer pool (see docs/BUFFER_QUEUE_MIGRATION_DESIGN.md).
 *
 * Single producer writes into batch buffers from a pool and pushes BatchHeader* to N SPSC queues.
 * N consumers dequeue from their queue and must call ReleaseBatch() to return the buffer to the pool.
 * Batch buffers are allocated in one or more hugepage regions (same pattern as buffer.cc).
 */

#include "queue_buffer.h"
#include "../common/configuration.h"
#include "../common/wire_formats.h"
#include "../cxl_manager/cxl_datastructure.h"
#include <glog/logging.h>
#include <chrono>
#include <thread>
#include <cstring>
#include <sys/mman.h>

namespace {
	constexpr size_t kPoolAcquireTimeoutMs = 100;
	constexpr size_t kQueueFullTimeoutMs = 100;
	constexpr size_t kQueueFullSleepMs = 1;
	constexpr size_t kMaxRegions = 4;  // allow multiple AddBuffers(); pool capacity = pool_slots_ * kMaxRegions
	constexpr size_t kAlign = 64;
	/** Target batch pool size (hugepage-backed, same as buffer.cc via mmap_large_buffer). */
	constexpr size_t kPoolSizeBytes = 16ULL * 1024 * 1024 * 1024;  // 16 GB
	inline size_t AlignUp(size_t size, size_t align) {
		return (size + align - 1) & ~(align - 1);
	}
}

QueueBuffer::QueueBuffer(size_t num_buf, size_t num_threads_per_broker, int client_id, size_t message_size, int order)
	: num_queues_(num_buf),
	  active_queues_(num_buf),
	  num_threads_per_broker_(num_threads_per_broker),
	  order_(order),
	  client_id_(client_id),
	  message_size_(message_size) {

	header_.client_id = client_id;
	header_.size = message_size;
	header_.total_order = 0;
	int padding = static_cast<int>(message_size % 64);
	if (padding) padding = 64 - padding;
	header_.paddedSize = message_size + padding + sizeof(Embarcadero::MessageHeader);
	header_.segment_header = nullptr;
	header_.logical_offset = static_cast<size_t>(-1);
	header_.next_msg_diff = 0;

	use_blog_header_ = (Embarcadero::HeaderUtils::ShouldUseBlogHeader() && order_ == 5);
	if (use_blog_header_) {
		memset(&blog_header_, 0, sizeof(blog_header_));
		blog_header_.client_id = client_id;
		blog_header_.received = 0;
	}

	queues_.reserve(num_queues_);
	for (size_t i = 0; i < num_queues_; i++) {
		queues_.push_back(std::make_unique<folly::ProducerConsumerQueue<Embarcadero::BatchHeader*>>(kQueueCapacity));
	}
	VLOG(5) << "QueueBuffer created num_queues=" << num_queues_
	        << " threads_per_broker=" << num_threads_per_broker_
	        << " message_size=" << message_size;
}

QueueBuffer::~QueueBuffer() {
	for (auto& p : batch_buffers_region_) {
		if (p.first && p.second) {
			munmap(p.first, p.second);
		}
	}
	batch_buffers_region_.clear();
}

bool QueueBuffer::AddBuffers(size_t /*buf_size*/) {
	// Publisher calls AddBuffers once per broker from the gRPC (SubscribeToCluster) thread.
	// Each call used to allocate ~16 GB and block for many seconds, so 3 brokers = 60+ s block → Init() timeout.
	// One region already provides enough slots (1 + num_queues_*kQueueCapacity); subsequent calls are no-ops.
	if (!batch_buffers_region_.empty()) {
		return true;
	}

	const size_t batch_size = BATCH_SIZE;
	batch_size_cached_.store(batch_size, std::memory_order_release);
	slot_size_ = AlignUp(sizeof(Embarcadero::BatchHeader) + batch_size, kAlign);
	// Pool size: at least kPoolSizeBytes (16 GB), and at least 1 + num_queues_*kQueueCapacity slots for correctness.
	// Same hugepage path as buffer.cc: mmap_large_buffer() uses MAP_HUGETLB (or THP fallback).
	const size_t min_slots = std::max<size_t>(256, num_queues_ * kQueueCapacity + 1);
	const size_t slots_for_16gb = static_cast<size_t>(kPoolSizeBytes / slot_size_);
	const size_t slots_this_region = std::max(min_slots, slots_for_16gb);

	// First call: set pool_slots_ and create pool with capacity for kMaxRegions
	if (!pool_) {
		pool_slots_ = slots_this_region;
		pool_ = std::make_unique<folly::MPMCQueue<Embarcadero::BatchHeader*>>(pool_slots_ * kMaxRegions);
	}

	size_t total_bytes = slots_this_region * slot_size_;
	// Check hugepage availability before allocating (logs and warns if insufficient).
	CheckHugePagesAvailable(total_bytes);

	size_t allocated = 0;
	void* region = nullptr;
	try {
		region = mmap_large_buffer(total_bytes, allocated);  // hugepage (MAP_HUGETLB) like buffer.cc
	} catch (const std::exception& e) {
		LOG(ERROR) << "QueueBuffer: mmap_large_buffer failed: " << e.what();
		return false;
	}
	if (!region || allocated < slots_this_region * slot_size_) {
		LOG(ERROR) << "QueueBuffer: insufficient allocation " << allocated << " need " << (slots_this_region * slot_size_);
		if (region) munmap(region, allocated);
		return false;
	}

	batch_buffers_region_.emplace_back(region, allocated);

	for (size_t i = 0; i < slots_this_region; i++) {
		Embarcadero::BatchHeader* slot = reinterpret_cast<Embarcadero::BatchHeader*>(
			reinterpret_cast<uint8_t*>(region) + i * slot_size_);
		pool_->write(slot);
	}

	VLOG(3) << "QueueBuffer::AddBuffers region_slots=" << slots_this_region
	        << " slot_size=" << slot_size_
	        << " pool_bytes=" << (slots_this_region * slot_size_)
	        << " total_regions=" << batch_buffers_region_.size();
	return true;
}

void QueueBuffer::AdvanceWriteBufId() {
	size_t n = active_queues_;
	if (n == 0) return;
	write_buf_id_ = (write_buf_id_ + 1) % n;
}

size_t QueueBuffer::SealCurrentAndAdvance() {
	if (!current_batch_) return 0;
	size_t data_size = current_batch_tail_ - sizeof(Embarcadero::BatchHeader);
	if (data_size == 0) return 0;
	size_t num_sealed = current_batch_num_msg_;

	Embarcadero::BatchHeader* h = current_batch_;
	// Start of payload in this slot (broker may overwrite with BLog logical offset)
	h->start_logical_offset = sizeof(Embarcadero::BatchHeader);
	h->batch_seq = batch_seq_.fetch_add(1, std::memory_order_relaxed);
	h->total_size = data_size;
	h->num_msg = static_cast<uint32_t>(current_batch_num_msg_);
	h->batch_complete = 0;

	std::atomic_thread_fence(std::memory_order_release);

	// [[DEFLECT_WHEN_FULL]] Try current queue; if full, try next queue in round-robin until one accepts.
	// Avoids blocking entire producer on one slow consumer. If all queues full, block on original queue.
	const size_t n = active_queues_;
	if (n == 0) return 0;
	size_t start_id = write_buf_id_;
	bool pushed = false;
	for (size_t i = 0; i < n; i++) {
		size_t idx = (start_id + i) % n;
		folly::ProducerConsumerQueue<Embarcadero::BatchHeader*>* q = queues_[idx].get();
		if (q->write(h)) {
			write_buf_id_ = (idx + 1) % n;
			pushed = true;
			break;
		}
	}
	if (!pushed) {
		// All queues full: block on original queue (same as previous behavior).
		constexpr int kQueueFullCheckInterval = 64;
		folly::ProducerConsumerQueue<Embarcadero::BatchHeader*>* q = queues_[start_id].get();
		auto queue_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kQueueFullTimeoutMs);
		bool logged = false;
		int spin_count = 0;
		while (!q->write(h)) {
			if (++spin_count % kQueueFullCheckInterval == 0 &&
			    std::chrono::steady_clock::now() > queue_deadline) {
				if (!logged) {
					LOG(ERROR) << "QueueBuffer: queue full timeout (consumer " << start_id << " slow?)";
					logged = true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(kQueueFullSleepMs));
				queue_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kQueueFullTimeoutMs);
			} else {
				Embarcadero::CXL::cpu_pause();
			}
		}
		write_buf_id_ = (start_id + 1) % n;
	}

	// Acquire next buffer from pool. [[PERF]] Spin with cpu_pause before yield.
	Embarcadero::BatchHeader* next = nullptr;
	constexpr int kPoolSpinBeforeYield = 128;
	int pool_spin = 0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPoolAcquireTimeoutMs);
	while (!pool_->read(next)) {
		if (++pool_spin % kPoolSpinBeforeYield == 0) {
			if (std::chrono::steady_clock::now() > deadline) {
				LOG(ERROR) << "QueueBuffer: pool acquire timeout";
				current_batch_ = nullptr;
				current_batch_tail_ = 0;
				return num_sealed;
			}
			std::this_thread::yield();
		} else {
			Embarcadero::CXL::cpu_pause();
		}
	}
	current_batch_ = next;
	current_batch_tail_ = sizeof(Embarcadero::BatchHeader);
	current_batch_num_msg_ = 0;
	return num_sealed;
}

std::pair<bool, size_t> QueueBuffer::Write(size_t client_order, char* msg, size_t len, size_t paddedSize) {
	size_t sealed_count = 0;
	const size_t v1_header_size = sizeof(Embarcadero::MessageHeader);
	const size_t v2_header_size = sizeof(Embarcadero::BlogMessageHeader);
	const size_t header_size = use_blog_header_ ? v2_header_size : v1_header_size;
	size_t stride = use_blog_header_ ? Embarcadero::wire::ComputeStrideV2(len) : paddedSize;

	if (use_blog_header_) {
		blog_header_.size = static_cast<uint32_t>(len);
		blog_header_.received = 0;
		blog_header_.ts = 0;
		blog_header_.batch_seq = batch_seq_.load(std::memory_order_relaxed);
	} else {
		header_.paddedSize = paddedSize;
		header_.size = len;
		header_.client_order = client_order;
	}

	if (!current_batch_) {
		Embarcadero::BatchHeader* next = nullptr;
		auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPoolAcquireTimeoutMs);
		while (!pool_->read(next)) {
			if (std::chrono::steady_clock::now() > deadline) {
				LOG(ERROR) << "QueueBuffer::Write pool exhausted";
				return {false, sealed_count};
			}
			std::this_thread::yield();
		}
		current_batch_ = next;
		current_batch_tail_ = sizeof(Embarcadero::BatchHeader);
		current_batch_num_msg_ = 0;
	}

	// If this message would exceed the slot, seal first then write into new buffer.
	if (current_batch_tail_ + stride > slot_size_) {
		sealed_count += SealCurrentAndAdvance();
		if (!current_batch_) return {false, sealed_count};  // pool timeout
	}

	uint8_t* base = reinterpret_cast<uint8_t*>(current_batch_);
	// [[PERF]] Prefetch ahead for streaming writes into 2MB batch: immediate line + next line + next message.
	__builtin_prefetch(base + current_batch_tail_, 1, 3);
	__builtin_prefetch(base + current_batch_tail_ + 64, 1, 2);
	__builtin_prefetch(base + current_batch_tail_ + stride, 1, 1);
	if (use_blog_header_) {
		memcpy(base + current_batch_tail_, &blog_header_, header_size);
		memcpy(base + current_batch_tail_ + header_size, msg, len);
	} else {
		memcpy(base + current_batch_tail_, &header_, header_size);
		memcpy(base + current_batch_tail_ + header_size, msg, len);
	}
	current_batch_tail_ += stride;
	current_batch_num_msg_++;

	const size_t batch_payload = current_batch_tail_ - sizeof(Embarcadero::BatchHeader);
	// Use cached BATCH_SIZE when set (AddBuffers ran); else fall back to macro (avoids sealing every msg when cache==0).
	const size_t cached = batch_size_cached_.load(std::memory_order_acquire);
	const size_t threshold = (cached != 0) ? cached : BATCH_SIZE;
	if (batch_payload >= threshold) {
		sealed_count += SealCurrentAndAdvance();
	}
	return {true, sealed_count};
}

void QueueBuffer::Seal() {
	SealCurrentAndAdvance();
}

size_t QueueBuffer::SealAll() {
	return SealCurrentAndAdvance();
}

void* QueueBuffer::Read(int bufIdx) {
	if (bufIdx < 0 || static_cast<size_t>(bufIdx) >= num_queues_) return nullptr;

	Embarcadero::BatchHeader* batch = nullptr;
	if (queues_[static_cast<size_t>(bufIdx)]->read(batch)) {
		// Force acquire so we see the producer's writes (total_size, num_msg, etc.) from
		// SealCurrentAndAdvance. Folly's queue may not guarantee full acquire on the data;
		// an explicit load of a written field pairs with the producer's release fence.
		(void)__atomic_load_n(reinterpret_cast<const size_t*>(&batch->total_size), __ATOMIC_ACQUIRE);
		(void)__atomic_load_n(reinterpret_cast<const uint32_t*>(&batch->num_msg), __ATOMIC_ACQUIRE);
		std::atomic_thread_fence(std::memory_order_acquire);
		// [[PERF]] Prefetch payload so consumer processing doesn't stall on first cache line.
		__builtin_prefetch(reinterpret_cast<const char*>(batch) + sizeof(Embarcadero::BatchHeader), 0, 3);
		return batch;
	}
	if (write_finished_.load(std::memory_order_acquire) || shutdown_.load(std::memory_order_acquire)) {
		return nullptr;
	}
	return nullptr;
}

bool QueueBuffer::IsValidPoolPointer(void* batch) const {
	if (!batch || batch_buffers_region_.empty() || slot_size_ == 0) return false;
	const auto* ptr = static_cast<const uint8_t*>(batch);
	for (const auto& region : batch_buffers_region_) {
		const auto* base = static_cast<const uint8_t*>(region.first);
		size_t size = region.second;
		if (ptr >= base && ptr < base + size) {
			// Must be at slot boundary (BatchHeader* at base + i*slot_size_)
			size_t offset = static_cast<size_t>(ptr - base);
			if (offset % slot_size_ == 0) return true;
			return false;
		}
	}
	return false;
}

void QueueBuffer::ReleaseBatch(void* batch) {
	if (!batch || !pool_) return;
#ifndef NDEBUG
	if (!IsValidPoolPointer(batch)) {
		LOG(ERROR) << "QueueBuffer::ReleaseBatch invalid pointer (not from this pool or double release?)";
		return;
	}
#endif
	// [[DESIGN]] No memset: buffer is internal; we send to broker with batch total_size only.
	// Only unoverwritten bytes are padding; nobody reads padding. memset would add cost with no benefit.
	pool_->write(static_cast<Embarcadero::BatchHeader*>(batch));
}

void QueueBuffer::WriteFinished() {
	write_finished_.store(true, std::memory_order_release);
}

void QueueBuffer::ReturnReads() {
	shutdown_.store(true, std::memory_order_release);
}

void QueueBuffer::SetActiveQueues(size_t active_count) {
	active_queues_ = (active_count <= num_queues_) ? active_count : num_queues_;
}

void QueueBuffer::WarmupBuffers() {
	VLOG(2) << "QueueBuffer::WarmupBuffers touching pool...";
	for (auto& region : batch_buffers_region_) {
		if (!region.first || region.second == 0) continue;
		volatile char* p = static_cast<volatile char*>(region.first);
		const size_t stride = default_huge_page_size();
		for (size_t i = 0; i < region.second; i += stride) {
			(void)p[i];
		}
		if (region.second > 0) (void)p[region.second - 1];
	}
}