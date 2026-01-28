/**
 * Embarcadero Lock-Free Buffer System
 * ==================================
 *
 * OVERVIEW:
 * This is a lock-free buffer implementation designed for a single-writer,
 * multiple-reader pattern. It uses a circular buffer approach with batch-oriented
 * writes to maximize throughput while eliminating lock contention.
 *
 * ARCHITECTURE:
 * - Multiple buffers are allocated (one per reader thread)
 * - Writer rotates through these buffers in round-robin fashion
 * - Each buffer is divided into "batches" marked by BatchHeader structures
 *   [BatchHeader](msg)(msg)......[BatchHeader](msg)......
 * - Writer writes messages into batches until reaching BATCH_SIZE or calls Seal()
 * - Readers continuously poll their assigned buffer for completed batches
 *
 * COORDINATION MECHANISM:
 * The coordination between writer and readers is achieved through careful memory
 * ordering and state variables, without using explicit locks.
 *
 * WRITER-CONTROLLED VARIABLES:
 * - bufs_[i].prod.tail: Current write position in the buffer
 * - bufs_[i].prod.writer_head: Start position of the current batch
 * - bufs_[i].prod.num_msg: Number of messages in the current batch
 * - write_buf_id_: ID of the buffer currently being written to
 * - batch_seq_: Atomically incremented sequence number for batches
 *
 * READER-CONTROLLED VARIABLES:
 * - bufs_[i].cons.reader_head: Position from which the reader is currently reading
 *   - This points to the batch header
 *
 * SYNCHRONIZATION POINTS:
 * 1. Writer -> Reader: BatchHeader fields (especially total_size and num_msg)
 *    - Writer updates these fields atomically when sealing a batch
 *    - Readers poll these fields to detect completed batches
 *
 * 2. Reader -> Writer: bufs_[i].reader_head
 *    - Writer can check this to know how much buffer space is available
 *    - Important for buffer wrapping logic
 *
 * MEMORY ORDERING:
 * Memory barriers are critical for correct operation:
 * - Writer uses memory_order_release when updating batch headers
 * - Reader uses memory_order_acquire when reading batch headers
 *
 * BUFFER LIFECYCLE:
 * 1. Writer adds message to current batch in current buffer
 * 2. If batch full (≥ BATCH_SIZE) or Seal() called, writer:
 *    a. Updates BatchHeader with metadata
 *    b. Uses memory barrier to ensure visibility
 *    c. Moves to next buffer
 * 3. Reader continually checks BatchHeader
 *    a. When total_size and num_msg are non-zero, batch is ready
 *    b. Reader processes batch and updates reader_head
 *
 * CRITICAL INVARIANTS:
 * 1. Only a single writer thread ever calls Write() and Seal()
 * 2. Each reader thread only reads from its assigned buffer
 * 3. BatchHeader fields total_size and num_msg must be updated last
 *    and only after all message data is written
 * 4. Memory barriers must be used at synchronization points
 *
 * FAILURE MODES:
 * - Memory ordering issues: Readers see partially written data
 * - Buffer overflow: Writer wraps around before reader finishes
 * - Reader starvation: Writer moves too quickly through buffers
 *
 * (TODO) PERFORMANCE CONSIDERATIONS:
 * - Avoid busy-waiting where possible
 * - Use exponential backoff in reader polling loops
 * - Properly size buffers based on message rate and processing time
 */

#include "buffer.h"
#include "../common/configuration.h"
#include "../common/wire_formats.h"
#include "../common/performance_utils.h"
#include "../cxl_manager/cxl_datastructure.h"
#include <thread>
#include <chrono>

