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
 * - bufs_[i].tail: Current write position in the buffer
 * - bufs_[i].writer_head: Start position of the current batch
 * - bufs_[i].num_msg: Number of messages in the current batch
 * - write_buf_id_: ID of the buffer currently being written to
 * - batch_seq_: Atomically incremented sequence number for batches
 *
 * READER-CONTROLLED VARIABLES:
 * - bufs_[i].reader_head: Position from which the reader is currently reading
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

	// Update header with current message info
	header_.paddedSize = paddedSize;
	header_.size = len;
	header_.client_order = client_order;

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
	}

	// (NOTE) Current logic does not restrictively check if newly written message goes out of BATCH_SIZE
	// If new message is very large (unlikely) it can degrade performance as a batch can be too large
	// to send over network.
	
	// Write header and message to buffer
	memcpy(static_cast<void*>((uint8_t*)buffer + tail), &header_, header_size);
	memcpy(static_cast<void*>((uint8_t*)buffer + tail + header_size), msg, len);

	// Memory barrier to ensure data is visible to readers
	//std::atomic_thread_fence(std::memory_order_release);

	// Update buffer state
	bufs_[write_buf_id_].tail += paddedSize;
	bufs_[write_buf_id_].num_msg++;
	
	// Check if current batch has reached BATCH_SIZE and seal it
	if ((bufs_[write_buf_id_].tail - head) >= BATCH_SIZE) {
		Seal();
	}


	return true;
}

void Buffer::Seal(){
	size_t lockedIdx = write_buf_id_;
	size_t head = bufs_[lockedIdx].writer_head;
	// Check if any data written
	if ((bufs_[lockedIdx].tail - head) > sizeof(Embarcadero::BatchHeader)) {
		Embarcadero::BatchHeader* batch_header = 
			reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[lockedIdx].buffer + head);

		batch_header->start_logical_offset = bufs_[lockedIdx].tail;
		batch_header->batch_seq = batch_seq_.fetch_add(1);
		batch_header->total_size = bufs_[lockedIdx].tail - head - sizeof(Embarcadero::BatchHeader);
		batch_header->num_msg = bufs_[lockedIdx].num_msg;

		// Update buffer state for next batch
		bufs_[lockedIdx].num_msg = 0;
		bufs_[lockedIdx].writer_head = bufs_[lockedIdx].tail;
		bufs_[lockedIdx].tail += sizeof(Embarcadero::BatchHeader);

		// Move to next buffer
		AdvanceWriteBufId();
	}
}

void* Buffer::Read(int bufIdx) {
	Embarcadero::BatchHeader* batch_header =
		reinterpret_cast<Embarcadero::BatchHeader*>((uint8_t*)bufs_[bufIdx].buffer + bufs_[bufIdx].reader_head);

	// Check if batch header contains valid data
	if (batch_header->total_size != 0 && batch_header->num_msg != 0) {
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

		bufs_[bufIdx].reader_head = next_head;
		return static_cast<void*>(batch_header);
	}

	// No writing in this buffer. Do not busy wait
	if (bufs_[bufIdx].writer_head == bufs_[bufIdx].tail) {
		return nullptr;
	}

	// Busy wait only when some messages are in the buffer.
	// Wait for the batch to be sealed. (Either full batch written or sealed by Client)
	// TODO(Jae) Replace checkcing total_size and num_msg if replace num_brokers in the batch header to seal
	auto start_time = std::chrono::steady_clock::now();
	const auto timeout = std::chrono::milliseconds(100);
	while (batch_header->total_size == 0 || batch_header->num_msg == 0) {
		auto current_time = std::chrono::steady_clock::now();
		if (current_time - start_time > timeout) {
			VLOG(4) << "Read wait timeout for buffer " << bufIdx;
			return nullptr; // Return null after timeout
		}
		std::this_thread::yield();
	}

	bufs_[bufIdx].reader_head = batch_header->start_logical_offset;
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
