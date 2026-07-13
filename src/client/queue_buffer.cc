/**
 * QueueBuffer: queue-of-pointers + batch buffer pool (see docs/BUFFER_QUEUE_MIGRATION_DESIGN.md).
 *
 * Single producer writes into batch buffers from a pool and pushes BatchHeader* to N SPSC queues.
 * N consumers dequeue from their queue and must call ReleaseBatch() to return the buffer to the pool.
 * Batch buffers are allocated in one or more hugepage regions (same pattern as buffer.cc).
 */

#include "queue_buffer.h"
#include "common/configuration.h"
#include "common/wire_formats.h"
#include "cxl_manager/cxl_datastructure.h"
#include <glog/logging.h>
#include <chrono>
#include <thread>
#include <cstring>
#include <cstdlib>
#include <sys/mman.h>

namespace {
	constexpr size_t kPoolAcquireTimeoutMs = 100;
	constexpr size_t kQueueFullTimeoutMs = 100;
	constexpr size_t kQueueFullSleepMs = 1;
	constexpr size_t kMaxRegions = 4;  // allow multiple AddBuffers(); pool capacity = pool_slots_ * kMaxRegions
	constexpr size_t kAlign = 64;
	// Steady-state publish pipeline budget. Callers historically passed multi-GB
	// hints (full dataset / threads×brokers×256MB); those must not inflate the
	// hugepage mmap. Exhausted pool → Write() backpressures until ReleaseBatch.
	constexpr size_t kDefaultPoolSizeBytes = 256ULL * 1024 * 1024;
	// Cap must cover the session unacked window (default ~12 GiB/s × 1s lease)
	// or Write() pool-acquires starve while PublishThreads hold slots waiting
	// on WaitForUnackedCapacity. Still << the old 48 GiB (24×1024×2MiB) blow-up.
	constexpr size_t kMaxPoolSizeBytes = 12ULL * 1024 * 1024 * 1024;
	// Slots kept in flight per SPSC queue (not kQueueCapacity=1024). Full-queue
	// sizing was 24×1024×2MiB ≈ 48GiB and blocked SubscribeToCluster past Init.
	constexpr size_t kPipelineSlotsPerQueue = 32;
	inline size_t AlignUp(size_t size, size_t align) {
		return (size + align - 1) & ~(align - 1);
	}
	size_t ResolveMaxPoolBytes() {
		if (const char* env = std::getenv("EMBARCADERO_QUEUE_POOL_MAX_BYTES")) {
			char* end = nullptr;
			unsigned long long parsed = std::strtoull(env, &end, 10);
			if (end != env && *end == '\0' && parsed >= (1ULL << 20)) {
				return static_cast<size_t>(parsed);
			}
			LOG(WARNING) << "Ignoring invalid EMBARCADERO_QUEUE_POOL_MAX_BYTES='" << env << "'";
		}
		return kMaxPoolSizeBytes;
	}
	inline uint16_t RuntimeSessionEpochHint() {
		static const uint16_t value = []() {
			const char* env = std::getenv("EMBARCADERO_SESSION_EPOCH");
			if (!env || env[0] == '\0') return static_cast<uint16_t>(0);
			char* end = nullptr;
			unsigned long parsed = std::strtoul(env, &end, 10);
			if (end == env || *end != '\0') return static_cast<uint16_t>(0);
			return static_cast<uint16_t>(parsed & 0xFFFFUL);
		}();
		return value;
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

	// ORDER=0 and ORDER=5 both use BlogMessageHeader when the feature flag is on.
	// ORDER=0's fast path skips PBR and DelegationThread; blog format is purely a
	// publisher-side wire layout change (stride via ComputeStrideV2 instead of paddedSize).
	use_blog_header_ = (Embarcadero::HeaderUtils::ShouldUseBlogHeader() && (order_ == 0 || order_ == 5));
	if (use_blog_header_) {
		memset(&blog_header_, 0, sizeof(blog_header_));
		blog_header_.client_id = client_id;
		blog_header_.received = 0;
	}

	// [[LINGER]] Time-based batch seal deadline. Bounds low-load publish->deliver latency by
	// sealing a partially-full batch once it has been open >= linger_us_, instead of waiting for
	// the size cap (~10 ms mean batch-fill at low offered load). Under load the size cap fires
	// first, so throughput is unchanged. Precedence: explicit EMBARCADERO_CLIENT_LINGER_US wins;
	// else in latency runtime mode a small default (300 us) is applied; else 0 (legacy size-only).
	// Set once here and never mutated, so the producer hot path reads it lock-free. Guarantee-safe:
	// seal *timing* does not affect total order / per-session FIFO / dedup (assigned downstream in
	// CommitEpoch and enforced by the subscriber hold buffer + session lease/fence/dedup); batch_seq_
	// still monotonically orders a session's batches regardless of when a batch seals.
	{
		linger_us_ = 0;
		if (const char* e = std::getenv("EMBARCADERO_CLIENT_LINGER_US")) {
			char* end = nullptr;
			long long v = std::strtoll(e, &end, 10);
			// Require a clean full parse (reject "300xyz"/"3.5") and clamp to [0, 60s]. The upper
			// bound also guarantees the hot-path std::chrono::microseconds(linger_us_)->nanoseconds
			// widening (x1000) below cannot overflow int64. strtoll returns LLONG_MAX on overflow,
			// which fails the clamp and disables linger rather than triggering UB.
			if (end != e && *end == '\0' && v >= 0 && v <= 60000000) {
				linger_us_ = static_cast<int64_t>(v);
			} else {
				LOG(WARNING) << "QueueBuffer: ignoring invalid EMBARCADERO_CLIENT_LINGER_US='" << e
				             << "' (want an integer 0..60000000 microseconds); linger disabled";
			}
		} else if (const char* mode = std::getenv("EMBARCADERO_RUNTIME_MODE")) {
			if (std::strcmp(mode, "latency") == 0) linger_us_ = 300;
		}
		if (linger_us_ > 0) {
			LOG(INFO) << "QueueBuffer: publisher batch linger enabled, linger_us=" << linger_us_
			          << " (size cap still fires first under load; seal timing is order/FIFO-neutral)";
		}
	}

	queues_.reserve(num_queues_);
	for (size_t i = 0; i < num_queues_; i++) {
		queues_.push_back(std::make_unique<folly::ProducerConsumerQueue<Embarcadero::BatchHeader*>>(kQueueCapacity));
	}
	queue_wait_mutexes_ = std::make_unique<std::mutex[]>(num_queues_);
	queue_wait_cvs_ = std::make_unique<std::condition_variable[]>(num_queues_);
	queue_epochs_ = std::make_unique<std::atomic<uint64_t>[]>(num_queues_);
	queue_active_ = std::make_unique<std::atomic<bool>[]>(num_queues_);
	queue_preferred_ = std::make_unique<std::atomic<bool>[]>(num_queues_);
	for (size_t i = 0; i < num_queues_; ++i) {
		queue_epochs_[i].store(0, std::memory_order_relaxed);
		queue_active_[i].store(true, std::memory_order_relaxed);
		queue_preferred_[i].store(false, std::memory_order_relaxed);
	}
}

QueueBuffer::~QueueBuffer() {
	for (auto& p : batch_buffers_region_) {
		if (p.first && p.second) {
			munmap(p.first, p.second);
		}
	}
	batch_buffers_region_.clear();
}

bool QueueBuffer::AddBuffers(size_t buf_size) {
	// Prefer a single region allocated from Publisher::Init (main thread). Later
	// calls from SubscribeToCluster / AddPublisherThreads are intentional no-ops.
	if (!batch_buffers_region_.empty()) {
		return true;
	}

	const size_t batch_size = BATCH_SIZE;
	batch_size_cached_.store(batch_size, std::memory_order_release);
	slot_size_ = AlignUp(sizeof(Embarcadero::BatchHeader) + batch_size, kAlign);

	const size_t queues = std::max<size_t>(1, num_queues_);
	const size_t min_slots =
		std::max<size_t>(256, queues * kPipelineSlotsPerQueue + 1);
	const size_t max_pool_bytes = ResolveMaxPoolBytes();
	const size_t hint_bytes =
		std::min(std::max(kDefaultPoolSizeBytes, buf_size), max_pool_bytes);
	const size_t slots_for_hint = std::max<size_t>(1, hint_bytes / slot_size_);
	const size_t max_slots = std::max<size_t>(min_slots, max_pool_bytes / slot_size_);
	const size_t slots_this_region =
		std::min(max_slots, std::max(min_slots, slots_for_hint));

	if (!pool_) {
		pool_slots_ = slots_this_region;
		pool_ = std::make_unique<folly::MPMCQueue<Embarcadero::BatchHeader*>>(
			pool_slots_ * kMaxRegions);
	}

	size_t total_bytes = slots_this_region * slot_size_;
	CheckHugePagesAvailable(total_bytes);

	size_t allocated = 0;
	void* region = nullptr;
	try {
		region = mmap_large_buffer(total_bytes, allocated);
	} catch (const std::exception& e) {
		LOG(ERROR) << "QueueBuffer: mmap_large_buffer failed: " << e.what();
		return false;
	}
	if (!region || allocated < slots_this_region * slot_size_) {
		LOG(ERROR) << "QueueBuffer: insufficient allocation " << allocated
		           << " need " << (slots_this_region * slot_size_);
		if (region) munmap(region, allocated);
		return false;
	}

	batch_buffers_region_.emplace_back(region, allocated);

	for (size_t i = 0; i < slots_this_region; i++) {
		Embarcadero::BatchHeader* slot = reinterpret_cast<Embarcadero::BatchHeader*>(
			reinterpret_cast<uint8_t*>(region) + i * slot_size_);
		memset(slot, 0, sizeof(Embarcadero::BatchHeader));
		pool_->write(slot);
	}

	LOG(INFO) << "QueueBuffer::AddBuffers region_slots=" << slots_this_region
	          << " slot_size=" << slot_size_
	          << " pool_bytes=" << (slots_this_region * slot_size_)
	          << " hint_bytes=" << buf_size
	          << " max_pool_bytes=" << max_pool_bytes
	          << " queues=" << queues
	          << " (pipeline_slots_per_queue=" << kPipelineSlotsPerQueue << ")";
	return true;
}

void QueueBuffer::AdvanceWriteBufId() {
	size_t n = active_queues_;
	if (n == 0) return;
	write_buf_id_ = (write_buf_id_ + 1) % n;
}

void QueueBuffer::NotifyQueueDataReady(size_t queue_idx) {
	if (queue_idx >= num_queues_) return;
	queue_epochs_[queue_idx].fetch_add(1, std::memory_order_release);
	queue_wait_cvs_[queue_idx].notify_one();
}

void QueueBuffer::NotifyAllWaiters() {
	for (size_t i = 0; i < num_queues_; ++i) {
		queue_wait_cvs_[i].notify_all();
	}
}

bool QueueBuffer::BeginProducerOp() {
	for (;;) {
		if (session_rollover_paused_fast_.load(std::memory_order_acquire)) {
			std::unique_lock<std::mutex> lock(session_rollover_mu_);
			session_rollover_cv_.wait(lock, [&]() {
				return !session_rollover_paused_ || shutdown_.load(std::memory_order_acquire);
			});
			if (shutdown_.load(std::memory_order_acquire)) return false;
			continue;
		}
		if (shutdown_.load(std::memory_order_acquire)) return false;
		active_producer_ops_.fetch_add(1, std::memory_order_acq_rel);
		if (!session_rollover_paused_fast_.load(std::memory_order_acquire)) {
			return true;
		}
		EndProducerOp();
	}
}

void QueueBuffer::EndProducerOp() {
	const size_t previous = active_producer_ops_.fetch_sub(1, std::memory_order_acq_rel);
	if (previous == 0) {
		active_producer_ops_.fetch_add(1, std::memory_order_relaxed);
		LOG(ERROR) << "QueueBuffer::EndProducerOp called with no active producer op";
		return;
	}
	if (previous == 1 && session_rollover_paused_fast_.load(std::memory_order_acquire)) {
		std::lock_guard<std::mutex> lock(session_rollover_mu_);
		session_rollover_cv_.notify_all();
	}
}

bool QueueBuffer::AcquireNextBatchFromPool(bool stop_on_shutdown, const char* context) {
	if (!pool_) {
		LOG(ERROR) << "QueueBuffer::" << (context ? context : "AcquireNextBatchFromPool")
		           << " called before pool initialization";
		current_batch_ = nullptr;
		current_batch_tail_ = 0;
		current_batch_num_msg_ = 0;
		return false;
	}
	Embarcadero::BatchHeader* next = nullptr;
	constexpr int kPoolSpinBeforeYield = 128;
	int pool_spin = 0;
	auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPoolAcquireTimeoutMs);
	while (!pool_->read(next)) {
		if (stop_on_shutdown && shutdown_.load(std::memory_order_relaxed)) {
			current_batch_ = nullptr;
			current_batch_tail_ = 0;
			current_batch_num_msg_ = 0;
			return false;
		}
		if (++pool_spin % kPoolSpinBeforeYield == 0) {
			if (std::chrono::steady_clock::now() > deadline) {
				LOG(ERROR) << "QueueBuffer::" << (context ? context : "AcquireNextBatchFromPool")
				           << " pool acquire timeout";
				current_batch_ = nullptr;
				current_batch_tail_ = 0;
				current_batch_num_msg_ = 0;
				return false;
			}
			std::this_thread::yield();
		} else {
			Embarcadero::CXL::cpu_pause();
		}
	}
	current_batch_ = next;
	// [[PERF]] Clear only the BatchHeader (128B), not the full 2MB slot.
	// All payload bytes are overwritten by Write() calls before sending.
	// All BatchHeader fields used by ORDER=0 fast path are set explicitly in
	// SealCurrentAndAdvance() and send_batch_header(). Safe for all orders because
	// the broker reads only total_size bytes of payload (never beyond the batch boundary).
	memset(current_batch_, 0, sizeof(Embarcadero::BatchHeader));
	current_batch_tail_ = sizeof(Embarcadero::BatchHeader);
	current_batch_num_msg_ = 0;
	// [[LINGER]] Always reset the batch-open stamp (Write() re-stamps on the first message of the
	// new batch; this keeps it well-defined for the linger check regardless of build flags).
	current_batch_first_submit_time_ = {};
	return true;
}