Buffer::Buffer(size_t num_buf, size_t num_threads_per_broker, int client_id, size_t message_size, int order) 
	: bufs_(num_buf), 
	num_threads_per_broker_(num_threads_per_broker), 
	order_(order),
	client_id_(client_id) {

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

		// [[BLOG_HEADER: Initialize BlogMessageHeader template for ORDER=5 direct emission]]
		// Only used if HeaderUtils::ShouldUseBlogHeader() is true and order_==5
		if (Embarcadero::HeaderUtils::ShouldUseBlogHeader() && order_ == 5) {
			memset(&blog_header_, 0, sizeof(blog_header_));
			blog_header_.client_id = client_id;
			blog_header_.received = 0;  // Will be set by publisher when message is ready
		}

		VLOG(5) << "Buffer created with " << num_buf << " buffers, " 
			<< num_threads_per_broker << " threads per broker, "
			<< "message size: " << message_size 
			<< ", padded size: " << header_.paddedSize
			<< (Embarcadero::HeaderUtils::ShouldUseBlogHeader() && order_ == 5 ? " (using BlogMessageHeader)" : "");
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
       // OPTIMIZED: 768MB buffer size - perfect for 10GB E2E throughput tests with 4 brokers
       // 
       // BUFFER SIZE RATIONALE FOR 768MB:
       // • E2E test sends 10.7GB total across 4 brokers = 2.675GB per broker
       // • 4 threads per broker × 768MB = 3.072GB buffer capacity per broker
       // • This provides 15% safety margin (3.072GB > 2.675GB) without buffer wrapping
       // • Total system memory: 16 buffers × 768MB = 12.3GB (sufficient for 10.7GB dataset)
       //
       // HUGEPAGE ALIGNMENT:
       // • System hugepage size: 2MB (confirmed from /proc/meminfo)
       // • 768MB ÷ 2MB = 384 hugepages (perfect alignment, no fragmentation)
       // • May fall back to THP but performance impact is acceptable for no-wrap benefit
       //
       // PERFORMANCE TRADE-OFF:
       // • Accepts potential THP fallback to eliminate buffer wrapping overhead
       // • Buffer wrapping causes ~60% message loss (39.4% → 100% completion)
       // • 768MB ensures complete dataset fits without wrapping for optimal throughput
       // Use configured buffer size from YAML instead of hard-coded value
       const Embarcadero::Configuration& config = Embarcadero::Configuration::getInstance();
       size_t configured_size = config.config().client.publisher.buffer_size_mb.get() * 1024 * 1024; // Convert MB to bytes
       buf_size = configured_size;
	
	VLOG(3) << "Buffer::AddBuffers using optimized buffer size: " << (buf_size / (1024*1024)) 
	        << "MB for reliable hugepage allocation and peak performance";

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
		bufs_[idx + i].prod.tail.store(sizeof(Embarcadero::BatchHeader), std::memory_order_relaxed);
#endif
	}

	return true;
}

void Buffer::AdvanceWriteBufId() {
	// FIXED: Simple round-robin across all buffers to ensure even distribution
	write_buf_id_ = (write_buf_id_ + 1) % num_buf_;
	
	// Calculate broker and thread from buffer ID
	i_ = write_buf_id_ / num_threads_per_broker_;  // broker ID
	j_ = write_buf_id_ % num_threads_per_broker_;  // thread ID within broker
}

void Buffer::WarmupBuffers() {
	VLOG(2) << "Starting buffer warmup to reduce measurement variance...";
	auto warmup_start = std::chrono::high_resolution_clock::now();
	
	// Pre-touch all allocated hugepage buffers to ensure virtual addresses are populated
	// This reduces variance during actual performance measurement by eliminating
	// page fault overhead and ensuring hugepages are fully committed
	size_t total_buffers_touched = 0;
	size_t total_bytes_touched = 0;
	
	for (size_t buf_idx = 0; buf_idx < num_buf_.load(); buf_idx++) {
		if (bufs_[buf_idx].buffer != nullptr && bufs_[buf_idx].len > 0) {
			void* buffer = bufs_[buf_idx].buffer;
			size_t buffer_size = bufs_[buf_idx].len;
			
			// Touch every page in the buffer (4KB stride for regular pages, 2MB for hugepages)
			// Use hugepage size stride for efficiency since we're using hugepages
			const size_t stride = default_huge_page_size(); // 2MB for hugepages
			volatile char* buf_ptr = static_cast<volatile char*>(buffer);
			
			for (size_t offset = 0; offset < buffer_size; offset += stride) {
				// Read and write to ensure page is fully committed
				volatile char temp = buf_ptr[offset];
				buf_ptr[offset] = temp;
			}
			
			// Also touch the last byte to ensure the entire buffer is committed
			if (buffer_size > 0) {
				volatile char temp = buf_ptr[buffer_size - 1];
				buf_ptr[buffer_size - 1] = temp;
			}
			
			total_buffers_touched++;
			total_bytes_touched += buffer_size;
		}
	}
	
	auto warmup_end = std::chrono::high_resolution_clock::now();
	double warmup_seconds = std::chrono::duration<double>(warmup_end - warmup_start).count();
	
	LOG(INFO) << "Buffer warmup completed: touched " << total_buffers_touched 
	          << " buffers (" << (total_bytes_touched / (1024*1024)) << " MB) in "
	          << std::fixed << std::setprecision(3) << warmup_seconds << "s";
}