size_t QueueBuffer::SealCurrentAndAdvance() {
	if (!current_batch_) {
		return 0;
	}
	size_t data_size = current_batch_tail_ - sizeof(Embarcadero::BatchHeader);
	if (data_size == 0) {
		return 0;
	}
	size_t num_sealed = current_batch_num_msg_;

	Embarcadero::BatchHeader* h = current_batch_;
	// Start of payload in this slot (broker may overwrite with BLog logical offset)
	h->start_logical_offset = sizeof(Embarcadero::BatchHeader);
	h->batch_seq = batch_seq_.fetch_add(1, std::memory_order_relaxed);
	h->session_epoch = RuntimeSessionEpochHint();
	h->session_epoch32 = 0;
	h->total_size = data_size;
	h->num_msg = static_cast<uint32_t>(current_batch_num_msg_);
	h->batch_complete = 0;

	std::atomic_thread_fence(std::memory_order_release);

#ifdef COLLECT_LATENCY_STATS
	{
		std::lock_guard<std::mutex> lock(batch_submit_time_mutex_);
		batch_submit_time_[h] = current_batch_first_submit_time_;
	}
#endif

	// [[DEFLECT_WHEN_FULL]] Try current queue; if full, try next queue in round-robin until one accepts.
	// Avoids blocking entire producer on one slow consumer. If all queues full, block on original queue.
	const size_t n = active_queues_;
	if (n == 0) return 0;
	const bool use_preferred = preferred_queue_count_.load(std::memory_order_relaxed) > 0;

	auto queue_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kQueueFullTimeoutMs);
	bool logged = false;
	int spin_count = 0;