#ifdef BATCH_OPTIMIZATION
bool Buffer::Write(size_t client_order, char* msg, size_t len, size_t paddedSize) {
	// [[BLOG_HEADER: Determine header format based on feature flag and order]]
	bool use_blog_header = Embarcadero::HeaderUtils::ShouldUseBlogHeader() && order_ == 5;
	static const size_t v1_header_size = sizeof(Embarcadero::MessageHeader);
	static const size_t v2_header_size = sizeof(Embarcadero::BlogMessageHeader);
	size_t header_size = use_blog_header ? v2_header_size : v1_header_size;
	
	// [[BLOG_HEADER: Compute stride based on header version]]
	// V1: stride = header + payload, aligned to 64B
	// V2: stride = 64B header + payload, aligned to 64B
	// Both use the same alignment but V2 always starts at 64B
	size_t stride;
	if (use_blog_header) {
		// V2: compute stride from payload bytes only
		stride = Embarcadero::wire::ComputeStrideV2(len);
	} else {
		// V1: use paddedSize (already aligned)
		stride = paddedSize;
	}

	void* buffer;
	size_t head, tail;

	// Update header with current message info
	if (use_blog_header) {
		// [[BLOG_HEADER: Populate BlogMessageHeader fields - Minimal Publisher Work]]
		// Paper spec: Receiver (NetworkManager) sets receiver region fields (bytes 0-15)
		// Publisher only sets size and metadata - avoid expensive operations like rdtsc()
		// This reduces publisher-side overhead and aligns with paper's Stage 1 (Receiver) responsibility
		blog_header_.size = static_cast<uint32_t>(len);  // Payload bytes only
		// received and ts will be set by NetworkManager (receiver) when batch is actually received
		// This avoids per-message rdtsc() overhead in publisher hot path
		blog_header_.received = 0;  // Receiver will set to 1 when received
		blog_header_.ts = 0;  // Receiver will set timestamp when received
		
		// Read-only metadata (bytes 48-63)
		blog_header_.batch_seq = batch_seq_.load(std::memory_order_relaxed);
		
		// Delegation and sequencer fields remain 0 (will be set by broker)
		// blog_header_.counter = 0;
		// blog_header_.processed_ts = 0;
		// blog_header_.total_order = 0;
		// blog_header_.ordered_ts = 0;
	} else {
		// [[V1: Legacy MessageHeader]]
		header_.paddedSize = paddedSize;
		header_.size = len;
		header_.client_order = client_order;
	}

	// Critical section for buffer access
	{
		size_t lockedIdx = write_buf_id_;
		buffer = bufs_[write_buf_id_].buffer;
		head = bufs_[write_buf_id_].prod.writer_head.load(std::memory_order_relaxed);
		tail = bufs_[write_buf_id_].prod.tail.load(std::memory_order_relaxed);

		// Check if buffer is full and needs to be wrapped
		if (tail + header_size + stride + stride /*buffer margin*/ > bufs_[lockedIdx].len) {
			// FIXED: Check if reader has consumed data before wrapping
			size_t reader_head = bufs_[lockedIdx].cons.reader_head.load(std::memory_order_relaxed);
			
			// If reader hasn't caught up, we need to wait or switch buffers
			if (reader_head == 0) {
				// Reader hasn't consumed any data - this buffer is still full
				// Switch to next buffer instead of wrapping current one
				VLOG(3) << "Buffer:" << write_buf_id_ << " full and reader hasn't consumed data. Switching to next buffer.";
				
				// Seal current batch before moving to next buffer
				Embarcadero::BatchHeader* batch_header = 
					reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[write_buf_id_].buffer + head);

				batch_header->start_logical_offset = bufs_[write_buf_id_].prod.tail.load(std::memory_order_relaxed);
				batch_header->batch_seq = batch_seq_.fetch_add(1);
				batch_header->total_size = bufs_[write_buf_id_].prod.tail.load(std::memory_order_relaxed) - head - sizeof(Embarcadero::BatchHeader);
				batch_header->num_msg = bufs_[write_buf_id_].prod.num_msg.load(std::memory_order_relaxed);

				// Move to next buffer (don't reset this buffer - let reader consume it)
				AdvanceWriteBufId();

				// Recursive call to write to the new buffer
				return Write(client_order, msg, len, paddedSize);
			} else {
				// Reader has consumed some data - safe to wrap buffer
				VLOG(3) << "Buffer:" << write_buf_id_ << " full. Reader consumed " << reader_head 
				        << " bytes. Safe to wrap buffer.";

				// Seal current batch before wrapping
				Embarcadero::BatchHeader* batch_header = 
					reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[write_buf_id_].buffer + head);

				batch_header->start_logical_offset = bufs_[write_buf_id_].prod.tail.load(std::memory_order_relaxed);
				batch_header->batch_seq = batch_seq_.fetch_add(1);
				batch_header->total_size = bufs_[write_buf_id_].prod.tail.load(std::memory_order_relaxed) - head - sizeof(Embarcadero::BatchHeader);
				batch_header->num_msg = bufs_[write_buf_id_].prod.num_msg.load(std::memory_order_relaxed);

				// Reset buffer state for new batch (safe because reader consumed data)
				bufs_[write_buf_id_].prod.num_msg.store(0, std::memory_order_relaxed);
				bufs_[write_buf_id_].prod.writer_head.store(0, std::memory_order_relaxed);
				bufs_[write_buf_id_].prod.tail.store(sizeof(Embarcadero::BatchHeader), std::memory_order_relaxed);
				
				// Reset reader head to indicate buffer is available for reuse
				bufs_[write_buf_id_].cons.reader_head.store(0, std::memory_order_relaxed);

				// Continue writing to same buffer (now wrapped)
				return Write(client_order, msg, len, paddedSize);
			}
		}
	}

	// (NOTE) Current logic does not restrictively check if newly written message goes out of BATCH_SIZE
	// If new message is very large (unlikely) it can degrade performance as a batch can be too large
	// to send over network.
	
	// [[BLOG_HEADER: Write header and message based on version]]
	if (use_blog_header) {
		// V2: Write BlogMessageHeader directly
		memcpy(static_cast<void*>((uint8_t*)buffer + tail), &blog_header_, v2_header_size);
		memcpy(static_cast<void*>((uint8_t*)buffer + tail + v2_header_size), msg, len);
	} else {
		// V1: Write MessageHeader
		memcpy(static_cast<void*>((uint8_t*)buffer + tail), &header_, v1_header_size);
		memcpy(static_cast<void*>((uint8_t*)buffer + tail + v1_header_size), msg, len);
	}

	// Update buffer state - keep it simple and fast
	size_t new_tail = bufs_[write_buf_id_].prod.tail.fetch_add(stride, std::memory_order_relaxed) + stride;
	bufs_[write_buf_id_].prod.num_msg.fetch_add(1, std::memory_order_relaxed);
	
	// Check if current batch has reached BATCH_SIZE and seal it
	// OPTIMIZATION: Use the already calculated new_tail instead of loading again
	const size_t EFFECTIVE_BATCH_SIZE = BATCH_SIZE;  // Config-driven batch size
	if ((new_tail - head) >= EFFECTIVE_BATCH_SIZE) {
		static thread_local size_t seal_logs = 0;
		if (++seal_logs <= 5 || seal_logs % 1000 == 0) {
			VLOG(3) << "Buffer::Write sealing batch at size=" << (new_tail - head)
			        << " (EFFECTIVE_BATCH_SIZE=" << EFFECTIVE_BATCH_SIZE << ")";
		}
		Seal();
	}


	return true;
}