retry_push:
	size_t start_id = write_buf_id_;
	bool pushed = false;
	for (size_t i = 0; i < n; i++) {
		size_t idx = (start_id + i) % n;
		if (!queue_active_[idx].load(std::memory_order_relaxed)) {
			continue;
		}
		if (use_preferred && !queue_preferred_[idx].load(std::memory_order_relaxed)) {
			continue;
		}
		folly::ProducerConsumerQueue<Embarcadero::BatchHeader*>* q = queues_[idx].get();
		if (q->write(h)) {
			write_buf_id_ = (idx + 1) % n;
			pushed = true;
			NotifyQueueDataReady(idx);
			break;
		}
	}
	if (!pushed) {
		// All active queues full: keep polling all active queues instead of blocking on just one.
		// This prevents head-of-line blocking if one queue's consumer is dead but hasn't timed out yet.
		bool found_active = false;
		for (size_t i = 0; i < n; i++) {
			if (!queue_active_[i].load(std::memory_order_relaxed)) {
				continue;
			}
			if (use_preferred && !queue_preferred_[i].load(std::memory_order_relaxed)) {
				continue;
			}
			found_active = true;
			break;
		}
		if (!found_active) {
			LOG(ERROR) << "QueueBuffer: all queues inactive. Cannot push batch.";
			ReleaseBatch(h);
			current_batch_ = nullptr;
			current_batch_tail_ = 0;
			return num_sealed;
		}

		constexpr int kQueueFullCheckInterval = 64;
		if (++spin_count % kQueueFullCheckInterval == 0) {
			if (shutdown_.load(std::memory_order_relaxed)) {
				ReleaseBatch(h);
				current_batch_ = nullptr;
				current_batch_tail_ = 0;
				return num_sealed;
			}
			if (std::chrono::steady_clock::now() > queue_deadline) {
				if (!logged) {
					LOG(WARNING) << "QueueBuffer: all active queues full (producers outpaced consumers). Retrying...";
					logged = true;
				}
				std::this_thread::sleep_for(std::chrono::milliseconds(kQueueFullSleepMs));
				queue_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kQueueFullTimeoutMs);
			} else {
				std::this_thread::yield();
			}
		} else {
			Embarcadero::CXL::cpu_pause();
		}
		goto retry_push;
	}

	if (!AcquireNextBatchFromPool(/*stop_on_shutdown=*/true, "SealCurrentAndAdvance")) {
		return num_sealed;
	}
	return num_sealed;
}