void Buffer::Seal(){
	size_t lockedIdx = write_buf_id_;
	size_t head = bufs_[lockedIdx].prod.writer_head.load(std::memory_order_relaxed);
	// Check if any data written
	if ((bufs_[lockedIdx].prod.tail.load(std::memory_order_relaxed) - head) > sizeof(Embarcadero::BatchHeader)) {
		Embarcadero::BatchHeader* batch_header = 
			reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[lockedIdx].buffer + head);

		batch_header->start_logical_offset = bufs_[lockedIdx].prod.tail.load(std::memory_order_relaxed);
		batch_header->batch_seq = batch_seq_.fetch_add(1);
		batch_header->total_size = bufs_[lockedIdx].prod.tail.load(std::memory_order_relaxed) - head - sizeof(Embarcadero::BatchHeader);
		batch_header->num_msg = bufs_[lockedIdx].prod.num_msg.load(std::memory_order_relaxed);
		
		// Note: batch_complete will be set by NetworkManager when batch is received
		// For locally created batches, sequencer will use fallback logic (checking paddedSize)
		batch_header->batch_complete = 0;  // Initialize batch completion flag

		// [[FIX: Memory Barrier]] Release fence ensures all batch header writes are visible
		// to reader threads before we update buffer state. Without this, readers may see
		// stale batch_header fields on non-x86 architectures or with aggressive compiler opts.
		std::atomic_thread_fence(std::memory_order_release);

		// Update buffer state for next batch
		bufs_[lockedIdx].prod.num_msg.store(0, std::memory_order_relaxed);
		bufs_[lockedIdx].prod.writer_head.store(bufs_[lockedIdx].prod.tail.load(std::memory_order_relaxed), std::memory_order_relaxed);
		bufs_[lockedIdx].prod.tail.fetch_add(sizeof(Embarcadero::BatchHeader), std::memory_order_relaxed);

		// Move to next buffer
		AdvanceWriteBufId();
	} else {
		LOG(INFO) << "Buffer::Seal: No data to seal in buffer " << lockedIdx 
		          << ", head=" << head << ", tail=" << bufs_[lockedIdx].prod.tail.load(std::memory_order_relaxed);
	}
}

void Buffer::SealAll() {
	const size_t total_bufs = num_buf_.load(std::memory_order_relaxed);
	size_t sealed_buffers = 0;
	size_t sealed_messages = 0;
	size_t sealed_bytes = 0;
	for (size_t idx = 0; idx < total_bufs; ++idx) {
		size_t head = bufs_[idx].prod.writer_head.load(std::memory_order_relaxed);
		size_t tail = bufs_[idx].prod.tail.load(std::memory_order_relaxed);
		if ((tail - head) > sizeof(Embarcadero::BatchHeader)) {
			Embarcadero::BatchHeader* batch_header =
				reinterpret_cast<Embarcadero::BatchHeader*>(
					reinterpret_cast<uint8_t*>(bufs_[idx].buffer) + head);

			batch_header->start_logical_offset = tail;
			batch_header->batch_seq = batch_seq_.fetch_add(1);
			batch_header->total_size = tail - head - sizeof(Embarcadero::BatchHeader);
			batch_header->num_msg = bufs_[idx].prod.num_msg.load(std::memory_order_relaxed);
			batch_header->batch_complete = 0;
			sealed_buffers++;
			sealed_messages += batch_header->num_msg;
			sealed_bytes += batch_header->total_size;

			// Reset buffer state for next batch
			bufs_[idx].prod.num_msg.store(0, std::memory_order_relaxed);
			bufs_[idx].prod.writer_head.store(tail, std::memory_order_relaxed);
			bufs_[idx].prod.tail.fetch_add(sizeof(Embarcadero::BatchHeader), std::memory_order_relaxed);
		}
	}
	if (sealed_buffers > 0) {
		LOG(INFO) << "Buffer::SealAll sealed_buffers=" << sealed_buffers
		          << " sealed_messages=" << sealed_messages
		          << " sealed_bytes=" << sealed_bytes;
	}
}