bool QueueBuffer::Write(size_t client_order, char* msg, size_t len, size_t paddedSize, size_t& sealed_out) {
	if (!BeginProducerOp()) return false;
	struct EndGuard {
		QueueBuffer* self;
		~EndGuard() { self->EndProducerOp(); }
	} guard{this};

	size_t sealed_count = 0;
	sealed_out = 0;
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
		if (!AcquireNextBatchFromPool(/*stop_on_shutdown=*/false, "Write")) {
			return false;
		}
	}

	// If this message would exceed the slot, seal first then write into new buffer.
	if (current_batch_tail_ + stride > slot_size_) {
		sealed_count += SealCurrentAndAdvance();
		if (!current_batch_) {
			// [[TAIL_SEAL_FIX 2026-07-12]] The batch we just sealed was pushed to a
			// queue and will be sent+acked, but AcquireNextBatchFromPool timed out for
			// THIS message. Surface the sealed count so the caller credits client_order_
			// (otherwise those messages are sent+acked yet never counted, wedging Poll's
			// drain a few hundred messages short of target — seen at N=3 full-stripe and
			// in E4a broker-kill). The caller retries this message under backpressure.
			sealed_out = sealed_count;
			return false;  // pool timeout
		}
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
	if (current_batch_num_msg_ == 0) {
		// [[LINGER]] Stamp the batch-open time unconditionally (was COLLECT_LATENCY_STATS-only).
		// Cost is one steady_clock::now() per batch (~20 ns), and it is required by the linger
		// seal below. Producer-exclusive field, no synchronization needed.
		current_batch_first_submit_time_ = std::chrono::steady_clock::now();
	}
	current_batch_tail_ += stride;
	current_batch_num_msg_++;

	const size_t batch_payload = current_batch_tail_ - sizeof(Embarcadero::BatchHeader);
	// Use cached BATCH_SIZE when set (AddBuffers ran); else fall back to macro (avoids sealing every msg when cache==0).
	const size_t cached = batch_size_cached_.load(std::memory_order_acquire);
	const size_t threshold = (cached != 0) ? cached : BATCH_SIZE;
	bool should_seal = (batch_payload >= threshold);
	// [[LINGER]] Time-based seal: bound how long the oldest message in this batch waits. Only
	// checked when linger is enabled AND the size cap has not already fired (so under load this
	// extra steady_clock::now() is never reached — the size path short-circuits it). Sealing here
	// is race-free: current_batch_/tail/num_msg are producer-exclusive within Write(), and seal
	// timing is neutral to total order / per-session FIFO / dedup (see linger_us_ decl). The
	// message that trips the deadline is included in this batch, so its own latency stays low; the
	// batch's OLDEST message has waited at most ~linger_us_ (mean ~linger_us_/2). NOTE: this bound
	// holds only while a Write() stream keeps flowing — the check runs solely inside Write(), never
	// from a background timer (a bg Seal() racing Write() would tear the batch header — forbidden).
	// A partial batch left by a producer that goes idle waits for the next Write()/Poll()/SealAll/
	// session-rollover/shutdown to seal, not for linger; no data is lost, only that tail's latency.
	if (!should_seal && linger_us_ > 0) {
		const auto age = std::chrono::steady_clock::now() - current_batch_first_submit_time_;
		if (age >= std::chrono::microseconds(linger_us_)) should_seal = true;
	}
	if (should_seal) {
		sealed_count += SealCurrentAndAdvance();
	}
	sealed_out = sealed_count;
	return true;
}