void* Buffer::Read(int bufIdx) {
	// [[FIX: Memory Ordering]] Use acquire to synchronize with writer's release fence
	Embarcadero::BatchHeader* batch_header =
		reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[bufIdx].buffer + bufs_[bufIdx].cons.reader_head.load(std::memory_order_acquire));

	// Check if batch header contains valid data
	if (batch_header->total_size != 0 && batch_header->num_msg != 0) {
		// [[FIX: Memory Barrier]] Acquire fence ensures we see all batch data written
		// before total_size/num_msg were set. Pairs with writer's release fence in Seal().
		std::atomic_thread_fence(std::memory_order_acquire);

		// Valid batch found, update reader head and return batch
		size_t next_head = batch_header->start_logical_offset;

		// Safety check for invalid next head pointer
		if (next_head > bufs_[bufIdx].len || next_head < sizeof(Embarcadero::BatchHeader)) {
			LOG(WARNING) << "Invalid next_head " << next_head
				<< " for buffer " << bufIdx
				<< " (len: " << bufs_[bufIdx].len << ")";

			// Return current batch but don't update reader_head
			return static_cast<void*>(batch_header);
		}

		bufs_[bufIdx].cons.reader_head.store(next_head, std::memory_order_relaxed);
		return static_cast<void*>(batch_header);
	}

	// No writing in this buffer. Do not busy wait
	if (bufs_[bufIdx].prod.writer_head.load(std::memory_order_relaxed) == bufs_[bufIdx].prod.tail.load(std::memory_order_relaxed)) {
		return nullptr;
	}

	// Busy wait only when some messages are in the buffer.
	// Wait for the batch to be sealed. (Either full batch written or sealed by Client)
	// [[LAST_PERCENT_ACK_FIX]] Use 2s timeout so we don't return nullptr before SealAll() runs.
	// [[FIX: Exponential Backoff]] Reduces CPU waste from 3 of 4 threads spinning on empty buffers
	// Phase 1: Fast spin (100 iters) - low latency when data arrives quickly
	// Phase 2: Yield (1000 iters) - cooperative for concurrent threads
	// Phase 3: Sleep - reduces CPU to ~25% vs constant yield()
	auto start_time = std::chrono::steady_clock::now();
	const auto timeout = std::chrono::seconds(2);
	constexpr size_t SPIN_ITERS = 100;
	constexpr size_t YIELD_ITERS = 1000;
	constexpr size_t TIME_CHECK_INTERVAL = 1000;
	size_t wait_iters = 0;

	while (batch_header->total_size == 0 || batch_header->num_msg == 0) {
		++wait_iters;

		if (wait_iters <= SPIN_ITERS) {
			// Phase 1: Fast spin for low-latency response
			Embarcadero::CXL::cpu_pause();
		} else if (wait_iters <= YIELD_ITERS) {
			// Phase 2: Yield to other threads
			std::this_thread::yield();
		} else {
			// Phase 3: Brief sleep to reduce CPU waste
			std::this_thread::sleep_for(std::chrono::microseconds(100));

			// Only check time in sleep phase (expensive operation)
			if ((wait_iters - YIELD_ITERS) % TIME_CHECK_INTERVAL == 0) {
				if (std::chrono::steady_clock::now() - start_time > timeout) {
					return nullptr;  // Read wait timeout for buffer
				}
			}
		}
	}

	bufs_[bufIdx].cons.reader_head.store(batch_header->start_logical_offset, std::memory_order_relaxed);
	return static_cast<void*>(batch_header);
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
		while (!shutdown_ && bufs_[bufIdx].prod.tail.load(std::memory_order_relaxed) - bufs_[bufIdx].cons.reader_head.load(std::memory_order_relaxed) < BATCH_SIZE) {
			std::this_thread::yield();
		}

		size_t head = bufs_[bufIdx].cons.reader_head.load(std::memory_order_relaxed);

		// Memory barrier to ensure we see the latest data
		//std::atomic_thread_fence(std::memory_order_acquire);

		len = bufs_[bufIdx].prod.tail.load(std::memory_order_relaxed) - head;
		if (len == 0) {
			return nullptr;
		}

		// For order level 3, always return a fixed batch size
		len = BATCH_SIZE;
		bufs_[bufIdx].cons.reader_head.fetch_add(BATCH_SIZE, std::memory_order_relaxed);

		return static_cast<void*>((uint8_t*)bufs_[bufIdx].buffer + head);
	} else {
		// For other order levels, read all available data
		while (!shutdown_ && bufs_[bufIdx].prod.tail.load(std::memory_order_relaxed) <= bufs_[bufIdx].cons.reader_head.load(std::memory_order_relaxed)) {
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

		size_t head = bufs_[bufIdx].cons.reader_head.load(std::memory_order_relaxed);

		// Memory barrier to ensure we see the latest data
		//std::atomic_thread_fence(std::memory_order_acquire);

		size_t tail = bufs_[bufIdx].prod.tail.load(std::memory_order_relaxed);
		len = tail - head;

		// Update reader head to current tail
		bufs_[bufIdx].cons.reader_head.store(tail, std::memory_order_relaxed);

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