void QueueBuffer::Seal() {
	if (!BeginProducerOp()) return;
	struct EndGuard {
		QueueBuffer* self;
		~EndGuard() { self->EndProducerOp(); }
	} guard{this};
	SealCurrentAndAdvance();
}

size_t QueueBuffer::SealAll() {
	if (!BeginProducerOp()) return 0;
	struct EndGuard {
		QueueBuffer* self;
		~EndGuard() { self->EndProducerOp(); }
	} guard{this};
	return SealCurrentAndAdvance();
}

void* QueueBuffer::Read(int bufIdx) {
	if (bufIdx < 0 || static_cast<size_t>(bufIdx) >= num_queues_) return nullptr;
	const size_t idx = static_cast<size_t>(bufIdx);

	Embarcadero::BatchHeader* batch = nullptr;
	if (queues_[idx]->read(batch)) {
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
	while (!write_finished_.load(std::memory_order_acquire) &&
	       !shutdown_.load(std::memory_order_acquire)) {
		// Empty queue fast path: short spin + event-driven wait avoids yield storms under producer starvation.
		static constexpr int kReadSpinCount = 64;
		for (int i = 0; i < kReadSpinCount; ++i) {
			Embarcadero::CXL::cpu_pause();
		}
		if (queues_[idx]->read(batch)) {
			(void)__atomic_load_n(reinterpret_cast<const size_t*>(&batch->total_size), __ATOMIC_ACQUIRE);
			(void)__atomic_load_n(reinterpret_cast<const uint32_t*>(&batch->num_msg), __ATOMIC_ACQUIRE);
			std::atomic_thread_fence(std::memory_order_acquire);
			__builtin_prefetch(reinterpret_cast<const char*>(batch) + sizeof(Embarcadero::BatchHeader), 0, 3);
			return batch;
		}

		uint64_t observed_epoch = queue_epochs_[idx].load(std::memory_order_acquire);
		if (queues_[idx]->read(batch)) {
			(void)__atomic_load_n(reinterpret_cast<const size_t*>(&batch->total_size), __ATOMIC_ACQUIRE);
			(void)__atomic_load_n(reinterpret_cast<const uint32_t*>(&batch->num_msg), __ATOMIC_ACQUIRE);
			std::atomic_thread_fence(std::memory_order_acquire);
			__builtin_prefetch(reinterpret_cast<const char*>(batch) + sizeof(Embarcadero::BatchHeader), 0, 3);
			return batch;
		}

		std::unique_lock<std::mutex> lock(queue_wait_mutexes_[idx]);
		queue_wait_cvs_[idx].wait_for(lock, std::chrono::microseconds(50), [&]() {
			return shutdown_.load(std::memory_order_acquire) ||
				write_finished_.load(std::memory_order_acquire) ||
				queue_epochs_[idx].load(std::memory_order_acquire) != observed_epoch;
		});
		lock.unlock();
		if (queues_[idx]->read(batch)) {
			(void)__atomic_load_n(reinterpret_cast<const size_t*>(&batch->total_size), __ATOMIC_ACQUIRE);
			(void)__atomic_load_n(reinterpret_cast<const uint32_t*>(&batch->num_msg), __ATOMIC_ACQUIRE);
			std::atomic_thread_fence(std::memory_order_acquire);
			__builtin_prefetch(reinterpret_cast<const char*>(batch) + sizeof(Embarcadero::BatchHeader), 0, 3);
			return batch;
		}
	}
	if (write_finished_.load(std::memory_order_acquire) || shutdown_.load(std::memory_order_acquire)) {
		// [[DRAIN_ON_SHUTDOWN]] Drain queue before returning nullptr so PublishThread sends
		// the final batch(es) pushed by SealAll() just before ReturnReads().
		static constexpr int kShutdownDrainTries = 64;
		for (int i = 0; i < kShutdownDrainTries; ++i) {
			if (queues_[idx]->read(batch)) {
				(void)__atomic_load_n(reinterpret_cast<const size_t*>(&batch->total_size), __ATOMIC_ACQUIRE);
				(void)__atomic_load_n(reinterpret_cast<const uint32_t*>(&batch->num_msg), __ATOMIC_ACQUIRE);
				std::atomic_thread_fence(std::memory_order_acquire);
				__builtin_prefetch(reinterpret_cast<const char*>(batch) + sizeof(Embarcadero::BatchHeader), 0, 3);
				return batch;
			}
			if ((i % 16) == 0) std::this_thread::yield();
		}
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
#ifdef COLLECT_LATENCY_STATS
	{
		std::lock_guard<std::mutex> lock(batch_submit_time_mutex_);
		batch_submit_time_.erase(batch);
	}
#endif
	// [[DESIGN]] No memset: buffer is internal; we send to broker with batch total_size only.
	// Only unoverwritten bytes are padding; nobody reads padding. memset would add cost with no benefit.
	pool_->write(static_cast<Embarcadero::BatchHeader*>(batch));
}

#ifdef COLLECT_LATENCY_STATS
bool QueueBuffer::GetBatchSubmitTime(void* batch, std::chrono::steady_clock::time_point* out_time) {
	if (!batch || !out_time) {
		return false;
	}
	std::lock_guard<std::mutex> lock(batch_submit_time_mutex_);
	auto it = batch_submit_time_.find(batch);
	if (it == batch_submit_time_.end()) {
		return false;
	}
	*out_time = it->second;
	return true;
}
#endif

void QueueBuffer::WriteFinished() {
	write_finished_.store(true, std::memory_order_release);
	NotifyAllWaiters();
}

void QueueBuffer::ReturnReads() {
	shutdown_.store(true, std::memory_order_release);
	NotifyAllWaiters();
	session_rollover_cv_.notify_all();
}

void QueueBuffer::SetActiveQueues(size_t active_count) {
	active_queues_ = (active_count <= num_queues_) ? active_count : num_queues_;
}

void QueueBuffer::MarkQueueInactive(size_t queue_idx) {
	if (queue_active_ && queue_idx < num_queues_) {
		queue_active_[queue_idx].store(false, std::memory_order_relaxed);
	}
}

void QueueBuffer::MarkQueueActive(size_t queue_idx) {
	if (queue_active_ && queue_idx < num_queues_) {
		queue_active_[queue_idx].store(true, std::memory_order_relaxed);
		NotifyQueueDataReady(queue_idx);
	}
}

bool QueueBuffer::IsQueueActive(size_t queue_idx) const {
	if (!queue_active_ || queue_idx >= num_queues_) return false;
	return queue_active_[queue_idx].load(std::memory_order_relaxed);
}

void QueueBuffer::SetPreferredQueues(const std::vector<size_t>& preferred_indices) {
	if (!queue_preferred_) return;
	size_t count = 0;
	for (size_t i = 0; i < num_queues_; ++i) {
		queue_preferred_[i].store(false, std::memory_order_relaxed);
	}
	for (size_t idx : preferred_indices) {
		if (idx >= num_queues_) continue;
		queue_preferred_[idx].store(true, std::memory_order_relaxed);
		++count;
	}
	preferred_queue_count_.store(count, std::memory_order_release);
}

void QueueBuffer::ClearPreferredQueues() {
	if (!queue_preferred_) return;
	for (size_t i = 0; i < num_queues_; ++i) {
		queue_preferred_[i].store(false, std::memory_order_relaxed);
	}
	preferred_queue_count_.store(0, std::memory_order_release);
}

void QueueBuffer::PauseSessionRollover() {
	std::unique_lock<std::mutex> lock(session_rollover_mu_);
	session_rollover_paused_ = true;
	session_rollover_paused_fast_.store(true, std::memory_order_release);
	std::atomic_thread_fence(std::memory_order_seq_cst);
	session_rollover_cv_.wait(lock, [&]() {
		return active_producer_ops_.load(std::memory_order_acquire) == 0 ||
		       shutdown_.load(std::memory_order_acquire);
	});
}

size_t QueueBuffer::SealAllForSessionRollover() {
	std::lock_guard<std::mutex> lock(session_rollover_mu_);
	if (!session_rollover_paused_ || active_producer_ops_.load(std::memory_order_acquire) != 0) {
		LOG(ERROR) << "QueueBuffer::SealAllForSessionRollover called without quiescing producers";
		return 0;
	}
	return SealCurrentAndAdvance();
}

void QueueBuffer::SetNextBatchSeqForNewSession(size_t next_batch_seq) {
	std::lock_guard<std::mutex> lock(session_rollover_mu_);
	if (!session_rollover_paused_ || active_producer_ops_.load(std::memory_order_acquire) != 0) {
		LOG(ERROR) << "QueueBuffer::SetNextBatchSeqForNewSession called without quiescing producers";
	}
	batch_seq_.store(next_batch_seq, std::memory_order_relaxed);
}

void QueueBuffer::ResumeSessionRollover() {
	{
		std::lock_guard<std::mutex> lock(session_rollover_mu_);
		session_rollover_paused_ = false;
		session_rollover_paused_fast_.store(false, std::memory_order_release);
	}
	session_rollover_cv_.notify_all();
}

void QueueBuffer::ResetBatchSeqForNewSession() {
	SetNextBatchSeqForNewSession(0);
}

void QueueBuffer::WarmupBuffers() {
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
